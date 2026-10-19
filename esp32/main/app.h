#ifndef NEBULAEDGE_APP_H
#define NEBULAEDGE_APP_H

/* LO QUE COMPARTEN LOS ARCHIVOS DE LA APLICACIÓN
 *
 * main.c pasaba de 1800 líneas y se está repartiendo en archivos por tema
 * (protocolos, compuerta, camino de datos, config, energía). Lo que más de uno
 * necesita conocer se declara acá; las definiciones siguen en main.c.
 *
 * Es un header de la aplicación, no la API de un componente: no se copia a
 * otro proyecto. Lo que usa un solo archivo se queda `static` en ese archivo. */

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "schema.pb-c.h"

/* Protocolos que puede tomar `protocol_conf` del Config. Los números los fija
 * el enum Protocol de schema.proto, que es también de donde los toma el
 * servidor; acá solo se les pone un nombre corto y se agrega PROTOCOL_COUNT. */
typedef enum {
    PROTOCOL_MQTT  = PROTOCOL__PROTOCOL_MQTT,
    PROTOCOL_UDP   = PROTOCOL__PROTOCOL_UDP,
    PROTOCOL_TCP   = PROTOCOL__PROTOCOL_TCP,
    PROTOCOL_BLE   = PROTOCOL__PROTOCOL_BLE,
    /* No es un transporte nuevo: es MQTT con otro uso. Baja un modelo e infiere,
     * sin telemetría. Ver infer.c. */
    PROTOCOL_INFER = PROTOCOL__PROTOCOL_INFER,

    PROTOCOL_COUNT   // cuántos hay; no es un protocolo
} protocol_t;

/* Definidas en main.c. */
extern QueueHandle_t xQueueConfig;
extern QueueHandle_t xQueueData;
extern QueueHandle_t xQueueConfigBle;
extern QueueHandle_t xQueueModel;
extern SemaphoreHandle_t ble_start_semaphore;
extern SemaphoreHandle_t protocol_change_semaphore;
extern Config *current_config;
extern TaskHandle_t xHandleCollectInertial;
extern TaskHandle_t xHandleCollectEnvironmental;
extern TaskHandle_t send_task[PROTOCOL_COUNT];
extern TaskHandle_t response_task[PROTOCOL_COUNT];

#define TAG "main_task"
#define TAG_COLLECT_INERTIAL "task_collect_inertial"
#define TAG_COLLECT_ENV "task_collect_env"

#define TAG_SEND_MQTT "task_send_mqtt"
#define TAG_SEND_UDP "task_send_udp"
#define TAG_SEND_TCP "task_send_tcp"
#define TAG_SEND_BLE "task_send_ble"

#define TAG_GET_RSP_MQTT "task_get_rsp_mqtt"
#define TAG_GET_RSP_BLE "task_get_rsp_ble"
#define TAG_GET_RSP_TCP "task_get_rsp_tcp"
#define TAG_GET_RSP_UDP "task_get_rsp_udp"
#define TAG_SEND_INFER "task_send_infer"
#define TAG_GET_RSP_INFER "task_get_rsp_infer"

/*****************************************************************/
/********************* TIEMPOS DEL FIRMWARE **********************/
/*****************************************************************/
/* Todos los tiempos fijos, en un solo lugar, con el porqué de cada uno.
 *
 * El equivalente del `Timeouts` del servidor (raspberry/server/models.py).
 * Antes estos números estaban repartidos por el archivo -un 3000 antes de
 * dormir, dos 5000 en ramas distintas del switch, un 50 duplicado en las dos
 * productoras-, así que ajustar el comportamiento obligaba a cazarlos de a uno
 * y no había dónde leer cuál era cuál.
 *
 * Los tiempos que SÍ son configurables desde el servidor no viven acá: vienen
 * en el Config (send_interval_s, env_interval_s, sleep_time_s). */

/* Los paquetes de control (ACK de config, aviso de deep sleep) no esperan
 * respuesta: se mandan varias veces seguidas para bajar la probabilidad de que
 * se pierdan, en vez de implementar un segundo hop de confirmación (que
 * agregaría otro flanco de pérdida en vez de reducir el riesgo). */
#define CONTROL_PKT_REDUNDANCY 3
#define CONTROL_PKT_REDUNDANCY_DELAY_MS 50

/* Entre el ACK de un cambio de protocolo y el cierre del transporte. Si se
 * cierra demasiado pronto el ACK no llega y el servidor da la sesión por
 * perdida. Era 4000 en MQTT y 2000 en UDP/TCP; unificado al mayor, que es la
 * dirección segura (el viaje al broker MQTT es el más lento). Solo se paga al
 * cambiar de protocolo, no en régimen. */
#define ACK_DRAIN_MS 4000

/* Entre soltar el bus I2C y dormirse de verdad. Le da margen a la radio para
 * terminar de vaciar lo que haya encolado antes de que se corte la
 * alimentación de los periféricos. */
#define DEEP_SLEEP_SETTLE_MS 3000

/* Espera tras abrir el transporte, antes de empezar a mandar. El servidor
 * necesita un momento para suscribirse al tópico (MQTT) o para terminar de
 * configurar la sesión (BLE); si se le manda antes, ese primer paquete se
 * pierde. */
#define SERVER_READY_MS 5000

/* Intentos de asociación al AP antes de que nebulaedge_wifi reinicie el chip.
 * Estaba repetido en las tres ramas del switch con el valor 15, y dos de los
 * tres comentarios decían "después de 10 intentos". */
#define WIFI_MAX_RETRY  15

/* Reintento de una productora cuando malloc() falla. Corto a propósito: es una
 * condición transitoria y volver a intentar enseguida es mejor que perder el
 * ritmo de muestreo. */
#define ALLOC_RETRY_MS 50

/* Antes de reiniciar por un protocol_conf inválido, para que el log alcance a
 * salir por el puerto serie. */
#define RESTART_LOG_FLUSH_MS 1000

#endif
