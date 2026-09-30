"""Frontera con protobuf: traduce entre los modelos del dominio y los bytes del cable.

UTILIDAD PRINCIPAL
    `DataCodec` serializa y deserializa todo lo que viaja entre el servidor y
    el ESP32, sea por BLE, MQTT, UDP o TCP. Es el único módulo que importa
    `schema_pb2` (el código generado desde schema.proto), así que es también el
    único lugar a tocar si cambia el formato de los mensajes.

        hacia el device:   ConfigData -> bytes
        desde el device:   bytes -> Environmental | Inertial | ConfigAckData | deep sleep

PAQUETES TIPADOS
    La telemetría viaja con un byte de tipo al principio, para poder saber qué
    mensaje protobuf viene detrás sin intentar decodificarlos todos:

        0x01  Environmental  BME688: temperatura, presión, humedad, gas
        0x02  Inertial       BMI270 + BMM350: acelerómetro, giroscopio, magnetómetro
        0x04  deep sleep     aviso de que el device se va a dormir

    Este byte es lo ÚNICO del framing que es igual en los cuatro protocolos:
    dónde empieza y termina cada mensaje lo resuelve cada Transport a su manera
    (ver transport.py).

    `deserialize_typed_packet()` es el punto de entrada de la telemetría: lee
    ese byte y delega en el deserializador que corresponda. El ACK de config no
    usa este esquema; va por `deserialize_config_ack()`.

SINCRONIZACIÓN CON EL FIRMWARE
    El schema tiene que calzar con el del firmware, en
    esp32/components/nebulaedge_proto_schema/schema.proto. Son dos copias del
    mismo contrato y se editan juntas; si se desincronizan, los paquetes
    quedan ilegibles de un lado.
"""
from __future__ import annotations
import schema_pb2

from models import Environmental, Inertial, ConfigData, ConfigAckData
from system import log


class DataCodec:
    """Esta clase permite que DatabaseRepository se desligue de protobuf.
    En caso de querer cambiar la forma de enviar los datos (JSON por ejemplo)
    solo se tendrá que modificar esto."""
    TYPE_ENVIRONMENTAL = 0x01
    TYPE_INERTIAL = 0x02
    # TYPE_ACK = 0x03 sería bueno implementarlo
    TYPE_DEEP_SLEEP = 0x04

    # Los tres tipos que llevan byte de prefijo, para poder descartar de una
    # que un paquete sea un ACK sin intentar decodificarlo (ver
    # is_typed_packet).
    TYPED_PREFIXES = frozenset({TYPE_ENVIRONMENTAL, TYPE_INERTIAL, TYPE_DEEP_SLEEP})

    @staticmethod
    def is_typed_packet(packet: bytes) -> bool:
        """True si el paquete empieza con uno de los bytes de tipo.

        Sirve para no pasarle telemetría al parser de ConfigAck. Un ConfigAck
        es protobuf crudo, sin prefijo: su primer byte es el tag del campo 1
        (0x0A), así que no colisiona con ninguno de los tipos.
        """
        return bool(packet) and packet[0] in DataCodec.TYPED_PREFIXES

    @staticmethod
    def split_typed_packet(packet: bytes) -> tuple[int | None, bytes]:
        """Extrae el tipo (1 byte) y el payload del paquete."""
        if not packet or len(packet) < 2:
            return None, b""
        return packet[0], packet[1:]

    @staticmethod
    def deserialize_typed_packet(packet: bytes) -> tuple["Environmental | Inertial | None", int]:
        """Parsea un paquete con prefijo de tipo y devuelve una tupla cuya primera posición es
           Environmental o Inertial, y en la segunda posición el indicador de tipo de paquete.
           En caso de no ser ninguno retorna [None, -1]"""
        pkt_type, payload = DataCodec.split_typed_packet(packet)
        if pkt_type == DataCodec.TYPE_ENVIRONMENTAL:
            return DataCodec.deserialize_environmental(payload), DataCodec.TYPE_ENVIRONMENTAL
        if pkt_type == DataCodec.TYPE_INERTIAL:
            return DataCodec.deserialize_inertial(payload), DataCodec.TYPE_INERTIAL
        if pkt_type == DataCodec.TYPE_DEEP_SLEEP:
            return payload, DataCodec.TYPE_DEEP_SLEEP
        return None, -1

    @staticmethod
    def serialize_environmental(data: Environmental) -> bytes:
        """Convierte Environmental -> protobuf Environmental -> bytes."""
        pb = schema_pb2.Environmental()
        pb.id_device = data.id_device
        pb.temperature = data.temperature
        pb.press = data.press
        pb.hum = data.hum
        pb.co = data.co
        pb.config_version_applied = data.config_version_applied
        pb.time_client = data.time_client
        return pb.SerializeToString()

    @staticmethod
    def deserialize_environmental(packet: bytes) -> Environmental | None:
        """Convierte bytes -> protobuf Environmental -> Environmental."""
        try:
            pb = schema_pb2.Environmental()
            pb.ParseFromString(packet)
        except Exception as e:
            log(f"Error al desempaquetar el paquete Environmental: {e}")
            return None

        # Convertir a objeto neutro (Environmental)
        return Environmental(
            id_device=pb.id_device,
            temperature=pb.temperature,
            press=pb.press,
            hum=pb.hum,
            co=pb.co,
            config_version_applied=pb.config_version_applied,
            time_client=pb.time_client
        )

    @staticmethod
    def serialize_inertial(data: "Inertial") -> bytes:
        """Convierte Inertial -> protobuf Inertial -> bytes."""
        pb = schema_pb2.Inertial()
        pb.id_device = data.id_device
        pb.acc_x = data.acc_x
        pb.acc_y = data.acc_y
        pb.acc_z = data.acc_z
        pb.gyr_x = data.gyr_x
        pb.gyr_y = data.gyr_y
        pb.gyr_z = data.gyr_z
        pb.mag_x = data.mag_x
        pb.mag_y = data.mag_y
        pb.mag_z = data.mag_z
        pb.config_version_applied = data.config_version_applied
        pb.time_client = data.time_client
        return pb.SerializeToString()

    @staticmethod
    def deserialize_inertial(packet: bytes) -> "Inertial | None":
        """Convierte bytes -> protobuf Inertial -> Inertial."""
        try:
            pb = schema_pb2.Inertial()
            pb.ParseFromString(packet)
        except Exception as e:
            log(f"Error al desempaquetar el paquete Inertial: {e}")
            return None

        return Inertial(
            id_device=pb.id_device,
            acc_x=pb.acc_x,
            acc_y=pb.acc_y,
            acc_z=pb.acc_z,
            gyr_x=pb.gyr_x,
            gyr_y=pb.gyr_y,
            gyr_z=pb.gyr_z,
            mag_x=pb.mag_x,
            mag_y=pb.mag_y,
            mag_z=pb.mag_z,
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
        pb.env_interval_s = config.env_interval_s
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
            log(f"Error al desempaquetar el paquete: {e}")
            return None

        return ConfigData(
            id_device=pb.id_device,
            config_version=pb.config_version,
            protocol_conf=pb.protocol_conf,
            acc_sampling=pb.acc_sampling,
            gyro_sensibility=pb.gyro_sensibility,
            bme688_sampling=pb.bme688_sampling,
            send_interval_s=pb.send_interval_s,
            env_interval_s=pb.env_interval_s,
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
            log(f"Error al desempaquetar el ACK: {e}")
            return None

        return ConfigAckData(
            id_device=pb.id_device,
            config_version=pb.config_version,
            applied=pb.applied,
            time_client=pb.time_client,
        )
