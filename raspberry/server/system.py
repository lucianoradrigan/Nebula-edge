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
        LocalWifiConfig      SSID y clave (del .env) e IP del servidor que van al device

LOS DATOS DE WIFI NO SON PARA EL SERVIDOR
    Viajan dentro de la configuración hacia el ESP32, que los necesita para
    unirse a la misma red y poder hablar por UDP, TCP o MQTT. El SSID y la clave
    son configuración (WIFI_SSID / WIFI_PASSWD); solo la IP se detecta.

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
    `hciconfig` (BlueZ) solo existe en Linux: fuera de la Raspberry -por
    ejemplo en un Mac- BLEAdapterResolver cae a su valor por defecto. La IP de
    una interfaz también se pide al kernel de Linux; en otro sistema se usa la
    de la ruta por defecto, o HOST_IP si está puesta.
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


def utc_epoch_now_us() -> int:
    """El epoch Unix actual en microsegundos, como entero.

    Sale de `time.time_ns()` y no de `time.time() * 1e6`: un float de 64 bits
    con el epoch de hoy redondea en el último microsegundo. Es la unidad de las
    marcas de tiempo que se usan para medir (Config.time_server_us, la hora de
    llegada de cada paquete), que no pasan por segundos en ningún punto.
    """
    return time.time_ns() // 1000

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
# El broker lo hospeda el servidor, en su propio contenedor (servicio `mqtt` de
# docker-compose.yml; el porqué está en mosquitto/mosquitto.conf). Estos dos
# valores los comparten el cliente del servidor, que se suscribe, y la URL que
# se le manda al device en la configuración.
#
# MQTT_HOST es dónde encuentra el broker el CLIENTE DEL SERVIDOR: con el
# contenedor del servidor en network_mode host y el puerto publicado por el del
# broker, es loopback. Lo que recibe el DEVICE es otra cosa: la IP del host, que
# se arma en repository.get_config().
MQTT_BROKER_PORT = int(os.environ.get("MQTT_PORT", "1883"))
MQTT_HOST = os.environ.get("MQTT_HOST", "127.0.0.1")

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

    # Último recurso, si no se encuentra ninguno. Antes era "hci1", que en la
    # Raspberry original era el de la placa; en una Pi 5 sin dongle el de la
    # placa es hci0 y no hay hci1, así que el servidor caía en un loop de
    # reinicios ("adapter 'hci1' not found").
    DEFAULT_ADAPTER = "hci0"

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

    @staticmethod
    def available_adapters() -> list[str]:
        """Los adaptadores que el kernel tiene registrados (hci0, hci1...).

        Las entradas con ':' (p.ej. hci0:64) son conexiones, no adaptadores.
        """
        try:
            names = os.listdir("/sys/class/bluetooth")
        except OSError:
            return []
        return sorted(n for n in names if n.startswith("hci") and ":" not in n)

    @classmethod
    def resolve(cls) -> str:
        """Resuelve el adaptador BLE a usar.

        Prioridad:
        1) `BLE_ADAPTER` si es un valor explícito (no `auto`/`usb`).
        2) Autodetección de adaptador USB.
        3) El primero que exista (el de la placa, si no hay dongle).
        4) Fallback a `hci0`.
        """
        env_value = os.getenv("BLE_ADAPTER", "").strip()

        # Un nombre explícito gana sobre todo lo demás.
        if env_value and env_value.lower() not in cls._AUTO_SENTINELS:
            return env_value

        detected = cls.detect_usb_adapter()
        if detected:
            return detected

        # Sin dongle se usa el de la placa, que funciona pero es menos estable:
        # en la Pi el banco usa un dongle USB justamente por eso. Se avisa para
        # que una desconexión BLE no se confunda con un bug.
        available = cls.available_adapters()
        if available:
            log(f"[BLE] No hay adaptador USB: se usa {available[0]}, el de la placa (menos estable)")
            return available[0]

        # Sin ninguno a la vista se cae al nombre por defecto. El valor centinela
        # NO se devuelve tal cual: antes, con BLE_ADAPTER=auto -que es lo que
        # pone docker-compose.yml- esta función devolvía la cadena "auto", y
        # bleak recibía "auto" como si fuera un nombre de interfaz.
        return cls.DEFAULT_ADAPTER

class LocalWifiConfig:
    """La red WiFi que se les entrega a los devices: SSID, clave e IP del servidor.

    DE DÓNDE SALE CADA DATO
        - SSID y clave: de WIFI_SSID / WIFI_PASSWD (raspberry/.env). Son
          configuración, no estado: en modo AP son las del AP que publica la Pi
          (las mismas con que se creó su perfil de NetworkManager); con la Pi
          como cliente de un router, las del router.
        - IP del servidor: HOST_IP si está puesta; si no, la IPv4 de
          WIFI_DEVICE leída del kernel; si tampoco, la de la ruta por defecto.

    POR QUÉ YA NO SE LEE CON nmcli
        Antes se preguntaba a nmcli, en cada config, por la conexión activa de
        WIFI_DEVICE. Mezclaba dos preguntas -a qué red está la Pi y a qué red
        deben ir los devices- y dependía de sudo, D-Bus y el estado de
        NetworkManager dentro del contenedor. En la prueba del 05-11, tras pasar
        la Pi del AP al router, devolvió vacío durante 3 h y se repartieron ~1000
        configs con la red vacía a 7 placas, que quedaron colgadas.
    """
    _warned_missing = False

    @staticmethod
    def interface_ip(device: str) -> str:
        """IPv4 de una interfaz, pedida al kernel (SIOCGIFADDR). "" si no tiene o no es Linux.

        Funciona dentro del contenedor porque el servicio corre con
        network_mode: host. Sin subprocesos ni permisos especiales."""
        if not device:
            return ""
        try:
            import fcntl
            import struct
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
                packed = fcntl.ioctl(s.fileno(), 0x8915, struct.pack("256s", device[:15].encode()))
            return socket.inet_ntoa(packed[20:24])
        except Exception:
            return ""

    @staticmethod
    def _local_ip_fallback() -> str:
        """IP de la ruta por defecto, abriendo un socket UDP hacia afuera (no envía nada)."""
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
                s.connect(("8.8.8.8", 80))
                return s.getsockname()[0]
        except Exception:
            log("No se pudo obtener IP.")
            return ""

    @classmethod
    def get(cls) -> tuple[str, str, str]:
        """Retorna (host_ip_addr, ssid, passwd)."""
        ssid = os.getenv("WIFI_SSID", "").strip()
        passwd = os.getenv("WIFI_PASSWD", "").strip()
        device = os.getenv("WIFI_DEVICE", "").strip() or "wlan0"
        host_ip = (os.getenv("HOST_IP", "").strip() or cls.interface_ip(device)
                   or cls._local_ip_fallback())
        if not ssid and not cls._warned_missing:
            log("[WiFi] WIFI_SSID está vacío en .env: los devices en WiFi no van a poder "
                "conectarse (ver README §10).")
            cls._warned_missing = True
        return host_ip, ssid, passwd

