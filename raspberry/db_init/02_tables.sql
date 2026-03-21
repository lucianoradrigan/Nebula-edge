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
    host_ip_addr VARCHAR(45),
    ssid VARCHAR(128),
    passwd VARCHAR(128),
    mqtt_broker VARCHAR(128)
);

CREATE TABLE log (
    id_device INT,
    time_client BIGINT,
    time_server BIGINT
);

CREATE TABLE data_1 (
    id_device VARCHAR(45),
    temperature FLOAT,
    press INT,
    hum INT,
    co FLOAT,
    rms FLOAT,
    amp_x FLOAT,
    freq_x FLOAT,
    amp_y FLOAT,
    freq_y FLOAT,
    amp_z FLOAT,
    freq_z FLOAT,
    mag_x FLOAT,
    mag_y FLOAT,
    mag_z FLOAT,
    config_version_applied INT
);

CREATE TABLE data_2 (
    id_device VARCHAR(45),
    acc_x FLOAT,
    acc_y FLOAT,
    acc_z FLOAT,
    gyr_x FLOAT,
    gyr_y FLOAT,
    gyr_z FLOAT,
    time_client BIGINT,
    config_version_applied INT
);
