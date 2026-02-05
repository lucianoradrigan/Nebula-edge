CREATE TABLE config (
    id_device VARCHAR(45) PRIMARY KEY,
    config_version INT,
    protocol_conf INT,
    acc_sampling INT,
    gyro_sensibility INT,
    bme688_sampling INT,
    send_interval_ms INT,
    discontinuous_sleep_time INT,
    discontinuous_window_size INT,
    tcp_port INT,
    udp_port INT,
    mqtt_broker VARCHAR(128)
);

CREATE TABLE log (
    id_device VARCHAR(45),
    status_report INT,
    protocol_report INT,
    batt_level INT,
    conf_peripheral INT,
    time_client INT,
    time_server INT
);

CREATE TABLE data_1 (
    id_device VARCHAR(45),
    temperature INT,
    press INT,
    hum INT,
    co FLOAT,
    rms FLOAT,
    amp_x FLOAT,
    freq_x FLOAT,
    amp_y FLOAT,
    freq_y FLOAT,
    amp_z FLOAT,
    freq_z FLOAT
);
