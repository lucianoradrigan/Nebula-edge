import paho.mqtt.client as mqtt
import schema_pb2
import queue

mqttc = None
packet_queue = queue.Queue()

# Callback para datos de sensores
def on_message_data(client, userdata, message):
    print(f"Mensaje de datos recibido.")
    # Agrega el paquete a la cola
    packet_queue.put(message.payload)  

def on_connect(client, userdata, flags, reason_code, properties):
    if reason_code.is_failure:
        print(f"Falló la conexión: {reason_code}")
    else:
        client.subscribe("/topic/nebulaedge/data")
        client.subscribe("/topic/nebulaedge/config")
        print("Suscrito a /topic/nebulaedge/data y /topic/nebulaedge/config")

def mqtt_publish(topic, data):
    global mqttc
    mqttc.publish(topic, data)
    print(f"Publish en {topic}")

def mqtt_start():
    global mqttc

    # Inicia cliente
    mqttc = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    mqttc.on_connect = on_connect

    # Asocia función callback por tópico
    mqttc.message_callback_add("/topic/nebulaedge/data", on_message_data)
    
    # Conecta al broker
    mqttc.connect("broker.hivemq.com", 1883, 60)
    mqttc.loop_start()

def mqtt_shutdown():
    global mqttc
    mqttc.loop_stop()
    mqttc.disconnect()

if __name__ == "__main__":
    mqtt_start()