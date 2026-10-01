-- Insertar valores iniciales segun create_tables.py
--
-- send_interval_s = 1 -> un Data_1 y un Data_2 por segundo. Los dos flujos
--    comparten el intervalo; 0 = sin espera.
INSERT INTO nebulaedge_schema.config (
    id_device, config_version, protocol_conf, acc_sampling, gyro_sensibility,
    bme688_sampling, send_interval_s, sleep_time_s, sleep_window_size,
    tcp_port, udp_port, mqtt_broker
) VALUES
('34:85:18:A2:DB:4E', 0, 0, 400, 500, 8, 1, 1, 10, 1819, 1235, ''),
('34:85:18:A2:DB:A2', 0, 2, 400, 500, 8, 1, 1, 10, 1820, 1236, ''),
('34:85:18:A2:DB:BE', 0, 3, 400, 500, 8, 1, 1, 10, 1818, 1234, ''),
('34:85:18:A2:DB:06', 0, 0, 400, 500, 8, 1, 1, 10, 1821, 1237, ''),
('34:85:18:A2:DE:1A', 0, 1, 400, 500, 8, 1, 1, 10, 1822, 1238, ''),
('DC:DA:0C:41:F6:8A', 0, 3, 400, 500, 8, 1, 1, 10, 1823, 1239, ''),
('58:BF:25:99:B4:92', 0, 1, 400, 500, 8, 1, 1, 10, 1824, 1240, ''),
('3C:61:05:65:A6:3E', 0, 1, 400, 500, 8, 1, 1, 10, 1825, 1241, ''),
('C0:49:EF:08:D0:C2', 0, 1, 400, 500, 8, 1, 1, 10, 1826, 1242, ''),
('C0:49:EF:08:CE:82', 0, 1, 400, 500, 8, 1, 1, 10, 1827, 1243, ''),
('24D9157B-EB1C-53B2-03BA-77A74DF652C7', 0, 1, 400, 500, 8, 1, 0, 10, 1830, 1246, '')
ON CONFLICT (id_device)
DO UPDATE SET
    config_version = EXCLUDED.config_version,
    protocol_conf = EXCLUDED.protocol_conf,
    acc_sampling = EXCLUDED.acc_sampling,
    gyro_sensibility = EXCLUDED.gyro_sensibility,
    bme688_sampling = EXCLUDED.bme688_sampling,
    send_interval_s = EXCLUDED.send_interval_s,
    sleep_time_s = EXCLUDED.sleep_time_s,
    sleep_window_size = EXCLUDED.sleep_window_size,
    tcp_port = EXCLUDED.tcp_port,
    udp_port = EXCLUDED.udp_port,
    mqtt_broker = EXCLUDED.mqtt_broker;
