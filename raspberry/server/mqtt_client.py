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

mqttc = None
packet_queue = queue.Queue()        # Fallback: mensajes cuyo tópico no trae id_device
device_queues: Dict[str, queue.Queue] = {}   # Telemetría, por device
ack_queues: Dict[str, queue.Queue] = {}      # ACK de config, por device

# El cliente es un único recurso por proceso: el lock protege el arranque
# contra dos sesiones que lo pidan al mismo tiempo (ver mqtt_start).
_mqtt_lock = threading.Lock()
_mqtt_started = False


def _get_device_queue(device_id: str) -> queue.Queue:
    """Obtiene/crea la cola de datos por dispositivo."""
    q = device_queues.get(device_id)
    if q is None:
        q = queue.Queue()
        device_queues[device_id] = q
    return q


def _get_ack_queue(device_id: str) -> queue.Queue:
    """Obtiene/crea la cola de ACK por dispositivo."""
    q = ack_queues.get(device_id)
    if q is None:
        q = queue.Queue()
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
        _get_device_queue(device_id).put(message.payload)
    else:
        # fallback global
        packet_queue.put(message.payload)


def on_message_ack(client, userdata, message):
    """Callback de mensajes de ACK de configuración."""
    device_id = _extract_device_id(message.topic)
    if device_id:
        _get_ack_queue(device_id).put(message.payload)

def on_connect(client, userdata, flags, reason_code, properties):
    """Callback de conexión: suscribe a tópicos de datos y ACKs."""
    if reason_code.is_failure:
        print(f"Falló la conexión: {reason_code}")
    else:
        client.subscribe("/topic/nebulaedge/+/data")
        client.subscribe("/topic/nebulaedge/+/config")
        client.subscribe("/topic/nebulaedge/+/config/ack")
        print("Suscrito a /topic/nebulaedge/+/data, /topic/nebulaedge/+/config y /topic/nebulaedge/+/config/ack")

def mqtt_publish(topic, data):
    """Publica un payload en el tópico especificado."""
    global mqttc
    mqttc.publish(topic, data)
    print(f"Publish en {topic}")


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
        mqttc.connect("broker.hivemq.com", 1883, 60)
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
