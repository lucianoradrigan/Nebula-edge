# NebulaEdge

Sistema edge con múltiples ESP32 que envían telemetría de sensores a una Raspberry Pi central.

La Raspberry:

- Entrega configuración inicial vía BLE a cada ESP32.
- Recibe telemetría por el protocolo configurado por dispositivo (MQTT, UDP, TCP o BLE).
- Guarda los datos en PostgreSQL.
- Detecta cambios de configuración en base de datos y los aplica en caliente sin apagar el sistema.

## 1) Arquitectura funcional

Flujo esperado en runtime:

1. El servidor en Raspberry escanea dispositivos BLE objetivo.
2. Al detectar un ESP32, busca su configuración en la tabla `config` (por `id_device`, MAC).
3. Envía esa configuración por BLE.
4. El ESP32 aplica la config y entra al protocolo seleccionado:
	- `0 = MQTT`
	- `1 = UDP`
	- `2 = TCP`
	- `3 = BLE`
5. El ESP32 envía telemetría (`Data_1`) usando protobuf.
6. La Raspberry inserta los datos en `data_1`.
7. Si cambia `config_version` en DB, la Raspberry reconfigura al ESP32 y ambos cambian de protocolo sin reinicio.

## 2) Estructura del repositorio

- `esp32/`: firmware ESP-IDF para los nodos ESP32.
- `raspberry/`: servidor Python + Docker Compose + PostgreSQL.
- `raspberry/db_init/`: scripts SQL de inicialización (`config`, `log`, `data_1`).
- `raspberry/server/`: lógica de escaneo BLE, sesiones MQTT/UDP/TCP/BLE y persistencia en DB.

## 3) Guía por rol

Si estás trabajando en el firmware del dispositivo:

- Revisar `esp32/README.md`.

Si estás trabajando en el servidor central:

- Revisar `raspberry/README.md`.

Este README raíz mantiene la visión completa e integración entre ambos.

## 4) Requisitos

Hardware mínimo:

1. Raspberry Pi 5.
2. De 1 a 8 placas ESP32 (no se ha testeado con mayor cantidad).
3. Dongle USB Bluetooth para la Raspberry (obligatorio para ejecutar el servidor, probado con TP-LINK).

Software en Raspberry:

1. Raspberry Pi OS.
2. Docker + Docker Compose.
3. Linux con acceso a Bluetooth (`bluez`, `rfkill`).
4. Cliente PostgreSQL (`psql`) para cambios de configuración en caliente.
4. (Opcional desarrollo local) Python 3.11.2.

Software para firmware ESP32:

1. ESP-IDF instalado (toolchain + `idf.py`).
2. Target usado en el proyecto: `esp32s3`.

## 5) Puesta en marcha rápida (Raspberry + DB + servidor)

Desde la raíz del repo:

```bash
cd raspberry
sudo docker compose build
sudo docker compose up -d
```

Ver logs del servidor:

```bash
sudo docker logs -f nebulaedge_server
```

Ver logs de base de datos:

```bash
sudo docker logs -f nebulaedge_db
```

Si quieres reinicializar completamente el server + base de datos (incluye seed):

```bash
cd raspberry
sudo docker compose down -v
sudo docker compose up --build -d
```

## 6) Firmware ESP32 (compilar y flashear)

Desde la raíz del repo:

```bash
cd esp32
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Notas:

- Ajusta el puerto serial (`/dev/ttyUSB0`, `/dev/ttyACM0`, etc.).
- El flujo implementado inicia con BLE para recibir config inicial.

## 7) Bluetooth en Raspberry (problema común)

Al primer uso del dongle puede venir bloqueado (`Soft blocked: yes`):

```bash
rfkill list
sudo rfkill unblock bluetooth
```

El servidor intenta usar un adaptador USB automáticamente (`BLE_ADAPTER=auto`).
Si necesitas forzar uno específico:

```bash
cd raspberry
BLE_ADAPTER=hci1 sudo docker compose up -d --build
```

## 8) Modelo de configuración por dispositivo

Cada ESP32 se identifica por MAC (`id_device`) en la tabla `config`.

Campos clave:

- `config_version`: debe incrementarse en cada cambio para gatillar reconfiguración.
- `protocol_conf`: protocolo activo (`0 MQTT`, `1 UDP`, `2 TCP`, `3 BLE`).
- `send_interval_ms`: intervalo de envío de datos.
- `discontinuous_sleep_time`: si `> 0`, habilita modo discontinuo con deep sleep.
- `discontinuous_window_size`: cantidad de paquetes enviados antes de dormir.
- `tcp_port`, `udp_port`, `mqtt_broker`.

## 9) Cambiar protocolo en caliente (sin apagar)

Instalar `psql` en Raspberry (si no está instalado):

```bash
sudo apt update
sudo apt install -y postgresql-client
```

Entrar a PostgreSQL en el contenedor:

```bash
sudo docker exec -it nebulaedge_db psql -U nebulaedge -d nebulaedge
```

Alternativa usando `psql` local contra el puerto publicado por Docker:

```bash
psql "postgresql://nebulaedge:1234@localhost:5432/nebulaedge"
```

Ejemplo: cambiar un ESP32 de TCP (`2`) a UDP (`1`) y actualizar versión:

```sql
UPDATE config
SET protocol_conf = 1,
	 config_version = config_version + 1,
	 udp_port = 1240,
	 send_interval_ms = 1000
WHERE id_device = '58:BF:25:99:B4:92';
```

Resultado esperado:

1. Raspberry detecta versión nueva.
2. Envía nueva config al ESP32 por el canal activo.
3. ESP32 responde ACK (`ConfigAck`).
4. Ambos migran al nuevo protocolo.

## 10) Modo continuo vs discontinuo (deep sleep)

Continuo:

- `discontinuous_sleep_time = 0`

Discontinuo:

- `discontinuous_sleep_time > 0`
- `discontinuous_window_size >= 1`

Ejemplo: enviar cada 1 segundo, 5 paquetes, dormir 60 segundos:

```sql
UPDATE config
SET send_interval_ms = 1000,
	 discontinuous_window_size = 5,
	 discontinuous_sleep_time = 60000,
	 config_version = config_version + 1
WHERE id_device = '58:BF:25:99:B4:92';
```

## 11) Verificar que todo está funcionando

Checklist mínimo:

1. `nebulaedge_db` y `nebulaedge_server` en estado `Up`.
2. El servidor muestra descubrimiento BLE y conexión de cada ESP32.
3. Existen filas en `data_1`.
4. Al cambiar `config_version`, se observa reconfiguración y ACK.

Consulta rápida:

```sql
SELECT id_device, config_version_applied, temperature, press
FROM data_1
ORDER BY id_device
LIMIT 20;
```

## 12) Desarrollo local sin Docker (opcional)

```bash
cd raspberry
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
cd server
python3 classes.py
```

Necesitas una instancia PostgreSQL accesible en `localhost` con:

- DB: `nebulaedge`
- user: `nebulaedge`
- password: `1234`

## 13) Problemas frecuentes

1. No aparece el ESP32 en BLE:
	- Revisa `rfkill list` y desbloquea Bluetooth.
	- Revisa que el dongle USB esté detectado (`hci1`/`hci0`).
2. No aplica cambios de config:
	- Verifica que incrementaste `config_version`.
3. No llegan datos por TCP/UDP/MQTT:
	- Revisa SSID/password entregados en config.
	- Verifica puertos y broker configurados.
4. La DB no se resetea con `up --build`:
	- Usa `docker compose down -v` antes de levantar.

## 14) Estado actual

El repositorio implementa:

- Configuración inicial vía BLE.
- Telemetría protobuf por MQTT/UDP/TCP/BLE.
- Persistencia en PostgreSQL.
- Cambio de protocolo/configuración en caliente basado en versión.
- Modo discontinuo con deep sleep.
