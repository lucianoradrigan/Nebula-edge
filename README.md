# NebulaEdge

Sistema edge con multiples ESP32 que envian telemetria de sensores a una Raspberry Pi central.

## 1) Que hace el sistema

Flujo en runtime:

1. La Raspberry descubre dispositivos por BLE.
2. Busca la configuracion del ESP32 por MAC (`id_device`) en `nebulaedge_schema.config`.
3. Envia configuracion inicial por BLE.
4. El ESP32 entra al protocolo indicado (`0 MQTT`, `1 UDP`, `2 TCP`, `3 BLE`).
5. El ESP32 envia telemetria protobuf (`Data_1`, `Data_2`).
6. La Raspberry persiste en PostgreSQL (`nebulaedge_schema.data_1`, `nebulaedge_schema.data_2`).
7. Si sube `config_version`, ambos cambian de config/protocolo en caliente (con `ConfigAck`).

## 2) Requisitos minimos

Hardware:

1. PC con Windows, Mac o Linux.
2. 1 Raspberry Pi 5.
3. 1 a 6 placas de desarrollo IM-V2 (ESP32-S3) (mas no validado en este repo).
4. Dongle Bluetooth USB en Raspberry (probado con TP-LINK).

Software en PC:

1. ESP-IDF `v5.4.1` limpio.
2. Target del proyecto: `esp32s3`.

Software en Raspberry:

1. Raspberry Pi OS (Linux).
2. Docker + Docker Compose.
3. Bluetooth habilitado (`bluez`, `rfkill`).
4. Cliente PostgreSQL (`psql`).
5. NetworkManager operativo (`nmcli`).

## 3) Setup


### Paso 1: Verificar entorno ESP-IDF en PC

```bash
cd esp32
idf.py --version
git -C "$IDF_PATH" status --porcelain
```

Esperado:

- `idf.py --version` debe mostrar `ESP-IDF v5.4.1`.
- `git status --porcelain` no debe mostrar cambios.

### Paso 2: Compilar y flashear ESP32-S3

```bash
cd esp32
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Nota: ajusta el puerto `/dev/ttyUSB0`, `/dev/ttyACM0` dependiendo del sistema en que operes.

Completando estos pasos la ESP32-S3 quedará con el firmware para conectarse al server y quedará ejecutando, imprimiendo sus logs en la consola (*importante para los pasos siguientes).

### Paso 3: Preparar Bluetooth en Raspberry

Si el Bluetooth esta bloqueado:

```bash
rfkill list
sudo rfkill unblock bluetooth
```

Si necesitas forzar adaptador BLE:

```bash
cd raspberry
BLE_ADAPTER=hci1 sudo docker compose up -d --build
```

### Paso 4: Levantar base de datos y servidor en Raspberry

```bash
cd raspberry
sudo docker compose up --build -d
```

Verifica logs:

```bash
sudo docker logs -f nebulaedge_db
sudo docker logs -f nebulaedge_server
```

### Paso 5: Registrar dispositivos ESP32-S3 en DB usando su MAC Bluetooth

El servidor (Raspberry) posee en su base de datos una lista de dispositivos que se pueden conectar a él, junto con configuraciones de sensores y credenciales de comunicación. El id de los dispositivos está dado por su dirección MAC Bluetooth.

En el paso 2, al imprimirse los logs de la ESP32-S3 en consola, se debe buscar la dirección MAC impresa que tiene el siguiente formato:

- `ID del device detectado: XX:XX:XX:XX:XX:XX`

Luego, editar [raspberry/db_init/03_seed.sql](raspberry/db_init/03_seed.sql) añadiendo la nueva tupla asociada al nuevo dispositivo. Ahí se encuentran los valores iniciales de las configuraciones de dispositivos. Ejemplo:

```sql
('C0:49:EF:08:CE:82', 0, 1, 400, 500, 8, 1, 1, 10, 1827, 1243, 'mqtt://broker.hivemq.com:1883')
```

Para que este cambio sea efectivo se debe hacer rebuild de la componente docker nebulaedge_db con los siguientes comandos:

```bash
cd raspberry
sudo docker compose down -v
sudo docker compose up --build -d
```

### Paso 6: Verificar que llega telemetria

En `psql`:

```sql
SELECT id_device, config_version_applied, temperature, press, time_client
FROM nebulaedge_schema.data_1
ORDER BY time_client DESC
LIMIT 20;
```

### Paso 7: Probar cambio de protocolo en caliente

En `psql`:

```sql
UPDATE nebulaedge_schema.config
SET protocol_conf = 1,
    config_version = config_version + 1,
    udp_port = 1240,
    send_interval_s = 1
WHERE id_device = '58:BF:25:99:B4:92';
```

Esperado:

1. Raspberry detecta nueva version.
2. Envia nueva config.
3. ESP32 responde `ConfigAck`.
4. Ambos migran al nuevo protocolo.

## 4) Base de datos

La base vive en PostgreSQL y usa el schema `nebulaedge_schema`. Ahí se guardan tres tipos de datos: configuracion inicial del dispositivo, telemetria de sensores y logs operativos del servidor.

Tablas principales:

- `nebulaedge_schema.config`: una fila por dispositivo (`id_device`) con la configuracion activa que la Raspberry lee por BLE.
- `nebulaedge_schema.data_1`: telemetria de sensores del paquete `Data_1`.
- `nebulaedge_schema.data_2`: telemetria de sensores del paquete `Data_2`.
- `nebulaedge_schema.log`: eventos de operacion del servidor, como conexion inicial, heartbeat y desconexion.

Qué guarda cada una:

- `config`: `id_device`, `config_version`, `protocol_conf`, `acc_sampling`, `gyro_sensibility`, `bme688_sampling`, `send_interval_s`, `sleep_time_s`, `sleep_window_size`, `tcp_port`, `udp_port`, `host_ip_addr`, `ssid`, `passwd`, `mqtt_broker`.
- `data_1`: `temperature`, `press`, `hum`, `co`, `rms`, ejes y frecuencias del acelerometro y magnetometro, mas `config_version_applied` y `time_client`.
- `data_2`: `acc_x`, `acc_y`, `acc_z`, `gyr_x`, `gyr_y`, `gyr_z`, `config_version_applied` y `time_client`.
- `log`: `status_report`, `protocol_report`, `batt_level`, `time_client`, `time_server`.

Consultas utiles:

```sql
SELECT *
FROM nebulaedge_schema.data_1
ORDER BY time_client DESC
LIMIT 20;
```

```sql
SELECT *
FROM nebulaedge_schema.data_2
ORDER BY time_client DESC
LIMIT 20;
```

```sql
SELECT *
FROM nebulaedge_schema.log
ORDER BY time_server DESC
LIMIT 20;
```

Si quieres modificar los dispositivos que arrancan con datos precargados, edita [raspberry/db_init/03_seed.sql](raspberry/db_init/03_seed.sql).

## 5) Campos clave de configuracion

- `id_device`: MAC Bluetooth del ESP32.
- `config_version`: incrementa en cada cambio.
- `protocol_conf`: `0 MQTT`, `1 UDP`, `2 TCP`, `3 BLE`.
- `send_interval_s`: intervalo de envio en segundos.
- `sleep_time_s`: deep sleep en segundos (`0` = continuo).
- `sleep_window_size`: cantidad de paquetes antes de dormir.
- `tcp_port`, `udp_port`, `mqtt_broker`.

## 6) Observaciones / limitaciones

1. Al tener `sleep_window_size` de tamaño 1, el cambio de protocolo no funciona bien.
2. Es fundamental hacer los cambios de protocolo de a uno, y esperar a que complete para hacer otro, de otra manera podría colapsar el servidor/no funcionar bien el sistema de ACK's.
3. El módulo microSD está en desarrollo para la nueva versión de placa de desarrollo IM-V3.
4. La raspberry enviará las credenciales de la SSID a la que esté actualmente conectada. Se está trabajando en un modo AP.

## 7) Estructura del repositorio

- `esp32/`: firmware ESP-IDF de los nodos.
- `raspberry/`: servidor Python, Docker Compose y PostgreSQL.
- `raspberry/db_init/`: scripts SQL de inicializacion.
- `raspberry/server/`: logica BLE, sesiones MQTT/UDP/TCP/BLE y persistencia.
