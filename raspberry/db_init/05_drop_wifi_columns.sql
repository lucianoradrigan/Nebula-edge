-- Saca de config las columnas de la red WiFi del device.
--
-- host_ip_addr, ssid y passwd nunca se leían: la red que recibe el device la
-- arma el servidor en cada config (system.py -> LocalWifiConfig), así que lo
-- escrito ahí no llegaba a ningún lado. En una base nueva 02_tables.sql ya no
-- las crea y esto no hace nada. En una que ya existe hay que correrlo a mano:
--
--   docker compose exec -T db psql -U nebulaedge -d nebulaedge < db_init/05_drop_wifi_columns.sql

ALTER TABLE nebulaedge_schema.config
    DROP COLUMN IF EXISTS host_ip_addr,
    DROP COLUMN IF EXISTS ssid,
    DROP COLUMN IF EXISTS passwd;
