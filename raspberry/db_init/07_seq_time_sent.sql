-- Agrega a data_1 y data_2 el número de secuencia y la hora de envío del device.
--
-- seq: un contador por flujo (Data_1 y Data_2) que el device pone al producir
-- cada paquete. Sigue a través del deep sleep y vuelve a 0 en arranque frío.
-- Con él la pérdida se cuenta exacta: un salto es un paquete perdido, un seq
-- repetido es un duplicado y un seq menor que el anterior es un reinicio.
--
-- time_sent: la hora del device justo antes de enviar, en µs. Junto a
-- time_server (llegada al servidor, en µs) da la latencia de un sentido, que
-- incluye el desfase del reloj del device (fijo entre dos ajustes).
--
-- Un firmware que no los manda deja los dos en NULL, igual que las filas
-- anteriores. En una base nueva 02_tables.sql ya crea las columnas y esto no
-- hace nada. En una que ya existe hay que correrlo a mano:
--
--   docker compose exec -T db psql -U nebulaedge -d nebulaedge < db_init/07_seq_time_sent.sql

ALTER TABLE nebulaedge_schema.data_1
    ADD COLUMN IF NOT EXISTS seq INT,
    ADD COLUMN IF NOT EXISTS time_sent TIMESTAMP;
ALTER TABLE nebulaedge_schema.data_2
    ADD COLUMN IF NOT EXISTS seq INT,
    ADD COLUMN IF NOT EXISTS time_sent TIMESTAMP;
