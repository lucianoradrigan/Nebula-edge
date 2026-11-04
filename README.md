# NebulaEdge

Sistema edge con multiples ESP32 que envian telemetria de sensores a una Raspberry Pi central.

## 1) Que hace el sistema

Flujo en runtime:

1. La Raspberry descubre dispositivos por BLE.
2. Busca la configuracion del ESP32 por MAC (`id_device`) en `nebulaedge_schema.config`.
3. Envia configuracion inicial por BLE.
4. El ESP32 entra al protocolo indicado (`0 MQTT`, `1 UDP`, `2 TCP`, `3 BLE`, `4 INFER`).
5. El ESP32 envia telemetria protobuf: `Data_2` (inercial) y `Data_1` (ambiental), al mismo ritmo.
6. La Raspberry persiste en PostgreSQL (`nebulaedge_schema.data_2`, `nebulaedge_schema.data_1`).
7. Si sube `config_version`, ambos cambian de config/protocolo en caliente (con `ConfigAck`).

`4 INFER` es distinto: no manda telemetria. El ESP32 baja por MQTT un modelo de
deteccion de fallas, lo verifica y responde pedidos de inferencia (ver seccion 9).

### 1.1) BLE como canal de control y de rescate

BLE es el canal de control en todos los modos: por ahi entra la configuracion
inicial, y el ESP32 lo deja activo (anuncio y caracteristica A) con cualquier
protocolo. En los modos WiFi (MQTT, UDP, TCP, INFER) los datos van por WiFi y
BLE queda solo para control; en el modo BLE lleva ademas los datos.

Eso da una via de recuperacion independiente del transporte de datos. Mientras
un device tiene sesion abierta, el servidor ignora sus anuncios BLE. Cuando la
sesion termina (timeout sin datos, enlace perdido, reinicio del servidor), el
escaneo lo vuelve a encontrar, le escribe la config por BLE y abre una sesion
nueva. Es el unico camino para recuperar un device que dejo de escuchar por
WiFi, y en el modo BLE no hay un canal aparte: se rehace la misma conexion.

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
2. Docker + Docker Compose (levanta tres servicios: servidor, PostgreSQL y el broker MQTT Mosquitto).
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

El servidor elige solo el adaptador: prefiere el dongle USB y, si no hay,
usa el de la placa (con un aviso en el log). Elige por tipo de bus y no por
nombre porque el dongle puede quedar como `hci0` o `hci1` segun el orden en que
arranquen. La eleccion se hace una vez, al arrancar el servidor: si el dongle
se conecta o reconecta despues, hay que reiniciar el servidor.

Si necesitas forzar un adaptador:

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
sudo docker logs -f nebulaedge_mqtt
```

El broker MQTT corre en su propio contenedor (`nebulaedge_mqtt`, puerto 1883) y
sobrevive a los reinicios del servidor; el porque esta en
[raspberry/mosquitto/mosquitto.conf](raspberry/mosquitto/mosquitto.conf). El
servidor lo encuentra por `MQTT_HOST` / `MQTT_PORT` (por defecto `127.0.0.1:1883`).

### Paso 5: Registrar dispositivos ESP32-S3 en DB usando su MAC Bluetooth

El servidor (Raspberry) posee en su base de datos una lista de dispositivos que se pueden conectar a él, junto con configuraciones de sensores y credenciales de comunicación. El id de los dispositivos está dado por su dirección MAC Bluetooth.

En el paso 2, al imprimirse los logs de la ESP32-S3 en consola, se debe buscar la dirección MAC impresa que tiene el siguiente formato:

- `ID del device detectado: XX:XX:XX:XX:XX:XX`

Luego, editar [raspberry/db_init/03_seed.sql](raspberry/db_init/03_seed.sql) añadiendo la nueva tupla asociada al nuevo dispositivo. Ahí se encuentran los valores iniciales de las configuraciones de dispositivos. Ejemplo:

```sql
('C0:49:EF:08:CE:82', 0, 1, 400, 500, 8, 1, 1, 10, 1827, 1243, '')
```

Para que este cambio sea efectivo se debe hacer rebuild de la componente docker nebulaedge_db con los siguientes comandos:

```bash
cd raspberry
sudo docker compose down -v
sudo docker compose up --build -d
```

Ojo: `down -v` borra el volumen de la base, con toda la telemetria y los
resultados guardados. En una base con datos, es mejor agregar el device con un
`INSERT` directo en `nebulaedge_schema.config` desde `psql`.

### Paso 6: Verificar que llega telemetria

En `psql`:

```sql
SELECT id_device, config_version_applied, temperature, press, time_server, time_sent
FROM nebulaedge_schema.data_1
ORDER BY time_server DESC
LIMIT 20;
```

Ojo: `id_device` en `data_1` / `data_2` es el que el device escribe en cada
paquete, su MAC. Si el servidor corre en macOS, la fila de `config` en cambio se
busca por el identificador que CoreBluetooth le asigna al device (un UUID, no la
MAC): para consultar la telemetria de un device hay que filtrar por su MAC.

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

La base vive en PostgreSQL y usa el schema `nebulaedge_schema`. Ahí se guardan configuracion de dispositivos, telemetria de sensores, logs operativos del servidor y, para el modo INFER, modelos y resultados de inferencia.

Tablas principales:

- `nebulaedge_schema.config`: una fila por dispositivo (`id_device`) con la configuracion activa que la Raspberry lee por BLE.
- `nebulaedge_schema.data_1`: telemetria del BME688 (paquete `Data_1`), al ritmo de `send_interval_s`.
- `nebulaedge_schema.data_2`: BMI270 + BMM350 (paquete `Data_2`), al ritmo de `send_interval_s`.
- `nebulaedge_schema.log`: eventos de operacion del servidor, como conexion inicial, heartbeat y desconexion.
- `nebulaedge_schema.model`: catalogo de modelos para INFER, uno por version (nunca se pisa una version).
- `nebulaedge_schema.infer_sample`: el set de prueba que el servidor manda al device en INFER.
- `nebulaedge_schema.inference_result`: un resultado por ventana inferida, junto a la verdad cuando se conoce.

Qué guarda cada una:

- `config`: `id_device`, `config_version`, `protocol_conf`, `acc_sampling`, `gyro_sensibility`, `bme688_sampling`, `send_interval_s`, `sleep_time_s`, `sleep_window_size`, `tcp_port`, `udp_port`, `mqtt_broker`, `model_version`, `infer_source`. La red que recibe el device (SSID, contraseña e IP del servidor) no se guarda: la arma el servidor en cada config (ver seccion 10).
- `data_1`: `temperature`, `press`, `hum`, `co` (hoy guarda la resistencia de gas cruda, no CO), mas `config_version_applied`, `time_client`, `time_server`, `seq` y `time_sent`.
- `data_2`: `acc_x/y/z`, `gyr_x/y/z`, `mag_x/y/z`, `config_version_applied`, `time_client`, `time_server`, `seq` y `time_sent`.
- En `data_1` y `data_2`:
  - `time_sent`: hora del device justo antes de enviar, en µs;
  - `time_server`: hora en que el servidor recibio el paquete, en µs;
  - `seq`: numero de secuencia del flujo, puesto por el device al producir el paquete;
  - `time_client`: hora del device al leer el sensor, en segundos enteros. Solo la tienen las filas guardadas antes de `seq`/`time_sent`: el servidor actual ya no lee ese campo del protobuf. Un device con firmware anterior sigue mandando datos, pero sus filas quedan sin hora del device (`time_client`, `seq` y `time_sent` en NULL) y su sesion no escribe heartbeat en `log`. Hay que reflashear todos los devices.
- `log`: `status_report`, `protocol_report`, `batt_level`, `time_client`, `time_server`.

Consultas utiles:

```sql
SELECT *
FROM nebulaedge_schema.data_1
ORDER BY time_server DESC
LIMIT 20;
```

```sql
SELECT *
FROM nebulaedge_schema.data_2
ORDER BY time_server DESC
LIMIT 20;
```

```sql
SELECT *
FROM nebulaedge_schema.log
ORDER BY time_server DESC
LIMIT 20;
```

Latencia de un sentido (incluye el desfase del reloj del device, que es fijo
entre dos ajustes de hora) y perdida por flujo:

```sql
-- Latencia en ms de los ultimos 20 paquetes de un device
SELECT seq, extract(epoch FROM time_server - time_sent) * 1000 AS latencia_ms
FROM nebulaedge_schema.data_1
WHERE id_device = '34:85:18:A2:DE:1A' AND time_sent IS NOT NULL
ORDER BY time_server DESC
LIMIT 20;

-- Perdidos, duplicados y reinicios en la ultima hora, por flujo
WITH d AS (
    SELECT seq - lag(seq) OVER (ORDER BY time_server) AS paso
    FROM nebulaedge_schema.data_1
    WHERE id_device = '34:85:18:A2:DE:1A' AND seq IS NOT NULL
      AND time_server > now() AT TIME ZONE 'UTC' - interval '1 hour'
)
SELECT count(*)                                  AS recibidos,
       sum(paso - 1) FILTER (WHERE paso > 1)     AS perdidos,
       count(*) FILTER (WHERE paso = 0)          AS duplicados,
       count(*) FILTER (WHERE paso < 0)          AS reinicios
FROM d;
```

Si quieres modificar los dispositivos que arrancan con datos precargados, edita [raspberry/db_init/03_seed.sql](raspberry/db_init/03_seed.sql).

Los scripts de `db_init/` los corre Postgres solo una vez, al crear el volumen.
En una base que ya existe, las tablas y columnas de INFER se agregan corriendo a
mano [raspberry/db_init/04_infer.sql](raspberry/db_init/04_infer.sql), que se
puede repetir sin romper nada:

```bash
cd raspberry
docker compose exec -T db psql -U nebulaedge -d nebulaedge < db_init/04_infer.sql
```

Lo mismo con [raspberry/db_init/05_drop_wifi_columns.sql](raspberry/db_init/05_drop_wifi_columns.sql),
que saca de `config` las columnas `host_ip_addr`, `ssid` y `passwd` de las
bases creadas antes de que se dejaran de usar, y con
[raspberry/db_init/06_data_time_server.sql](raspberry/db_init/06_data_time_server.sql),
que agrega `time_server` a `data_1` y `data_2` (las filas anteriores quedan en
NULL), y con [raspberry/db_init/07_seq_time_sent.sql](raspberry/db_init/07_seq_time_sent.sql),
que agrega `seq` y `time_sent`.

## 5) Campos clave de configuracion

- `id_device`: MAC Bluetooth del ESP32.
- `config_version`: incrementa en cada cambio.
- `protocol_conf`: `0 MQTT`, `1 UDP`, `2 TCP`, `3 BLE`, `4 INFER`.
- `send_interval_s`: segundos entre paquetes, para `Data_1` y `Data_2`. `0` = sin espera, tan rapido como dejen el bus y el transporte.
- `sleep_time_s`: deep sleep en segundos (`0` = continuo).
- `sleep_window_size`: cantidad de paquetes antes de dormir.
- `tcp_port`, `udp_port`, `mqtt_broker` (vacio = el broker del servidor).
- `model_version`: solo en INFER, el modelo de la tabla `model` que el device debe tener activo.
- `infer_source`: solo en INFER, que se infiere: `0` features del set de prueba, `1` ventanas crudas del set de prueba (el device calcula las features), `2` sensores propios (en el contrato, todavia sin implementar).

## 6) Observaciones / limitaciones

1. Al tener `sleep_window_size` de tamaño 1, el cambio de protocolo no funciona bien.
2. Es fundamental hacer los cambios de protocolo de a uno, y esperar a que complete para hacer otro, de otra manera podría colapsar el servidor/no funcionar bien el sistema de ACK's.
3. El módulo microSD está en desarrollo para la nueva versión de placa de desarrollo IM-V3.
4. El Bluetooth de la placa de la Raspberry es poco estable; el banco usa un dongle USB. Con el modo AP en el WiFi de la placa, el Bluetooth de la placa deja de conectar (comparten chip y antena), así que el modo AP va en un dongle WiFi USB, con el BLE en el dongle Bluetooth (ver seccion 10.1).
5. El adaptador BLE se elige al arrancar el servidor: un dongle conectado o reconectado después no se usa hasta reiniciarlo.
6. Si el AP se levanta en el WiFi de la placa porque no encontró una red conocida, no vuelve sola al router cuando reaparece. Con el AP en un dongle propio (seccion 10.1) no aplica.
7. Cambiar la Raspberry de red (router <-> AP) exige subir `config_version` de cada device (seccion 10).

## 7) Estructura del repositorio

- `esp32/`: firmware ESP-IDF de los nodos (ver [esp32/README.md](esp32/README.md)).
- `raspberry/`: servidor Python, Docker Compose y PostgreSQL.
- `raspberry/db_init/`: scripts SQL de inicializacion.
- `raspberry/mosquitto/`: configuracion del broker MQTT.
- `raspberry/server/`: el servidor propiamente tal (detalle abajo).
- `ml/`: entrenamiento y exportacion de los modelos de INFER (ver [ml/README.md](ml/README.md)).

### 7.1) Modulos del servidor

Cada archivo tiene una sola responsabilidad y arranca con un docstring que
explica para que sirve. En el orden en que se encadenan durante una sesion:

| Archivo | Para que sirve |
|---|---|
| `server.py` | Punto de entrada. Es lo que ejecuta el contenedor. |
| `discovery.py` | Escanea BLE, entrega la config inicial y abre una sesion por dispositivo. |
| `protocol_dispatch.py` | Elige que sesion corre segun `protocol_conf`, y la vuelve a elegir si el protocolo cambia. |
| `sessions.py` | El bucle de sesion: recibir, persistir, comparar version de config, empujar y esperar ACK. Identico para los 4 protocolos de telemetria. |
| `infer_session.py` | `InferDeviceSession`: reemplaza ese bucle en INFER. Transfiere el modelo y recorre el set de prueba. |
| `transport.py` | Lo unico que cambia entre protocolos: como se mueven los bytes (`UdpTransport`, `TcpTransport`, `MqttTransport`, `BleTransport`, `InferTransport`). |
| `packet_router.py` | Clasifica cada paquete entrante (telemetria / deep sleep / descartar) y persiste la telemetria. |
| `config_resolver.py` | Decide si hay que empujar una config nueva. Funcion pura, sin I/O. |
| `repository.py` | Unico modulo que habla con Postgres. |

De apoyo: `codec.py` (protobuf), `models.py` (dataclasses del dominio),
`system.py` (reloj UTC, adaptador BLE, WiFi del host), `mqtt_client.py` (cliente
MQTT compartido del proceso; el broker es el contenedor `mqtt`), `gatt_uuids.py`
(UUIDs del servicio GATT).

### 7.2) Agregar un protocolo nuevo

El diseno esta pensado para que no haya que desarmar nada. Para dar de alta
CoAP, LoRa o el que sea:

1. Escribir una clase nueva en `transport.py` que herede de `Transport`
   (abrir, recibir, enviar, cerrar).
2. Agregar la subclase de sesion en `sessions.py`:
   `class CoAPDeviceSession(ProtocolSession): transport_cls = CoapTransport`
3. Agregar una linea al dict `session_classes` de `protocol_dispatch.py`.

El bucle de sesion, el manejo de ACK, los timeouts y la persistencia se heredan
sin tocar nada.

## 8) Tests del servidor

La suite levanta sesiones reales contra sockets y colas locales; no necesita
Postgres, ni broker MQTT, ni hardware. Postgres se reemplaza por un repositorio
en memoria y BLE por un doble de `bleak`.

```bash
cd raspberry
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
cd server
../.venv/bin/python -m unittest discover -s tests
```

Esperado: `Ran 95 tests ... OK`.

Cobertura actual: las cuatro sesiones de protocolo (telemetria, cambio de
config con ACK, deep sleep, cierre por timeout, resincronizacion del reloj del
device, enlace BLE perdido, ACKs viejos en las colas MQTT y base de datos con
una version mas vieja que la del device), la sesion INFER (transferencia del
modelo, corrida del set de prueba, reenvio de un pedido sin respuesta,
resultados sin pedido), el cierre de sesion en el descubrimiento aunque falle
la base, el reporte de fallos de escritura en la base, y el contrato con el
firmware (incluido que las dos copias de `schema.proto` sean iguales). Lo que **no** cubre: el SQL
real contra Postgres, y que `bleak`/BlueZ se comporten como el doble. BLE es el
unico de los cuatro que no se puede validar de verdad sin un ESP32 enfrente.

## 9) Modo INFER (deteccion de fallas en el device)

Con `protocol_conf = 4` el ESP32 no manda telemetria: corre un modelo de
TensorFlow Lite Micro que clasifica fallas de rodamiento (sano, pista interior,
pista exterior, elemento rodante) y estima su severidad, a partir de ventanas
de vibracion de 2048 muestras.

1. **Modelo.** Se entrena y exporta en `ml/` (ver [ml/README.md](ml/README.md)).
   `export_model.py` arma un `INSERT` para la tabla `model`, y `export_testset.py`
   el set de prueba para `infer_sample`. Se cargan con `psql`.
2. **Config.** En `config`: `protocol_conf = 4`, `model_version = <la version>`,
   `infer_source = 0` (features) o `1` (ventanas crudas), y se sube
   `config_version`.
3. **Transferencia.** El servidor publica un manifiesto retenido por MQTT y el
   device pide el modelo en trozos de 2 KB. El avance queda en NVS, asi que se
   retoma tras un reinicio. Hay dos ranuras de 512 KB (`model_a`, `model_b`):
   el modelo nuevo se escribe en la que no esta activa.
4. **Verificacion.** sha256, carga en TFLM y un autotest con una fila conocida.
   Si algo falla, el device rechaza el modelo y sigue con el anterior.
5. **Inferencia.** El servidor recorre el set de prueba, una ventana a la vez,
   y guarda cada respuesta junto a la verdad en `inference_result`. Al terminar
   loguea accuracy, MAE de severidad y tiempos medidos en el device.

Resultado de referencia (modelo v3, set CWRU de 3 hp, 769 ventanas): 99,09% de
accuracy y MAE de severidad 0,0721, igual que en el computador. Inferencia de
~5 ms por ventana en la IM-V2; con ventana cruda, el calculo de features en el
device suma ~190 ms.

El contrato completo (mensajes, topicos y tipos de paquete 0x05-0x07) esta en
`schema.proto`.

## 10) Red WiFi de los devices y modo AP

La red que recibe el device (SSID, contraseña e IP del servidor) la arma el
servidor en cada config (`system.py` -> `LocalWifiConfig`), no la tabla
`config`:

- Si `WIFI_SSID` / `WIFI_PASSWD` estan definidas en `raspberry/.env`, se usan
  esas. Es el caso de la Raspberry conectada por cable a un router.
- Si estan vacias, se lee con `nmcli` la conexion activa de `WIFI_DEVICE`
  (`wlan0` por defecto): la WiFi a la que esta conectada la Raspberry, o el AP
  que levanto ella misma.

El device solo cambia de red con una config de `config_version` mayor: una
config con la misma version se descarta como ya aplicada, aunque traiga otro
SSID. Al pasar la Raspberry del router al AP (o al reves) hay que subir
`config_version` de cada device; reiniciar el servidor no alcanza. Si el device
no logra conectarse a la red nueva, se reinicia y la recibe por BLE
(seccion 1.1).

### 10.1) Modo AP en un dongle WiFi USB

La Raspberry publica su propia red desde un adaptador WiFi USB, y el BLE va por
el dongle Bluetooth USB. El WiFi de la placa queda sin usar: con el AP en el
WiFi de la placa, el Bluetooth de la placa no logra conectar (comparten chip y
antena, y el AP le deja muy poco tiempo de radio). Medido en banco: con el AP
arriba en `wlan0`, todos los handshakes BLE por `hci0` fallaron, y el primero
entro apenas se bajo. Como BLE es el canal de rescate, el modo AP necesita los
dos dongles.

Probado en Raspberry Pi OS Bookworm (NetworkManager 1.42) con un TP-Link
TL-WN823N (RTL8192EU, driver `rtl8xxxu`) y un dongle Bluetooth TP-Link.

1. Nombre fijo para el dongle WiFi. Sin esto puede quedar como `wlan1` o
   `wlan0` segun el orden de arranque. En
   `/etc/systemd/network/10-nebulaedge-wlan-ap.link` (con la MAC del dongle,
   `ip link`):

   ```ini
   [Match]
   MACAddress=<mac del dongle>

   [Link]
   Name=wlan_ap
   ```

   Se aplica al reconectar el dongle o al reiniciar.

2. Sacar el WiFi de la placa de NetworkManager, en
   `/etc/NetworkManager/conf.d/90-nebulaedge-wlan0-unmanaged.conf`:

   ```ini
   [keyfile]
   unmanaged-devices=interface-name:wlan0
   ```

   y `sudo systemctl reload NetworkManager`. **No usar `rfkill` para esto**:
   NetworkManager toma cualquier radio WiFi bloqueada como "WiFi apagado" y baja
   tambien el dongle; y `nmcli radio wifi on` las desbloquea todas.

3. El perfil del AP, amarrado al dongle:

   ```bash
   sudo nmcli con add type wifi ifname wlan_ap con-name nebulaedge_ap autoconnect yes \
     ssid nebulaedge_ap 802-11-wireless.mode ap 802-11-wireless.band bg \
     802-11-wireless.channel 6 wifi-sec.key-mgmt wpa-psk wifi-sec.proto rsn \
     wifi-sec.pairwise ccmp wifi-sec.group ccmp wifi-sec.psk '<clave>' \
     ipv4.method shared ipv6.method ignore connection.autoconnect-priority 10
   sudo nmcli con up nebulaedge_ap
   ```

   - `band bg`: el ESP32-S3 solo usa 2,4 GHz.
   - `ipv4.method shared`: la Raspberry queda en `10.42.0.1` y entrega IPs por
     DHCP.
   - Como el dongle solo sirve para el AP, el perfil se levanta siempre al
     arrancar; la Raspberry sigue saliendo a internet por cable.

4. En `raspberry/.env`: `WIFI_DEVICE=wlan_ap`, y `WIFI_SSID` / `WIFI_PASSWD`
   vacios. Reiniciar el servidor (`docker compose up -d server`) y subir
   `config_version` de cada device. El broker y los puertos UDP/TCP ya escuchan
   en todas las interfaces.

5. Si el servidor registra `start scan falló: NotReady`, el dongle Bluetooth
   quedo bloqueado: `rfkill list`, `sudo rfkill unblock <id>`,
   `sudo hciconfig hci1 up` y reiniciar el servidor.

Probado en banco sobre el AP: TCP, MQTT y UDP (el device se une al AP en unos
14 s), reinicio de la placa (vuelve por BLE en unos 60 s, casi todo la espera
del cierre de la sesion anterior) e INFER con ventanas crudas (769/769
respondidas, 99,09 % de accuracy, 354 ms de ida y vuelta, igual que con el
router).

Para volver al router: restaurar `WIFI_SSID` / `WIFI_PASSWD` en `.env`,
reiniciar el servidor y subir `config_version`.
