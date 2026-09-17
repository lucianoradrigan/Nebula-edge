-- Insertar valores iniciales segun create_tables.py
--
-- send_interval_s = 1  -> un paquete inertial por segundo
-- env_interval_s  = 10 -> un paquete environmental cada 10 s
--    El ambiental va mas lento a proposito: temperatura, presion y humedad no
--    cambian en un segundo, y repetir el mismo valor gasta radio y bateria.
INSERT INTO nebulaedge_schema.config (
    id_device, config_version, protocol_conf, acc_sampling, gyro_sensibility,
    bme688_sampling, send_interval_s, env_interval_s, sleep_time_s, sleep_window_size,
    tcp_port, udp_port, mqtt_broker
) VALUES
('34:85:18:A2:DB:4E', 0, 0, 400, 500, 8, 1, 10, 1, 10, 1819, 1235, 'mqtt://broker.hivemq.com:1883'),
('34:85:18:A2:DB:A2', 0, 2, 400, 500, 8, 1, 10, 1, 10, 1820, 1236, 'mqtt://broker.hivemq.com:1883'),
('34:85:18:A2:DB:BE', 0, 3, 400, 500, 8, 1, 10, 1, 10, 1818, 1234, 'mqtt://broker.hivemq.com:1883'),
('34:85:18:A2:DB:06', 0, 0, 400, 500, 8, 1, 10, 1, 10, 1821, 1237, 'mqtt://broker.hivemq.com:1883'),
('34:85:18:A2:DE:1A', 0, 1, 400, 500, 8, 1, 10, 1, 10, 1822, 1238, 'mqtt://broker.hivemq.com:1883'),
('DC:DA:0C:41:F6:8A', 0, 3, 400, 500, 8, 1, 10, 1, 10, 1823, 1239, 'mqtt://broker.hivemq.com:1883'),
('58:BF:25:99:B4:92', 0, 1, 400, 500, 8, 1, 10, 1, 10, 1824, 1240, 'mqtt://broker.hivemq.com:1883'),
('3C:61:05:65:A6:3E', 0, 1, 400, 500, 8, 1, 10, 1, 10, 1825, 1241, 'mqtt://broker.hivemq.com:1883'),
('C0:49:EF:08:D0:C2', 0, 1, 400, 500, 8, 1, 10, 1, 10, 1826, 1242, 'mqtt://broker.hivemq.com:1883'),
('C0:49:EF:08:CE:82', 0, 1, 400, 500, 8, 1, 10, 1, 10, 1827, 1243, 'mqtt://broker.hivemq.com:1883')
ON CONFLICT (id_device)
DO UPDATE SET
    config_version = EXCLUDED.config_version,
    protocol_conf = EXCLUDED.protocol_conf,
    acc_sampling = EXCLUDED.acc_sampling,
    gyro_sensibility = EXCLUDED.gyro_sensibility,
    bme688_sampling = EXCLUDED.bme688_sampling,
    send_interval_s = EXCLUDED.send_interval_s,
    env_interval_s = EXCLUDED.env_interval_s,
    sleep_time_s = EXCLUDED.sleep_time_s,
    sleep_window_size = EXCLUDED.sleep_window_size,
    tcp_port = EXCLUDED.tcp_port,
    udp_port = EXCLUDED.udp_port,
    mqtt_broker = EXCLUDED.mqtt_broker;
