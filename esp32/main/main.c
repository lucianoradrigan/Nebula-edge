#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_sleep.h"
#include "nvs_flash.h"
#include "freertos/semphr.h"

#include "nebulaedge_wifi.h"
#include "nebulaedge_mqtt.h"
#include "nebulaedge_udp.h"
#include "nebulaedge_tcp.h"
#include "nebulaedge_ble.h"
#include "nebulaedge_defs.h"
#include "nebulaedge_config_store.h"
#include "nebulaedge_device.h"
#include "nebulaedge_i2c.h"
#include "bmm350.h"
#include "bme688.h"
#include "bmi270.h"
#include "nebulaedge_microsd.h"
#include "nebulaedge_sdstorage.h"

#include "schema.pb-c.h"

QueueHandle_t xQueueConfig = NULL;
QueueHandle_t xQueueData = NULL;
QueueHandle_t xQueueConfigBle = NULL;

Config *current_config = NULL;

/* El bus I2C y su pinout son de la aplicación: es la única parte que sabe en
 * qué placa corre. El componente nebulaedge_i2c ya no los compila adentro. */
i2c_master_bus_handle_t bus_handle = NULL;
static const i2c_bus_config_t board_i2c = {
    .scl_io  = I2C_MASTER_SCL_IO,
    .sda_io  = I2C_MASTER_SDA_IO,
    .freq_hz = I2C_MASTER_FREQ_HZ,
};

/* Protocolos que puede tomar `protocol_conf` del Config. Los números son parte
 * del formato de cable (los fija schema.proto), así que el enum no los
 * renumera: les pone nombre para que los switch dejen de comparar contra 0..3
 * sueltos. */
typedef enum {
    PROTOCOL_MQTT = 0,
    PROTOCOL_UDP  = 1,
    PROTOCOL_TCP  = 2,
    PROTOCOL_BLE  = 3,

    PROTOCOL_COUNT   // cuántos hay; no es un protocolo
} protocol_t;

/* Dos productoras de telemetría, una por ritmo. Los sensores tienen tiempos
 * naturales muy distintos: la temperatura cambia en minutos y el acelerómetro
 * en milisegundos. Con una sola task había que elegir un intervalo único, que
 * sobremuestreaba el ambiente o submuestreaba el movimiento. Ahora cada una
 * corre a lo suyo y ambas escriben en la misma xQueueData. */
TaskHandle_t xHandleCollectInertial = NULL;       // cada send_interval_s
TaskHandle_t xHandleCollectEnvironmental = NULL;  // cada env_interval_s

/* Un par de tasks por protocolo, indexadas por protocol_t. Antes eran ocho
 * globales sueltas con el protocolo metido en el nombre, que es lo que obligaba
 * a escribir cuatro veces cada cosa que las tocara. */
TaskHandle_t send_task[PROTOCOL_COUNT] = {NULL};
TaskHandle_t response_task[PROTOCOL_COUNT] = {NULL};

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

static uint32_t data_window_count = 0;


/* Los paquetes de control (ACK de config, aviso de deep sleep) no esperan respuesta:
 * se mandan varias veces seguidas para bajar la probabilidad de que se pierdan, en
 * vez de implementar un segundo hop de confirmación (que agregaría otro flanco de
 * pérdida en vez de reducir el riesgo). */
#define CONTROL_PKT_REDUNDANCY 3
#define CONTROL_PKT_REDUNDANCY_DELAY_MS 50

/* Cuánto se espera, tras mandar el ACK de un cambio de protocolo, antes de
 * cerrar el transporte: si se cierra demasiado pronto el ACK no llega y el
 * servidor da la sesión por perdida. Era 4000 en MQTT y 2000 en UDP/TCP;
 * unificado al mayor, que es la dirección segura (el viaje al broker MQTT es
 * el más lento). Solo se paga al cambiar de protocolo, no en régimen. */
#define ACK_DRAIN_MS 4000

/* Vacía xQueueData liberando la memoria de cada packet_t pendiente.
 *
 * xQueueReset() por sí solo descarta los elementos en cola SIN liberar
 * su packet_t.data (reservada con malloc() en las tasks productoras):
 * cada cambio de config/protocolo (4 sitios: vTaskGetResponseMQTT/UDP/
 * TCP/BLE) perdía esa memoria. Se llama en el mismo punto donde antes
 * se llamaba xQueueReset(xQueueData) directamente. */
static void drain_and_free_data_queue(void) {
    packet_t pkt;
    while (xQueueReceive(xQueueData, &pkt, 0) == pdTRUE) {
        free(pkt.data);
    }
}

/* COMPUERTA DEL CAMINO DE DATOS
 *
 * Las cuatro tasks que tocan telemetría —las dos productoras, la de envío y la
 * de respuesta— se detenían con vTaskSuspend() desde otra task. Eso congela a
 * la víctima en la instrucción exacta en que iba: si justo estaba dentro de
 * malloc() o free(), queda con el lock del heap tomado y la siguiente reserva
 * de CUALQUIER task del sistema se bloquea para siempre. Las cuatro reservan o
 * liberan memoria en cada vuelta, así que la ventana existía de verdad — es el
 * cuelgue que se veía.
 *
 * El mutex I2C no tapaba ese hueco: solo cubría las transacciones del bus, no
 * el heap. Y para lo que decía proteger tampoco hacía falta, porque el driver
 * i2c_master de ESP-IDF ya serializa cada transacción con su propio lock
 * interno (bus_lock_mux).
 *
 * La solución es no interrumpirlas: se les PIDE que paren y ellas se detienen
 * solas en un punto donde no tienen nada tomado (el tope de su bucle). La
 * única suspensión que queda en el archivo es vTaskSuspend(NULL), en la task
 * de respuesta: suspenderse a sí misma sí es seguro, porque ocurre en un punto
 * que la propia task eligió. */
#define GATE_RUN            (1 << 0)  // permiso para correr
#define GATE_PAUSE_REQ      (1 << 1)  // hay una pausa pedida (despierta los sleeps)
#define GATE_INERTIAL_IDLE  (1 << 2)  // la task Inertial ya está detenida
#define GATE_ENV_IDLE       (1 << 3)  // la task Environmental ya está detenida
#define GATE_SEND_IDLE      (1 << 4)  // la task de envío ya está detenida
#define GATE_RSP_IDLE       (1 << 5)  // la task de respuesta ya está detenida

/* Cuánto se espera a que lleguen a la compuerta. Una lectura de sensor a medio
 * hacer puede tardar lo suyo si el bus está lento; pasado esto se sigue igual,
 * porque ya nadie queda suspendido y lo peor que pasa es un paquete de más. */
#define GATE_PAUSE_TIMEOUT_MS 5000

/* Cada cuánto vuelven al punto seguro las tasks de envío y respuesta.
 *
 * Las dos se bloqueaban para siempre esperando (una la cola de datos, la otra
 * el transporte). Con un bloqueo infinito no hay forma de pedirles que paren:
 * por eso ahora esperan con timeout y, si no llegó nada, vuelven al tope del
 * bucle y miran la compuerta. El costo es despertar unas pocas veces por
 * segundo sin hacer nada; a cambio, nadie tiene que congelarlas desde afuera. */
#define TASK_POLL_MS 250

static EventGroupHandle_t sensor_gate = NULL;

/* Punto seguro. Va al tope del bucle de cada productora: si hay una pausa
 * pedida, la task levanta su bit de "detenida" y se bloquea acá hasta que
 * vuelva el permiso. */
static void sensor_gate_wait(EventBits_t idle_bit) {
    if (sensor_gate == NULL) {
        return;
    }

    while ((xEventGroupGetBits(sensor_gate) & GATE_RUN) == 0) {
        xEventGroupSetBits(sensor_gate, idle_bit);
        xEventGroupWaitBits(sensor_gate, GATE_RUN, pdFALSE, pdTRUE, portMAX_DELAY);
        xEventGroupClearBits(sensor_gate, idle_bit);
    }
}

/* Reemplaza a vTaskDelay() para ritmar la producción. Si llega una petición de
 * pausa mientras la task duerme, despierta de inmediato en vez de dejar
 * esperando el intervalo entero: con env_interval_s en 10 s o más, pausar
 * tardaría eso en completarse. */
static void sensor_gate_sleep(uint32_t ms) {
    TickType_t ticks = pdMS_TO_TICKS(ms) + 1;

    if (sensor_gate == NULL) {
        vTaskDelay(ticks);
        return;
    }

    xEventGroupWaitBits(sensor_gate, GATE_PAUSE_REQ, pdFALSE, pdTRUE, ticks);
}

/* Pide a las tasks del camino de datos que se detengan y espera a que lleguen
 * al punto seguro. Reemplaza al antiguo vTaskSuspend() sobre ellas.
 *
 * `expected` dice a CUÁLES esperar, porque el alcance no es el mismo siempre:
 * al cambiar de protocolo hay que detener también la task de envío, pero al
 * entrar en deep sleep esa misma task es la que está llamando acá y esperarla
 * sería esperarse a sí misma. Los envoltorios de abajo arman cada caso.
 *
 * El drenado de la cola se hace adentro, y dos veces. Antes estaba afuera y
 * solo una vez, ANTES de detener las tasks: cualquier paquete producido entre
 * el drenado y la detención se colaba al protocolo siguiente. El segundo
 * drenado, ya con todas detenidas, cierra esa ventana.
 *
 * El primero tampoco sobra: si la cola está llena y la consumidora ya se
 * detuvo, la productora está bloqueada en xQueueSend(portMAX_DELAY) y nunca
 * llegaría a la compuerta. Vaciar la cola es justo lo que la desbloquea. */
static void pause_data_tasks(EventBits_t expected) {
    if (sensor_gate == NULL) {
        return;
    }

    xEventGroupClearBits(sensor_gate, GATE_RUN);
    xEventGroupSetBits(sensor_gate, GATE_PAUSE_REQ);

    drain_and_free_data_queue();

    // Todavía no existe ninguna: no hay nada a qué esperar.
    if (expected == 0) {
        return;
    }

    EventBits_t got = xEventGroupWaitBits(sensor_gate, expected, pdFALSE, pdTRUE,
                                          pdMS_TO_TICKS(GATE_PAUSE_TIMEOUT_MS));
    if ((got & expected) == expected) {
        ESP_LOGI(TAG, "Tasks detenidas en punto seguro (bits=0x%02X)", (unsigned)expected);
    }
    else {
        ESP_LOGW(TAG, "Timeout esperando que las tasks se detengan (esperado=0x%02X, got=0x%02X)",
                 (unsigned)expected, (unsigned)got);
    }

    // Segundo drenado: lo que alcanzaron a encolar antes de detenerse.
    drain_and_free_data_queue();
}

/* Bits de las dos productoras, que hay que detener en todos los casos. */
static EventBits_t collect_idle_bits(void) {
    EventBits_t bits = 0;
    if (xHandleCollectInertial)      bits |= GATE_INERTIAL_IDLE;
    if (xHandleCollectEnvironmental) bits |= GATE_ENV_IDLE;
    return bits;
}

/* Cambio de protocolo: lo llama la task de respuesta, así que detiene las
 * productoras y la task de envío (a sí misma no: se auto-suspende después, que
 * es seguro). */
static void pause_for_protocol_change(protocol_t id) {
    EventBits_t expected = collect_idle_bits();
    if (send_task[id]) expected |= GATE_SEND_IDLE;
    pause_data_tasks(expected);
}

/* Deep sleep: lo llama la task de ENVÍO, así que detiene las productoras y la
 * task de respuesta. A la de envío no se la espera — es la que está acá. */
static void pause_for_deep_sleep(protocol_t id) {
    EventBits_t expected = collect_idle_bits();
    if (response_task[id]) expected |= GATE_RSP_IDLE;
    pause_data_tasks(expected);
}

/* Devuelve el permiso de producir. Limpia también los bits de "detenida" para
 * que la próxima pausa no los lea de la vuelta anterior. */
static void resume_collect_tasks(void) {
    if (sensor_gate == NULL) {
        return;
    }

    xEventGroupClearBits(sensor_gate, GATE_PAUSE_REQ | GATE_INERTIAL_IDLE |
                                  GATE_ENV_IDLE | GATE_SEND_IDLE | GATE_RSP_IDLE);
    xEventGroupSetBits(sensor_gate, GATE_RUN);
}

/*******************************************************************/
/*************** TABLA DE PROTOCOLOS (protocol_ops_t) **************/
/*******************************************************************/

/* Los cuatro protocolos hacen lo mismo con piezas distintas: mandar
 * telemetría, mandar un ACK, esperar una config, cerrar el transporte. Eso
 * estaba escrito cuatro veces —una task de envío y una de respuesta por
 * protocolo— y las copias fueron divergiendo: órdenes distintos, delays
 * distintos, logs que estaban en una y no en otra.
 *
 * Acá cada protocolo describe SOLO sus piezas propias. Las tasks son una sola
 * función instanciada cuatro veces: xTaskCreate() recibe la tabla por
 * pvParameters, que hasta ahora estaba declarado y sin usar en las ocho.
 *
 * Es el equivalente en C de lo que el servidor ya tiene con la clase Transport:
 * agregar un protocolo es una entrada más en la tabla, no cuatro funciones
 * nuevas repartidas por el archivo. */

/* Tamaño del buffer donde se recibe una config cruda en UDP y TCP. Vive en la
 * pila de la task de respuesta (4096 B), así que hay espacio de sobra. */
#define CONFIG_RECV_BUF_BYTES 256

typedef struct {
    const char *name;            // para los logs: "MQTT", "UDP", "TCP", "BLE"
    protocol_t  id;              // el mismo valor que protocol_conf del Config

    /* Manda un paquete de telemetría ya serializado. En MQTT va al tópico
     * /data; en BLE, a la característica B. */
    void (*send_data)(const uint8_t *data, size_t size);

    /* Manda UN ACK de config ya serializado. El caller repite la llamada
     * `control_repeats` veces. En MQTT va al tópico /config/ack; en BLE, a la
     * característica D, que es distinta de la de telemetría. */
    void (*send_ack)(const Config *cfg, const uint8_t *buf, size_t size);

    /* Bloquea hasta que llegue una config nueva y la devuelve desempaquetada,
     * o NULL si lo que llegó no servía. El caller la libera.
     *
     * Esta es la firma que hace que todo lo demás encaje. Los cuatro reciben
     * en formas incompatibles: MQTT saca un Config* ya desempaquetado de una
     * cola, BLE saca un packet_t crudo de otra, UDP y TCP bloquean en recv()
     * sobre un buffer. Devolver Config* absorbe las tres diferencias, y todo
     * lo que viene después pasa a existir una sola vez. */
    Config *(*recv_config)(void);

    /* Cierra el transporte antes de cambiar de protocolo o de dormir.
     * En BLE no hace nada: el enlace no se cierra desde acá. */
    void (*close)(void);

    const char *send_tag;        // tag de log de la task de envío
    const char *rsp_tag;         // tag de log de la task de respuesta

    /* Veces que se repite un paquete de control. 3 en los de socket, donde
     * nadie confirma; 1 en BLE, donde el link layer ya retransmite. */
    uint8_t control_repeats;

    /* Espera entre el ACK y el cierre del transporte, para que el ACK alcance
     * a salir. 0 en BLE: no cierra nada, así que no hay nada que drenar. */
    uint32_t ack_drain_ms;

    /* Delays propios de BLE, acá como datos en vez de escondidos en el cuerpo
     * de su task. 0 en los otros tres. */
    uint32_t pre_send_delay_ms;      // aire al enlace antes de mandar, en modo discontinuo
    uint32_t last_packet_delay_ms;   // respiro antes del último paquete de la ventana
} protocol_ops_t;

/* ------------------------------------------------------------------- MQTT */

static void mqtt_send_data(const uint8_t *data, size_t size) {
    char topic_data[128];
    snprintf(topic_data, sizeof(topic_data), "/topic/nebulaedge/%s/data", current_config->id_device);

    int msg_id = mqtt_publish(topic_data, data, size, 0);
    if (msg_id < 0) {
        ESP_LOGE(TAG_SEND_MQTT, "Error al publicar por MQTT. msg_id=%d", msg_id);
    }
    else {
        ESP_LOGI(TAG_SEND_MQTT, "Paquete publicado por MQTT. msg_id=%d", msg_id);
    }
}

static void mqtt_send_ack(const Config *cfg, const uint8_t *buf, size_t size) {
    char topic_ack[128];
    snprintf(topic_ack, sizeof(topic_ack), "/topic/nebulaedge/%s/config/ack", cfg->id_device);
    mqtt_publish(topic_ack, buf, size, 0);
}

/* El componente MQTT desempaqueta la config y deja el Config* en la cola. */
static Config *mqtt_recv_config(void) {
    Config *cfg = NULL;
    if (xQueueReceive(xQueueConfig, &cfg, pdMS_TO_TICKS(TASK_POLL_MS)) != pdTRUE) {
        return NULL;    // nada todavía; la task vuelve a mirar la compuerta
    }
    return cfg;
}

/* -------------------------------------------------------------------- UDP */

static void udp_send_data(const uint8_t *data, size_t size) {
    nebulaedge_udp_send(data, size);
}

static void udp_send_ack(const Config *cfg, const uint8_t *buf, size_t size) {
    (void)cfg;    // UDP no direcciona dentro de la conexión
    nebulaedge_udp_send(buf, size);
}

static Config *udp_recv_config(void) {
    uint8_t buffer[CONFIG_RECV_BUF_BYTES];

    // Bloquea acá (y cede la CPU) hasta recibir algo o hasta el timeout.
    size_t len_recv = nebulaedge_udp_receive(buffer, sizeof(buffer));
    if (len_recv == 0) {
        vTaskDelay(1);
        return NULL;    // timeout del socket; la task vuelve a mirar la compuerta
    }

    Config *cfg = config__unpack(NULL, len_recv, buffer);
    if (!cfg) {
        ESP_LOGI(TAG_GET_RSP_UDP, "UDP: error al desempaquetar");
    }
    return cfg;
}

/* -------------------------------------------------------------------- TCP */

static void tcp_send_data(const uint8_t *data, size_t size) {
    tcp_send((uint8_t *)data, size);
}

static void tcp_send_ack(const Config *cfg, const uint8_t *buf, size_t size) {
    (void)cfg;
    tcp_send((uint8_t *)buf, size);
}

static Config *tcp_recv_config(void) {
    uint8_t buffer[CONFIG_RECV_BUF_BYTES];

    size_t len_recv = tcp_receive(buffer, sizeof(buffer));
    if (len_recv == 0) {
        vTaskDelay(1);
        return NULL;    // timeout del socket; la task vuelve a mirar la compuerta
    }

    Config *cfg = config__unpack(NULL, len_recv, buffer);
    if (!cfg) {
        ESP_LOGI(TAG_GET_RSP_TCP, "TCP: error al desempaquetar");
    }
    return cfg;
}

/* -------------------------------------------------------------------- BLE */

static void ble_send_data(const uint8_t *data, size_t size) {
    set_char_with_notify(IDX_CHAR_VAL_B_BLE, data, size);
}

static void ble_send_ack(const Config *cfg, const uint8_t *buf, size_t size) {
    (void)cfg;

    /* Char D, distinta de la de telemetría: el ACK queda ahí legible, así que
     * si se pierde la notificación el servidor lo reconcilia leyéndolo
     * (BleTransport.confirm_config_applied). */
    esp_err_t ret = set_char_with_notify(IDX_CHAR_VAL_D_BLE, buf, size);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG_GET_RSP_BLE, "BLE: ACK no se pudo notificar (%s); queda legible en char D.",
                 esp_err_to_name(ret));
    }
}

/* En BLE la config llega cruda por la cola y hay que desempaquetarla acá. */
static Config *ble_recv_config(void) {
    packet_t pkt;

    if (xQueueReceive(xQueueConfigBle, &pkt, pdMS_TO_TICKS(TASK_POLL_MS)) != pdTRUE) {
        return NULL;    // nada todavía; la task vuelve a mirar la compuerta
    }

    if (pkt.data == NULL || pkt.size == 0) {
        /* free() incondicional: antes este camino hacía `continue` sin liberar,
         * así que un paquete con data != NULL y size 0 perdía la memoria. */
        free(pkt.data);
        return NULL;
    }

    Config *cfg = config__unpack(NULL, pkt.size, pkt.data);
    free(pkt.data);

    if (cfg == NULL) {
        ESP_LOGW(TAG_GET_RSP_BLE, "Error al desempaquetar el mensaje protobuf de configuración");
    }
    else {
        ESP_LOGI(TAG_GET_RSP_BLE, "BLE: Se recibió información de configuración!");
    }
    return cfg;
}

/* El enlace BLE no se cierra al cambiar de protocolo: queda activo siempre. */
static void ble_close(void) { }

/* ----------------------------------------------------------- LA TABLA */

static const protocol_ops_t PROTOCOLS[] = {
    [PROTOCOL_MQTT] = {
        .name = "MQTT", .id = PROTOCOL_MQTT,
        .send_data = mqtt_send_data, .send_ack = mqtt_send_ack,
        .send_tag = TAG_SEND_MQTT, .rsp_tag = TAG_GET_RSP_MQTT,
        .recv_config = mqtt_recv_config, .close = mqtt_finish,
        .control_repeats = CONTROL_PKT_REDUNDANCY, .ack_drain_ms = ACK_DRAIN_MS,
    },
    [PROTOCOL_UDP] = {
        .name = "UDP", .id = PROTOCOL_UDP,
        .send_data = udp_send_data, .send_ack = udp_send_ack,
        .send_tag = TAG_SEND_UDP, .rsp_tag = TAG_GET_RSP_UDP,
        .recv_config = udp_recv_config, .close = nebulaedge_udp_close_socket,
        .control_repeats = CONTROL_PKT_REDUNDANCY, .ack_drain_ms = ACK_DRAIN_MS,
    },
    [PROTOCOL_TCP] = {
        .name = "TCP", .id = PROTOCOL_TCP,
        .send_data = tcp_send_data, .send_ack = tcp_send_ack,
        .send_tag = TAG_SEND_TCP, .rsp_tag = TAG_GET_RSP_TCP,
        .recv_config = tcp_recv_config, .close = tcp_close_socket,
        .control_repeats = CONTROL_PKT_REDUNDANCY, .ack_drain_ms = ACK_DRAIN_MS,
    },
    [PROTOCOL_BLE] = {
        .name = "BLE", .id = PROTOCOL_BLE,
        .send_data = ble_send_data, .send_ack = ble_send_ack,
        .send_tag = TAG_SEND_BLE, .rsp_tag = TAG_GET_RSP_BLE,
        .recv_config = ble_recv_config, .close = ble_close,
        /* Una sola vez: el link layer de BLE ya retransmite lo que se encoló, y
         * set_char_with_notify() reintenta por su cuenta si el stack rechaza el
         * envío por congestión. */
        .control_repeats = 1,
        .pre_send_delay_ms = 1000,
        .last_packet_delay_ms = 3000,
    },
};

/* Deep sleep helper para modo discontinuo.
 * Se llama en la función de envío de cada protocolo. */
static void deep_sleep_if_needed(void) {

    // Si no existe configuración actual
    if (!current_config) {
        return;
    }

    data_window_count++;

    // Caso en que deep sleep está desactivado
    if (current_config->sleep_time_s <= 0) {
        return;
    }

    int window_size = current_config->sleep_window_size;
    if (window_size <= 0) {
        window_size = 1;
    }

    // Aún no se envía toda la ventana, retorna
    if (data_window_count < (uint32_t)window_size) {
        return;
    }
    data_window_count = 0;

    // Acá se tiene condición de ventana completa, ya se puede entrar en deep sleep
    uint64_t sleep_us = (uint64_t)current_config->sleep_time_s * 1000000ULL;
    
    const protocol_ops_t *proto = &PROTOCOLS[current_config->protocol_conf];

    /* Detiene las productoras y la task de respuesta antes de dormir. La de
     * respuesta hay que pararla porque el aviso de deep sleep cierra el
     * transporte y no tiene sentido que siga esperando config sobre él.
     *
     * A la task de ENVÍO no se la espera: es la que está ejecutando esto. */
    ESP_LOGI(TAG, "Deteniendo tasks antes de dormir");
    pause_for_deep_sleep(proto->id);

    /* Avisa al server que se va a dormir, antes de cerrar. No se espera
     * respuesta, así que se repite lo que diga la tabla: tres veces en los de
     * socket, una sola en BLE — acá eso importa más todavía, porque cada
     * reenvío son milisegundos despierto antes de dormir.
     *
     * Va por send_data() y no por send_ack(): el flag viaja por el mismo canal
     * que la telemetría (tópico /data en MQTT, característica B en BLE), no por
     * el de los ACKs. */
    for (int i = 0; i < proto->control_repeats; i++) {
        proto->send_data((const uint8_t *)DEEP_SLEEP_FLAG, DEEP_SLEEP_FLAG_LEN);
        vTaskDelay(pdMS_TO_TICKS(CONTROL_PKT_REDUNDANCY_DELAY_MS));
    }

    proto->close();

    config_store_save(current_config);
    
    /* Cada driver se saca del bus solo. Tiene que ser ANTES del
     * i2c_master_deinit: borrar el bus invalida los handles de sus slaves. */
    bmm350_deinit();
    bme688_deinit();
    bmi270_deinit();
    i2c_master_deinit(&bus_handle);
    
    // Delay de precaución
    vTaskDelay(pdMS_TO_TICKS(3000));
    
    ESP_LOGI(TAG, "Entrando deep sleep por %lu s", (unsigned long)current_config->sleep_time_s);
    device_clock_save_before_deep_sleep(sleep_us);
    
    esp_sleep_enable_timer_wakeup(sleep_us);
    esp_deep_sleep_start();
}

// Recibe dos configuraciones y retorna un booleano si son diferentes o iguales
bool config_has_changed(Config *old, Config *new) {
    /* Sin config previa, cualquiera es nueva. El caller de más arriba ya
     * pregunta por NULL antes de comparar versiones, así que acá faltaba la
     * misma guarda: sin ella esto desreferencia old y revienta. */
    if (!old) {
        return new != NULL;
    }
    if (!new) {
        return false;
    }

    if (new->config_version > old->config_version) {
        ESP_LOGI(TAG, "Nueva configuración recibida. Versión %ld.", new->config_version);
        return true;
    }

    // Igual o más vieja: no hay nada que aplicar.
    return false;
}

/* Envía el ACK de una configuración recibida, por el protocolo `over`.
 *
 * OJO con por qué `over` es un parámetro en vez de leerse de
 * current_config->protocol_conf: cuando el ACK confirma un cambio DE
 * protocolo, current_config ya es la configuración nueva, pero el transporte
 * que sigue abierto es el viejo. El ACK tiene que salir por el viejo — es lo
 * último que se manda antes de cerrarlo. Cada task de respuesta pasa su propio
 * protocolo, que es justamente el que tiene abierto.
 *
 * Envío redundante: no se espera respuesta, así que se manda varias veces en
 * vez de pedir una confirmación (que agregaría otro flanco de pérdida en vez
 * de reducir el riesgo). Cuántas, lo dice la tabla de cada protocolo. */
static void send_config_ack(protocol_t over, const Config *cfg, bool applied) {
    if (!cfg || !cfg->id_device) return;

    const protocol_ops_t *proto = &PROTOCOLS[over];

    ConfigAck ack = CONFIG_ACK__INIT;
    ack.id_device = cfg->id_device;
    ack.config_version = cfg->config_version;
    ack.applied = applied;
    ack.time_client = device_clock_now_s();

    size_t size = config_ack__get_packed_size(&ack);
    uint8_t *buf = malloc(size);
    if (!buf) {
        ESP_LOGE(TAG, "No hay memoria para el ACK de config");
        return;
    }
    config_ack__pack(&ack, buf);

    for (int i = 0; i < proto->control_repeats; i++) {
        proto->send_ack(cfg, buf, size);
        vTaskDelay(pdMS_TO_TICKS(CONTROL_PKT_REDUNDANCY_DELAY_MS));
    }

    free(buf);
    ESP_LOGI(TAG, "%s: ACK de config enviado.", proto->name);
}

/* Encola un paquete ya serializado, liberando su memoria si la cola lo rechaza.
 * Lo comparten las dos tasks productoras. */
static void enqueue_packet(packet_t *pkt, const char *tag) {
    int send = xQueueSend(xQueueData, pkt, portMAX_DELAY);
    if (send == pdTRUE) {
        ESP_LOGI(tag, "Se ha insertado correctamente en la xQueue");
    }
    else {
        ESP_LOGW(tag, "xQueueSend falló (code=%d), paquete descartado y memoria liberada", send);
        free(pkt->data);
    }
}

/* Intervalo del flujo rápido (Inertial). */
static uint32_t interval_inertial_s(void) {
    if (!current_config) {
        return 1;
    }
    uint32_t s = current_config->send_interval_s;
    return s > 0 ? s : 1;
}

/* Intervalo del flujo lento. env_interval_s = 0 significa "el mismo que el
 * rápido", para que una config antigua sin ese campo siga comportándose como
 * antes en vez de girar en vacío. */
static uint32_t environmental_interval_s(void) {
    if (!current_config) {
        return 1;
    }
    uint32_t env_s = current_config->env_interval_s;
    if (env_s == 0) {
        env_s = current_config->send_interval_s;
    }
    return env_s > 0 ? env_s : 1;
}

// GEN_DATA (rápido): BMI270 + BMM350 -> paquete Inertial, cada send_interval_s.
void vTaskCollectInertial(void *pvParameters) {
    for (;;) {
        // Punto seguro: acá no hay memoria reservada ni bus tomado.
        sensor_gate_wait(GATE_INERTIAL_IDLE);

        Inertial inertial = INERTIAL__INIT;
        inertial.id_device = device_id();
        inertial.config_version_applied = current_config ? current_config->config_version : 0;
        inertial.time_client = device_clock_now_s();

        /* Recogida de datos inerciales. Cada driver devuelve su propio tipo en
         * unidades físicas; traducirlo al mensaje protobuf es trabajo de acá,
         * que es la única parte que conoce el formato de cable.
         *
         * El acelerómetro manda: si su lectura falla no se envía el paquete,
         * porque antes se iba con ceros indistinguibles de reposo real. El
         * magnetómetro es complementario, así que si falla solo quedan sus
         * tres ejes en cero y el resto del paquete sigue siendo válido. */
        bmi270_reading_t imu;
        if (bmi270_read(&imu) != ESP_OK) {
            sensor_gate_sleep(interval_inertial_s() * 1000U);
            continue;
        }
        inertial.acc_x = imu.acc_x_ms2;
        inertial.acc_y = imu.acc_y_ms2;
        inertial.acc_z = imu.acc_z_ms2;
        inertial.gyr_x = imu.gyr_x_rads;
        inertial.gyr_y = imu.gyr_y_rads;
        inertial.gyr_z = imu.gyr_z_rads;

        bmm350_reading_t mag;
        if (bmm350_read(&mag) == ESP_OK) {
            inertial.mag_x = mag.mag_x_ut;
            inertial.mag_y = mag.mag_y_ut;
            inertial.mag_z = mag.mag_z_ut;
        }

        // Serializa el mensaje protobuf, con el byte de tipo por delante
        packet_t packet;
        packet.size = inertial__get_packed_size(&inertial) + 1;
        packet.data = malloc(packet.size);
        if (packet.data == NULL) {
            ESP_LOGE(TAG_COLLECT_INERTIAL, "Error: no se pudo reservar memoria para el paquete");
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        packet.data[0] = 0x02;
        inertial__pack(&inertial, packet.data + 1);
        ESP_LOGI(TAG_COLLECT_INERTIAL, "Paquete Inertial generado");

        enqueue_packet(&packet, TAG_COLLECT_INERTIAL);

        // Ritma la producción. Despierta antes si se pide una pausa.
        sensor_gate_sleep(interval_inertial_s() * 1000U);
    }
}

// GEN_DATA (lento): BME688 -> paquete Environmental, cada env_interval_s.
void vTaskCollectEnvironmental(void *pvParameters) {
    for (;;) {
        // Punto seguro: acá no hay memoria reservada ni bus tomado.
        sensor_gate_wait(GATE_ENV_IDLE);

        Environmental env = ENVIRONMENTAL__INIT;
        env.id_device = device_id();
        env.config_version_applied = current_config ? current_config->config_version : 0;
        env.time_client = device_clock_now_s();

        /* Recogida de datos ambientales. El driver devuelve su propio tipo en
         * unidades físicas; traducirlo al mensaje protobuf es trabajo de acá,
         * que es la única parte que conoce el formato de cable.
         *
         * Si la lectura falla NO se manda el paquete: antes se enviaba con
         * ceros, indistinguibles de una medición legítima de cero. */
        bme688_reading_t ambient;
        if (bme688_read(&ambient) != ESP_OK) {
            ESP_LOGW(TAG_COLLECT_ENV, "Lectura del BME688 falló, se omite el paquete");
            sensor_gate_sleep(environmental_interval_s() * 1000U);
            continue;
        }
        env.temperature = ambient.temperature_c;
        env.press       = ambient.pressure_pa;
        env.hum         = ambient.humidity_pct;
        env.co          = ambient.gas_resistance_ohm;

        // Serializa el mensaje protobuf, con el byte de tipo por delante
        packet_t packet;
        packet.size = environmental__get_packed_size(&env) + 1;
        packet.data = malloc(packet.size);
        if (packet.data == NULL) {
            ESP_LOGE(TAG_COLLECT_ENV, "Error: no se pudo reservar memoria para el paquete");
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        packet.data[0] = 0x01;
        environmental__pack(&env, packet.data + 1);
        ESP_LOGI(TAG_COLLECT_ENV, "Paquete Environmental generado");

        enqueue_packet(&packet, TAG_COLLECT_ENV);

        // Ritma la producción. Despierta antes si se pide una pausa.
        sensor_gate_sleep(environmental_interval_s() * 1000U);
    }
}

/* Task de envío de telemetría. UNA sola función, instanciada cuatro veces —
 * una por protocolo— pasándole su tabla por pvParameters.
 *
 * Los cuatro cuerpos eran casi idénticos (12 de 14 líneas entre UDP y TCP):
 * sacar de la cola, mandar, respaldar en SD si hay deep sleep, liberar, y
 * evaluar si toca dormir. Lo único propio de cada uno es la línea que manda,
 * que ahora la pone la tabla. */
void vTaskSendData(void *pvParameters) {
    const protocol_ops_t *proto = pvParameters;
    packet_t packet;

    for (;;) {
        // Punto seguro: acá no hay memoria reservada ni transporte a medias.
        sensor_gate_wait(GATE_SEND_IDLE);

        // Con timeout, para poder volver acá arriba si se pide una pausa.
        if (xQueueReceive(xQueueData, &packet, pdMS_TO_TICKS(TASK_POLL_MS)) != pdTRUE) {
            continue;
        }

        // Delay de precaución en modo discontinuo. Solo BLE lo declara.
        if (current_config->sleep_time_s > 0 && proto->pre_send_delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(proto->pre_send_delay_ms));
        }

        proto->send_data(packet.data, packet.size);

        // Respaldo local, solo en modo deep sleep.
        if (current_config->sleep_time_s > 0) {
            data_to_sd(packet.data, packet.size);
        }

        free(packet.data);

        /* Respiro antes del último paquete de la ventana, para que no se pierda
         * al cortarse el enlace por el deep sleep. Solo BLE lo declara. */
        if (proto->last_packet_delay_ms > 0 &&
            data_window_count + 1 == (uint32_t)current_config->sleep_window_size) {
            vTaskDelay(pdMS_TO_TICKS(proto->last_packet_delay_ms));
        }

        deep_sleep_if_needed();
    }
}

/* Task de respuesta: espera configuraciones nuevas y las aplica. UNA sola
 * función, instanciada cuatro veces —una por protocolo— con su tabla por
 * pvParameters.
 *
 * Es la lógica delicada del firmware: comparar versiones, mandar el ACK,
 * reemplazar la config global, detener a todos, cerrar el transporte y avisarle
 * a app_main que cambie de protocolo. Estaba escrita cuatro veces, y las copias
 * ya habían divergido en el orden de las operaciones y en los delays.
 *
 * Lo único propio de cada protocolo es de dónde sale la config y cómo se cierra
 * el transporte: las dos cosas las pone la tabla. */
void vTaskGetResponse(void *pvParameters) {
    const protocol_ops_t *proto = pvParameters;

    for (;;) {
        // Punto seguro: acá no hay config a medio desempaquetar ni memoria viva.
        sensor_gate_wait(GATE_RSP_IDLE);

        /* Espera una config, con timeout. Devuelve NULL si no llegó nada o si
         * lo que llegó no servía; cada protocolo ya logueó el motivo. */
        Config *new_config = proto->recv_config();
        if (!new_config) {
            continue;
        }

        /* Config más vieja que la aplicada: se rechaza con un ACK negativo, para
         * que el servidor sepa que llegó y que NO se aplicó. */
        if (current_config && new_config->config_version < current_config->config_version) {
            ESP_LOGW(proto->rsp_tag, "Config antigua: %ld < %ld",
                     (long)new_config->config_version, (long)current_config->config_version);
            send_config_ack(proto->id, new_config, false);
            config__free_unpacked(new_config, NULL);
            continue;
        }

        /* Misma versión: se confirma y se descarta. No hay nada que cambiar. */
        if (!config_has_changed(current_config, new_config)) {
            ESP_LOGI(proto->rsp_tag, "La configuración recibida es la misma");
            send_config_ack(proto->id, new_config, true);
            config__free_unpacked(new_config, NULL);
            continue;
        }

        ESP_LOGI(proto->rsp_tag, "Configuración cambiada!");

        // Reemplaza la configuración global y reinicia la ventana de deep sleep.
        if (current_config) config__free_unpacked(current_config, NULL);
        current_config = new_config;
        data_window_count = 0;

        /* El ACK sale ANTES de detener nada, y por `proto->id` en vez de por el
         * protocolo de la config nueva: el transporte abierto sigue siendo
         * este, y el ACK es lo último que se manda antes de cerrarlo. */
        send_config_ack(proto->id, current_config, true);

        /* Detiene la task de envío y las productoras, y vacía la cola: los
         * paquetes del protocolo anterior no deben colarse al siguiente. */
        ESP_LOGI(proto->rsp_tag, "Se detienen las tasks del camino de datos");
        pause_for_protocol_change(proto->id);

        // Le da tiempo al ACK de llegar antes de cerrar. BLE no cierra: 0.
        if (proto->ack_drain_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(proto->ack_drain_ms));
        }

        proto->close();

        // Señala a app_main que debe cambiar de protocolo.
        if (xSemaphoreGive(semaphore)) {
            ESP_LOGI(proto->rsp_tag, "Libera semáforo para cambio de protocolo");
        }

        /* Auto-suspensión hasta que este protocolo vuelva a activarse. Es
         * segura, a diferencia de suspender a otra task: la víctima es esta
         * misma, en un punto donde no tiene nada reservado ni tomado. */
        ESP_LOGI(proto->rsp_tag, "Suspendiendo task de respuesta");
        vTaskSuspend(NULL);
    }
}







/* Arranca las tasks que necesita un protocolo y las deja corriendo.
 *
 * Las dos productoras son de todos y se crean una sola vez en la vida del
 * programa; el par envío/respuesta es propio de cada protocolo y se crea la
 * primera vez que ese protocolo se usa. Después de eso, cambiar de protocolo
 * solo suspende y reanuda: por eso los cuatro `if` preguntan antes de crear.
 *
 * Esto era el mismo bloque repetido en las cuatro ramas del switch de
 * app_main. */
static void start_protocol_tasks(const protocol_ops_t *proto) {
    if (!xHandleCollectInertial)
        xTaskCreate(vTaskCollectInertial, TAG_COLLECT_INERTIAL, 4096, NULL, 2, &xHandleCollectInertial);
    if (!xHandleCollectEnvironmental)
        xTaskCreate(vTaskCollectEnvironmental, TAG_COLLECT_ENV, 4096, NULL, 2, &xHandleCollectEnvironmental);
    if (!send_task[proto->id])
        xTaskCreate(vTaskSendData, proto->send_tag, 4096, (void *)proto, 2, &send_task[proto->id]);
    if (!response_task[proto->id])
        xTaskCreate(vTaskGetResponse, proto->rsp_tag, 4096, (void *)proto, 3, &response_task[proto->id]);

    /* resume_collect_tasks() devuelve el permiso a todas las que esperan en la
     * compuerta: las dos productoras y la de envío. La de respuesta además se
     * auto-suspende al salir de su protocolo, así que a esa hay que reanudarla
     * de verdad — auto-suspenderse es seguro, es suspender a OTRA lo que no. */
    resume_collect_tasks();
    vTaskResume(response_task[proto->id]);
}

void app_main() {

    /****************************************************************/
    /***********************  INICIALIZACIÓN ************************/
    /****************************************************************/

    // Inicializa NVS
    ESP_ERROR_CHECK(nvs_flash_init());

    // Obtiene MAC BT del dispositivo
    device_id_init();

    // Inicializa semáforos binario. Inician cerrados.
    semaphore = xSemaphoreCreateBinary();
    semaphore_ble = xSemaphoreCreateBinary();

    /* Compuerta de las tasks de sensores. Arranca con el permiso dado, para que
     * una productora recién creada produzca sin esperar a nadie. */
    sensor_gate = xEventGroupCreate();
    if (sensor_gate == NULL) {
        ESP_LOGE(TAG, "No se pudo crear el event group de las tasks de sensores");
    }
    else {
        xEventGroupSetBits(sensor_gate, GATE_RUN);
    }

    // Crea queues para pasar datos entre tasks
    xQueueData = xQueueCreate(100, sizeof(packet_t));
    xQueueConfig = xQueueCreate(5, sizeof(Config *));
    xQueueConfigBle = xQueueCreate(5, sizeof(packet_t));

    /* Entrega a los componentes lo que necesitan para avisar hacia acá.
     *
     * MQTT y BLE reciben por callback, desde la task de su propia pila: ese
     * callback no puede bloquearse esperando a vTaskGetResponse, así que deja
     * el dato en una cola y retorna. UDP y TCP no necesitan nada de esto
     * porque su recv() lo hace nuestra propia task.
     *
     * Las colas y el semáforo son de la aplicación y se los pasamos: antes los
     * componentes los alcanzaban con `extern` por nombre, lo que impedía
     * copiarlos a otro proyecto sin arrastrar esas globales. */
    mqtt_set_config_queue(xQueueConfig);
    ble_set_config_queue(xQueueConfigBle);
    ble_set_start_semaphore(semaphore);

    /********************************************************************/
    /********************* CONFIG INICIAL (NVS/BLE) *********************/
    /********************************************************************/

    // Recoge la información de la NVS solo si despierta de un deep sleep.
    // En caso de reinicio espera configuración vía BLE.
    esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();
    if (wake_cause == ESP_SLEEP_WAKEUP_TIMER) {
        current_config = config_store_load();
        if (current_config) {
            ESP_LOGI(TAG, "Config cargada desde NVS (wake-up por deep sleep)");
            device_clock_restore_after_deep_sleep();
            data_window_count = 0;
        }
    }
    else {
        config_store_clear();
    }

    /********************************************************************/
    /************************ INICIO VÍA BLE  ***************************/
    /********************************************************************/
    
    // BLE queda activo siempre
    ble_init();

    // Espera recepción de paquete si no se recogió ninguno desde la NVS
    if (!current_config) {

        ESP_LOGI(TAG, "esperando configuración vía BLE");
        packet_t init_pkt;
        if (xQueueReceive(xQueueConfigBle, &init_pkt, portMAX_DELAY) == pdTRUE) {
            if (init_pkt.data == NULL || init_pkt.size == 0) {
                ESP_LOGI(TAG, "configuración BLE vacía. Reiniciando...");
                esp_restart();
            }
            current_config = config__unpack(NULL, init_pkt.size, init_pkt.data);
            free(init_pkt.data);
            if (current_config == NULL) {
                ESP_LOGI(TAG, "error al desempaquetar paquete de configuración. Reiniciando...");
                esp_restart();
            }
            device_clock_set(current_config->time_client);
            data_window_count = 0;
            ESP_LOGI(TAG, "configuración leída correctamente");
        }
    } 

    /* PERSISTENCIA EN SD: DESACTIVADA.
     *
     * El montaje está comentado porque en la IM-V2 ocupa el GPIO 1 y falla.
     * Consecuencia que NO es evidente leyendo el resto del código: las tasks
     * de envío siguen llamando a data_to_sd() cuando sleep_time_s > 0, pero
     * esa función corta de inmediato en is_sd_mounted() y no escribe nada.
     * O sea que el firmware parece guardar respaldo local y no lo hace.
     *
     * Para reactivarlo hay que resolver antes el conflicto de pines en
     * nebulaedge_defs.h (PIN_NUM_CS). */
    // esp_err_t sd_ret = mount_sd();
    // if (sd_ret != ESP_OK) {
    //     ESP_LOGW(TAG, "SD no disponible, se continúa sin persistencia local: %s", esp_err_to_name(sd_ret));
    // }

    /****************************************************************/
    /********** ITERACIÓN QUE MANEJA DE CAMBIOS DE PROTOCOLO ********/
    /****************************************************************/
    while (1) {
        /* Al cambiar de protocolo se rehace el bus completo para asegurar una
         * transmisión I2C limpia (en la primera iteración no hay nada que
         * liberar). Cada driver se saca del bus solo, antes de borrarlo. */
        bmi270_deinit();
        bme688_deinit();
        bmm350_deinit();
        i2c_master_deinit(&bus_handle);

        // Inicializa el bus con el pinout de esta placa
        ESP_ERROR_CHECK(i2c_master_init(&bus_handle, &board_i2c));

        /* Cada driver se agrega al bus y se configura. La dirección I2C la
         * conoce cada uno; acá solo van los parámetros de medición. */
        bmm350_init(bus_handle, 400, 4);
        bme688_init(bus_handle, current_config->bme688_sampling, current_config->bme688_sampling, current_config->bme688_sampling);
        bmi270_init(bus_handle, current_config->acc_sampling, 4, 8, 400, current_config->gyro_sensibility); 
    
        switch (current_config->protocol_conf) {

            /****************************************************************/
            /**************************  MQTT *******************************/
            /****************************************************************/
            case PROTOCOL_MQTT: {
                // Estructura de configuración de wifi
                global_wifi_config wifi_config = {
                    .ssid = current_config->ssid,
                    .password = current_config->passwd,

                    // Hacer enums para simplificar
                    .auth_mode = WIFI_AUTH_WPA_WPA2_PSK,
                    .max_retry = 15,                           // Después de 15 intentos reinicia la ESP
                    .retry_delay_ms = 0,
                };
                wifi_start_if_needed(&wifi_config);

                // Broker MQTT provisto por configuración
                const char *broker = current_config->mqtt_broker;

                // Estructura de configuración MQTT: debe ser visible desde main
                mqtt_config_global mqtt_config = {
                    .broker = broker,
                };

                // Empieza la conexión mqtt en el broker configurado: visibilidad main
                mqtt_start(&mqtt_config);

                // Da tiempo a la Raspberry para conectarse al broker antes de enviar datos
                vTaskDelay(5000 / portTICK_PERIOD_MS);

                // Suscribe al tópico de configuración por dispositivo
                char topic_cfg[128];
                snprintf(topic_cfg, sizeof(topic_cfg), "/topic/nebulaedge/%s/config", current_config->id_device);
                ESP_LOGI(TAG_SEND_MQTT, "topic_cfg: %s", topic_cfg);
                mqtt_subscribe(topic_cfg, 0);
                
                start_protocol_tasks(&PROTOCOLS[PROTOCOL_MQTT]);

                // Punto de bloqueo
                if (xSemaphoreTake(semaphore, portMAX_DELAY)) {
                    ESP_LOGI(TAG, "Se sale de MQTT (cambio de protocolo)");
                }

                wifi_deinit_sta();

                break;
            }

            /****************************************************************/
            /**************************  UDP  *******************************/
            /****************************************************************/
            case PROTOCOL_UDP: {
                // Estructura de configuración de wifi
                global_wifi_config wifi_config = {
                    .ssid = current_config->ssid,
                    .password = current_config->passwd,

                    // Hacer enums para simplificar
                    .auth_mode = WIFI_AUTH_WPA_WPA2_PSK,
                    .max_retry = 15,                           // Después de 10 intentos reinicia la ESP
                    .retry_delay_ms = 0,
                };
                wifi_start_if_needed(&wifi_config);
                
                udp_params_t params = {
                    .ip_host = current_config->host_ip_addr,
                    .port = current_config->udp_port,
                    .ip_version = IPV4,
                };

                nebulaedge_udp_open_socket(&params);

                start_protocol_tasks(&PROTOCOLS[PROTOCOL_UDP]);

                // Punto de bloqueo
                if (xSemaphoreTake(semaphore, portMAX_DELAY)) {
                    ESP_LOGI(TAG, "se libera semáforo, se sale de UDP correctamente");
                }

                wifi_deinit_sta();

                break;
            }

            /****************************************************************/
            /**************************  TCP  *******************************/
            /****************************************************************/
            case PROTOCOL_TCP: {
                // Estructura de configuración de wifi
                global_wifi_config wifi_config = {
                    .ssid = current_config->ssid,
                    .password = current_config->passwd,

                    // Hacer enums para simplificar
                    .auth_mode = WIFI_AUTH_WPA_WPA2_PSK,
                    .max_retry = 15,                           // Después de 10 intentos reinicia la ESP
                    .retry_delay_ms = 0,
                };
                wifi_start_if_needed(&wifi_config);

                tcp_params_t params = {
                    .ip_host = current_config->host_ip_addr,
                    .port = current_config->tcp_port,
                    .ip_version = IPV4,
                };

                // Abre socket TCP
                tcp_open_socket(&params);
                // Conecta
                if (nebulaedge_tcp_connect() != 0) {
                    // Cierra el socket
                    tcp_close_socket();
                    ESP_LOGI(TAG, "Retrying TCP connection...");
                    wifi_deinit_sta();
                    continue;
                }

                start_protocol_tasks(&PROTOCOLS[PROTOCOL_TCP]);

                // Punto de bloqueo
                if (xSemaphoreTake(semaphore, portMAX_DELAY)) {
                    ESP_LOGI(TAG, "Se sale de TCP correctamente");
                }

                wifi_deinit_sta();

                break;
            }
            
            /****************************************************************/
            /**************************  BLE  *******************************/
            /****************************************************************/
            case PROTOCOL_BLE: {
                
                ESP_LOGI(TAG, "esperando conexión BLE para iniciar tasks...");

                // Semáforo se libera cuando se escribe configuración en charact. C de BLE.
                // Mientras tanto queda bloqueado aquí.
                if (xSemaphoreTake(semaphore, portMAX_DELAY)) {
                    ESP_LOGI(TAG, "toma semáforo, empieza recepción BLE");          
                }

                // Da tiempo a la Raspberry para estar lista
                vTaskDelay(5000 / portTICK_PERIOD_MS);

                start_protocol_tasks(&PROTOCOLS[PROTOCOL_BLE]);

                // Punto de bloqueo
                if (xSemaphoreTake(semaphore, portMAX_DELAY)) {
                    ESP_LOGI(TAG, "se libera semáforo, se sale de BLE correctamente");
                }

                break;
            }

            // Protocolo inválido
            default: {
                ESP_LOGE(TAG, "Selección de protocolo inválida: %ld", current_config->protocol_conf);
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_restart();
                break;
            }
        }
    }
}