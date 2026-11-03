-- Agrega a data_1 y data_2 la hora en que el servidor recibió cada paquete.
--
-- Hasta acá la telemetría solo guardaba time_client, la hora del device, así
-- que no había cómo saber cuándo llegó cada paquete. time_server la marca
-- PacketRouter.route() al recibirlo, con fracción de segundo. Las filas
-- anteriores quedan con time_server en NULL.
--
-- En una base nueva 02_tables.sql ya crea las columnas y esto no hace nada. En
-- una que ya existe hay que correrlo a mano:
--
--   docker compose exec -T db psql -U nebulaedge -d nebulaedge < db_init/06_data_time_server.sql

ALTER TABLE nebulaedge_schema.data_1 ADD COLUMN IF NOT EXISTS time_server TIMESTAMP;
ALTER TABLE nebulaedge_schema.data_2 ADD COLUMN IF NOT EXISTS time_server TIMESTAMP;
