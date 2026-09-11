"""Modelos neutros (dataclasses) del dominio NebulaEdge.

Movido desde classes.py sin cambios de lógica. Estas clases son el tipo
"de línea" que usan DataCodec, DatabaseRepository y las sesiones de
protocolo; no dependen de protobuf ni de ningún transporte.
"""
from __future__ import annotations
from dataclasses import dataclass


@dataclass
class Timeouts:
    ble_connect_sec: float = 30.0          # Timeout de intentos de conexión BLE (BleakClient)
    scan_restart_sec: float = 60.0         # Reinicio periódico de scanner BLE
    connect_cooldown_sec: float = 20.0     # Cooldown entre reconexiones por device
    config_ack_sec: float = 2.0            # Ventana para ACK de config (MQTT/UDP/TCP/BLE)
    config_ack_retries: int = 10           # Reintentos de ACK de config (MQTT/UDP/TCP/BLE)
    no_data_grace_sec: float = 50.0       # Gracia extra al esperar un dato
    mqtt_poll_sec: float = 0.1             # Sleep de polling MQTT
    ble_ack_short_sec: float = 3.0         # Timeout corto por intento en _wait_ble_ack
    config_poll_sec: float = 15.0          # Intervalo de sondeo proactivo de config, sin depender de telemetría

@dataclass
class Data_1:
    """Modelo neutro de telemetría (equivalente a protobuf Data_1)."""
    id_device: str          # Cambiar por device_id
    temperature: float
    press: int
    hum: int
    co: float
    rms: float
    amp_x: float
    freq_x: float
    amp_y: float
    freq_y: float
    amp_z: float
    freq_z: float
    mag_x: float
    mag_y: float
    mag_z: float
    config_version_applied: int
    time_client: int

@dataclass
class Data_2:
    """Modelo neutro de telemetría (equivalente a protobuf Data_2)."""
    id_device: str
    acc_x: float
    acc_y: float
    acc_z: float
    gyr_x: float
    gyr_y: float
    gyr_z: float
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
    send_interval_s: int
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
