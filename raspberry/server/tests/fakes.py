"""Dobles de prueba compartidos por los cuatro tests de sesión.

UTILIDAD PRINCIPAL
    Dar a los tests todo lo que una sesión necesita de su entorno, sin levantar
    nada externo:

        FakeRepo        repositorio en memoria; cumple el mismo contrato que
                        DatabaseRepository y guarda lo insertado en listas,
                        para poder hacerle assert
        FakeBLEDevice   lo mínimo que la sesión le pide a un BLEDevice de bleak
                        (la dirección MAC)
        free_port()     puerto libre del SO, para que dos tests en paralelo no
                        choquen
        make_config()   ConfigData armada, con la versión que pida el test
        data_1_packet() / data_2_packet() / config_ack_packet()
                        paquetes ya serializados, idénticos a los que mandaría
                        el firmware
        tcp_framed()    le pone a un paquete el prefijo de largo que exige TCP

    Los paquetes se construyen con el `DataCodec` real, no a mano: si cambia el
    formato del protocolo, estos tests fallan, que es justamente lo que se
    quiere.
"""
from __future__ import annotations
import hashlib
import socket

from codec import DataCodec
from models import (
    ConfigData, ConfigAckData, Data_1, Data_2,
    ModelInfo, ModelChunkRequestData, ModelStatusData,
    InferSample, InferResultData,
)

DEVICE_ID = "AA:BB:CC:DD:EE:01"

# Mismo formato que DEEP_SLEEP_FLAG del firmware (nebulaedge_defs.c): {0x04,'d','s'}
DEEP_SLEEP_PACKET = bytes([DataCodec.TYPE_DEEP_SLEEP]) + b"ds"


def tcp_framed(payload: bytes) -> bytes:
    """Antepone el largo en 2 bytes big-endian, como hace tcp_send() del firmware.

    Escrito a mano y no reusando TcpTransport a propósito: así el test fija el
    formato de forma independiente de la implementación que está probando. Si
    alguien cambia el prefijo en transport.py sin cambiarlo en el firmware,
    estos tests tienen que fallar.
    """
    return len(payload).to_bytes(2, "big") + payload


def free_port(kind: int = socket.SOCK_DGRAM) -> int:
    """Pide al SO un puerto libre y lo devuelve.

    Evita que dos tests (o dos corridas seguidas) choquen en un puerto fijo.
    """
    with socket.socket(socket.AF_INET, kind) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def make_config(version: int, *, udp_port: int = 0, tcp_port: int = 0,
                device_id: str = DEVICE_ID, protocol_conf: int = 1,
                model_version: int = 0) -> ConfigData:
    """ConfigData de prueba; lo que importa por test es la versión y el puerto."""
    return ConfigData(
        id_device=device_id,
        config_version=version,
        protocol_conf=protocol_conf,
        acc_sampling=400,
        gyro_sensibility=500,
        bme688_sampling=8,
        send_interval_s=1,
        sleep_time_s=0,
        sleep_window_size=10,
        tcp_port=tcp_port,
        udp_port=udp_port,
        host_ip_addr="127.0.0.1",
        ssid="ssid-test",
        passwd="pass-test",
        mqtt_broker="mqtt://broker:1883",
        time_client=1_735_000_000,
        model_version=model_version,
    )


def make_model(version: int = 1, size: int = 5000, test_dataset: str | None = "cwru_3hp") -> ModelInfo:
    """Modelo de prueba. Los bytes siguen un patrón para que un trozo
    desplazado se note al comparar."""
    tflite = bytes(i % 251 for i in range(size))
    return ModelInfo(
        model_version=version, tflite=tflite,
        sha256=hashlib.sha256(tflite).digest(),
        n_features=20, n_classes=4, window_size=2048, fs_hz=12000.0,
        feature_set_version=1, output_severity_index=1, output_fault_index=0,
        feat_mean=[0.0] * 20, feat_std=[1.0] * 20,
        test_input=[0.5] * 20, test_output=[0.25, 0.7, 0.1, 0.1, 0.1],
        test_dataset=test_dataset,
    )


def model_chunk_request_packet(model_version: int, chunk_index: int,
                               device_id: str = DEVICE_ID) -> bytes:
    """Pedido de trozo tal como lo arma el firmware: [0x06][protobuf]."""
    req = ModelChunkRequestData(id_device=device_id, model_version=model_version,
                                chunk_index=chunk_index)
    return bytes([DataCodec.TYPE_MODEL_CHUNK_REQUEST]) + DataCodec.serialize_model_chunk_request(req)


def model_status_packet(state: int, *, model_version: int = 1, applied_version: int = 1,
                        chunks_received: int = 0, error: int = 0, active: int | None = None,
                        arena: int = 0, device_id: str = DEVICE_ID) -> bytes:
    """Estado tal como lo arma el firmware: [0x05][protobuf]."""
    st = ModelStatusData(
        id_device=device_id, model_version=model_version, state=state,
        chunks_received=chunks_received, error=error,
        config_version_applied=applied_version, time_client=1_735_000_004,
        model_version_active=model_version if active is None else active,
        arena_used_bytes=arena,
    )
    return bytes([DataCodec.TYPE_MODEL_STATUS]) + DataCodec.serialize_model_status(st)


def make_sample(index: int, true_fault: int = 1) -> InferSample:
    return InferSample(
        dataset="cwru_3hp", sample_index=index, source_file="IR007_3.mat", window_index=index,
        true_fault=true_fault, true_severity=1 / 3, rpm=1730.0, fs_hz=12000.0,
        features=[0.1 * index] * 20, window=[0.01] * 2048,
    )


def infer_result_packet(request_id: int, pred_fault: int, status: int = 0, model_version: int = 1,
                        device_id: str = DEVICE_ID, input_kind: int = 0) -> bytes:
    """Respuesta tal como la arma el firmware: [0x07][protobuf]."""
    r = InferResultData(
        id_device=device_id, request_id=request_id, model_version=model_version, status=status,
        severity=0.33, probs=[0.1, 0.7, 0.1, 0.1], pred_fault=pred_fault, feature_us=0,
        inference_us=1000, config_version_applied=1, time_client=1_735_000_005, features=[],
        input_kind=input_kind,
    )
    return bytes([DataCodec.TYPE_INFER_RESULT]) + DataCodec.serialize_infer_result(r)


def data_1_packet(applied_version: int, device_id: str = DEVICE_ID) -> bytes:
    """Paquete ambiental tal como lo arma el firmware: [tipo][protobuf]."""
    d = Data_1(
        id_device=device_id, temperature=21.0, press=101000, hum=40, co=100.0,
        config_version_applied=applied_version, time_client=1_735_000_001,
    )
    return bytes([DataCodec.TYPE_DATA_1]) + DataCodec.serialize_data_1(d)


def data_2_packet(applied_version: int, device_id: str = DEVICE_ID) -> bytes:
    """Paquete inercial tal como lo arma el firmware: [tipo][protobuf]."""
    d = Data_2(
        id_device=device_id, acc_x=0.5, acc_y=-9.8, acc_z=0.1,
        gyr_x=0.001, gyr_y=-0.002, gyr_z=0.003,
        mag_x=7, mag_y=8, mag_z=9,
        config_version_applied=applied_version, time_client=1_735_000_002,
    )
    return bytes([DataCodec.TYPE_DATA_2]) + DataCodec.serialize_data_2(d)


def config_ack_packet(version: int, applied: bool = True,
                      device_id: str = DEVICE_ID) -> bytes:
    ack = ConfigAckData(
        id_device=device_id, config_version=version,
        applied=applied, time_client=1_735_000_003,
    )
    return DataCodec.serialize_config_ack(ack)


class FakeRepo:
    """Repositorio en memoria. Cumple por duck typing lo que usan las sesiones.

    `db_version` se puede mover en caliente durante un test para simular que
    alguien cambió la configuración en la BD.
    """

    def __init__(self, db_version: int, *, udp_port: int = 0, tcp_port: int = 0,
                 protocol_conf: int = 1, model_version: int = 0,
                 models: dict[int, ModelInfo] | None = None):
        self.db_version = db_version
        self.udp_port = udp_port
        self.tcp_port = tcp_port
        self.protocol_conf = protocol_conf
        self.model_version = model_version
        self.models = models or {}      # catálogo de la tabla `model`
        self.data_1: list[Data_1] = []
        self.data_2: list[Data_2] = []
        self.logs: list = []

    async def get_config_async(self, device_id):
        cfg = make_config(self.db_version, udp_port=self.udp_port,
                          tcp_port=self.tcp_port, device_id=device_id,
                          protocol_conf=self.protocol_conf,
                          model_version=self.model_version)
        cfg.infer_source = self.infer_source
        return cfg

    async def get_model_async(self, model_version):
        return self.models.get(model_version)

    # --- fase C: set de test e inference_result, en memoria ---
    infer_source = 0
    samples: list = []
    completed_before = 0        # lo que count_completed_inferences devuelve

    def _ensure_infer_tables(self):
        if not hasattr(self, "inference_rows"):
            self.inference_rows: dict[int, dict] = {}

    async def get_infer_samples_async(self, dataset):
        return list(self.samples)

    async def count_completed_inferences_async(self, *args):
        return self.completed_before

    async def insert_inference_request_async(self, device_id, request_id, run_id, model_version,
                                             input_kind, config_version, sample, time_sent):
        self._ensure_infer_tables()
        self.inference_rows[request_id] = dict(run_id=run_id, sample=sample, input_kind=input_kind,
                                               result=None)
        return True

    async def complete_inference_async(self, device_id, result, time_server):
        self._ensure_infer_tables()
        self.inference_rows[result.request_id]["result"] = result
        return True

    async def insert_unsolicited_inference_async(self, device_id, result, time_server):
        if not hasattr(self, "unsolicited_rows"):
            self.unsolicited_rows: list = []
        self.unsolicited_rows.append(result)
        return True

    async def inference_run_summary_async(self, run_id):
        self._ensure_infer_tables()
        rows = [r for r in self.inference_rows.values() if r["run_id"] == run_id]
        ok = [r for r in rows if r["result"] and r["result"].status == 0]
        acc = sum(r["result"].pred_fault == r["sample"].true_fault for r in ok) / len(ok) if ok else None
        return dict(sent=len(rows), ok=len(ok), accuracy=acc, mae=0.0,
                    inference_us=1000.0, feature_us=None, rtt_s=0.01)

    # Devuelven True como el repositorio real: "la fila quedó guardada".
    # `insert_ok = False` simula una base caída sin tener que tener una.
    insert_ok = True

    async def insert_data_1_async(self, d):
        self.data_1.append(d)
        return self.insert_ok

    async def insert_data_2_async(self, d):
        self.data_2.append(d)
        return self.insert_ok

    async def insert_log_async(self, log):
        self.logs.append(log)
        return self.insert_ok


class FakeBLEDevice:
    """Las sesiones solo usan `.address` del BLEDevice de bleak."""

    def __init__(self, address: str = DEVICE_ID):
        self.address = address
