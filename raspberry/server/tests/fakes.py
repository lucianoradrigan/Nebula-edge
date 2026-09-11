"""Dobles de prueba compartidos: repositorio en memoria, device BLE falso y
constructores de paquetes/config. Nada de Postgres ni de hardware.
"""
from __future__ import annotations
import socket

from codec import DataCodec
from models import ConfigData, ConfigAckData, Data_1, Data_2

DEVICE_ID = "AA:BB:CC:DD:EE:01"

# Mismo formato que DEEP_SLEEP_FLAG del firmware (nebulaedge_defs.c): {0x04,'d','s'}
DEEP_SLEEP_PACKET = bytes([DataCodec.TYPE_DEEP_SLEEP]) + b"ds"


def free_port(kind: int = socket.SOCK_DGRAM) -> int:
    """Pide al SO un puerto libre y lo devuelve.

    Evita que dos tests (o dos corridas seguidas) choquen en un puerto fijo.
    """
    with socket.socket(socket.AF_INET, kind) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def make_config(version: int, *, udp_port: int = 0, tcp_port: int = 0,
                device_id: str = DEVICE_ID) -> ConfigData:
    """ConfigData de prueba; lo que importa por test es la versión y el puerto."""
    return ConfigData(
        id_device=device_id,
        config_version=version,
        protocol_conf=1,
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
    )


def data_1_packet(applied_version: int, device_id: str = DEVICE_ID) -> bytes:
    """Paquete de telemetría tal como lo arma el firmware: [tipo][protobuf]."""
    d = Data_1(
        id_device=device_id, temperature=21.0, press=101000, hum=40, co=100.0,
        rms=0.1, amp_x=1, freq_x=2, amp_y=3, freq_y=4, amp_z=5, freq_z=6,
        mag_x=7, mag_y=8, mag_z=9,
        config_version_applied=applied_version, time_client=1_735_000_001,
    )
    return bytes([DataCodec.TYPE_DATA_1]) + DataCodec.serialize_data_1(d)


def data_2_packet(applied_version: int, device_id: str = DEVICE_ID) -> bytes:
    d = Data_2(
        id_device=device_id, acc_x=0.5, acc_y=-9.8, acc_z=0.1,
        gyr_x=0.001, gyr_y=-0.002, gyr_z=0.003,
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

    def __init__(self, db_version: int, *, udp_port: int = 0, tcp_port: int = 0):
        self.db_version = db_version
        self.udp_port = udp_port
        self.tcp_port = tcp_port
        self.data_1: list[Data_1] = []
        self.data_2: list[Data_2] = []
        self.logs: list = []

    async def get_config_async(self, device_id):
        return make_config(self.db_version, udp_port=self.udp_port,
                           tcp_port=self.tcp_port, device_id=device_id)

    async def insert_data_1_async(self, d):
        self.data_1.append(d)

    async def insert_data_2_async(self, d):
        self.data_2.append(d)

    async def insert_log_async(self, log):
        self.logs.append(log)


class FakeBLEDevice:
    """Las sesiones solo usan `.address` del BLEDevice de bleak."""

    def __init__(self, address: str = DEVICE_ID):
        self.address = address
