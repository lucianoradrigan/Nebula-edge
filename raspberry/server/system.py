"""Todo lo que el servidor le pregunta al sistema operativo del host.

UTILIDAD PRINCIPAL
    Aislar en un solo módulo lo que depende de la máquina donde corre el
    servidor -el sistema operativo y el entorno- para que el resto del código
    no tenga que saber de subprocess, de comandos de Linux ni de variables de
    entorno:

        utc_epoch_now()      la hora actual, en epoch Unix UTC
        log()                imprime con marca de tiempo, para cruzar con el device
        database_dsn()       DSN de Postgres (PG_HOST / PG_DB / PG_USER / PG_PASSWORD)
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


_PROCESS_START = time.monotonic()


def log(message) -> None:
    """Imprime un mensaje con hora de pared y segundos desde que arrancó el proceso.

    POR QUÉ NO print() PELADO
        El firmware loguea con el reloj del ESP32 -milisegundos desde SU
        arranque- y el servidor no tenía ninguna marca de tiempo. Cuando un
        paquete se perdía entre los dos lados no había forma de alinear las dos
        trazas y decir cuál ocurrió antes.

    LOS DOS NÚMEROS SON PARA COSAS DISTINTAS
        La hora de pared sirve para cruzar con logs externos (la base, el
        broker, journalctl). Los segundos desde el arranque sirven para medir
        intervalos sin tener que restar horas a mano, y para comparar contra el
        contador del device, que también cuenta desde su propio arranque.
    """
    now = time.time()
    stamp = time.strftime("%H:%M:%S", time.localtime(now))
    millis = int((now % 1) * 1000)
    since = time.monotonic() - _PROCESS_START
    print(f"[{stamp}.{millis:03d} +{since:8.3f}s] {message}")


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

# Valores por defecto de la base. Son los mismos que docker-compose.yml le pasa
# al contenedor de Postgres, y están acá para que el servidor arranque sin
# configuración extra: clonar el repo y levantarlo tiene que seguir funcionando
# en un comando. No son secretos -están commiteados-; para una base que no sea
# la de desarrollo, se sobrescriben por entorno.
_DB_DEFAULTS = {
    "host": "localhost",
    "dbname": "nebulaedge",
    "user": "nebulaedge",
    "password": "1234",
}

# Segundos que psycopg2 puede pasar intentando ABRIR una conexión.
#
# POR QUÉ NO ALCANZA CON EL DEFAULT
#     Sin este parámetro el connect no tiene tope propio y queda a merced del
#     timeout de TCP del sistema. Si la base deja de aceptar pero el puerto
#     sigue abierto -el caso típico es el contenedor caído con el proxy de
#     Docker todavía escuchando- el connect se cuelga sin devolver.
#
#     Y no se cuelga solo: las llamadas *_async corren en hilos de
#     asyncio.to_thread, así que cada intento ocupa un hilo del executor. Con
#     los hilos tomados, cualquier to_thread posterior queda en cola y el
#     servidor entero se queda inerte, sin loguear nada, aunque los devices
#     sigan transmitiendo. Medido en banco: 4,5 minutos sin una sola línea de
#     salida tras una caída de la base de 30 segundos.
#
#     Con el tope, el intento falla rápido, el hilo se libera y el siguiente
#     reintento vuelve a probar: cuando la base vuelve, el servidor la toma
#     sin necesidad de reiniciarlo.
# BROKER MQTT PROPIO
#
# El servidor hospeda su propio broker (mqtt_broker.py), así que estos tres
# valores los comparten el broker que escucha, el cliente del servidor que se
# suscribe y la URL que se le manda al device en la configuración. Están acá y
# no en mqtt_broker.py para que repository.py pueda armar esa URL sin importar
# amqtt.
MQTT_BROKER_PORT = int(os.environ.get("MQTT_PORT", "1883"))
MQTT_BROKER_BIND = os.environ.get("MQTT_BIND", "0.0.0.0")
# El cliente del servidor no sale a la red: el broker es este mismo proceso.
MQTT_LOCAL_HOST = os.environ.get("MQTT_LOCAL_HOST", "127.0.0.1")

_DB_CONNECT_TIMEOUT_S = 5


def database_dsn() -> str:
    """Arma el DSN de Postgres leyendo PG_HOST / PG_DB / PG_USER / PG_PASSWORD.

    POR QUÉ ESTÁ ACÁ Y NO EN discovery.py
        Antes el DSN era un parámetro por defecto de `DeviceDiscovery.__init__`,
        o sea que el módulo que escanea anuncios BLE era el dueño de la
        contraseña de la base. Y como nadie le pasaba nunca ese parámetro, ese
        "default" era en realidad la configuración de todo el sistema, metida en
        la firma de un constructor.

        Además quedaba duplicado con docker-compose.yml, que define las mismas
        tres cosas para el contenedor de Postgres. Dos fuentes de verdad para
        una sola credencial: cambiar una y no la otra deja al servidor sin poder
        conectarse, con el error apareciendo en repository.py, lejos del cambio
        que lo causó. Ahora compose pasa las mismas variables a los dos
        servicios y esta función es el único lugar que las arma.

    Cada variable se puede poner por separado; las que falten caen al valor de
    desarrollo.
    """
    env = {
        "host": os.getenv("PG_HOST", "").strip(),
        "dbname": os.getenv("PG_DB", "").strip(),
        "user": os.getenv("PG_USER", "").strip(),
        "password": os.getenv("PG_PASSWORD", "").strip(),
    }
    fields = {key: env[key] or default for key, default in _DB_DEFAULTS.items()}
    fields["connect_timeout"] = str(_DB_CONNECT_TIMEOUT_S)
    return " ".join(f"{key}={value}" for key, value in fields.items())


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
                log(f"Adaptador WIFI a usar: {device}")
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
            log("No se pudo obtener IP.")
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
