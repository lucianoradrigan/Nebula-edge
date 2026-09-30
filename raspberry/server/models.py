"""Los tipos del dominio: dataclasses que no dependen de protobuf ni de la BD.

UTILIDAD PRINCIPAL
    Definir el vocabulario común que se pasan entre sí el codec, el
    repositorio, el router y las sesiones, para que nadie tenga que manipular
    mensajes protobuf ni filas de Postgres directamente.

        Timeouts        todos los tiempos del servidor, en un solo lugar
        BleContext      lo que solo el transporte BLE necesita
        ConfigData      configuración de un device (protocolo, sensores, red)
        Data_1   telemetría ambiental del BME688 (ritmo lento)
        Data_2        acelerómetro, giroscopio y magnetómetro (ritmo rápido)
        ConfigAckData   confirmación del device de que aplicó una versión
        Log             evento de operación (conexión, heartbeat, desconexión)

POR QUÉ DOS MENSAJES DE TELEMETRÍA Y NO UNO
    Porque los dos grupos de sensores tienen ritmos naturales distintos. La
    temperatura, la presión y la humedad cambian en segundos o minutos; el
    acelerómetro, en milisegundos. Mandarlos juntos obliga a elegir un solo
    intervalo, y cualquiera que se elija sobremuestrea uno o submuestrea el
    otro. Separados, cada uno viaja a su propio intervalo (`send_interval_s`
    para los dos flujos).

    Son el punto donde convergen las tres representaciones del mismo dato: el
    mensaje protobuf que viaja por el cable, la fila de la tabla, y el objeto
    que usa la lógica de sesión. Por eso este módulo no importa nada del
    proyecto: todos lo importan a él, y así no hay ciclos.

AJUSTAR LOS TIEMPOS
    `Timeouts` se construye una vez en `DeviceDiscovery` y baja por toda la
    cadena hasta los transportes. Cambiar un valor acá afecta a los cuatro
    protocolos; para tocar solo uno, el lugar es su Transport (transport.py).
"""
from __future__ import annotations
from dataclasses import dataclass
from typing import Any


@dataclass
class BleContext:
    """Lo que el transporte BLE necesita y los otros tres no tienen.

    Antes estos seis campos viajaban sueltos por toda la cadena -descubrimiento,
    despacho y DeviceSession- uno por uno. MQTT, UDP y TCP los recibían y los
    ignoraban: la clase base tenía la forma de uno solo de sus cuatro casos, y
    agregarle algo a BLE ensanchaba la firma de los cuatro.

    Los tipos van como Any a propósito: este módulo no importa nada del proyecto
    ni de bleak, para que no haya ciclos (ver la cabecera del archivo).
    """
    device: Any                 # BLEDevice de bleak
    adapter: str | None = None  # interfaz BlueZ; el backend de macOS lo ignora
    scanner_lock: Any = None    # asyncio.Lock para coordinar el escaneo
    scanner_stop: Any = None    # callable que detiene el scanner
    scanner_start: Any = None   # callable que lo reanuda
    client: Any = None          # conexión que dejó abierta el descubrimiento


@dataclass(frozen=True)
class Timeouts:
    """Todos los tiempos del servidor, centralizados.

    Se arma una vez en DeviceDiscovery y baja por descubrimiento -> despacho
    -> sesión -> transporte. Los tests construyen uno con valores chicos para
    que la suite corra en segundos en vez de minutos.

    FROZEN A PROPÓSITO
        Todas las sesiones vivas comparten LA MISMA instancia, que es lo que
        hace cierto el "centralizados" de arriba. Sin congelar, un
        `session.timeouts.config_ack_sec = 0` en cualquier parte se lo cambiaría
        a todos los devices a la vez. Nadie escribe hoy en estos campos, así que
        congelarlo no cambia comportamiento: solo deja que el lenguaje garantice
        lo que antes era una convención. Para una variante se construye otro
        (`Timeouts(**valores)` o `dataclasses.replace(t, campo=...)`).
    """
    # --- descubrimiento BLE (discovery.py) ---
    ble_connect_sec: float = 30.0       # Espera máxima al conectar con BleakClient
    scan_restart_sec: float = 60.0      # Cada cuánto se reinicia el scanner, para que no se cuelgue
    connect_cooldown_sec: float = 20.0  # Mínimo entre dos intentos de conexión al mismo device

    # --- espera de datos (sessions.py) ---
    no_data_grace_sec: float = 50.0     # Gracia sobre el intervalo de envío antes de dar por muerta la sesión
    config_poll_sec: float = 15.0       # Cada cuánto se consulta la BD aunque no llegue telemetría

    # --- resincronización de la hora (sessions.py) ---
    clock_resync_sec: float = 300.0     # Cada cuánto se reenvía la config vigente solo para poner el reloj
                                        # del device en hora. Ver _resync_device_clock().

    # --- handshake de configuración (sessions.py) ---
    config_ack_sec: float = 2.0         # Ventana de espera del ACK, por intento
    config_ack_retries: int = 10        # Intentos antes de dar la config por no aplicada
    ble_ack_short_sec: float = 3.0      # Ventana de ACK propia de BLE; reemplaza a config_ack_sec
                                        # cuando el transporte la declara (Transport.ack_window_sec)

@dataclass
class Data_1:
    """Telemetría ambiental del BME688 (equivalente a protobuf Data_1).

    Viaja cada `send_interval_s` segundos, al mismo ritmo que Data_2.
    """
    id_device: str          # Cambiar por device_id
    temperature: float
    press: int
    hum: int
    co: float               # OJO: hoy trae la resistencia de gas cruda, no CO
    config_version_applied: int
    time_client: int

@dataclass
class Data_2:
    """Acelerómetro y giroscopio (BMI270) más magnetómetro (BMM350).

    Equivalente a protobuf Data_2. Viaja cada `send_interval_s` segundos.
    """
    id_device: str
    acc_x: float
    acc_y: float
    acc_z: float
    gyr_x: float
    gyr_y: float
    gyr_z: float
    mag_x: float
    mag_y: float
    mag_z: float
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
    send_interval_s: int    # Intervalo del flujo rápido (Data_2); 0 = sin espera
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
