from __future__ import annotations
import psycopg2
import schema_pb2
import asyncio
import socket
import queue
import os
import time
import traceback
import subprocess
from typing import Dict, Optional, Callable, Any

from ble import *
from mqtt import *

from bleak import BleakScanner, BleakClient
from bleak.exc import BleakDBusError
from bleak.backends.device import BLEDevice
from enum import Enum
from dataclasses import dataclass
from psycopg2.extensions import connection

def _detect_usb_adapter() -> str:
    """Devuelve el nombre del adaptador BLE con Bus USB si existe.

    Usa `hciconfig` y busca la interfaz cuyo bus sea USB (ej: hci1).
    Retorna cadena vacía si no encuentra uno.
    """
    try:
        result = subprocess.run(["hciconfig"], capture_output=True, text=True, check=True)
    except Exception:
        return ""
    current = ""
    for line in result.stdout.splitlines():
        line = line.strip()
        if line.startswith("hci") and ":" in line:
            current = line.split(":", 1)[0]
            if "Bus: USB" in line:
                return current
        elif current and "Bus: USB" in line:
            return current
    return ""

def _resolve_ble_adapter() -> str:
    """Resuelve el adaptador BLE a usar.

    Prioridad:
    1) `BLE_ADAPTER` si es un valor explícito (no `auto`/`usb`).
    2) Autodetección de adaptador USB.
    3) Fallback a `hci1`.
    """
    env_value = os.getenv("BLE_ADAPTER", "").strip()
    if env_value and env_value.lower() not in ("auto", "usb"):
        return env_value
    detected = _detect_usb_adapter()
    if detected:
        return detected
    return "hci1" if not env_value else env_value

class LocalWifiConfig:
    """Obtiene datos de WiFi local usando nmcli con cache temporal."""
    _CACHE_TTL_SEC = 10.0
    _CACHE: dict[str, object] = {
        "ts": 0.0,
        "ssid": "",
        "passwd": "",
        "host_ip_addr": "",
    }

    @staticmethod
    def _run_cmd(args: list[str]) -> str:
        """Ejecuta un comando y retorna stdout limpio o cadena vacía en error."""
        try:
            result = subprocess.run(args, capture_output=True, text=True, check=True)
            return result.stdout.strip()
        except Exception:
            return ""

    @classmethod
    def _active_wifi_device(cls) -> str:
        """Detecta el dispositivo WiFi activo conectado (ej: wlan0)."""
        output = cls._run_cmd(["sudo", "nmcli", "-t", "-f", "DEVICE,TYPE,STATE", "dev", "status"])
        for line in output.splitlines():
            parts = line.split(":", 2)
            if len(parts) != 3:
                continue
            device, dev_type, state = parts
            if dev_type == "wifi" and state == "connected":
                return device
        return ""

    @classmethod
    def _active_connection_name(cls, device: str) -> str:
        """Obtiene el nombre de la conexión activa para el dispositivo dado."""
        if not device:
            return ""
        return cls._run_cmd(["sudo", "nmcli", "-t", "-g", "GENERAL.CONNECTION", "dev", "show", device])

    @classmethod
    def _active_wifi_ssid(cls, device: str, conn_name: str) -> str:
        """Obtiene el SSID activo desde el nombre de conexión o el scan actual."""
        if conn_name:
            ssid = cls._run_cmd(["sudo", "nmcli", "-t", "-g", "802-11-wireless.ssid", "connection", "show", conn_name])
            if ssid:
                return ssid
        output = cls._run_cmd(["sudo", "nmcli", "-t", "-f", "ACTIVE,SSID", "dev", "wifi"])
        for line in output.splitlines():
            if line.startswith("yes:"):
                return line.split(":", 1)[1]
        return ""

    @classmethod
    def _active_wifi_psk(cls, conn_name: str) -> str:
        """Obtiene la PSK guardada de la conexión activa (si existe)."""
        if not conn_name:
            return ""
        return cls._run_cmd(["sudo", "nmcli", "-s", "-g", "802-11-wireless-security.psk", "connection", "show", conn_name])

    @classmethod
    def _active_wifi_ip(cls, device: str) -> str:
        """Obtiene la IP IPv4 asignada al dispositivo WiFi activo."""
        if device:
            ip_addr = cls._run_cmd(["sudo", "nmcli", "-t", "-g", "IP4.ADDRESS", "dev", "show", device])
            if ip_addr:
                return ip_addr.split("/", 1)[0]
        return ""

    @staticmethod
    def _local_ip_fallback() -> str:
        """Resuelve IP local abriendo un socket UDP a internet como fallback."""
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
                s.connect(("8.8.8.8", 80))
                return s.getsockname()[0]
        except Exception:
            return ""

    @classmethod
    def get(cls, cache_ttl_sec: float | None = None) -> tuple[str, str, str]:
        """Retorna (host_ip_addr, ssid, passwd) con cache de corto plazo."""
        ttl = cls._CACHE_TTL_SEC if cache_ttl_sec is None else cache_ttl_sec
        now = time.monotonic()
        last_ts = float(cls._CACHE.get("ts", 0.0))
        if now - last_ts <= ttl:
            return (
                str(cls._CACHE.get("host_ip_addr", "")),
                str(cls._CACHE.get("ssid", "")),
                str(cls._CACHE.get("passwd", "")),
            )

        device = cls._active_wifi_device()
        conn_name = cls._active_connection_name(device)
        ssid = cls._active_wifi_ssid(device, conn_name)
        passwd = cls._active_wifi_psk(conn_name)
        host_ip_addr = cls._active_wifi_ip(device) or cls._local_ip_fallback()

        cls._CACHE.update({
            "ts": now,
            "ssid": ssid or "",
            "passwd": passwd or "",
            "host_ip_addr": host_ip_addr or "",
        })
        return host_ip_addr or "", ssid or "", passwd or ""

@dataclass
class Timeouts:
    ble_connect_sec: float = 30.0          # Timeout de intentos de conexión BLE (BleakClient)
    scan_restart_sec: float = 60.0         # Reinicio periódico de scanner BLE
    connect_cooldown_sec: float = 20.0     # Cooldown entre reconexiones por device
    config_ack_sec: float = 60.0           # Ventana para ACK de config (MQTT/UDP/TCP/BLE)
    config_ack_retries: int = 10           # Reintentos de ACK de config (MQTT/UDP/TCP/BLE)
    no_data_grace_sec: float = 60.0        # Gracia extra al esperar un dato
    mqtt_poll_sec: float = 0.1             # Sleep de polling MQTT
    mqtt_queue_timeout_sec: float = 1.0    # Timeout de device_queue.get()
    ble_ack_short_sec: float = 3.0         # Timeout corto por intento en _wait_ble_ack
    ble_conn_retries: int = 10             # Reintentos de conexión BLE persistente

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
        self.ble_adapter = _resolve_ble_adapter()                            # Forzar adaptador BLE
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
                return

            self.devices[addr] = self.State.CONNECTING
            print(f"Device {addr} encontrado")

            try:
                db_conn = psycopg2.connect(self.db_dsn)
                config = ConfigRepository(db_conn).get(addr)
            except Exception as e:
                print(f"Error al conectar/obtener config para {addr}: {e}")
                self.devices.pop(addr, None)
                continue
            finally:
                if db_conn:
                    db_conn.close()

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

                if self.active_tasks:
                    print(f"Tasks activas: {len(self.active_tasks)} -> {list(self.active_tasks.keys())}")
                else:
                    print("Tasks activas: 0")
                print(f"Creando task de sesión por dispositivo {addr}")
                task = asyncio.create_task(self._device_session(device, config))
                self.active_tasks[addr] = task

            except Exception as e:
                print(f"Fallo en la primera conexión BLE con {addr}: {type(e).__name__}: {e!r}")
                traceback.print_exc()
                self.devices.pop(addr, None)

            finally:
                # Reactiva scanner
                if self.scanner is not None:
                    async with self.scanner_lock:
                        await self._scanner_start()

    async def _device_session(self, device: BLEDevice, initial_config: "ConfigData"):
        """Wrapper de sesión por dispositivo, asegura limpieza al terminar."""
        try:
            await handle_protocol(
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
            # Permite re-descubrimiento si se pierde la conexión
            print(f"Pop device {addr}")
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
    scanner_stop=None,
    scanner_start=None,
    ble_adapter: str | None = None,
    timeouts: Timeouts | None = None,
):
    """Despacha la sesión según el protocolo configurado en `ConfigData`."""
    config = initial_config
    timeouts = timeouts or Timeouts()
    db_conn = psycopg2.connect(db_dsn)
    config_repo = ConfigRepository(db_conn)
    telemetry_repo = TelemetryRepository(db_conn)

    device_id = device.address

    while True:
        idx = config.protocol_conf
        if idx == 0:
            config = await MQTTDeviceSession(
                device,
                config,
                config_repo,
                telemetry_repo,
                scanner_lock,
                scanner_stop,
                scanner_start,
                ble_adapter,
                timeouts,
            ).run()
        elif idx == 1:
            config = await UDPDeviceSession(
                device,
                config,
                config_repo,
                telemetry_repo,
                scanner_lock,
                scanner_stop,
                scanner_start,
                ble_adapter,
                timeouts,
            ).run()
        elif idx == 2:
            config = await TCPDeviceSession(
                device,
                config,
                config_repo,
                telemetry_repo,
                scanner_lock,
                scanner_stop,
                scanner_start,
                ble_adapter,
                timeouts,
            ).run()
        elif idx == 3:
            config = await BLEDeviceSession(
                device,
                config,
                config_repo,
                telemetry_repo,
                scanner_lock,
                scanner_stop,
                scanner_start,
                ble_adapter,
                timeouts,
            ).run()

        if config is None:
            print(f"Sesión finalizada para {device_id}")
            break

class DeviceSession:
    """Clase base para sesiones de protocolo (MQTT/UDP/TCP/BLE)."""
    def __init__(
        self,
        device: BLEDevice,                              # Dispositivo BLE asociado a la sesión
        initial_config: ConfigData,                     # Configuración inicial del dispositivo
        config_repo: ConfigRepository,                  # Repositorio de configuración (BD)
        telemetry_repo: TelemetryRepository,            # Repositorio de telemetría (BD)
        scanner_lock: asyncio.Lock | None = None,       # Lock para coordinar el scanner BLE
        scanner_stop: Callable[[], Any] | None = None,  # Callback para detener el scanner
        scanner_start: Callable[[], Any] | None = None, # Callback para iniciar el scanner
        ble_adapter: str | None = None,                 # Adaptador BLE a usar
        timeouts: Timeouts | None = None,               # Timeouts centralizados
    ):
        """Inicializa contexto de dispositivo y repositorios."""
        self.device = device                            # Dispositivo BLE asociado a la sesión
        self.device_id = device.address                 # ID del dispositivo (MAC)
        self.config_repo = config_repo                  # Acceso a configuraciones en BD
        self.telemetry_repo = telemetry_repo            # Acceso a telemetría en BD
        self.config = initial_config                    # Configuración actual en memoria del device
        self.scanner_lock = scanner_lock                # Lock para coordinar el scanner BLE
        self.scanner_stop = scanner_stop                # Función para detener el scanner
        self.scanner_start = scanner_start              # Función para iniciar el scanner
        self.ble_adapter = ble_adapter or "hci1"         # Adaptador BLE a usar
        self.timeouts = timeouts or Timeouts()          # Timeouts centralizados
        self.ble_conn_retries = timeouts.ble_conn_retries

    def _sleep_timeout_sec(self) -> float:
        """Calcula timeout de recepción según `send_interval_ms` y `discontinuous_sleep_time` (ms)."""
        send_interval_ms = max(0, self.config.send_interval_ms or 0)
        discontinuous_sleep_ms = max(0, self.config.discontinuous_sleep_time or 0)
        send_interval = send_interval_ms / 1000.0
        discontinuous_sleep = discontinuous_sleep_ms / 1000.0
        base = max(send_interval, discontinuous_sleep)

        if base > 0:
            grace = max(self.timeouts.no_data_grace_sec, 2 * base)
            return base + grace
        return self.timeouts.no_data_grace_sec

class MQTTDeviceSession(DeviceSession):
    """Sesión MQTT: recibe data, persiste y aplica cambios de config con ACK."""
    async def run(self):
        mqtt_start()

        device_queue = get_data_queue(self.device_id)
        ack_queue = get_ack_queue(self.device_id)
        data_topic = f"/topic/nebulaedge/{self.device_id}/data"
        config_topic = f"/topic/nebulaedge/{self.device_id}/config"
        
        while True:
            try:
                # Espera por un paquete
                serialized_data_1 = device_queue.get(timeout=self.timeouts.mqtt_queue_timeout_sec)
                data_1 = DataCodec.deserialize_data_1(serialized_data_1)
                
                if data_1 is None:
                    print("Paquete MQTT vacío")
                    continue
                
                print(f'Paquete MQTT recibido de {self.device_id}')
                self.telemetry_repo.insert_data_1(data_1)    

            except queue.Empty:
                pass  

            except Exception as e:
                print(f"Error procesando datos: {e}")

            await asyncio.sleep(self.timeouts.mqtt_poll_sec)
            
            # Obtiene configuración desde DB
            new_config = self.config_repo.get(self.device_id)
            if new_config is None:
                continue

            # Detecta cambio por versión
            if self.config.config_version < new_config.config_version:
                print(f"Cambio de protocolo: {self.config.protocol_conf} -> {new_config.protocol_conf} para {self.device_id}")

                # Serializa y envía nueva configuración (espera ACK antes de cambiar)
                serialized_config = DataCodec.serialize_config(new_config)

                mqtt_publish(config_topic, serialized_config)
                for _ in range(self.timeouts.config_ack_retries):
                    try:
                        payload = ack_queue.get(timeout=self.timeouts.config_ack_sec)
                        ack = DataCodec.deserialize_config_ack(payload)
                        if (
                            ack
                            and ack.id_device == self.device_id
                            and ack.config_version == new_config.config_version
                            and ack.applied
                        ):
                            mqtt_shutdown()
                            return new_config
                    except queue.Empty:
                        pass

                print(f"ACK MQTT de config v{new_config.config_version} no recibido para {self.device_id}. Cerrando sesión.")
                mqtt_shutdown()
                return None
            
class UDPDeviceSession(DeviceSession):
    """Sesión UDP: recibe data, persiste y aplica cambios de config con ACK."""
    async def _send_config_and_wait_ack(self, sock: socket.socket, udp_addr, db_config: "ConfigData") -> "ConfigData | None":
        """Envía config por UDP y espera ACK válido."""

        loop = asyncio.get_running_loop()
        print(f"Cambio de protocolo: {self.config.protocol_conf} -> {db_config.protocol_conf} para {self.device_id}")
        serialized_config = DataCodec.serialize_config(db_config)

        for _ in range(self.timeouts.config_ack_retries):
            await loop.sock_sendto(sock, serialized_config, udp_addr)
            # Espera ACK dentro de una ventana temporal (timeout total por intento)
            deadline = time.monotonic() + self.timeouts.config_ack_sec
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                try:
                    pkt, _ = await asyncio.wait_for(loop.sock_recvfrom(sock, 1024), timeout=remaining)
                except asyncio.TimeoutError:
                    break

                # Si el paquete es ACK válido, termina el cambio de protocolo
                ack = DataCodec.deserialize_config_ack(pkt)
                if (
                    ack
                    and ack.id_device == self.device_id
                    and ack.config_version == db_config.config_version
                    and ack.applied
                ):
                    print(f"ACK UDP recibido para {self.device_id} v{db_config.config_version}")
                    return db_config

                # Si llega telemetría durante la espera, se inserta y se sigue esperando
                data_1 = DataCodec.deserialize_data_1(pkt)
                if data_1 is not None:
                    self.telemetry_repo.insert_data_1(data_1)
                    continue
        print(f"ACK UDP de config v{db_config.config_version} no recibido para {self.device_id}. Cerrando sesión.")
        return None

    async def run(self):
        loop = asyncio.get_running_loop()
        host = '0.0.0.0'                        # Escucha en todas las interfaces
        port = self.config.udp_port              # Puerto del servidor

        # Se crea un socket IPv4, UDP
        # En caso de fallar, retornar. handle_protocol hará un nuevo intento
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:

            # Reutiliza puerto si es que está abierto
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            
            # Configura socket como no bloqueante
            s.setblocking(False)

            # Conexión al host
            s.bind((host, port))
            print(f'Servidor UDP escuchando en {host}:{port}')

            while True:
                # Escucha para recibir paquete (no bloqueante)
                timeout_sec = self._sleep_timeout_sec()
                try:
                    serialized_data_1, udp_addr = await asyncio.wait_for(loop.sock_recvfrom(s, 1024), timeout=timeout_sec)
                except asyncio.TimeoutError:
                    print(f"Timeout UDP ({timeout_sec}s) sin datos de {self.device_id}. " "Cerrando sesión para permitir reconexión.")
                    return None

                print(f'Paquete UDP recibido de {self.device_id} en puerto {port}')

                # Desempaqueta y obtiene protobuf tipo Data1
                data_1 = DataCodec.deserialize_data_1(serialized_data_1)
                if data_1 is None:
                    continue

                # Inserta en la base de datos
                self.telemetry_repo.insert_data_1(data_1)
                
                # Obtiene configuración desde DB
                db_config = self.config_repo.get(self.device_id)
                if db_config is None:
                    continue

                # Compara versiones actuales de config DEVICE vs versión BD
                applied_version = data_1.config_version_applied
                if applied_version > db_config.config_version:
                    print(f"Config aplicada detectada en UDP ({applied_version}) para {self.device_id}. " "Cerrando sesión para reconfigurar.")
                    return db_config
                elif applied_version < db_config.config_version:
                    if db_config.config_version <= self.config.config_version:
                        # Ya se envió esta config en el cambio de protocolo; espera que el device la aplique
                        continue
                    new_cfg = await self._send_config_and_wait_ack(s, udp_addr, db_config)
                    return new_cfg
                
class TCPDeviceSession(DeviceSession):
    """Sesión TCP: recibe data, persiste y aplica cambios de config con ACK."""
    async def _send_config_and_wait_ack(self, conn: socket.socket, db_config: "ConfigData") -> "ConfigData | None":
        """Envía config por TCP y espera ACK válido."""
        loop = asyncio.get_running_loop()
        print(f"Cambio de protocolo: {self.config.protocol_conf} -> {db_config.protocol_conf} para {self.device_id}")
        serialized_config = DataCodec.serialize_config(db_config)

        for _ in range(self.timeouts.config_ack_retries):
            try:
                await loop.sock_sendall(conn, serialized_config)
            except Exception as e:
                print(f"Error enviando/recibiendo ACK TCP: {e}")
                continue

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
                    return db_config

                # Si llega telemetría durante la espera, se inserta y se sigue esperando
                data_1 = DataCodec.deserialize_data_1(pkt)
                if data_1 is not None:
                    self.telemetry_repo.insert_data_1(data_1)
                    continue

        print(f"ACK TCP de config v{db_config.config_version} no recibido para {self.device_id}. Cerrando sesión.")
        return None

    async def run(self):
        loop = asyncio.get_running_loop()
        host = '0.0.0.0'
        port = self.config.tcp_port
        print("Entrando a TCP")
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.setblocking(False)
            s.bind((host, port))
            s.listen()
            print(f'Servidor TCP escuchando en {host}:{port}')
            conn, tcp_addr = await loop.sock_accept(s)
            print('Conexión establecida desde', tcp_addr)
            with conn:
                conn.setblocking(False)
                while True:
                    try:
                        data_1_packet = await loop.sock_recv(conn, 1024)
                        if not data_1_packet:
                            return None
                        print(f'Paquete TCP recibido de {self.device_id}')
                        data_1 = DataCodec.deserialize_data_1(data_1_packet)
                        if data_1 is None:
                            continue
                        self.telemetry_repo.insert_data_1(data_1)
                    except Exception as e:
                        print(f"Error inesperado: {e}")
                        return None

                    # Obtiene configuración desde DB
                    db_config = self.config_repo.get(self.device_id)
                    if db_config is None:
                        continue

                    # Compara versiones actuales de config DEVICE vs versión BD
                    applied_version = data_1.config_version_applied
                    if applied_version > db_config.config_version:
                        print(f"Config aplicada detectada en TCP ({applied_version}) para {self.device_id}. " "Cerrando sesión para reconfigurar.")
                        return db_config
                    elif applied_version < db_config.config_version:
                        if db_config.config_version <= self.config.config_version:
                            # Ya se envió esta config en el cambio de protocolo; espera que el device la aplique
                            continue
                        new_cfg = await self._send_config_and_wait_ack(conn, db_config)
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
        for attempt in range(self.ble_conn_retries):
            try:
                paused_scanner = False

                if self.scanner_lock is not None and self.scanner_stop is not None:
                    async with self.scanner_lock:
                        await self.scanner_stop()
                        paused_scanner = True

                # Durante este bloque estará emparejado con la ESP32
                async with BleakClient(self.device, adapter=self.ble_adapter) as client:
                    if not client.is_connected:
                        print(f"No se pudo conectar a {self.device_id} para RECIBIR DATOS")
                        await asyncio.sleep(0.2)
                        continue

                    if paused_scanner and self.scanner_lock is not None and self.scanner_start is not None:
                        async with self.scanner_lock:
                            await self.scanner_start()
                        paused_scanner = False

                    ## MODIFICACIÓN GPT
                    try:
                        if hasattr(client, "_backend") and hasattr(client._backend, "_get_services"):
                            await client._backend._get_services()
                        if hasattr(client, "_backend") and hasattr(client._backend, "_acquire_mtu"):
                            await client._backend._acquire_mtu()
                    except Exception as e:
                        print(f"[BLE] MTU negotiation falló: {e}")

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
                    packet_count = 0
                    window_size = self.config.discontinuous_window_size or 0
                    use_window = (self.config.discontinuous_sleep_time or 0) > 0 and window_size > 0
                    while True:
                        try:               
                            await asyncio.sleep(0.1)

                            print(f"Esperando datos vía notify...")
                            timeout_sec = self._sleep_timeout_sec()
                            data_1_packet = await asyncio.wait_for(data_queue.get(), timeout=timeout_sec)
                            data_1 = DataCodec.deserialize_data_1(data_1_packet)
                            if data_1 is None:
                                continue
                            print(f'Paquete BLE recibido de {self.device_id}')

                            # Caso deep sleep
                            packet_count += 1
                            if use_window and packet_count >= window_size:
                                print(
                                    f"Ventana BLE completa ({packet_count}/{window_size}) para {self.device_id}. "
                                    "Cerrando sesión para permitir deep sleep/reconexión."
                                )
                                await client.stop_notify(UUID_CHAR_B)
                                await client.stop_notify(UUID_CHAR_D)

                                # Cierra la task
                                return None

                            # Inserta en la base de datos
                            self.telemetry_repo.insert_data_1(data_1)
  
                            # Obtiene configuración de BD
                            db_config = self.config_repo.get(self.device_id)

                            if db_config is None:
                                continue

                            # Compara versiones actuales de config DEVICE vs versión BD
                            # Caso mayor: una configuración nueva ya se aplicó por lo que el server tiene que adecuarse
                            applied_version = data_1.config_version_applied
                            if applied_version > db_config.config_version:
                                print(f"Config aplicada detectada en BLE ({applied_version}) para {self.device_id}. " "Cerrando sesión para reconfigurar.")
                                await client.stop_notify(UUID_CHAR_B)
                                await client.stop_notify(UUID_CHAR_D)
                                return db_config

                            # Caso menor: se detecta configuración nueva en la BD y se envía al device
                            elif applied_version < db_config.config_version:
                                if db_config.config_version <= self.config.config_version:
                                    # Ya se envió esta config en el cambio de protocolo; espera que el device la aplique
                                    continue
                                if not await self._send_config_and_wait_ack(client, ack_queue, db_config):
                                    await client.stop_notify(UUID_CHAR_B)
                                    await client.stop_notify(UUID_CHAR_D)
                                    return None

                                await client.stop_notify(UUID_CHAR_B)
                                await client.stop_notify(UUID_CHAR_D)
                                return db_config

                        except asyncio.TimeoutError:
                            print(f"Timeout BLE ({timeout_sec}s) sin datos de {self.device_id}. " "Cerrando conexión para permitir reconexión.")
                            await client.stop_notify(UUID_CHAR_B)
                            await client.stop_notify(UUID_CHAR_D)
                            return None

                        except Exception as e:
                            print(f"Error: {e}")
                            continue
                    
            except TimeoutError:
                print(
                    f"Timeout conectando al dispositivo {self.device_id} "
                    f"(intento {attempt + 1}/{self.ble_conn_retries})"
                )
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

class ConfigRepository:
    """Repositorio de configuración de ESP32.

    Encapsula lecturas de configuración desde la BD para un dispositivo.
    """
    def __init__(self, db: connection):
        """Guarda la conexión a BD a reutilizar en consultas."""
        self.db = db

    def get(self, device_id: str) -> ConfigData | None:
        """Obtiene la configuración de un dispositivo específico."""
        cursor = self.db.cursor()
        try:
            cursor.execute("""
                SELECT id_device, config_version, protocol_conf, acc_sampling, gyro_sensibility,
                    bme688_sampling, send_interval_ms, discontinuous_sleep_time, discontinuous_window_size,
                    tcp_port, udp_port, mqtt_broker
                FROM config
                WHERE id_device = %s;
            """, (device_id,))
            
            row = cursor.fetchone()
            
            if row:
                host_ip_addr, ssid, passwd = LocalWifiConfig.get()
                return ConfigData(
                    id_device=row[0],
                    config_version=row[1] if row[1] is not None else 0,
                    protocol_conf=row[2],
                    acc_sampling=row[3],
                    gyro_sensibility=row[4],
                    bme688_sampling=row[5],
                    send_interval_ms=row[6],
                    discontinuous_sleep_time=row[7],
                    discontinuous_window_size=row[8],
                    tcp_port=row[9],
                    udp_port=row[10],
                    host_ip_addr=host_ip_addr,
                    ssid=ssid,
                    passwd=passwd,
                    mqtt_broker=row[11]
                )
            else:
                return None
        finally:
            cursor.close()

class TelemetryRepository:
    """Repositorio de datos generados por ESP32."""
    def __init__(self, db: connection):
        """Guarda la conexión a BD a reutilizar en inserts."""
        self.db = db

    def insert_data_1(self, data_1: Data_1):
        """Inserta datos de sensor en la BD."""
        cursor = self.db.cursor()
        try:
            cursor.execute("""
                INSERT INTO data_1 (
                    id_device, temperature, press, hum, co, rms,
                    amp_x, freq_x, amp_y, freq_y, amp_z, freq_z
                ) VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s)
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
                data_1.freq_z
            ))
            self.db.commit()
        finally:
            cursor.close()

class DataCodec:
    """Esta clase permite que TelemetryRepository se desligue de protobuf.
    En caso de querer cambiar la forma de enviar los datos (JSON por ejemplo)
    solo se tendrá que modificar esto."""
    @staticmethod
    def serialize_data_1(data: Data_1) -> bytes:
        """Convierte Data_1 -> protobuf Data_1 -> bytes."""
        pb = schema_pb2.Data_1()
        pb.id_device = data.id_device
        pb.temperature = data.temperature
        pb.press = data.press
        pb.hum = data.hum
        pb.co = data.co
        pb.rms = data.rms
        pb.amp_x = data.amp_x
        pb.freq_x = data.freq_x
        pb.amp_y = data.amp_y
        pb.freq_y = data.freq_y
        pb.amp_z = data.amp_z
        pb.freq_z = data.freq_z
        pb.config_version_applied = data.config_version_applied
        return pb.SerializeToString()

    @staticmethod
    def deserialize_data_1(packet: bytes) -> Data_1 | None:
        """Convierte bytes -> protobuf Data_1 -> Data_1."""
        try:
            pb = schema_pb2.Data_1()
            pb.ParseFromString(packet)
        except Exception as e:
            print(f"Error al desempaquetar el paquete: {e}")
            return None
        
        # Convertir a objeto neutro (Data_1)
        return Data_1(
            id_device=pb.id_device,
            temperature=pb.temperature,
            press=pb.press,
            hum=pb.hum,
            co=pb.co,
            rms=pb.rms,
            amp_x=pb.amp_x,
            freq_x=pb.freq_x,
            amp_y=pb.amp_y,
            freq_y=pb.freq_y,
            amp_z=pb.amp_z,
            freq_z=pb.freq_z,
            config_version_applied=pb.config_version_applied,
        )

    @staticmethod
    def serialize_config(config: ConfigData) -> bytes:
        """Convierte ConfigData -> protobuf Config -> bytes."""
        pb = schema_pb2.Config()
        pb.id_device = config.id_device
        pb.config_version = config.config_version
        pb.protocol_conf = config.protocol_conf
        pb.acc_sampling = config.acc_sampling
        pb.gyro_sensibility = config.gyro_sensibility
        pb.bme688_sampling = config.bme688_sampling
        pb.send_interval_ms = config.send_interval_ms
        pb.discontinuous_sleep_time = config.discontinuous_sleep_time
        pb.discontinuous_window_size = config.discontinuous_window_size
        pb.tcp_port = config.tcp_port
        pb.udp_port = config.udp_port
        pb.host_ip_addr = config.host_ip_addr
        pb.ssid = config.ssid
        pb.passwd = config.passwd
        pb.mqtt_broker = config.mqtt_broker
        return pb.SerializeToString()

    @staticmethod
    def deserialize_config(packet: bytes) -> ConfigData | None:
        """Convierte bytes -> protobuf Config -> ConfigData."""
        try:
            pb = schema_pb2.Config()
            pb.ParseFromString(packet)
        except Exception as e:
            print(f"Error al desempaquetar el paquete: {e}")
            return None

        return ConfigData(
            id_device=pb.id_device,
            config_version=pb.config_version,
            protocol_conf=pb.protocol_conf,
            acc_sampling=pb.acc_sampling,
            gyro_sensibility=pb.gyro_sensibility,
            bme688_sampling=pb.bme688_sampling,
            send_interval_ms=pb.send_interval_ms,
            discontinuous_sleep_time=pb.discontinuous_sleep_time,
            discontinuous_window_size=pb.discontinuous_window_size,
            tcp_port=pb.tcp_port,
            udp_port=pb.udp_port,
            host_ip_addr=pb.host_ip_addr,
            ssid=pb.ssid,
            passwd=pb.passwd,
            mqtt_broker=pb.mqtt_broker,
        )

    @staticmethod
    def serialize_config_ack(ack: "ConfigAckData") -> bytes:
        """Convierte ConfigAckData -> protobuf ConfigAck -> bytes."""
        pb = schema_pb2.ConfigAck()
        pb.id_device = ack.id_device
        pb.config_version = ack.config_version
        pb.applied = ack.applied
        return pb.SerializeToString()

    @staticmethod
    def deserialize_config_ack(packet: bytes) -> "ConfigAckData" | None:
        """Convierte bytes -> protobuf ConfigAck -> ConfigAckData."""
        try:
            pb = schema_pb2.ConfigAck()
            pb.ParseFromString(packet)
        except Exception as e:
            print(f"Error al desempaquetar el ACK: {e}")
            return None

        return ConfigAckData(
            id_device=pb.id_device,
            config_version=pb.config_version,
            applied=pb.applied,
        )

@dataclass
class Data_1:
    """Modelo neutro de telemetría (equivalente a protobuf Data_1)."""
    id_device: str          # Cambiar por device_id
    temperature: int      
    press: int            
    hum: int              
    co: float     
    rms: float    
    amp_x: float  
    freq_x: float 
    amp_y: float  
    freq_y: float 
    amp_z: float  
    freq_z: float 
    config_version_applied: int

@dataclass
class ConfigData:
    """Modelo neutro de configuración (equivalente a protobuf Config)."""
    id_device: str
    config_version: int
    protocol_conf: int
    acc_sampling: int
    gyro_sensibility: int
    bme688_sampling: int
    send_interval_ms: int
    discontinuous_sleep_time: int
    discontinuous_window_size: int
    tcp_port: int
    udp_port: int
    host_ip_addr: str     
    ssid: str             
    passwd: str
    mqtt_broker: str

@dataclass
class ConfigAckData:
    """Modelo neutro de ACK de configuración (equivalente a protobuf ConfigAck)."""
    id_device: str
    config_version: int
    applied: bool

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
