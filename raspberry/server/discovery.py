"""Descubrimiento BLE de los nodos y arranque de su sesión.

UTILIDAD PRINCIPAL
    Es la puerta de entrada de todo dispositivo al sistema. Escanea anuncios
    BLE sin parar y, por cada ESP32 que reconoce como propio, hace el
    handshake inicial y le abre una sesión.

FLUJO POR DISPOSITIVO DESCUBIERTO
    1. Filtra el anuncio por nombre y manufacturer data (`_is_target_advertisement`).
    2. Busca su configuración en Postgres usando la MAC como id_device.
    3. Se conecta por BLE y escribe la config en la característica A.
    4. Registra la conexión en la tabla `log`.
    5. Lanza una task de sesión (`protocol_dispatch.handle_protocol`) y vuelve a escanear.

CONCURRENCIA
    Cada dispositivo corre en su propia task de asyncio, así que varios nodos
    avanzan en paralelo. `active_tasks` evita abrir dos sesiones para el mismo
    device y `last_connect_attempt` impone un cooldown entre reintentos.

CUIDADO CON EL SCANNER
    bleak recomienda no escanear mientras se establece una conexión, así que
    el escaneo se detiene antes de conectar y se reactiva después. Como las
    sesiones BLE también necesitan pausarlo, el acceso va protegido por
    `scanner_lock` y los callbacks `_scanner_stop`/`_scanner_start` se le
    pasan a la sesión.
"""
from __future__ import annotations
import asyncio
import time
from enum import Enum
from typing import Dict

from bleak import BleakScanner, BleakClient
from bleak.exc import BleakDBusError
from bleak.backends.device import BLEDevice

from gatt_uuids import UUID_CHAR_A
from models import Timeouts, ConfigData, Log
from codec import DataCodec
from system import utc_epoch_now, BLEAdapterResolver
from repository import DatabaseRepository
from protocol_dispatch import handle_protocol, PROTOCOL_BLE


class DeviceDiscovery:
    """Gestiona el descubrimiento BLE y crea sesiones por dispositivo.

    Flujo general:
    1. Escanea anuncios BLE y filtra por nombre y manufacturer data.
    2. Obtiene configuración inicial desde la BD.
    3. Escribe la config inicial por BLE.
    4. Lanza una sesión por dispositivo para manejar el protocolo activo.
    """
    class State(Enum):
        DISCOVERED = 0
        CONNECTING = 1
        CONNECTED = 2

    def __init__(
        self,
        name_target="ESP_NEBULAEDGE",
        db_dsn="host=localhost dbname=nebulaedge user=nebulaedge password=1234",
        max_parallel_connects: int = 8,
        timeouts: Timeouts | None = None,
    ):
        """Inicializa parámetros de escaneo BLE y acceso a BD."""
        self.name_target = name_target                                      # Nombre BLE objetivo
        self.db_dsn = db_dsn                                                # DSN de conexión a la base de datos
        self.devices = {}                                                   # Estado por dispositivo descubierto
        self.shared_queue = asyncio.Queue()                                 # Cola de dispositivos descubiertos
        self.max_parallel_connects = max_parallel_connects                  # Límite de conexiones simultáneas
        self.timeouts = timeouts or Timeouts()                              # Timeouts centralizados
        self.last_connect_attempt: Dict[str, float] = {}                    # Diccionario de últimos intentos por dispositivo
        self.active_tasks: Dict[str, asyncio.Task] = {}                     # Tasks activas por dispositivo
        self.scanner = None                                                 # Instancia de BleakScanner
        self.scanner_lock = asyncio.Lock()                                  # Lock para start/stop del scanner
        self.scanner_running = False                                        # Estado de escaneo
        self.ble_adapter = BLEAdapterResolver.resolve()                     # Forzar adaptador BLE
        print(f"[BLE] Usando adaptador: {self.ble_adapter}")

    async def _scanner_stop(self):
        """Detiene el escaneo BLE si está activo."""
        if self.scanner is None:
            return
        if not self.scanner_running:
            return
        try:
            await self.scanner.stop()
            self.scanner_running = False
        except BleakDBusError as e:
            print(f"[BLE] stop scan falló: {e}")

    async def _scanner_start(self):
        """Inicia el escaneo BLE si está detenido."""
        if self.scanner is None:
            return
        if self.scanner_running:
            return
        try:
            await self.scanner.start()
            self.scanner_running = True
        except BleakDBusError as e:
            print(f"[BLE] start scan falló: {e}")

    def _is_target_advertisement(self, device, adv) -> bool:
        """Valida si el advertisement BLE corresponde a un dispositivo target."""
        name = adv.local_name
        if self.name_target != name:
            return False

        # Validar manufacturer data (acepta formatos típicos de Bleak)
        mfg_data = adv.manufacturer_data or {}
        payload = mfg_data.get(76, b"")  # 0x004C Apple (usado en firmware)
        if payload:
            return payload.endswith(b"\x01") or payload.endswith(b"\x02")
        return False

    def on_connect(self, device, adv):
        """Callback de descubrimiento BLE usado por BleakScanner cuando se detecta advertisements de
           cualquier dispositivo. Encola los dispositivos target."""
        addr = device.address
        now = time.monotonic()

        # Evita la reconexión a un mismo dispositivo
        if addr in self.devices or addr in self.active_tasks:
            return

        # Chequea que sea un dispositivo objetivo
        if not self._is_target_advertisement(device, adv):
            return

        # Evita reconexiones demasiado frecuentes por dispositivo (cooldown)
        last = self.last_connect_attempt.get(addr, 0.0)
        if now - last < self.timeouts.connect_cooldown_sec:
            return
        self.last_connect_attempt[addr] = now

        # Cambia el estado del dispositivo y encola
        self.devices[addr] = self.State.DISCOVERED
        self.shared_queue.put_nowait(device)

    async def connection_worker(self):
        """Consume la cola de descubrimientos y crea sesiones BLE por dispositivo. Debe correr en un task aparte."""
        print("Esperando conexiones BLE...")
        while True:
            device = await self.shared_queue.get()
            addr = device.address

            # Comprobación para evitar dos task para el mismo dispositivo
            if addr in self.active_tasks:
                continue

            self.devices[addr] = self.State.CONNECTING
            print(f"Device {addr} encontrado")

            config = None
            try:
                config = await DatabaseRepository(self.db_dsn).get_config_async(addr)
                if config:
                    print(f"[WiFi] SSID: {config.ssid}, Contraseña: {config.passwd}")
            except Exception as e:
                print(f"Error al conectar/obtener config para {addr}: {e}")
                self.devices.pop(addr, None)
                continue

            if config is None:
                print(f"No se encontró configuración para device_id {addr}")
                self.devices.pop(addr, None)
                continue

            serialized_config = DataCodec.serialize_config(config)

            # Cierra scanner activo para realizar conexión BT (recomendado en documentación de Bleak)
            if self.scanner is not None:
                async with self.scanner_lock:
                    await self._scanner_stop()

            # Si el protocolo es BLE, esta misma conexión es la que va a usar
            # la sesión: cerrarla acá obligaría al transporte a reconectar de
            # inmediato, y cada conexión nueva paga otro descubrimiento de
            # servicios en BlueZ. Para los otros protocolos sí se cierra: el
            # device se va a WiFi y no vuelve a usar BLE.
            reuse_connection = config.protocol_conf == PROTOCOL_BLE
            client = BleakClient(
                device,
                timeout=self.timeouts.ble_connect_sec,
                adapter=self.ble_adapter,
            )
            connection_handed_over = False

            try:
                await client.connect()
                print(f"Conectado exitosamente a {addr}")
                self.devices[addr] = self.State.CONNECTED
                await client.write_gatt_char(UUID_CHAR_A, serialized_config, response=True)

                # Registra envío de configuración inicial al dispositivo.
                await DatabaseRepository(self.db_dsn).insert_log_async(
                    Log(
                        id_device=config.id_device,
                        status_report=1,
                        protocol_report=config.protocol_conf,
                        batt_level=100,
                        time_client=config.time_client,
                        time_server=config.time_client
                    )
                )

                if self.active_tasks:
                    print(f"Tasks activas: {len(self.active_tasks)} -> {list(self.active_tasks.keys())}")
                else:
                    print("Tasks activas: 0")
                print(f"Creando task de sesión por dispositivo {addr}")
                task = asyncio.create_task(
                    self._device_session(device, config, client if reuse_connection else None)
                )
                self.active_tasks[addr] = task
                connection_handed_over = reuse_connection

            except Exception as e:
                print(f"Fallo en la primera conexión BLE con {addr}: {type(e).__name__}: {e!r}")
                # traceback.print_exc()
                print(f"Pop device {addr}")
                self.devices.pop(addr, None)

            finally:
                # La conexión se cierra salvo que se la haya entregado a una
                # sesión, que a partir de ese momento es su dueña.
                if not connection_handed_over:
                    try:
                        await client.disconnect()
                    except Exception:
                        pass

                # Reactiva scanner
                if self.scanner is not None:
                    async with self.scanner_lock:
                        await self._scanner_start()

    async def _device_session(self, device: BLEDevice, initial_config: "ConfigData", ble_client=None):
        """Wrapper de sesión por dispositivo, asegura limpieza al terminar.

        `ble_client` es la conexión que quedó abierta del handshake inicial
        cuando el protocolo es BLE. La sesión la adopta y normalmente la cierra
        ella misma; el cierre de acá es la red de seguridad para cuando nunca
        llegó a adoptarla (protocol_conf inválido, excepción al abrir, cancelación).
        """
        last_protocol = -1
        try:
            last_protocol = await handle_protocol(
                device,
                self.db_dsn,
                initial_config,
                scanner_lock=self.scanner_lock,
                scanner_stop=self._scanner_stop,
                scanner_start=self._scanner_start,
                ble_adapter=self.ble_adapter,
                timeouts=self.timeouts,
                ble_client=ble_client,
            )
        finally:
            if ble_client is not None and ble_client.is_connected:
                try:
                    await ble_client.disconnect()
                except Exception:
                    pass
            addr = device.address
            self.active_tasks.pop(addr, None)
            server_time = utc_epoch_now()
            # Permite re-descubrimiento si se pierde la conexión
            print(f"Pop device {addr}")
            # Registra la desconexión
            await DatabaseRepository(self.db_dsn).insert_log_async(
                Log(
                    id_device=addr,
                    status_report=0,
                    protocol_report=last_protocol,
                    batt_level=100,
                    time_client=server_time,
                    time_server=server_time
                )
            )
            self.devices.pop(addr, None)

    async def run(self):
        """Inicia el escaneo BLE y reinicia periódicamente el adaptador."""
        asyncio.create_task(self.connection_worker())
        self.scanner = BleakScanner(self.on_connect, adapter=self.ble_adapter)
        async with self.scanner_lock:
            await self._scanner_start()
        try:
            while True:
                # Reinicia el escaneo periódicamente para evitar bloqueos del adaptador BLE
                await asyncio.sleep(self.timeouts.scan_restart_sec)
                async with self.scanner_lock:
                    await self._scanner_stop()
                    await self._scanner_start()
        finally:
            if self.scanner is not None:
                async with self.scanner_lock:
                    await self._scanner_stop()
