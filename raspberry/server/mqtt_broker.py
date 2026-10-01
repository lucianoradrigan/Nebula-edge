"""Broker MQTT propio: el servidor hospeda el broker, no se conecta a uno ajeno.

POR QUÉ ESTÁ ACÁ
    Antes el servidor y los devices se encontraban en un broker PÚBLICO de
    terceros (broker.hivemq.com). Eso significa que toda la telemetría salía a
    internet y que el sistema dependía de un servicio que no controlamos: si se
    cae, o si cambia de política, MQTT deja de funcionar y no hay nada que
    hacer. Ahora el broker corre dentro de este mismo proceso y escucha en la
    red local; los devices llegan a él porque la URL viaja en el paquete de
    configuración (repository.get_config), así que el firmware no cambia.

QUIÉN HABLA CON QUIÉN
        ESP32  --publica-->  este broker  <--suscrito--  mqtt_client.py
    Las dos puntas son clientes del mismo broker. El cliente del servidor se
    conecta a 127.0.0.1: está hospedando, no sale a ningún lado.

PAHO NO ES UN BROKER
    paho-mqtt es una librería CLIENTE: no puede escuchar conexiones. Se sigue
    usando como cliente (mqtt_client.py). Lo que escucha es amqtt, que es un
    broker en asyncio puro y por eso se puede levantar en el mismo event loop
    del servidor en vez de pedir un proceso aparte.

CICLO DE VIDA
    Es un recurso del PROCESO, como el cliente MQTT: lo levanta server.py al
    arrancar y lo baja al terminar. Ninguna sesión de device debe tocarlo.
"""
from __future__ import annotations

from amqtt.broker import Broker

from system import MQTT_BROKER_BIND, MQTT_BROKER_PORT, log

_broker: Broker | None = None


def _broker_config() -> dict:
    """Configuración mínima: un listener TCP y nada más.

    `sys_interval` en 0 apaga los tópicos $SYS/ de estadísticas: no los lee
    nadie y publicarlos cada N segundos solo agrega ruido al log.

    RUIDO DE ARRANQUE ESPERADO, NO SON ERRORES
    amqtt escribe tres líneas al levantar que no hay forma de sacar sin
    romperlo: "Configuration parameter 'password-file' not found" (el plugin de
    autenticación por archivo, que no usamos), "'sys_interval' key is not set or
    is None" y un DeprecationWarning de psutil por el plugin de $SYS. Probado:
    declarar `plugins: {}` o dejar solo el plugin anónimo APAGA LA ENTREGA de
    mensajes, así que el ruido se queda y se documenta acá.
    """
    return {
        "listeners": {
            "default": {
                "type": "tcp",
                "bind": f"{MQTT_BROKER_BIND}:{MQTT_BROKER_PORT}",
                # 0 = sin tope. La cantidad de devices la limita la red, no esto.
                "max_connections": 0,
            },
        },
        "sys_interval": 0,
        "auth": {"allow_anonymous": True},
        "topic_check": {"enabled": False},
    }


async def broker_start() -> bool:
    """Levanta el broker. True si quedó escuchando.

    No propaga: si el puerto está ocupado -por ejemplo, por un mosquitto que ya
    estaba corriendo en la máquina- el resto del servidor tiene que seguir
    funcionando, porque los otros tres protocolos no dependen de MQTT. Se
    reporta y se sigue.
    """
    global _broker
    if _broker is not None:
        return True

    try:
        _broker = Broker(_broker_config())
        await _broker.start()
    except Exception as e:
        _broker = None
        log(f"ERROR al levantar el broker MQTT en {MQTT_BROKER_BIND}:{MQTT_BROKER_PORT}: {e}")
        log("Los devices en MQTT no van a poder publicar. Los otros protocolos siguen.")
        return False

    log(f"Broker MQTT propio escuchando en {MQTT_BROKER_BIND}:{MQTT_BROKER_PORT}")
    return True


async def broker_shutdown() -> None:
    """Baja el broker. Solo para el apagado del proceso completo."""
    global _broker
    if _broker is None:
        return
    try:
        await _broker.shutdown()
    except Exception as e:
        log(f"Error al bajar el broker MQTT: {e}")
    finally:
        _broker = None
