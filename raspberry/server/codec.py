"""Frontera con protobuf: traduce entre los modelos del dominio y los bytes del cable.

UTILIDAD PRINCIPAL
    `DataCodec` serializa y deserializa todo lo que viaja entre el servidor y
    el ESP32, sea por BLE, MQTT, UDP o TCP. Es el único módulo que importa
    `schema_pb2` (el código generado desde schema.proto), así que es también el
    único lugar a tocar si cambia el formato de los mensajes.

        hacia el device:   ConfigData -> bytes
        desde el device:   bytes -> Data_1 | Data_2 | ConfigAckData | deep sleep

PAQUETES TIPADOS
    La telemetría viaja con un byte de tipo al principio, para poder saber qué
    mensaje protobuf viene detrás sin intentar decodificarlos todos:

        0x01  Data_1  BME688: temperatura, presión, humedad, gas
        0x02  Data_2       BMI270 + BMM350: acelerómetro, giroscopio, magnetómetro
        0x04  deep sleep     aviso de que el device se va a dormir
        0x05  ModelStatus        INFER: estado del modelo y prueba de vida
        0x06  ModelChunkRequest  INFER: el device pide un trozo del modelo
        0x07  InferResult        INFER: la respuesta a un pedido de inferencia

    Los valores se definen en el enum PacketType de schema.proto.

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

from models import (
    Data_1, Data_2, ConfigData, ConfigAckData,
    ModelInfo, ModelChunkRequestData, ModelStatusData,
    InferSample, InferResultData,
)
from system import log


class DataCodec:
    """Esta clase permite que DatabaseRepository se desligue de protobuf.
    En caso de querer cambiar la forma de enviar los datos (JSON por ejemplo)
    solo se tendrá que modificar esto."""
    # Los valores salen del enum PacketType de schema.proto, que es también de
    # donde los toma el firmware. El 3 no se usa: el ConfigAck viaja sin
    # prefijo de tipo (ver deserialize_config_ack).
    TYPE_DATA_1 = schema_pb2.PACKET_TYPE_DATA_1
    TYPE_DATA_2 = schema_pb2.PACKET_TYPE_DATA_2
    TYPE_DEEP_SLEEP = schema_pb2.PACKET_TYPE_DEEP_SLEEP
    # Los tres que manda el device en INFER.
    TYPE_MODEL_STATUS = schema_pb2.PACKET_TYPE_MODEL_STATUS
    TYPE_MODEL_CHUNK_REQUEST = schema_pb2.PACKET_TYPE_MODEL_CHUNK_REQUEST
    TYPE_INFER_RESULT = schema_pb2.PACKET_TYPE_INFER_RESULT

    # Los tipos que llevan byte de prefijo, para poder descartar de una que un
    # paquete sea un ACK sin intentar decodificarlo (ver is_typed_packet).
    TYPED_PREFIXES = frozenset({TYPE_DATA_1, TYPE_DATA_2, TYPE_DEEP_SLEEP,
                                TYPE_MODEL_STATUS, TYPE_MODEL_CHUNK_REQUEST,
                                TYPE_INFER_RESULT})

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
    def deserialize_typed_packet(packet: bytes) -> tuple["Data_1 | Data_2 | None", int]:
        """Parsea un paquete con prefijo de tipo y devuelve una tupla cuya primera posición es
           Data_1 o Data_2, y en la segunda posición el indicador de tipo de paquete.
           En caso de no ser ninguno retorna [None, -1]"""
        pkt_type, payload = DataCodec.split_typed_packet(packet)
        if pkt_type == DataCodec.TYPE_DATA_1:
            return DataCodec.deserialize_data_1(payload), DataCodec.TYPE_DATA_1
        if pkt_type == DataCodec.TYPE_DATA_2:
            return DataCodec.deserialize_data_2(payload), DataCodec.TYPE_DATA_2
        if pkt_type == DataCodec.TYPE_DEEP_SLEEP:
            return payload, DataCodec.TYPE_DEEP_SLEEP
        if pkt_type == DataCodec.TYPE_MODEL_STATUS:
            return DataCodec.deserialize_model_status(payload), DataCodec.TYPE_MODEL_STATUS
        if pkt_type == DataCodec.TYPE_MODEL_CHUNK_REQUEST:
            return DataCodec.deserialize_model_chunk_request(payload), DataCodec.TYPE_MODEL_CHUNK_REQUEST
        if pkt_type == DataCodec.TYPE_INFER_RESULT:
            return DataCodec.deserialize_infer_result(payload), DataCodec.TYPE_INFER_RESULT
        return None, -1

    @staticmethod
    def serialize_data_1(data: Data_1) -> bytes:
        """Convierte Data_1 -> protobuf Data_1 -> bytes."""
        pb = schema_pb2.Data_1()
        pb.id_device = data.id_device
        pb.temperature = data.temperature
        pb.press = data.press
        pb.hum = data.hum
        pb.co = data.co
        pb.config_version_applied = data.config_version_applied
        pb.time_client = data.time_client
        return pb.SerializeToString()

    @staticmethod
    def deserialize_data_1(packet: bytes) -> Data_1 | None:
        """Convierte bytes -> protobuf Data_1 -> Data_1."""
        try:
            pb = schema_pb2.Data_1()
            pb.ParseFromString(packet)
        except Exception as e:
            log(f"Error al desempaquetar el paquete Data_1: {e}")
            return None

        # Convertir a objeto neutro (Data_1)
        return Data_1(
            id_device=pb.id_device,
            temperature=pb.temperature,
            press=pb.press,
            hum=pb.hum,
            co=pb.co,
            config_version_applied=pb.config_version_applied,
            time_client=pb.time_client
        )

    @staticmethod
    def serialize_data_2(data: "Data_2") -> bytes:
        """Convierte Data_2 -> protobuf Data_2 -> bytes."""
        pb = schema_pb2.Data_2()
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
    def deserialize_data_2(packet: bytes) -> "Data_2 | None":
        """Convierte bytes -> protobuf Data_2 -> Data_2."""
        try:
            pb = schema_pb2.Data_2()
            pb.ParseFromString(packet)
        except Exception as e:
            log(f"Error al desempaquetar el paquete Data_2: {e}")
            return None

        return Data_2(
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
        pb.sleep_time_s = config.sleep_time_s
        pb.sleep_window_size = config.sleep_window_size
        pb.tcp_port = config.tcp_port
        pb.udp_port = config.udp_port
        pb.host_ip_addr = config.host_ip_addr
        pb.ssid = config.ssid
        pb.passwd = config.passwd
        pb.mqtt_broker = config.mqtt_broker
        pb.time_client = config.time_client
        pb.model_version = config.model_version
        pb.infer_source = config.infer_source
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
            sleep_time_s=pb.sleep_time_s,
            sleep_window_size=pb.sleep_window_size,
            tcp_port=pb.tcp_port,
            udp_port=pb.udp_port,
            host_ip_addr=pb.host_ip_addr,
            ssid=pb.ssid,
            passwd=pb.passwd,
            mqtt_broker=pb.mqtt_broker,
            time_client=pb.time_client,
            model_version=pb.model_version,
            infer_source=pb.infer_source,
        )

    @staticmethod
    def serialize_config_ack(ack: "ConfigAckData") -> bytes:
        """Convierte ConfigAckData -> protobuf ConfigAck -> bytes."""
        pb = schema_pb2.ConfigAck()
        pb.id_device = ack.id_device
        pb.config_version = ack.config_version
        pb.applied = ack.applied
        pb.time_client = ack.time_client
        pb.model_version_active = ack.model_version_active
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
            model_version_active=pb.model_version_active,
        )

    # ------------------------------------------------------------- INFER

    @staticmethod
    def model_n_chunks(model: ModelInfo, chunk_size: int) -> int:
        return (len(model.tflite) + chunk_size - 1) // chunk_size

    @staticmethod
    def serialize_model_manifest(device_id: str, model: ModelInfo | None, chunk_size: int) -> bytes:
        """Manifiesto del modelo que le toca al device.

        Con `model` None sale un manifiesto con model_version 0: "no hay nada
        que bajar". Se publica igual, y no se omite, porque va retenido: si no
        se reemplazara, el device seguiría viendo el de un modelo anterior.
        """
        pb = schema_pb2.ModelManifest()
        pb.id_device = device_id
        if model is None:
            return pb.SerializeToString()

        pb.model_version = model.model_version
        pb.size_bytes = len(model.tflite)
        pb.chunk_size = chunk_size
        pb.n_chunks = DataCodec.model_n_chunks(model, chunk_size)
        pb.sha256 = model.sha256
        pb.n_features = model.n_features
        pb.n_classes = model.n_classes
        pb.window_size = model.window_size
        pb.fs_hz = model.fs_hz
        pb.feature_set_version = model.feature_set_version
        pb.output_severity_index = model.output_severity_index
        pb.output_fault_index = model.output_fault_index
        pb.feat_mean.extend(model.feat_mean)
        pb.feat_std.extend(model.feat_std)
        pb.test_input.extend(model.test_input)
        pb.test_output.extend(model.test_output)
        return pb.SerializeToString()

    @staticmethod
    def serialize_model_chunk(model: ModelInfo, chunk_index: int, chunk_size: int) -> bytes:
        """Trozo `chunk_index` del .tflite. El caller valida que exista."""
        offset = chunk_index * chunk_size
        pb = schema_pb2.ModelChunk()
        pb.model_version = model.model_version
        pb.chunk_index = chunk_index
        pb.offset = offset
        pb.data = model.tflite[offset:offset + chunk_size]
        return pb.SerializeToString()

    @staticmethod
    def deserialize_model_chunk_request(packet: bytes) -> ModelChunkRequestData | None:
        try:
            pb = schema_pb2.ModelChunkRequest()
            pb.ParseFromString(packet)
        except Exception as e:
            log(f"Error al desempaquetar el pedido de trozo: {e}")
            return None
        return ModelChunkRequestData(
            id_device=pb.id_device,
            model_version=pb.model_version,
            chunk_index=pb.chunk_index,
        )

    @staticmethod
    def deserialize_model_status(packet: bytes) -> ModelStatusData | None:
        try:
            pb = schema_pb2.ModelStatus()
            pb.ParseFromString(packet)
        except Exception as e:
            log(f"Error al desempaquetar el ModelStatus: {e}")
            return None
        return ModelStatusData(
            id_device=pb.id_device,
            model_version=pb.model_version,
            state=pb.state,
            chunks_received=pb.chunks_received,
            error=pb.error,
            config_version_applied=pb.config_version_applied,
            time_client=pb.time_client,
            model_version_active=pb.model_version_active,
            selftest_max_error=pb.selftest_max_error,
            arena_used_bytes=pb.arena_used_bytes,
            invoke_us=pb.invoke_us,
        )

    # Los dos que siguen arman lo que manda el DEVICE. El servidor no los
    # usa: existen para que los tests construyan esos paquetes con el codec
    # real, igual que con serialize_data_1().

    @staticmethod
    def serialize_model_chunk_request(req: ModelChunkRequestData) -> bytes:
        pb = schema_pb2.ModelChunkRequest()
        pb.id_device = req.id_device
        pb.model_version = req.model_version
        pb.chunk_index = req.chunk_index
        return pb.SerializeToString()

    @staticmethod
    def serialize_model_status(st: ModelStatusData) -> bytes:
        pb = schema_pb2.ModelStatus()
        pb.id_device = st.id_device
        pb.model_version = st.model_version
        pb.state = st.state
        pb.chunks_received = st.chunks_received
        pb.error = st.error
        pb.config_version_applied = st.config_version_applied
        pb.time_client = st.time_client
        pb.model_version_active = st.model_version_active
        pb.selftest_max_error = st.selftest_max_error
        pb.arena_used_bytes = st.arena_used_bytes
        pb.invoke_us = st.invoke_us
        return pb.SerializeToString()

    @staticmethod
    def serialize_infer_request(request_id: int, model_version: int, sample: InferSample,
                                raw_window: bool) -> bytes:
        """Pedido de inferencia: features (C1) o la ventana cruda (C2), nunca
        las dos. Con la ventana van rpm y fs_hz, que el device necesita para
        calcular las bandas de orden."""
        pb = schema_pb2.InferRequest()
        pb.request_id = request_id
        pb.model_version = model_version
        if raw_window:
            pb.window.extend(sample.window)
            pb.rpm = sample.rpm
            pb.fs_hz = sample.fs_hz
        else:
            pb.features.extend(sample.features)
        return pb.SerializeToString()

    @staticmethod
    def deserialize_infer_result(packet: bytes) -> InferResultData | None:
        try:
            pb = schema_pb2.InferResult()
            pb.ParseFromString(packet)
        except Exception as e:
            log(f"Error al desempaquetar el InferResult: {e}")
            return None
        return InferResultData(
            id_device=pb.id_device,
            request_id=pb.request_id,
            model_version=pb.model_version,
            status=pb.status,
            severity=pb.severity,
            probs=list(pb.probs),
            pred_fault=pb.pred_fault,
            feature_us=pb.feature_us,
            inference_us=pb.inference_us,
            config_version_applied=pb.config_version_applied,
            time_client=pb.time_client,
            features=list(pb.features),
            input_kind=pb.input_kind,
        )

    @staticmethod
    def serialize_infer_result(r: InferResultData) -> bytes:
        """Lo arma el DEVICE; existe para los tests."""
        pb = schema_pb2.InferResult()
        pb.id_device = r.id_device
        pb.request_id = r.request_id
        pb.model_version = r.model_version
        pb.status = r.status
        pb.severity = r.severity
        pb.probs.extend(r.probs)
        pb.pred_fault = r.pred_fault
        pb.feature_us = r.feature_us
        pb.inference_us = r.inference_us
        pb.config_version_applied = r.config_version_applied
        pb.time_client = r.time_client
        pb.features.extend(r.features)
        pb.input_kind = r.input_kind
        return pb.SerializeToString()

    @staticmethod
    def deserialize_infer_request(packet: bytes):
        """Para los tests: devuelve el protobuf tal cual."""
        pb = schema_pb2.InferRequest()
        pb.ParseFromString(packet)
        return pb

    @staticmethod
    def deserialize_model_manifest(packet: bytes):
        """Para los tests: devuelve el protobuf tal cual."""
        pb = schema_pb2.ModelManifest()
        pb.ParseFromString(packet)
        return pb

    @staticmethod
    def deserialize_model_chunk(packet: bytes):
        """Para los tests: devuelve el protobuf tal cual."""
        pb = schema_pb2.ModelChunk()
        pb.ParseFromString(packet)
        return pb
