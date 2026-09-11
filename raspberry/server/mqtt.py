import paho.mqtt.client as mqtt
import queue
import threading
from typing import Dict

mqttc = None
packet_queue = queue.Queue()
device_queues: Dict[str, queue.Queue] = {}
ack_queues: Dict[str, queue.Queue] = {}

# Guarda del estado del cliente compartido (ver mqtt_start/mqtt_shutdown más
# abajo): el cliente MQTT es un único recurso por proceso, no por device.
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

    Idempotente a propósito. Antes, MQTTDeviceSession.run() llamaba esto al
    entrar cada vez que CUALQUIER device pasaba a protocolo MQTT: cada
    llamada reinstanciaba `mqttc`, pisando el cliente que ya estuviera
    usando otro device. El cliente viejo quedaba conectado con su hilo de
    loop_start() corriendo para siempre (nadie más tenía la referencia para
    detenerlo), y si esa otra sesión llamaba después a mqtt_shutdown(),
    apagaba la conexión de TODOS los devices, no solo la suya.

    El cliente MQTT es un recurso compartido por proceso, no por sesión: ya
    enruta por device vía tópicos wildcard (/topic/nebulaedge/+/...) y las
    colas por device_id de este módulo. Por eso ahora arranca una sola vez
    y las sesiones individuales no lo apagan al terminar (ver
    mqtt_shutdown).
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

    Pensada para el apagado del proceso completo, no para el fin de la
    sesión de un device individual (que ya no la llama): ver mqtt_start.
    """
    global mqttc, _mqtt_started
    with _mqtt_lock:
        if not _mqtt_started:
            return
        mqttc.loop_stop()
        mqttc.disconnect()
        _mqtt_started = False

if __name__ == "__main__":
    mqtt_start()