from __future__ import annotations
from datetime import datetime
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


def local_epoch_now() -> int:
    """Retorna epoch local ajustado al huso horario del servidor."""
    utc_epoch = int(time.time())
    local_offset = datetime.now().astimezone().utcoffset()
    local_offset_sec = int(local_offset.total_seconds()) if local_offset else 0
    return utc_epoch + local_offset_sec

class BLEAdapterResolver:
    """Utilidades para resolver qué adaptador BLE usar."""

    @staticmethod
    def detect_usb_adapter() -> str:
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

    @classmethod
    def resolve(cls) -> str:
        """Resuelve el adaptador BLE a usar.

        Prioridad:
        1) `BLE_ADAPTER` si es un valor explícito (no `auto`/`usb`).
        2) Autodetección de adaptador USB.
        3) Fallback a `hci1`.
        """
        env_value = os.getenv("BLE_ADAPTER", "").strip()
        if env_value and env_value.lower() not in ("auto", "usb"):
            return env_value
        detected = cls.detect_usb_adapter()
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
        """Ejecuta un comando y retorna stdout o cadena vacia en error.

        Conserva espacios significativos y solo elimina saltos de linea finales.
        """
        try:
            result = subprocess.run(args, capture_output=True, text=True, check=True)
            return result.stdout.rstrip('\n')
        except Exception:
            return ""

    @staticmethod
    def _run_cmd_with_status(args: list[str]) -> tuple[bool, str, str]:
        """Ejecuta comando y retorna (ok, stdout, stderr)."""
        try:
            result = subprocess.run(args, capture_output=True, text=True, check=True)
            return True, result.stdout.rstrip("\n"), result.stderr.rstrip("\n")
        except subprocess.CalledProcessError as e:
            return False, (e.stdout or "").rstrip("\n"), (e.stderr or "").rstrip("\n")
        except Exception as e:
            return False, "", str(e)

    @classmethod
    def _wifi_ssids_available(cls) -> set[str]:
        """Retorna SSIDs detectados por scan de nmcli."""
        output = cls._run_cmd(["sudo", "nmcli", "-t", "-f", "SSID", "dev", "wifi", "list", "--rescan", "auto"])
        return {line.strip() for line in output.splitlines() if line.strip()}

    @classmethod
    def connect_specific_network(cls, ssid: str, passwd: str = "", device: str | None = None) -> str:
        """Conecta a un SSID específico.

        Retornos:
        - "connected": conectado correctamente.
        - "not_found": el SSID no aparece en el scan.
        - "error": fallo al intentar conectar.
        """
        target_ssid = (ssid or "").strip()
        if not target_ssid:
            return "not_found"

        available = cls._wifi_ssids_available()
        if target_ssid not in available:
            return "not_found"

        cmd = ["sudo", "nmcli", "dev", "wifi", "connect", target_ssid]
        if passwd:
            cmd.extend(["password", passwd])
        if device:
            cmd.extend(["ifname", device])

        ok, _, err = cls._run_cmd_with_status(cmd)
        if not ok:
            print(f"[WiFi] Error conectando a '{target_ssid}': {err}")
            return "error"

        cls._CACHE["ts"] = 0.0
        return "connected"

    @classmethod
    def active_wifi_device(cls) -> str:
        """Detecta el dispositivo WiFi activo conectado (ej: wlan0)."""
        output = cls._run_cmd(["sudo", "nmcli", "-t", "-f", "DEVICE,TYPE,STATE", "dev", "status"])
        for line in output.splitlines():
            parts = line.split(":", 2)
            if len(parts) != 3:
                continue
            device, dev_type, state = parts
            if dev_type == "wifi" and state == "connected":
                print(f"Adaptador WIFI a usar: {device}")
                return device
        return ""

    @classmethod
    def activate_wpa2_ap(cls, ssid: str, passwd: str, device: str) -> str:
        """Activa un Access Point WPA2 (WPA-PSK + RSN) con SSID y password.

        Retornos:
        - "ap_active": AP levantado correctamente.
        - "invalid_args": password inválido (<8 chars) o ssid vacío.
        - "error": fallo al crear/activar el AP.
        """
        target_ssid = (ssid or "").strip()
        if len(passwd) < 8 or target_ssid == "":
            return "invalid_args"

        conn_name = f"ap-{target_ssid}"

        # Limpia perfil previo si existe para evitar conflictos de propiedades.
        cls._run_cmd(["sudo", "nmcli", "connection", "delete", conn_name])

        ok, _, err = cls._run_cmd_with_status([
            "sudo", "nmcli", "connection", "add",
            "type", "wifi",
            "ifname", device,
            "con-name", conn_name,
            "autoconnect", "no",
            "ssid", target_ssid,
        ])
        if not ok:
            print(f"[WiFi] Error creando perfil AP '{conn_name}': {err}")
            return "error"

        ok, _, err = cls._run_cmd_with_status([
            "sudo", "nmcli", "connection", "modify", conn_name,
            "802-11-wireless.mode", "ap",
            "802-11-wireless-security.key-mgmt", "wpa-psk",
            "802-11-wireless-security.proto", "rsn",
            "802-11-wireless-security.psk", passwd,
            "ipv4.method", "shared",
            "ipv6.method", "ignore",
        ])
        if not ok:
            print(f"[WiFi] Error configurando WPA2 AP '{conn_name}': {err}")
            return "error"

        ok, _, err = cls._run_cmd_with_status(["sudo", "nmcli", "connection", "up", conn_name])
        if not ok:
            print(f"[WiFi] Error activando AP '{conn_name}': {err}")
            return "error"

        cls._CACHE["ts"] = 0.0
        return "ap_active"

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
        for _ in range(3):
            psk = cls._run_cmd(["sudo", "nmcli", "-s", "-g", "802-11-wireless-security.psk", "connection", "show", conn_name])
            if psk and psk != "--":
                return psk
            time.sleep(0.05)

        return ""

    @classmethod
    def active_wifi_ip(cls, device: str) -> str:
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
            print("No se pudo obtener IP.")
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

        # device = cls.active_wifi_device()
        device = "wlan0"
        # device = "wlan1"
        conn_name = cls._active_connection_name(device)
        ssid = cls._active_wifi_ssid(device, conn_name)
        passwd = cls._active_wifi_psk(conn_name)
        host_ip_addr = cls.active_wifi_ip(device) or cls._local_ip_fallback()

        cls._CACHE.update({
            "ts": now,
            "ssid": ssid or "",
            "passwd": passwd or "",
            "host_ip_addr": host_ip_addr or "",
        })
        return host_ip_addr or "", ssid or "", passwd or ""

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
                config = DatabaseRepository(self.db_dsn).get_config(addr)
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
                    DatabaseRepository(self.db_dsn).insert_log(
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
            server_time = local_epoch_now()
            # Permite re-descubrimiento si se pierde la conexión
            print(f"Pop device {addr}")
            # Registra la desconexión
            DatabaseRepository(self.db_dsn).insert_log(
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
        self.ble_conn_retries = timeouts.ble_conn_retries
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
            server_time = local_epoch_now()
            try:
                self.database_repo.insert_log(
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

class MQTTDeviceSession(DeviceSession):
    """Sesión MQTT: recibe data, persiste y aplica cambios de config con ACK."""
    async def run(self):
        mqtt_start()

        device_queue = get_data_queue(self.device_id)
        ack_queue =  get_ack_queue(self.device_id)
        data_topic = f"/topic/nebulaedge/{self.device_id}/data"
        config_topic = f"/topic/nebulaedge/{self.device_id}/config"
        timeout_sec = self.timeouts.no_data_grace_sec

        while True:
            data = None
            try:
                # Espera por un paquete
                packet = await asyncio.to_thread(
                    device_queue.get,
                    True,
                    timeout_sec,
                )

                # Flag que indica deep sleep
                if packet == b"ds":
                    print("Se detectó deep sleep")
                    continue

                # # (este código se repite mucho)
                # Desempaqueta y obtiene protobuf tipo Data1/Data2
                data, data_type = DataCodec.deserialize_typed_packet(packet)
                if data == None or data_type == -1:
                    continue
                if data_type in (DataCodec.TYPE_DATA_1, DataCodec.TYPE_DATA_2):
                    self._update_last_client_time(data)
                if data_type == DataCodec.TYPE_DATA_1:
                    print(f"MQTT: Paquete Data_1 recibido de {self.device_id}")
                    self.database_repo.insert_data_1(data)
                elif data_type == DataCodec.TYPE_DATA_2:
                    print(f"MQTT: Paquete Data_2 recibido de {self.device_id}")
                    self.database_repo.insert_data_2(data)
                else:
                    continue

            except queue.Empty:
                print(f"Timeout MQTT ({timeout_sec}s) sin datos de {self.device_id}. " "Cerrando sesión para permitir reconexión.")
                return None
            except Exception as e:
                print(f"Error procesando datos: {e}")

            # Pausa para que otras tareas de asyncio se ejecuten (otras sesiones)
            await asyncio.sleep(self.timeouts.mqtt_poll_sec)

            # Obtiene configuración desde DB
            db_config = self.database_repo.get_config(self.device_id)
            if db_config is None:
                continue

            # Compara versiones actuales de config DEVICE vs versión BD
            applied_version = data.config_version_applied
            if applied_version > db_config.config_version:
                print(f"Config aplicada detectada en MQTT ({applied_version}) para {self.device_id}. " "Cerrando sesión para reconfigurar.")
                mqtt_shutdown()
                return db_config

            elif applied_version < db_config.config_version:
                if db_config.config_version <= self.config.config_version:
                    # Ya se envió esta config en el cambio de protocolo; espera que el device la aplique
                    continue
                print(f"Cambio de protocolo: {self.config.protocol_conf} -> {db_config.protocol_conf} para {self.device_id}")

                # Serializa y envía nueva configuración (espera ACK antes de cambiar)
                serialized_config = DataCodec.serialize_config(db_config)

                mqtt_publish(config_topic, serialized_config)
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
                            mqtt_shutdown()
                            return db_config
                    except queue.Empty:
                        pass

                print(f"ACK MQTT de config v{db_config.config_version} no recibido para {self.device_id}. Cerrando sesión.")
                mqtt_shutdown()
                return None
            
class UDPDeviceSession(DeviceSession):
    """Sesión UDP: recibe data, persiste y aplica cambios de config con ACK."""
    async def _send_config(self, sock: socket.socket, udp_addr, db_config: "ConfigData") -> bool:
        """Serializa y envía configuración vía UDP. Retorna booleano en caso de éxito o error."""
        loop = asyncio.get_running_loop()
        print(f"Cambio de protocolo: {self.config.protocol_conf} -> {db_config.protocol_conf} para {self.device_id}")
        serialized_config = DataCodec.serialize_config(db_config)

        try:
            await loop.sock_sendto(sock, serialized_config, udp_addr)
            return True
        except Exception as e:
            print(f"Error enviando config UDP: {e}")
            return False

    async def _wait_ack(self, sock: socket.socket, db_config: "ConfigData") -> "ConfigData | None":
        """Espera ACK válido por UDP. Retorna la configuración aplicada, o None si es que hubo error."""
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
                # Desempaqueta y obtiene protobuf tipo Data1/Data2
                data, data_type = DataCodec.deserialize_typed_packet(pkt)
                if data == None or data_type == -1:
                    continue
                if data_type in (DataCodec.TYPE_DATA_1, DataCodec.TYPE_DATA_2):
                    self._update_last_client_time(data)
                if data_type == DataCodec.TYPE_DATA_1:
                    print(f"UDP: Paquete Data_1 recibido de {self.device_id}.")
                    self.database_repo.insert_data_1(data)
                if data_type == DataCodec.TYPE_DATA_2:
                    print(f"UDP: Paquete Data_2 recibido de {self.device_id}.")
                    self.database_repo.insert_data_2(data)
                else:
                    continue

        print(f"ACK UDP de config v{db_config.config_version} no recibido para {self.device_id}. Cerrando sesión.")
        return None

    async def run(self):
        ''' Flujo programa: 
            1. abre socket UDP
            2. espera por un paquete de datos con un determinado timeout. si se llega al timeout, cierra sesión
            3. inserta en la base de datos
            4. obtiene configuración de la base de datos
            5. compara la versión de esta última configuración con la versión de config integrada en el paquete
                5.1. si la versión del paquete es mayor, cierra sesión UDP. si es menor, envía configuración
                     dicha configuración nueva al dispositivo y cierra sesión UDP
            6. vuelve al punto 2
        '''
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
                    packet, udp_addr = await asyncio.wait_for(loop.sock_recvfrom(s, 1024), timeout=timeout_sec)
                except asyncio.TimeoutError:
                    print(f"Timeout UDP ({timeout_sec}s) sin datos de {self.device_id}. " "Cerrando sesión para permitir reconexión.")
                    return None

                ## Común

                # Flag que indica deep sleep
                if packet == b"ds":
                    print("Se detectó deep sleep, reinicia socket")
                    break

                # Desempaqueta y obtiene protobuf tipo Data1/Data2
                data, data_type = DataCodec.deserialize_typed_packet(packet)
                if data == None or data_type == -1:
                    continue
                if data_type in (DataCodec.TYPE_DATA_1, DataCodec.TYPE_DATA_2):
                    self._update_last_client_time(data)
                if data_type == DataCodec.TYPE_DATA_1:
                    print(f"UDP: Paquete Data_1 recibido de {self.device_id} en puerto {port}")
                    self.database_repo.insert_data_1(data)
                if data_type == DataCodec.TYPE_DATA_2:
                    print(f"UDP: Paquete Data_2 recibido de {self.device_id} en puerto {port}")
                    self.database_repo.insert_data_2(data)
                else:
                    continue

                # Obtiene configuración desde DB
                db_config = self.database_repo.get_config(self.device_id)
                if db_config is None:
                    continue

                ## Común

                # Compara versiones actuales de config DEVICE vs versión BD
                applied_version = data.config_version_applied
                if applied_version > db_config.config_version:
                    print(f"Config aplicada detectada en UDP ({applied_version}) para {self.device_id}. " "Cerrando sesión para reconfigurar.")
                    return db_config
                elif applied_version < db_config.config_version:
                    if db_config.config_version <= self.config.config_version:
                        # Ya se envió esta config en el cambio de protocolo; espera que el device la aplique
                        continue
                    if not await self._send_config(s, udp_addr, db_config):
                        return None
                    new_cfg = await self._wait_ack(s, db_config)
                    return new_cfg

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

                # Si llega telemetría durante la espera, se inserta y se sigue esperando (este código se repite mucho)
                data, data_type = DataCodec.deserialize_typed_packet(pkt)
                if data == None or data_type == -1:
                    continue
                if data_type in (DataCodec.TYPE_DATA_1, DataCodec.TYPE_DATA_2):
                    self._update_last_client_time(data)
                if data_type == DataCodec.TYPE_DATA_1:
                    print(f"TCP: Paquete Data_1 recibido de {self.device_id}")
                    self.database_repo.insert_data_1(data)
                if data_type == DataCodec.TYPE_DATA_2:
                    print(f"TCP: Paquete Data_2 recibido de {self.device_id}")
                    self.database_repo.insert_data_2(data)
                else:
                    continue

        print(f"ACK TCP de config v{db_config.config_version} no recibido para {self.device_id}. Cerrando sesión.")
        return None

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

                        # Recibe dato
                        timeout_sec = self._sleep_timeout_sec()
                        try:
                            packet = await asyncio.wait_for(loop.sock_recv(conn, 1024), timeout=timeout_sec)
                        except asyncio.TimeoutError:
                            print(f"Timeout TCP ({timeout_sec}s) sin datos de {self.device_id}. " "Cerrando sesión para permitir reconexión.")
                            return None
                        except Exception as e:
                            print(f"Recepción TCP falló: {e}")
                        if not packet:
                            print("Conexión TCP cerrada por el otro extremo.")
                            break

                        ## Común

                        # Flag que indica deep sleep
                        if packet == b"ds":
                            print("Se detectó deep sleep, reinicia socket")
                            break

                        # (este código se repite mucho)
                        # Desempaqueta y obtiene protobuf tipo Data1/Data2
                        data, data_type = DataCodec.deserialize_typed_packet(packet)
                        if data == None or data_type == -1:
                            continue
                        if data_type in (DataCodec.TYPE_DATA_1, DataCodec.TYPE_DATA_2):
                            self._update_last_client_time(data)
                        if data_type == DataCodec.TYPE_DATA_1:
                            print(f"TCP: Paquete Data_1 recibido de {self.device_id} en puerto {port}")
                            self.database_repo.insert_data_1(data)
                        if data_type == DataCodec.TYPE_DATA_2:
                            print(f"TCP: Paquete Data_2 recibido de {self.device_id} en puerto {port}")
                            self.database_repo.insert_data_2(data)
                        else:
                            continue

                        # Obtiene configuración desde DB
                        db_config = self.database_repo.get_config(self.device_id)
                        if db_config is None:
                            continue

                        ## Común

                        # Compara versiones actuales de config DEVICE vs versión BD
                        applied_version = data.config_version_applied
                        if applied_version > db_config.config_version:
                            print(f"Config aplicada detectada en TCP ({applied_version}) para {self.device_id}. " "Cerrando sesión para reconfigurar.")
                            return db_config
                        elif applied_version < db_config.config_version:
                            if db_config.config_version <= self.config.config_version:
                                # Ya se envió esta config en el cambio de protocolo; espera que el device la aplique
                                continue
                            if not await self._send_config(conn, db_config):
                                return None
                            new_cfg = await self._wait_ack(conn, db_config)
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
                    while True:
                        try:            
                            await asyncio.sleep(0.1)
                            
                            timeout_sec = self._sleep_timeout_sec()
                            packet = await asyncio.wait_for(data_queue.get(), timeout=timeout_sec)

                            # Común

                            # (este código se repite mucho)
                            # Desempaqueta y obtiene protobuf tipo Data1/Data2
                            data, data_type = DataCodec.deserialize_typed_packet(packet)
                            if data == None or data_type == -1:
                                continue
                            if data_type in (DataCodec.TYPE_DATA_1, DataCodec.TYPE_DATA_2):
                                self._update_last_client_time(data)
                            if data_type == DataCodec.TYPE_DATA_1:
                                print(f"BLE: Paquete Data_1 recibido de {self.device_id}")
                                self.database_repo.insert_data_1(data)
                            if data_type == DataCodec.TYPE_DATA_2:
                                print(f"BLE: Paquete Data_2 recibido de {self.device_id}")
                                self.database_repo.insert_data_2(data)
                            if data_type == DataCodec.TYPE_DEEP_SLEEP:
                                print(f"BLE: Dispositivo {self.device_id} entrando en deep sleep. Cerrando sesión para permitir reconexión.")
                                await client.stop_notify(UUID_CHAR_B)
                                await client.stop_notify(UUID_CHAR_D)
                                # Cierra la task
                                # return None
                                # continue_outer = True

                                # Vuelve a intentar conexión BLE persistente (cuando despierte)
                                await asyncio.sleep(timeout_sec)
                                break

                            else:
                                continue
  
                            # Obtiene configuración de BD
                            db_config = self.database_repo.get_config(self.device_id)
                            if db_config is None:
                                continue

                            print("esto se ejecuta")

                            # Compara versiones actuales de config DEVICE vs versión BD
                            # Caso mayor: una configuración nueva ya se aplicó por lo que el server tiene que adecuarse
                            applied_version = data.config_version_applied
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
    """Repositorio general para interactuar con la BD."""
    def __init__(self, db_dsn: str):
        """Guarda DSN para crear conexiones a BD por operación."""
        self.db_dsn = db_dsn

    @staticmethod
    def _int_to_db_datetime(value: int) -> datetime:
        """Convierte un Unix timestamp en segundos a datetime naive UTC."""
        return datetime.utcfromtimestamp(int(value))

    def get_config(self, device_id: str) -> ConfigData | None:
        """Obtiene la configuración de un dispositivo específico."""
        with psycopg2.connect(self.db_dsn) as db:
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
                    time_cli = local_epoch_now()
                    # print(
                    #     f"[TIME] send_local_epoch={time_cli} "
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
            with psycopg2.connect(self.db_dsn) as db:
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
            with psycopg2.connect(self.db_dsn) as db:
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
            with psycopg2.connect(self.db_dsn) as db:
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

class DataCodec:
    """Esta clase permite que DatabaseRepository se desligue de protobuf.
    En caso de querer cambiar la forma de enviar los datos (JSON por ejemplo)
    solo se tendrá que modificar esto."""
    TYPE_DATA_1 = 0x01
    TYPE_DATA_2 = 0x02
    # TYPE_ACK = 0x03 sería bueno implementarlo
    TYPE_DEEP_SLEEP = 0x04

    @staticmethod
    def split_typed_packet(packet: bytes) -> tuple[int | None, bytes]:
        """Extrae el tipo (1 byte) y el payload del paquete."""
        if not packet or len(packet) < 2:
            return None, b""
        return packet[0], packet[1:]

    @staticmethod
    def deserialize_typed_packet(packet: bytes) -> tuple["Data_1 | Data_2 | None", int]:
        """Parsea un paquete con prefijo de tipo y devuelve una tupla cuya primera posición es Data_1 o Data_2
           y en la segunda posición el indicador de tipo de paquete. En caso de no ser ninguno retorna [None, -1]"""
        pkt_type, payload = DataCodec.split_typed_packet(packet)
        if pkt_type == DataCodec.TYPE_DATA_1:
            return DataCodec.deserialize_data_1(payload), DataCodec.TYPE_DATA_1
        if pkt_type == DataCodec.TYPE_DATA_2:
            return DataCodec.deserialize_data_2(payload), DataCodec.TYPE_DATA_2
        if pkt_type == DataCodec.TYPE_DEEP_SLEEP:
            return payload, DataCodec.TYPE_DEEP_SLEEP
        return None, -1

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
        pb.mag_x = data.mag_x
        pb.mag_y = data.mag_y
        pb.mag_z = data.mag_z
        pb.config_version_applied = data.config_version_applied
        pb.time_client = data.time_client
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
            mag_x=pb.mag_x,
            mag_y=pb.mag_y,
            mag_z=pb.mag_z,
            config_version_applied=pb.config_version_applied,
            time_client=pb.time_client
        )

    @staticmethod
    def serialize_data_2(data: "Data_2") -> bytes:
        """Convierte Data_2 -> protobuf Data_2 -> bytes."""
        pb = schema_pb2.Data_2()
        pb.id_device = data.id_device
        pb.acc_x = data.acc_x
        pb.acc_y = data.acc_y
        pb.acc_z = data.acc_z
        pb.gyr_x = data.gyr_x
        pb.gyr_y = data.gyr_y
        pb.gyr_z = data.gyr_z
        pb.config_version_applied = data.config_version_applied
        pb.time_client = data.time_client
        return pb.SerializeToString()

    @staticmethod
    def deserialize_data_2(packet: bytes) -> "Data_2 | None":
        """Convierte bytes -> protobuf Data_2 -> Data_2."""
        try:
            pb = schema_pb2.Data_2()
            pb.ParseFromString(packet)
        except Exception as e:
            print(f"Error al desempaquetar el paquete Data_2: {e}")
            return None

        return Data_2(
            id_device=pb.id_device,
            acc_x=pb.acc_x,
            acc_y=pb.acc_y,
            acc_z=pb.acc_z,
            gyr_x=pb.gyr_x,
            gyr_y=pb.gyr_y,
            gyr_z=pb.gyr_z,
            config_version_applied=pb.config_version_applied,
            time_client=pb.time_client,
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
        pb.send_interval_s = config.send_interval_s
        pb.sleep_time_s = config.sleep_time_s
        pb.sleep_window_size = config.sleep_window_size
        pb.tcp_port = config.tcp_port
        pb.udp_port = config.udp_port
        pb.host_ip_addr = config.host_ip_addr
        pb.ssid = config.ssid
        pb.passwd = config.passwd
        pb.mqtt_broker = config.mqtt_broker
        pb.time_client = config.time_client
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
            send_interval_s=pb.send_interval_s,
            sleep_time_s=pb.sleep_time_s,
            sleep_window_size=pb.sleep_window_size,
            tcp_port=pb.tcp_port,
            udp_port=pb.udp_port,
            host_ip_addr=pb.host_ip_addr,
            ssid=pb.ssid,
            passwd=pb.passwd,
            mqtt_broker=pb.mqtt_broker,
            time_client=pb.time_client
        )

    @staticmethod
    def serialize_config_ack(ack: "ConfigAckData") -> bytes:
        """Convierte ConfigAckData -> protobuf ConfigAck -> bytes."""
        pb = schema_pb2.ConfigAck()
        pb.id_device = ack.id_device
        pb.config_version = ack.config_version
        pb.applied = ack.applied
        pb.time_client = ack.time_client
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
            time_client=pb.time_client,
        )

@dataclass
class Timeouts:
    ble_connect_sec: float = 30.0          # Timeout de intentos de conexión BLE (BleakClient)
    scan_restart_sec: float = 60.0         # Reinicio periódico de scanner BLE
    connect_cooldown_sec: float = 20.0     # Cooldown entre reconexiones por device
    config_ack_sec: float = 2.0            # Ventana para ACK de config (MQTT/UDP/TCP/BLE)
    config_ack_retries: int = 10           # Reintentos de ACK de config (MQTT/UDP/TCP/BLE)
    no_data_grace_sec: float = 50.0       # Gracia extra al esperar un dato
    mqtt_poll_sec: float = 0.1             # Sleep de polling MQTT
    mqtt_queue_timeout_sec: float = 1.0    # Timeout de device_queue.get()
    ble_ack_short_sec: float = 3.0         # Timeout corto por intento en _wait_ble_ack
    ble_conn_retries: int = 10             # Reintentos de conexión BLE persistente

@dataclass
class Data_1:
    """Modelo neutro de telemetría (equivalente a protobuf Data_1)."""
    id_device: str          # Cambiar por device_id
    temperature: float      
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
    mag_x: float
    mag_y: float
    mag_z: float
    config_version_applied: int
    time_client: int

@dataclass
class Data_2:
    """Modelo neutro de telemetría (equivalente a protobuf Data_2)."""
    id_device: str
    acc_x: float
    acc_y: float
    acc_z: float
    gyr_x: float
    gyr_y: float
    gyr_z: float
    config_version_applied: int
    time_client: int

@dataclass
class ConfigData:
    """Modelo neutro de configuración (equivalente a protobuf Config)."""
    id_device: str
    config_version: int
    protocol_conf: int
    acc_sampling: int
    gyro_sensibility: int
    bme688_sampling: int
    send_interval_s: int
    sleep_time_s: int
    sleep_window_size: int
    tcp_port: int
    udp_port: int
    host_ip_addr: str
    ssid: str             
    passwd: str
    mqtt_broker: str
    time_client: int

@dataclass
class ConfigAckData:
    """Modelo neutro de ACK de configuración (equivalente a protobuf ConfigAck)."""
    id_device: str
    config_version: int
    applied: bool
    time_client: int

@dataclass
class Log:
    """Modelo neutro de log de eventos."""
    id_device: str
    status_report: int
    protocol_report: int
    batt_level: int
    time_client: int
    time_server: int

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
