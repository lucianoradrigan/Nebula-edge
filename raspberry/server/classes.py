from __future__ import annotations
from contextlib import contextmanager
from datetime import datetime
from psycopg2.pool import ThreadedConnectionPool
import asyncio
import socket
import queue
import threading
import time
from typing import Dict, Optional, Callable, Any

from ble import *
from mqtt import *

from bleak import BleakScanner, BleakClient
from bleak.exc import BleakDBusError
from bleak.backends.device import BLEDevice
from enum import Enum

from models import Timeouts, Data_1, Data_2, ConfigData, ConfigAckData, Log
from codec import DataCodec
from system import utc_epoch_now, BLEAdapterResolver, LocalWifiConfig
from config_resolver import ConfigResolver, ConfigDecision
from router import PacketRouter, PacketOutcome
from transport import Transport, UdpTransport


class MasterConnection:
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

            try:
                async with BleakClient(
                    device,
                    timeout=self.timeouts.ble_connect_sec,
                    adapter=self.ble_adapter,
                ) as client:
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
                task = asyncio.create_task(self._device_session(device, config))
                self.active_tasks[addr] = task

            except Exception as e:
                print(f"Fallo en la primera conexión BLE con {addr}: {type(e).__name__}: {e!r}")
                # traceback.print_exc()
                print(f"Pop device {addr}")
                self.devices.pop(addr, None)

            finally:
                # Reactiva scanner
                if self.scanner is not None:
                    async with self.scanner_lock:
                        await self._scanner_start()

    async def _device_session(self, device: BLEDevice, initial_config: "ConfigData"):
        """Wrapper de sesión por dispositivo, asegura limpieza al terminar."""
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
            )
        finally:
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

async def handle_protocol(
    device: BLEDevice,
    db_dsn: str,
    initial_config: ConfigData,
    scanner_lock: asyncio.Lock | None = None,
    scanner_stop = None,
    scanner_start = None,
    ble_adapter: str | None = None,
    timeouts: Timeouts | None = None,
):
    """Despacha la sesión según el protocolo configurado en `ConfigData`."""
    config = initial_config
    idx = None
    timeouts = timeouts or Timeouts()
    database_repo = DatabaseRepository(db_dsn)
    session_classes = {
        0: MQTTDeviceSession,
        1: UDPDeviceSession,
        2: TCPDeviceSession,
        3: BLEDeviceSession,
    }

    device_id = device.address
    while True:
        idx = config.protocol_conf
        session_cls = session_classes.get(idx)
        if session_cls is None:
            print(f"Protocol_conf inválido ({idx}) para {device_id}. Cerrando sesión.")
            idx = -1
            break

        session = session_cls(
            device,
            config,
            database_repo,
            scanner_lock,
            scanner_stop,
            scanner_start,
            ble_adapter,
            timeouts,
        )

        # Heartbeat / loggeo rutinario
        heartbeat_task = asyncio.create_task(session._protocol_heartbeat_loop())
        try:
            config = await session.run()
        finally:
            heartbeat_task.cancel()
            try:
                await heartbeat_task
            except asyncio.CancelledError:
                pass

        if config is None:
            print(f"Sesión finalizada para {device_id}")
            break
        
    # Se retorna último protocolo, con propósito de loggeo
    return idx

class DeviceSession:
    """Clase base para sesiones de protocolo (MQTT/UDP/TCP/BLE)."""
    def __init__(
        self,
        device: BLEDevice,                              # Dispositivo BLE asociado a la sesión
        initial_config: ConfigData,                     # Configuración inicial del dispositivo
        database_repo: DatabaseRepository,              # Repositorio general de base de datos
        scanner_lock: asyncio.Lock | None = None,       # Lock para coordinar el scanner BLE
        scanner_stop: Callable[[], Any] | None = None,  # Callback para detener el scanner
        scanner_start: Callable[[], Any] | None = None, # Callback para iniciar el scanner
        ble_adapter: str | None = None,                 # Adaptador BLE a usar
        timeouts: Timeouts | None = None,               # Timeouts centralizados
    ):
        """Inicializa contexto de dispositivo y repositorios."""
        self.device = device                            # Dispositivo BLE asociado a la sesión
        self.device_id = device.address                 # ID del dispositivo (MAC)
        self.database_repo = database_repo              # Repositorio compartido para config y telemetría
        self.config = initial_config                    # Configuración actual en memoria del device
        self.scanner_lock = scanner_lock                # Lock para coordinar el scanner BLE
        self.scanner_stop = scanner_stop                # Función para detener el scanner
        self.scanner_start = scanner_start              # Función para iniciar el scanner
        self.ble_adapter = ble_adapter or "hci1"         # Adaptador BLE a usar
        self.timeouts = timeouts or Timeouts()          # Timeouts centralizados
        self._router = PacketRouter(database_repo)      # Decodifica + persiste paquetes de telemetría
        self._last_client_time: int | None = None       # Último time_client recibido desde Data_1/Data_2

    def _update_last_client_time(self, data: Any):
        """Actualiza el último timestamp de cliente observado en paquetes de datos."""
        ts = getattr(data, "time_client", None)
        if isinstance(ts, int):
            self._last_client_time = ts

    async def _protocol_heartbeat_loop(self, interval_sec: float = 10.0):
        """Inserta un log periódico mientras el protocolo de sesión está activo."""
        while True:
            await asyncio.sleep(interval_sec)
            if self._last_client_time is None:
                continue
            server_time = utc_epoch_now()
            try:
                await self.database_repo.insert_log_async(
                    Log(
                        id_device=self.device_id,
                        status_report=2,
                        protocol_report=self.config.protocol_conf,
                        batt_level=100,
                        time_client=self._last_client_time,
                        time_server=server_time,
                    )
                )
            except Exception as e:
                print(f"No se pudo insertar heartbeat para {self.device_id}: {e}")

    def _sleep_timeout_sec(self) -> float:
        """Calcula timeout de recepción según `send_interval_s` (s) y `sleep_time_s` (s)."""
        send_interval_s = max(0, self.config.send_interval_s or 0)
        discontinuous_sleep_s = max(0, self.config.sleep_time_s or 0)
        send_interval = float(send_interval_s)
        discontinuous_sleep = float(discontinuous_sleep_s)
        base = max(send_interval, discontinuous_sleep)

        if base > 0:
            grace = max(self.timeouts.no_data_grace_sec, 1.2 * base) # default 2 * base
            return base + grace
        return self.timeouts.no_data_grace_sec

    async def _proactive_config_push(self, push_and_wait: Callable[["ConfigData"], Any]) -> "ConfigData | None":
        """Consulta la BD sin haber recibido un paquete y empuja la config si cambió.

        Se llama entre reintentos de espera (cada `config_poll_sec`) para no depender
        de que llegue telemetría para detectar un cambio de config. `push_and_wait(db_config)`
        debe enviar la config por el canal correspondiente y esperar su ACK, retornando
        True si se confirmó.
        """
        db_config = await self.database_repo.get_config_async(self.device_id)
        if db_config is None or db_config.config_version <= self.config.config_version:
            return None
        applied = await push_and_wait(db_config)
        return db_config if applied else None

class ProtocolSession(DeviceSession):
    """Sesión genérica: el flujo es idéntico para todos los protocolos.

    Lo único que cambia entre uno y otro es cómo se mueven los bytes, y eso
    vive en un `Transport` (transport.py). Una sesión concreta solo declara
    cuál usar:

        class UDPDeviceSession(ProtocolSession):
            transport_cls = UdpTransport

    Por ahora solo UDP corre por acá; MQTT/TCP/BLE siguen con su propia
    implementación mientras se portan de a uno.
    """

    transport_cls: type[Transport]

    async def _wait_ack(self, tx: Transport, db_config: "ConfigData") -> bool:
        """Espera un ACK de config válido. Retorna True si el device la aplicó.

        Mientras espera sigue procesando la telemetría que llegue (no se
        descarta data por estar en medio de un cambio de config).
        """
        for _ in range(self.timeouts.config_ack_retries):
            deadline = time.monotonic() + self.timeouts.config_ack_sec
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break

                pkt = await tx.recv(remaining)
                if pkt is None:
                    break

                ack = DataCodec.deserialize_config_ack(pkt)
                if (
                    ack
                    and ack.id_device == self.device_id
                    and ack.config_version == db_config.config_version
                    and ack.applied
                ):
                    print(f"ACK {tx.name} recibido para {self.device_id} v{db_config.config_version}")
                    return True

                # No era ACK: si es telemetría se inserta y se sigue esperando.
                routed = await self._router.route(pkt, self.device_id, source=tx.name)
                if routed.outcome != PacketOutcome.TELEMETRY:
                    continue
                self._update_last_client_time(routed.data)

        print(f"ACK {tx.name} de config v{db_config.config_version} no recibido para {self.device_id}. Cerrando sesión.")
        return False

    async def _push_and_wait(self, tx: Transport, db_config: "ConfigData") -> bool:
        """Envía una config nueva al device y espera su ACK."""
        print(f"Cambio de protocolo: {self.config.protocol_conf} -> {db_config.protocol_conf} para {self.device_id}")
        try:
            await tx.send(DataCodec.serialize_config(db_config))
        except Exception as e:
            print(f"Error enviando config {tx.name}: {e}")
            return False
        return await self._wait_ack(tx, db_config)

    async def run(self) -> "ConfigData | None":
        async with self.transport_cls(self.config) as tx:
            while True:
                # Espera un paquete, sondeando la BD proactivamente en ventanas
                # cortas mientras no llega nada (no depende de que llegue
                # telemetría para enterarse de un cambio de config).
                timeout_sec = self._sleep_timeout_sec()
                deadline = time.monotonic() + timeout_sec
                while True:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        print(f"Timeout {tx.name} ({timeout_sec}s) sin datos de {self.device_id}. " "Cerrando sesión para permitir reconexión.")
                        return None

                    packet = await tx.recv(min(remaining, self.timeouts.config_poll_sec))
                    if packet is not None:
                        break

                    # Sin paquete: aprovecha de revisar si cambió la config en BD.
                    if tx.can_send:
                        new_cfg = await self._proactive_config_push(
                            lambda cfg: self._push_and_wait(tx, cfg)
                        )
                        if new_cfg is not None:
                            return new_cfg

                routed = await self._router.route(packet, self.device_id, source=tx.name)
                if routed.outcome == PacketOutcome.IGNORED:
                    continue
                if routed.outcome == PacketOutcome.DEEP_SLEEP:
                    print(f"{tx.name}: Se detectó deep sleep de {self.device_id}, se sigue escuchando")
                    continue
                data = routed.data
                self._update_last_client_time(data)

                # Obtiene configuración desde DB
                db_config = await self.database_repo.get_config_async(self.device_id)
                if db_config is None:
                    continue

                # Compara versiones actuales de config DEVICE vs versión BD
                applied_version = data.config_version_applied
                decision = ConfigResolver.evaluate(applied_version, db_config, self.config)
                if decision.decision == ConfigDecision.APPLIED_NEWER:
                    print(f"Config aplicada detectada en {tx.name} ({applied_version}) para {self.device_id}. " "Cerrando sesión para reconfigurar.")
                    return decision.db_config
                elif decision.decision == ConfigDecision.ALREADY_SENT:
                    # Ya se envió esta config en el cambio de protocolo; espera que el device la aplique
                    continue
                elif decision.decision == ConfigDecision.PUSH:
                    applied = await self._push_and_wait(tx, decision.db_config)
                    return decision.db_config if applied else None

class MQTTDeviceSession(DeviceSession):
    """Sesión MQTT: recibe data, persiste y aplica cambios de config con ACK."""

    async def _wait_ack_mqtt(self, ack_queue, db_config: "ConfigData") -> bool:
        """Espera ACK válido por MQTT. Retorna True si se confirmó."""
        for _ in range(self.timeouts.config_ack_retries):
            try:
                payload = await asyncio.to_thread(
                    ack_queue.get,
                    True,
                    self.timeouts.config_ack_sec,
                )
                ack = DataCodec.deserialize_config_ack(payload)
                if (
                    ack
                    and ack.id_device == self.device_id
                    and ack.config_version == db_config.config_version
                    and ack.applied
                ):
                    print(f"ACK MQTT de recibido para {self.device_id} v{db_config.config_version}")
                    return True
            except queue.Empty:
                pass

        print(f"ACK MQTT de config v{db_config.config_version} no recibido para {self.device_id}. Cerrando sesión.")
        return False

    async def _push_and_wait(self, config_topic: str, ack_queue, db_config: "ConfigData") -> bool:
        """Publica config y espera su ACK. Adapta al formato de _proactive_config_push."""
        print(f"Cambio de protocolo: {self.config.protocol_conf} -> {db_config.protocol_conf} para {self.device_id}")
        serialized_config = DataCodec.serialize_config(db_config)
        mqtt_publish(config_topic, serialized_config)
        return await self._wait_ack_mqtt(ack_queue, db_config)

    async def run(self):
        # mqtt_start() es idempotente: conecta el cliente MQTT compartido del
        # proceso solo si aún no está corriendo. Esta sesión NO lo apaga al
        # terminar (ver mqtt.py): otros devices en MQTT pueden seguir usándolo.
        mqtt_start()

        device_queue = get_data_queue(self.device_id)
        ack_queue =  get_ack_queue(self.device_id)
        config_topic = f"/topic/nebulaedge/{self.device_id}/config"

        while True:
            data = None

            # Espera por un paquete, sondeando la BD proactivamente en ventanas cortas
            # mientras no llega nada (no depende de que llegue telemetría).
            full_timeout = self._sleep_timeout_sec()
            deadline = time.monotonic() + full_timeout
            packet = None
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    print(f"Timeout MQTT ({full_timeout}s) sin datos de {self.device_id}. " "Cerrando sesión para permitir reconexión.")
                    return None
                wait = min(remaining, self.timeouts.config_poll_sec)
                try:
                    packet = await asyncio.to_thread(device_queue.get, True, wait)
                    break
                except queue.Empty:
                    new_cfg = await self._proactive_config_push(
                        lambda cfg: self._push_and_wait(config_topic, ack_queue, cfg)
                    )
                    if new_cfg is not None:
                        return new_cfg
                    continue

            try:
                routed = await self._router.route(packet, self.device_id, source="MQTT")
                if routed.outcome == PacketOutcome.IGNORED:
                    continue
                if routed.outcome == PacketOutcome.DEEP_SLEEP:
                    print(f"MQTT: Se detectó deep sleep de {self.device_id}")
                    continue
                data = routed.data
                self._update_last_client_time(data)
            except Exception as e:
                print(f"Error procesando datos: {e}")
                continue

            # Pausa para que otras tareas de asyncio se ejecuten (otras sesiones)
            await asyncio.sleep(self.timeouts.mqtt_poll_sec)

            # Obtiene configuración desde DB
            db_config = await self.database_repo.get_config_async(self.device_id)
            if db_config is None:
                continue

            # Compara versiones actuales de config DEVICE vs versión BD
            applied_version = data.config_version_applied
            decision = ConfigResolver.evaluate(applied_version, db_config, self.config)
            if decision.decision == ConfigDecision.APPLIED_NEWER:
                print(f"Config aplicada detectada en MQTT ({applied_version}) para {self.device_id}. " "Cerrando sesión para reconfigurar.")
                return decision.db_config
            elif decision.decision == ConfigDecision.ALREADY_SENT:
                # Ya se envió esta config en el cambio de protocolo; espera que el device la aplique
                continue
            elif decision.decision == ConfigDecision.PUSH:
                applied = await self._push_and_wait(config_topic, ack_queue, decision.db_config)
                return decision.db_config if applied else None

class UDPDeviceSession(ProtocolSession):
    """Sesión UDP: toda la lógica está en ProtocolSession, solo cambia el transporte."""
    transport_cls = UdpTransport

class TCPDeviceSession(DeviceSession):
    """Sesión TCP: recibe data, persiste y aplica cambios de config con ACK."""
    async def _send_config(self, conn: socket.socket, db_config: "ConfigData") -> bool:
        """Serializa y envía configuración vía TCP. Retorna booleano en caso de éxito o error."""
        loop = asyncio.get_running_loop()
        print(f"Cambio de protocolo: {self.config.protocol_conf} -> {db_config.protocol_conf} para {self.device_id}")
        serialized_config = DataCodec.serialize_config(db_config)

        try:
            await loop.sock_sendall(conn, serialized_config)
            return True
        except Exception as e:
            print(f"Error enviando config TCP: {e}")
            return False

    async def _wait_ack(self, conn: socket.socket, db_config: "ConfigData") -> "ConfigData | None":
        """Espera ACK válido por TCP. Retorna la configuración aplicada, o None si es que hubo error."""
        loop = asyncio.get_running_loop()

        # Intentos que se harán
        for _ in range(self.timeouts.config_ack_retries):
            
            # Espera ACK dentro de una ventana temporal (timeout total por intento)
            deadline = time.monotonic() + self.timeouts.config_ack_sec
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                try:
                    pkt = await asyncio.wait_for(loop.sock_recv(conn, 1024), timeout=remaining)
                except asyncio.TimeoutError:
                    break
                if not pkt:
                    return None

                # Si el paquete es ACK válido, termina el cambio de protocolo
                ack = DataCodec.deserialize_config_ack(pkt)
                if (
                    ack
                    and ack.id_device == self.device_id
                    and ack.config_version == db_config.config_version
                    and ack.applied
                ):
                    print(f"ACK TCP recibido para {self.device_id} v{db_config.config_version}")
                    return db_config

                # Si llega telemetría durante la espera, se inserta y se sigue esperando
                routed = await self._router.route(pkt, self.device_id, source="TCP")
                if routed.outcome != PacketOutcome.TELEMETRY:
                    continue
                self._update_last_client_time(routed.data)

        print(f"ACK TCP de config v{db_config.config_version} no recibido para {self.device_id}. Cerrando sesión.")
        return None

    async def _push_and_wait(self, conn: socket.socket, db_config: "ConfigData") -> bool:
        """Envía config y espera su ACK. Adapta _send_config/_wait_ack al formato de _proactive_config_push."""
        if not await self._send_config(conn, db_config):
            return False
        return await self._wait_ack(conn, db_config) is not None

    async def run(self):
        loop = asyncio.get_running_loop()
        host = '0.0.0.0'
        port = self.config.tcp_port
        print("Entrando a TCP")

        # While para soportar cierres y aperturas de socket por deep sleep
        while True:

            # Apertura de socket
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                s.setblocking(False)
                s.bind((host, port))
                s.listen()
                print(f'Servidor TCP escuchando en {host}:{port}')

                # Timeout de espera de conexion segun la configuracion actual
                timeout_sec = self._sleep_timeout_sec()
                try:
                    conn, tcp_addr = await asyncio.wait_for(loop.sock_accept(s), timeout=timeout_sec)
                except asyncio.TimeoutError:
                    print(f"Timeout esperando conexion TCP ({timeout_sec}s)")
                    return None

                print('Conexión establecida desde', tcp_addr)

                with conn:
                    # Para que otras tasks ejecuten en "paralelo"
                    conn.setblocking(False)

                    # While de recepción de datos y consulta de config a DB
                    while True:

                        # Recibe dato, sondeando la BD proactivamente en ventanas cortas
                        # mientras no llegue nada (no depende de que llegue telemetría).
                        timeout_sec = self._sleep_timeout_sec()
                        deadline = time.monotonic() + timeout_sec
                        packet = None
                        while True:
                            remaining = deadline - time.monotonic()
                            if remaining <= 0:
                                print(f"Timeout TCP ({timeout_sec}s) sin datos de {self.device_id}. " "Cerrando sesión para permitir reconexión.")
                                return None
                            wait = min(remaining, self.timeouts.config_poll_sec)
                            try:
                                packet = await asyncio.wait_for(loop.sock_recv(conn, 1024), timeout=wait)
                                break
                            except asyncio.TimeoutError:
                                new_cfg = await self._proactive_config_push(
                                    lambda cfg: self._push_and_wait(conn, cfg)
                                )
                                if new_cfg is not None:
                                    return new_cfg
                                continue
                            except Exception as e:
                                print(f"Recepción TCP falló: {e}")
                                break
                        if not packet:
                            print("Conexión TCP cerrada por el otro extremo.")
                            break

                        ## Común

                        routed = await self._router.route(packet, self.device_id, source="TCP")
                        if routed.outcome == PacketOutcome.IGNORED:
                            continue
                        if routed.outcome == PacketOutcome.DEEP_SLEEP:
                            print(f"TCP: Se detectó deep sleep de {self.device_id}, reinicia socket")
                            break
                        data = routed.data
                        self._update_last_client_time(data)

                        # Obtiene configuración desde DB
                        db_config = await self.database_repo.get_config_async(self.device_id)
                        if db_config is None:
                            continue

                        ## Común

                        # Compara versiones actuales de config DEVICE vs versión BD
                        applied_version = data.config_version_applied
                        decision = ConfigResolver.evaluate(applied_version, db_config, self.config)
                        if decision.decision == ConfigDecision.APPLIED_NEWER:
                            print(f"Config aplicada detectada en TCP ({applied_version}) para {self.device_id}. " "Cerrando sesión para reconfigurar.")
                            return decision.db_config
                        elif decision.decision == ConfigDecision.ALREADY_SENT:
                            # Ya se envió esta config en el cambio de protocolo; espera que el device la aplique
                            continue
                        elif decision.decision == ConfigDecision.PUSH:
                            if not await self._send_config(conn, decision.db_config):
                                return None
                            new_cfg = await self._wait_ack(conn, decision.db_config)
                            return new_cfg

class BLEDeviceSession(DeviceSession):
    """Sesión BLE: recibe notificaciones y aplica cambios de config con ACK."""

    async def _send_config_and_wait_ack(self, client: BleakClient, ack_queue: "asyncio.Queue[bytes]", db_config: "ConfigData") -> bool:
        """Envía config por BLE y espera ACK válido."""
        config_packet = DataCodec.serialize_config(db_config)

        await client.write_gatt_char(UUID_CHAR_A, config_packet, response=True)
        print(
            f"Enviando config v{db_config.config_version} por BLE a {self.device_id} "
            f"(proto {self.config.protocol_conf}->{db_config.protocol_conf})"
        )

        print("Se escribió configuración nueva en la característica A")
        if not await self._wait_ble_ack(client, ack_queue, db_config.config_version):
            print(f"ACK BLE no recibido para {self.device_id} v{db_config.config_version}")
            return False
            
        print(f"ACK BLE recibido. Cerrando sesión para aplicar config v{db_config.config_version} " f"en {self.device_id}")
        return True

    async def _wait_ble_ack(
        self,
        client: BleakClient,
        ack_queue: "asyncio.Queue[bytes]",
        expected_version: int,
        timeout_sec: float | None = None,
        retries: int | None = None,
    ) -> bool:
        """Espera ACK BLE y, si se pierde el notify, reconcilia leyendo la config aplicada.

        Flujo:
        - Espera notificación en char D.
        - Si no llega, lee char D y/o char A para confirmar versión aplicada.
        """
        timeout_sec = timeout_sec or self.timeouts.ble_ack_short_sec
        retries = retries or self.timeouts.config_ack_retries

        for _ in range(retries):
            try:
                payload = await asyncio.wait_for(ack_queue.get(), timeout=timeout_sec)
            except asyncio.TimeoutError:
                payload = None

            if payload:
                if payload in (b"ok", b"OK"):
                    await asyncio.sleep(0.1)
                    continue

                ack = DataCodec.deserialize_config_ack(payload)
                if (
                    ack
                    and ack.id_device == self.device_id
                    and ack.config_version == expected_version
                    and ack.applied
                ):
                    return True

            # Reconciliación: leer ACK persistente en D
            try:
                ack_payload = await asyncio.wait_for(
                    client.read_gatt_char(UUID_CHAR_D),
                    timeout=timeout_sec,
                )
                ack = DataCodec.deserialize_config_ack(ack_payload)
                if (
                    ack
                    and ack.id_device == self.device_id
                    and ack.config_version == expected_version
                    and ack.applied
                ):
                    return True
            except asyncio.TimeoutError:
                pass
            except Exception:
                pass

            # Reconciliación: leer config persistente en A
            try:
                cfg_payload = await asyncio.wait_for(
                    client.read_gatt_char(UUID_CHAR_A),
                    timeout=timeout_sec,
                )
                cfg = DataCodec.deserialize_config(cfg_payload)
                if (
                    cfg
                    and cfg.id_device == self.device_id
                    and cfg.config_version == expected_version
                ):
                    return True
            except asyncio.TimeoutError:
                pass
            except Exception:
                pass

            await asyncio.sleep(0.1)
        return False

    async def run(self):
        """Mantiene una conexión BLE persistente y procesa notificaciones."""

        # While para soportar cierres y aperturas de socket por deep sleep
        while True:

            # Variable para hacer continue de este while desde el while anidado
            # continue_outer = False
            
            try:
                paused_scanner = False

                if self.scanner_lock is not None and self.scanner_stop is not None:
                    async with self.scanner_lock:
                        await self.scanner_stop()
                        paused_scanner = True

                print(f"BLE: Modo persistente. Intentando conectar al dispositivo {self.device_id}")

                # Durante este bloque estará emparejado con la ESP32
                async with BleakClient(self.device, adapter=self.ble_adapter) as client:
                    if not client.is_connected:
                        print(f"No se pudo conectar a {self.device_id} para RECIBIR DATOS. Reintentando")
                        await asyncio.sleep(0.2)
                        continue

                    if paused_scanner and self.scanner_lock is not None and self.scanner_start is not None:
                        async with self.scanner_lock:
                            await self.scanner_start()
                        paused_scanner = False

                    print(f"BLE persistente iniciado correctamente para {self.device_id}")
                        
                    # Señal de inicio por característica C (señaliza semáforo en ESP32)
                    await client.write_gatt_char(UUID_CHAR_C, b'start', response=True)

                    # Inicializa queues
                    loop = asyncio.get_running_loop()
                    ack_queue: asyncio.Queue[bytes] = asyncio.Queue()
                    data_queue: asyncio.Queue[bytes] = asyncio.Queue()

                    # Callbacks para encolar datos que llegan por notificaciones
                    def _on_ack(_, data: bytearray):
                        loop.call_soon_threadsafe(ack_queue.put_nowait, bytes(data))
                    def _on_data(_, data: bytearray):
                        loop.call_soon_threadsafe(data_queue.put_nowait, bytes(data))

                    # Char A: configuración
                    # Char B: telemetría
                    # Char C: semáforo (write)
                    # Char D: ACKs (notify)
                    await client.start_notify(UUID_CHAR_D, _on_ack)
                    await client.start_notify(UUID_CHAR_B, _on_data)

                    # Bucle recepción telemetría
                    timeout_sec = self._sleep_timeout_sec()
                    deadline = time.monotonic() + timeout_sec
                    while True:
                        try:
                            await asyncio.sleep(0.1)

                            remaining = deadline - time.monotonic()
                            if remaining <= 0:
                                print(f"Timeout BLE ({timeout_sec}s) sin datos de {self.device_id}. " "Cerrando conexión para permitir reconexión.")
                                await client.stop_notify(UUID_CHAR_B)
                                await client.stop_notify(UUID_CHAR_D)
                                return None

                            # Sondea la BD proactivamente en ventanas cortas mientras no
                            # llega nada, sin depender de que llegue telemetría.
                            wait = min(remaining, self.timeouts.config_poll_sec)
                            try:
                                packet = await asyncio.wait_for(data_queue.get(), timeout=wait)
                            except asyncio.TimeoutError:
                                new_cfg = await self._proactive_config_push(
                                    lambda cfg: self._send_config_and_wait_ack(client, ack_queue, cfg)
                                )
                                if new_cfg is not None:
                                    await client.stop_notify(UUID_CHAR_B)
                                    await client.stop_notify(UUID_CHAR_D)
                                    return new_cfg
                                continue

                            # Llegó un paquete real: se renueva la ventana completa
                            timeout_sec = self._sleep_timeout_sec()
                            deadline = time.monotonic() + timeout_sec

                            # Común

                            routed = await self._router.route(packet, self.device_id, source="BLE")
                            if routed.outcome == PacketOutcome.IGNORED:
                                continue
                            if routed.outcome == PacketOutcome.DEEP_SLEEP:
                                print(f"BLE: Dispositivo {self.device_id} entrando en deep sleep. Cerrando sesión para permitir reconexión.")
                                await client.stop_notify(UUID_CHAR_B)
                                await client.stop_notify(UUID_CHAR_D)

                                # Vuelve a intentar conexión BLE persistente (cuando despierte)
                                await asyncio.sleep(timeout_sec)
                                break

                            data = routed.data
                            self._update_last_client_time(data)

                            # Obtiene configuración de BD
                            db_config = await self.database_repo.get_config_async(self.device_id)
                            if db_config is None:
                                continue

                            # Compara versiones actuales de config DEVICE vs versión BD
                            applied_version = data.config_version_applied
                            decision = ConfigResolver.evaluate(applied_version, db_config, self.config)

                            # Caso mayor: una configuración nueva ya se aplicó por lo que el server tiene que adecuarse
                            if decision.decision == ConfigDecision.APPLIED_NEWER:
                                print(f"Config aplicada detectada en BLE ({applied_version}) para {self.device_id}. " "Cerrando sesión para reconfigurar.")
                                await client.stop_notify(UUID_CHAR_B)
                                await client.stop_notify(UUID_CHAR_D)
                                return decision.db_config

                            # Ya se envió esta config en el cambio de protocolo; espera que el device la aplique
                            elif decision.decision == ConfigDecision.ALREADY_SENT:
                                continue

                            # Caso menor: se detecta configuración nueva en la BD y se envía al device
                            elif decision.decision == ConfigDecision.PUSH:
                                if not await self._send_config_and_wait_ack(client, ack_queue, decision.db_config):
                                    await client.stop_notify(UUID_CHAR_B)
                                    await client.stop_notify(UUID_CHAR_D)
                                    return None

                                await client.stop_notify(UUID_CHAR_B)
                                await client.stop_notify(UUID_CHAR_D)
                                return decision.db_config

                        except Exception as e:
                            print(f"Error: {e}")
                            continue
                    
            except TimeoutError:
                print(f"Timeout conectando al dispositivo {self.device_id}. Reintentando")
                await asyncio.sleep(0.2)
            finally:
                if (
                    self.scanner_lock is not None
                    and self.scanner_start is not None
                    and self.scanner_stop is not None
                ):
                    # Asegura que el scanner quede activo si falló la conexión
                    async with self.scanner_lock:
                        await self.scanner_start()

        return None

class DatabaseRepository:
    """Repositorio general para interactuar con la BD.

    OJO: antes cada método (get_config/insert_data_1/insert_data_2/insert_log)
    hacía `with psycopg2.connect(self.db_dsn) as db:` por llamada. Ese "with"
    de psycopg2 solo hace commit/rollback al salir, NO cierra la conexión: quedaba
    una conexión abierta sin cerrar por cada paquete de telemetría insertado.
    Con 2 inserts por send_interval_s por device (Data_1 + Data_2), eso agota
    max_connections de Postgres en minutos. Ahora se pide/devuelve una conexión
    de un pool compartido (ver _connection()), del mismo modo para todos los
    métodos, sin cambiar ninguna de sus firmas ni la forma en que se instancia
    DatabaseRepository en el resto del archivo.
    """

    # Un pool por DSN, compartido por todas las instancias de DatabaseRepository
    # (el código de más abajo instancia esta clase "al pasar" en varios sitios,
    # p.ej. `DatabaseRepository(self.db_dsn).insert_log_async(...)`; si el pool
    # fuera de instancia, cada una de esas instancias efímeras abriría el suyo).
    # ThreadedConnectionPool porque insert_*_async/get_config_async corren en
    # threads del pool de asyncio.to_thread, no todos en el mismo hilo.
    _pools: dict[str, ThreadedConnectionPool] = {}
    _pools_lock = threading.Lock()

    def __init__(self, db_dsn: str):
        """Guarda DSN para pedir conexiones al pool compartido."""
        self.db_dsn = db_dsn

    def _get_pool(self) -> ThreadedConnectionPool:
        db_pool = self._pools.get(self.db_dsn)
        if db_pool is not None:
            return db_pool
        with self._pools_lock:
            db_pool = self._pools.get(self.db_dsn)
            if db_pool is None:
                # minconn=1: no abre conexiones de más si nunca se usa este DSN.
                # maxconn=20: generoso para la cantidad de devices esperada y
                # cómodo bajo el max_connections=100 por defecto de Postgres.
                db_pool = ThreadedConnectionPool(1, 20, self.db_dsn)
                self._pools[self.db_dsn] = db_pool
        return db_pool

    @contextmanager
    def _connection(self):
        """Pide una conexión del pool y siempre la devuelve al salir.

        Si algo falla dentro del `with`, hace rollback antes de devolverla:
        sin esto, una conexión reciclada del pool quedaría con una
        transacción abierta y la siguiente consulta que la reciba fallaría
        con "current transaction is aborted".
        """
        db_pool = self._get_pool()
        conn = db_pool.getconn()
        try:
            yield conn
        except Exception:
            conn.rollback()
            raise
        finally:
            db_pool.putconn(conn)

    @staticmethod
    def _int_to_db_datetime(value: int) -> datetime:
        """Convierte un epoch Unix (segundos, UTC real) a datetime naive UTC
        para guardar en columnas TIMESTAMP (sin huso horario) de Postgres.

        Asume que `value` es un epoch UTC de verdad. Antes no lo era
        siempre: `time_client` (del device) sí, pero `time_server`
        (calculado acá con la vieja `local_epoch_now()`) traía sumado el
        offset horario local, así que dos columnas de la misma fila de
        `log` quedaban en escalas de tiempo distintas. Ver utc_epoch_now()
        en system.py.
        """
        return datetime.utcfromtimestamp(int(value))

    def get_config(self, device_id: str) -> ConfigData | None:
        """Obtiene la configuración de un dispositivo específico."""
        with self._connection() as db:
            with db.cursor() as cursor:
                cursor.execute("""
                    SELECT id_device, config_version, protocol_conf, acc_sampling, gyro_sensibility,
                        bme688_sampling, send_interval_s, sleep_time_s, sleep_window_size,
                        tcp_port, udp_port, mqtt_broker
                    FROM nebulaedge_schema.config
                    WHERE id_device = %s;
                """, (device_id,))

                row = cursor.fetchone()

                if row:
                    host_ip_addr, ssid, passwd = LocalWifiConfig.get(cache_ttl_sec=0)
                    time_cli = utc_epoch_now()
                    # print(
                    #     f"[TIME] send_utc_epoch={time_cli} "
                    #     f"local_now={datetime.fromtimestamp(time_cli)}"
                    # )
                    return ConfigData(
                        id_device=row[0],
                        config_version=row[1],
                        protocol_conf=row[2],
                        acc_sampling=row[3],
                        gyro_sensibility=row[4],
                        bme688_sampling=row[5],
                        send_interval_s=row[6],
                        sleep_time_s=row[7],
                        sleep_window_size=row[8],
                        tcp_port=row[9],
                        udp_port=row[10],
                        host_ip_addr=host_ip_addr,
                        ssid=ssid,
                        passwd=passwd,
                        mqtt_broker=row[11],
                        time_client=time_cli        # Timestamp en segundos
                    )
                else:
                    return None

    def insert_data_1(self, data_1: Data_1):
        """Inserta datos de sensor en la BD."""
        try:
            with self._connection() as db:
                with db.cursor() as cursor:
                    cursor.execute("""
                        INSERT INTO nebulaedge_schema.data_1 (
                            id_device, temperature, press, hum, co, rms,
                            amp_x, freq_x, amp_y, freq_y, amp_z, freq_z,
                            mag_x, mag_y, mag_z, config_version_applied, time_client
                        ) VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s)
                    """, (
                        data_1.id_device,
                        data_1.temperature,
                        data_1.press,
                        data_1.hum,
                        data_1.co,
                        data_1.rms,
                        data_1.amp_x,
                        data_1.freq_x,
                        data_1.amp_y,
                        data_1.freq_y,
                        data_1.amp_z,
                        data_1.freq_z,
                        data_1.mag_x,
                        data_1.mag_y,
                        data_1.mag_z,
                        data_1.config_version_applied,
                        self._int_to_db_datetime(data_1.time_client)
                    ))
                    db.commit()
        except AttributeError:
            return
        except Exception as e:
            print(e)

    def insert_data_2(self, data_2: "Data_2"):
        """Inserta datos Data_2 en la BD."""
        try:
            with self._connection() as db:
                with db.cursor() as cursor:
                    cursor.execute("""
                        INSERT INTO nebulaedge_schema.data_2 (
                            id_device, acc_x, acc_y, acc_z,
                            gyr_x, gyr_y, gyr_z,
                            config_version_applied, time_client
                        ) VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s)
                    """, (
                        data_2.id_device,
                        data_2.acc_x,
                        data_2.acc_y,
                        data_2.acc_z,
                        data_2.gyr_x,
                        data_2.gyr_y,
                        data_2.gyr_z,
                        data_2.config_version_applied,
                        self._int_to_db_datetime(data_2.time_client),
                    ))
                    db.commit()
        except AttributeError:
            return
        except Exception as e:
            print(e)

    def insert_log(self, log: "Log"):
        """Inserta un evento de log en la BD."""
        try:
            with self._connection() as db:
                with db.cursor() as cursor:
                    cursor.execute("""
                        INSERT INTO nebulaedge_schema.log (
                            id_device, status_report, protocol_report, batt_level,
                            time_client, time_server
                        ) VALUES (%s, %s, %s, %s, %s, %s)
                    """, (
                        log.id_device,
                        log.status_report,
                        log.protocol_report,
                        log.batt_level,
                        self._int_to_db_datetime(log.time_client),
                        self._int_to_db_datetime(log.time_server),
                    ))
                    db.commit()
        except AttributeError:
            return
        except Exception as e:
            print(e)

    # psycopg2 es sincrónico/bloqueante: cada método de arriba abre su propia
    # conexión y espera la red. Llamado directo desde una corutina, congela
    # el event loop completo (todas las demás sesiones de devices se detienen).
    # Estos envoltorios delegan al thread pool de asyncio, igual que ya se hace
    # con device_queue.get/ack_queue.get en las sesiones MQTT.
    async def get_config_async(self, device_id: str) -> ConfigData | None:
        return await asyncio.to_thread(self.get_config, device_id)

    async def insert_data_1_async(self, data_1: "Data_1") -> None:
        await asyncio.to_thread(self.insert_data_1, data_1)

    async def insert_data_2_async(self, data_2: "Data_2") -> None:
        await asyncio.to_thread(self.insert_data_2, data_2)

    async def insert_log_async(self, log: "Log") -> None:
        await asyncio.to_thread(self.insert_log, log)

if __name__ == "__main__":
    master = MasterConnection()
    try:
        asyncio.run(master.run())
    except KeyboardInterrupt:
        print("\nCerrando programa...")
        # # Cancelar todas las tasks activas
        # for task in master.active_tasks:
        #     task.cancel()
        # print("Tasks canceladas")
