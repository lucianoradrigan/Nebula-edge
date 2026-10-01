# ESP32 (Dispositivo)

Este documento describe cómo compilar, flashear y validar el firmware de los nodos ESP32 dentro de NebulaEdge.

## 1) Qué hace el firmware

Cada ESP32:

1. Espera configuración inicial por BLE.
2. Aplica esa configuración y cambia al protocolo indicado.
3. Envía telemetría de sensores en formato protobuf.
4. Detecta cambios de configuración en caliente mediante `config_version`.
5. Puede operar en modo continuo o discontinuo con deep sleep.

Mapeo de protocolos (`protocol_conf`):

- `0`: MQTT
- `1`: UDP
- `2`: TCP
- `3`: BLE

## 2) Sensores y datos recolectados

El firmware considera los siguientes sensores en la placa:

1. **BME688 (ambiental)**
- Temperatura
- Presión
- Humedad
- Gas (resistencia de gas / CO equivalente según conversión del firmware)

2. **BMI270 (IMU)**
- Aceleración en ejes X, Y, Z
- Giroscopio en ejes X, Y, Z
- Magnitudes derivadas (RMS/amplitud/frecuencia, según procesamiento)

3. **BMM350 (magnetómetro)**
- Campo magnético en ejes X, Y, Z

En resumen, el sistema recolecta variables ambientales, inerciales y magnéticas, y las envía a la Raspberry para almacenamiento y procesamiento.

Nota: el formato exacto de los mensajes puede cambiar entre versiones del firmware.

## 3) Requisitos

1. ESP-IDF instalado y funcionando (`idf.py`).
2. Puerto serial disponible para flasheo (`/dev/ttyUSB0`, `/dev/ttyACM0`, etc.).
3. Raspberry y servidor NebulaEdge activos para entregar configuración BLE.

## 4) Compilar y flashear

Desde la raíz del repositorio:

```bash
cd esp32
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Notas:

- Ajusta el puerto según tu entorno.
- El target usado por este proyecto es `esp32s3`.

## 5) Flujo de arranque esperado

1. El dispositivo inicia BLE.
2. Espera la configuración inicial desde Raspberry.
3. Al recibir configuración válida, ejecuta el protocolo seleccionado.
4. Si la configuración cambia en DB (con nueva versión), el dispositivo aplica cambios sin reiniciar manualmente.

## 6) Modo continuo y deep sleep

Campos de configuración relevantes:

- `send_interval_s`: intervalo entre envíos en segundos.
- `sleep_time_s`: tiempo de deep sleep en segundos.
- `sleep_window_size`: cantidad de paquetes antes de dormir.

Comportamiento:

- Si `sleep_time_s = 0`, el envío es continuo.
- Si `sleep_time_s > 0`, el envío es discontinuo y luego entra a deep sleep.

## 7) Validación rápida

1. Levantar server y DB en Raspberry.
2. Flashear ESP32 y abrir monitor serial.
3. Confirmar que el dispositivo recibe configuración BLE.
4. Confirmar que empieza a transmitir por el protocolo configurado.
5. Cambiar `protocol_conf` y aumentar `config_version` en DB.
6. Verificar en monitor que se aplica el cambio en caliente.

## 8) Troubleshooting

1. El ESP32 no recibe configuración BLE:
- Revisar que el servidor en Raspberry esté corriendo.
- Revisar Bluetooth desbloqueado y dongle detectado en Raspberry.
2. No migra de protocolo:
- Confirmar que se incrementó `config_version` en DB.
3. No conecta por WiFi en TCP/UDP/MQTT:
- Verificar `ssid`, `passwd` y `host_ip_addr` entregados por la Raspberry.

## 9) Estructura del firmware

El proyecto sigue la organización estándar de ESP-IDF: `main/` orquesta y cada
pieza reutilizable vive en `components/`.

`main/main.c` es el orquestador: crea las colas de FreeRTOS, decide si la
configuración viene de NVS (al despertar de deep sleep) o de BLE (en arranque
en frío), levanta las tasks de sensores y la del protocolo activo, y maneja el
ciclo de deep sleep.

Componentes, por rol:

| Rol | Componentes |
|---|---|
| Drivers de sensor | `bme688` (ambiental), `bmi270` (IMU), `bmm350` (magnetómetro). Portables: solo dependen de `nebulaedge_i2c`. |
| Buses | `nebulaedge_i2c` (sensores). El SPI lo inicializa cada cliente: `nebulaedge_microsd` y `nebulaedge_busscan` |
| Protocolos de salida | `nebulaedge_mqtt`, `nebulaedge_udp`, `nebulaedge_tcp`, `nebulaedge_ble` |
| Red | `nebulaedge_wifi` |
| Datos | `nebulaedge_proto_schema` (schema.proto + código generado) |
| Almacenamiento local | `nebulaedge_microsd` (montar/desmontar), `nebulaedge_sdstorage` (escritura de paquetes). Solo escribe en modo deep sleep. |
| Definiciones compartidas | `nebulaedge_defs` |

Notas:

- **Los drivers de sensor son copiables a otro proyecto.** No conocen protobuf
  ni el formato de cable: cada uno devuelve su propio tipo en unidades físicas
  (`bme688_reading_t`, `bmi270_reading_t`, `bmm350_reading_t`) y `main.c` lo
  traduce al mensaje. Su única dependencia del proyecto es `nebulaedge_i2c`, así
  que copiar esas dos carpetas alcanza. Las constantes del chip (dirección I2C,
  ODR) viven en el header del driver, no en `nebulaedge_defs`.
- Los tres `*_read()` devuelven `esp_err_t`. Si el sensor está caído o todavía
  no hay muestra nueva, NO tocan la estructura de salida: así el caller
  distingue un fallo de una medición legítima de cero, cosa que antes era
  imposible.
- `nebulaedge_ble` cumple doble rol: es el canal por el que llega la
  configuración inicial (siempre), y además uno de los cuatro protocolos de
  telemetría (cuando `protocol_conf = 3`).
- Los UUIDs del servicio GATT deben coincidir con los declarados en
  [raspberry/server/gatt_uuids.py](../raspberry/server/gatt_uuids.py).
- `schema.proto` está duplicado a propósito entre firmware y servidor: son dos
  copias del mismo contrato y se editan juntas. Para regenerar el código C, ver
  [components/nebulaedge_proto_schema/README.md](components/nebulaedge_proto_schema/README.md).
- **La persistencia en microSD solo escribe en modo deep sleep**: las tasks de
  envío llaman a `sdstorage_write_packet()` únicamente cuando `sleep_time_s > 0`.
  El interruptor es `SD_PERSISTENCE` en `main/main.c`; con el interruptor en 0
  esa llamada corta en `sd_is_mounted()` y no escribe nada, sin que el resto del
  código lo note. En la IM-V2 el chip select cuelga del IO0 del expansor
  FXL6408: a SDSPI se le dice que no hay chip select y lo mueve el componente,
  porque entregárselo como pin virtual reiniciaba el chip por interrupt watchdog
  (el porqué está en `sd_mount()`, en `nebulaedge_microsd.c`).
- El montaje va con `.format_if_mount_failed` en true, heredado del bringup:
  hace que una tarjeta virgen sirva sin prepararla a mano, pero también formatea
  una que no monte por cualquier otro motivo.
- Las funciones públicas de `nebulaedge_tcp` llevan prefijo del componente
  (`nebulaedge_tcp_connect`) porque lwIP ya exporta un `tcp_connect` global y el
  enlace falla con símbolo duplicado.

## 10) Relación con la documentación general

- Guía completa del sistema: `README.md` (raíz).
- Módulos del servidor y cómo agregar un protocolo nuevo: sección 7 del README
  de la raíz.