"""Cliente MQTT compartido por todo el proceso, con una cola por dispositivo.

UTILIDAD PRINCIPAL
    Mantener UNA sola conexión al broker para todo el servidor -no una por
    device- y repartir lo que llega en colas separadas según el tópico:

        /topic/nebulaedge/{id}/data         -> cola de datos de ese device
        /topic/nebulaedge/{id}/config/ack   -> cola de ACK de ese device

    `MqttTransport` (transport.py) es el único consumidor: llama a
    `mqtt_start()` y después lee de las dos colas de su device.

CICLO DE VIDA: UN RECURSO DEL PROCESO, NO DE LA SESIÓN
    `mqtt_start()` es idempotente: conecta la primera vez y no hace nada las
    siguientes, así que cada sesión que arranca puede llamarlo sin coordinarse
    con las demás.

    `mqtt_shutdown()` es para el apagado del servidor completo. El cierre de la
    sesión de un device NO debe llamarlo: dejaría sin broker a todos los demás
    devices que estén usando MQTT en ese momento.

POR QUÉ COLAS SINCRÓNICAS Y NO asyncio.Queue
    Las llena el hilo interno de paho (`loop_start()`), que no es el event loop
    de asyncio y no puede tocar sus estructuras de forma segura. El transporte
    las consume sin bloquear, haciendo `get_nowait()` cada pocos milisegundos.
"""
import paho.mqtt.client as mqtt
import queue
import threading
from typing import Dict
from system import MQTT_BROKER_PORT, MQTT_LOCAL_HOST, log

mqttc = None
packet_queue = queue.Queue()        # Fallback: mensajes cuyo tópico no trae id_device
device_queues: Dict[str, queue.Queue] = {}   # Telemetría, por device
ack_queues: Dict[str, queue.Queue] = {}      # ACK de config, por device

# TOPE DE LAS COLAS POR DEVICE
#
# Sin tope, cualquiera que publique en /topic/nebulaedge/<lo que sea>/data hace
# crecer la memoria del servidor sin límite: la cola se crea a partir del id que
# venga en el tópico -no se comprueba que ese device exista- y solo la drena la
# sesión de ese device, que para un id inventado no existe nunca. Medido en
# banco: 5000 publicaciones a un device inexistente dejaban 5000 mensajes en
# cola, con maxsize=0 y sin una sola línea en el log. Desde que el servidor
# hospeda su propio broker con acceso anónimo (mqtt_broker.py), eso es
# alcanzable desde la red local.
#
# 1000 mensajes son unos 8 minutos de telemetría al ritmo normal de un device
# (2 paquetes/s): mucho más que cualquier hueco legítimo entre dos lecturas de
# la sesión.
_QUEUE_MAX = 1000

# Un aviso por device, no uno por mensaje descartado: a 2 paquetes/s, avisar
# cada vez llenaría el log sin agregar información.
_dropping_reported: set[str] = set()


def _put_drop_oldest(q: queue.Queue, device_id: str, payload: bytes, kind: str) -> None:
    """Encola descartando el MÁS VIEJO si la cola está llena.

    Se descarta el viejo y no el nuevo, igual que en la cola de configuración de
    BLE: para telemetría el dato fresco vale más que el atrasado. Que la cola se
    llene significa que nadie la está drenando, así que guardar lo viejo sería
    guardar basura.
    """
    try:
        q.put_nowait(payload)
        return
    except queue.Full:
        pass

    try:
        q.get_nowait()          # hace lugar tirando el más viejo
    except queue.Empty:
        pass
    try:
        q.put_nowait(payload)
    except queue.Full:
        pass

    if device_id not in _dropping_reported:
        _dropping_reported.add(device_id)
        log(f"Cola de {kind} de {device_id} llena ({_QUEUE_MAX}): se descarta lo más viejo. "
            "Nadie la está drenando: o no hay sesión para ese device, o publica más rápido de lo que se lee.")

# El cliente es un único recurso por proceso: el lock protege el arranque
# contra dos sesiones que lo pidan al mismo tiempo (ver mqtt_start).
_mqtt_lock = threading.Lock()
_mqtt_started = False


def _get_device_queue(device_id: str) -> queue.Queue:
    """Obtiene/crea la cola de datos por dispositivo."""
    q = device_queues.get(device_id)
    if q is None:
        q = queue.Queue(maxsize=_QUEUE_MAX)
        device_queues[device_id] = q
    return q


def _get_ack_queue(device_id: str) -> queue.Queue:
    """Obtiene/crea la cola de ACK por dispositivo."""
    q = ack_queues.get(device_id)
    if q is None:
        q = queue.Queue(maxsize=_QUEUE_MAX)
        ack_queues[device_id] = q
    return q


def _extract_device_id(topic: str) -> str | None:
    """Extrae el id_device desde el tópico MQTT estándar."""
    # Esperado: /topic/nebulaedge/{id}/data o /topic/nebulaedge/{id}/config
    parts = topic.strip("/").split("/")
    if len(parts) >= 4 and parts[0] == "topic" and parts[1] == "nebulaedge":
        return parts[2]
    return None

# Callback para datos de sensores
def on_message_data(client, userdata, message):
    """Callback de mensajes de data: enruta por dispositivo."""
    device_id = _extract_device_id(message.topic)
    if device_id:
        _put_drop_oldest(_get_device_queue(device_id), device_id, message.payload, "telemetría")
    else:
        # fallback global
        packet_queue.put(message.payload)


def on_message_ack(client, userdata, message):
    """Callback de mensajes de ACK de configuración."""
    device_id = _extract_device_id(message.topic)
    if device_id:
        _put_drop_oldest(_get_ack_queue(device_id), device_id, message.payload, "ACK")

def on_connect(client, userdata, flags, reason_code, properties):
    """Callback de conexión: suscribe a tópicos de datos y ACKs."""
    if reason_code.is_failure:
        log(f"Falló la conexión: {reason_code}")
    else:
        client.subscribe("/topic/nebulaedge/+/data")
        client.subscribe("/topic/nebulaedge/+/config")
        client.subscribe("/topic/nebulaedge/+/config/ack")
        log("Suscrito a /topic/nebulaedge/+/data, /topic/nebulaedge/+/config y /topic/nebulaedge/+/config/ack")

def mqtt_publish(topic, data):
    """Publica un payload en el tópico especificado."""
    global mqttc
    mqttc.publish(topic, data)
    log(f"Publish en {topic}")


def get_data_queue(device_id: str) -> queue.Queue:
    """API pública para acceder a la cola de data por dispositivo."""
    return _get_device_queue(device_id)


def get_ack_queue(device_id: str) -> queue.Queue:
    """API pública para acceder a la cola de ACK por dispositivo."""
    return _get_ack_queue(device_id)

def mqtt_start():
    """Inicia el cliente MQTT compartido si todavía no está corriendo.

    Idempotente a propósito: cualquier sesión que pase a protocolo MQTT lo
    llama al entrar, sin coordinarse con las demás. Si reinstanciara el
    cliente en cada llamada, pisaría el que ya estuvieran usando otros
    devices, y ese cliente huérfano quedaría conectado con su hilo de
    loop_start() corriendo para siempre, sin nadie que lo pueda detener.

    Un solo cliente alcanza porque la separación por device ya está resuelta
    más arriba: se suscribe con wildcards (/topic/nebulaedge/+/...) y reparte
    en las colas por device_id de este módulo.
    """
    global mqttc, _mqtt_started
    if _mqtt_started:
        return
    with _mqtt_lock:
        if _mqtt_started:
            return

        mqttc = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        mqttc.on_connect = on_connect
        mqttc.message_callback_add("/topic/nebulaedge/+/data", on_message_data)
        mqttc.message_callback_add("/topic/nebulaedge/+/config/ack", on_message_ack)
        # El broker es este mismo proceso (mqtt_broker.py): el cliente del
        # servidor se conecta a loopback. Antes acá estaba broker.hivemq.com,
        # un broker público de terceros por el que pasaba toda la telemetría.
        mqttc.connect(MQTT_LOCAL_HOST, MQTT_BROKER_PORT, 60)
        mqttc.loop_start()
        _mqtt_started = True

def mqtt_shutdown():
    """Detiene el cliente MQTT compartido y su hilo de loop_start().

    Solo para el apagado del proceso completo. Llamarla al terminar la sesión
    de un device dejaría sin broker a todos los demás que estén usando MQTT.
    """
    global mqttc, _mqtt_started
    with _mqtt_lock:
        if not _mqtt_started:
            return
        mqttc.loop_stop()
        mqttc.disconnect()
        _mqtt_started = False
