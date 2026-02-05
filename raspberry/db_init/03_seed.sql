-- Insertar valores iniciales según create_tables.py
INSERT INTO config (
    id_device, config_version, protocol_conf, acc_sampling, gyro_sensibility,
    bme688_sampling, send_interval_ms, discontinuous_sleep_time, discontinuous_window_size,
    tcp_port, udp_port, mqtt_broker
) VALUES
('34:85:18:A2:DB:4E', 1, 1, 100, 250, 60, 500, 0, 1, 1819, 1235, 'mqtt://broker.hivemq.com:1883'),
('34:85:18:A2:DB:A2', 1, 1, 100, 250, 60, 500, 0, 1, 1820, 1236, 'mqtt://broker.hivemq.com:1883'),
('34:85:18:A2:DB:BE', 1, 1, 100, 250, 60, 0, 0, 1, 1818, 1234, 'mqtt://broker.hivemq.com:1883'),
('34:85:18:A2:DB:06', 1, 1, 100, 250, 60, 0, 0, 1, 1821, 1237, 'mqtt://broker.hivemq.com:1883'),
('34:85:18:A2:DE:1A', 1, 1, 100, 250, 60, 0, 0, 1, 1822, 1238, 'mqtt://broker.hivemq.com:1883'),
('DC:DA:0C:41:F6:8A', 1, 1, 100, 250, 60, 0, 0, 1, 1823, 1239, 'mqtt://broker.hivemq.com:1883'),
('58:BF:25:99:B4:92', 1, 1, 100, 250, 60, 0, 0, 1, 1824, 1240, 'mqtt://broker.hivemq.com:1883'),
('3C:61:05:65:A6:3E', 1, 1, 100, 250, 60, 0, 0, 1, 1825, 1241, 'mqtt://broker.hivemq.com:1883'),
('C0:49:EF:08:D0:C2', 1, 1, 100, 250, 60, 0, 0, 1, 1826, 1242, 'mqtt://broker.hivemq.com:1883'),
('C0:49:EF:08:CE:82', 1, 1, 100, 250, 60, 0, 0, 1, 1827, 1243, 'mqtt://broker.hivemq.com:1883')
ON CONFLICT (id_device)
DO UPDATE SET
    config_version = EXCLUDED.config_version,
    protocol_conf = EXCLUDED.protocol_conf,
    tcp_port = EXCLUDED.tcp_port,
    udp_port = EXCLUDED.udp_port,
    mqtt_broker = EXCLUDED.mqtt_broker,
    send_interval_ms = EXCLUDED.send_interval_ms,
    discontinuous_window_size = EXCLUDED.discontinuous_window_size;