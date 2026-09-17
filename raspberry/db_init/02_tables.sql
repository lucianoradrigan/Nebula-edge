CREATE SCHEMA IF NOT EXISTS nebulaedge_schema;

CREATE TABLE nebulaedge_schema.config (
    id_device VARCHAR(45) PRIMARY KEY,
    config_version INT,
    protocol_conf INT,
    acc_sampling INT,
    gyro_sensibility INT,
    bme688_sampling INT,
    send_interval_s INT,        -- Segundos entre paquetes inertial (flujo rapido)
    env_interval_s INT,         -- Segundos entre paquetes environmental (flujo lento)
    sleep_time_s INT,
    sleep_window_size INT,
    tcp_port INT,
    udp_port INT,
    host_ip_addr VARCHAR(45),
    ssid VARCHAR(128),
    passwd VARCHAR(128),
    mqtt_broker VARCHAR(128)
);

CREATE TABLE nebulaedge_schema.log (
    id_device VARCHAR(45),
    status_report INT,
    protocol_report INT,
    batt_level INT,
    time_client TIMESTAMP,
    time_server TIMESTAMP
);

-- Telemetria ambiental (BME688). Ritmo lento: env_interval_s.
CREATE TABLE nebulaedge_schema.environmental (
    id_device VARCHAR(45),
    temperature FLOAT,
    press INT,
    hum INT,
    co FLOAT,                   -- OJO: hoy guarda resistencia de gas cruda, no CO
    config_version_applied INT,
    time_client TIMESTAMP
);

-- Telemetria inercial (BMI270 + BMM350). Ritmo rapido: send_interval_s.
CREATE TABLE nebulaedge_schema.inertial (
    id_device VARCHAR(45),
    acc_x FLOAT,
    acc_y FLOAT,
    acc_z FLOAT,
    gyr_x FLOAT,
    gyr_y FLOAT,
    gyr_z FLOAT,
    mag_x FLOAT,
    mag_y FLOAT,
    mag_z FLOAT,
    config_version_applied INT,
    time_client TIMESTAMP
);
