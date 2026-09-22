"""Todo lo que el servidor le pregunta al sistema operativo del host.

UTILIDAD PRINCIPAL
    Aislar en un solo módulo las tres cosas que dependen de la máquina donde
    corre el servidor, para que el resto del código no tenga que saber de
    subprocess ni de comandos de Linux:

        utc_epoch_now()      la hora actual, en epoch Unix UTC
        BLEAdapterResolver   qué adaptador BLE usar (hciconfig / BLE_ADAPTER)
        LocalWifiConfig      SSID, password e IP local del host, vía nmcli

LOS DATOS DE WIFI NO SON PARA EL SERVIDOR
    Viajan dentro de la configuración hacia el ESP32, que los necesita para
    unirse a la misma red y poder hablar por UDP, TCP o MQTT. Por eso se leen
    de la red a la que la Raspberry está conectada en ese momento, y por eso se
    consultan con un cache corto: cambian poco, pero pueden cambiar.

POR QUÉ UTC Y NO HORA LOCAL
    `utc_epoch_now()` devuelve UTC real, sin sumarle el offset local. Es lo que
    el device usa tal cual para su `settimeofday()`, y lo que la BD asume al
    guardar los timestamps. Sumarle el offset produciría un número que no
    corresponde a ningún instante real, y dejaría `time_client` y `time_server`
    en escalas distintas dentro de una misma fila de `log`.

POR QUÉ SE LLAMA system.py Y NO platform.py
    Un módulo local llamado platform.py taparía el módulo estándar del mismo
    nombre para todo el proceso, incluido bleak, que lo importa internamente.

PORTABILIDAD
    `hciconfig` (BlueZ) y `nmcli` (NetworkManager) solo existen en Linux. Fuera
    de la Raspberry -por ejemplo corriendo el servidor en un Mac- esas llamadas
    fallan en silencio: BLEAdapterResolver cae a su valor por defecto y
    LocalWifiConfig devuelve SSID y password vacíos, con la IP resuelta por el
    fallback de socket. El servidor arranca igual, y BLE funciona, pero los
    devices que usen WiFi no recibirían credenciales válidas.

    Para ese caso están las variables de entorno WIFI_SSID, WIFI_PASSWD y
    HOST_IP: si están puestas, ganan sobre lo que diga nmcli. Sirven tanto para
    desarrollar fuera de la Raspberry como para forzar una red de pruebas
    estando en ella.
"""
from __future__ import annotations
import os
import socket
import subprocess
import time


def utc_epoch_now() -> int:
    """Retorna el epoch Unix actual (segundos UTC reales).

    El epoch Unix no tiene huso horario: `time.time()` ya es UTC por
    definición, así que no hay ningún offset que sumar acá. Este valor viaja
    como `ConfigData.time_client` hasta el device -que lo usa tal cual en su
    `settimeofday()`- y como `Log.time_server` a la BD, donde se guarda con
    `datetime.utcfromtimestamp()`. Todo el sistema (firmware, BD y logs) queda
    en la misma escala.
    """
    return int(time.time())

class BLEAdapterResolver:
    """Utilidades para resolver qué adaptador BLE usar.

    Solo tiene efecto sobre BlueZ (Linux). El backend CoreBluetooth de macOS
    ignora el kwarg `adapter=`, así que ahí lo que se resuelva da igual.
    """

    # Adaptador de la placa. Es el último recurso, cuando no hay ninguno USB.
    DEFAULT_ADAPTER = "hci1"

    # Valores de BLE_ADAPTER que significan "decidilo vos", no un nombre.
    _AUTO_SENTINELS = ("auto", "usb")

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

        # Un nombre explícito gana sobre todo lo demás.
        if env_value and env_value.lower() not in cls._AUTO_SENTINELS:
            return env_value

        detected = cls.detect_usb_adapter()
        if detected:
            return detected

        # Sin adaptador USB detectado se cae al de la placa. El valor centinela
        # NO se devuelve tal cual: antes, con BLE_ADAPTER=auto -que es lo que
        # pone docker-compose.yml- esta función devolvía la cadena "auto", y
        # bleak recibía "auto" como si fuera un nombre de interfaz.
        return cls.DEFAULT_ADAPTER

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
    def _env_overrides(cls) -> tuple[str, str, str]:
        """Lee WIFI_SSID / WIFI_PASSWD / HOST_IP del entorno.

        Cada una gana sobre lo que devuelva nmcli, y se pueden poner por
        separado. Sin nmcli -corriendo el servidor en un Mac, por ejemplo- son
        la única forma de que los devices en WiFi reciban credenciales usables.
        """
        return (
            os.getenv("HOST_IP", "").strip(),
            os.getenv("WIFI_SSID", "").strip(),
            os.getenv("WIFI_PASSWD", "").strip(),
        )

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

        env_ip, env_ssid, env_passwd = cls._env_overrides()

        # Si el entorno define las tres, no hace falta molestar a nmcli: son
        # cuatro subprocesos que en un Mac fallan igual.
        if env_ip and env_ssid and env_passwd:
            ssid, passwd, host_ip_addr = env_ssid, env_passwd, env_ip
        else:
            # device = cls.active_wifi_device()
            device = "wlan0"
            # device = "wlan1"
            conn_name = cls._active_connection_name(device)
            ssid = env_ssid or cls._active_wifi_ssid(device, conn_name)
            passwd = env_passwd or cls._active_wifi_psk(conn_name)
            host_ip_addr = env_ip or cls.active_wifi_ip(device) or cls._local_ip_fallback()

        cls._CACHE.update({
            "ts": now,
            "ssid": ssid or "",
            "passwd": passwd or "",
            "host_ip_addr": host_ip_addr or "",
        })
        return host_ip_addr or "", ssid or "", passwd or ""
