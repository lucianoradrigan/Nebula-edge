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
#include "esp_app_desc.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "freertos/semphr.h"

#include "nebulaedge_wifi.h"
#include "nebulaedge_mqtt.h"
#include "nebulaedge_udp.h"
#include "nebulaedge_tcp.h"
#include "nebulaedge_ble.h"
#include "nebulaedge_defs.h"
#include "board_pinout.h"
#include "nebulaedge_config_store.h"
#include "nebulaedge_busscan.h"
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

/* Semáforo de arranque: lo da la task que recibe la primera configuración y lo
 * toman las tasks de envío para no mandar nada antes de estar configuradas.
 * Es de la aplicación. Vivía en nebulaedge_defs.c, o sea que un componente era
 * dueño de una primitiva de sincronización de main.c; nebulaedge_ble lo recibe
 * inyectado con ble_set_start_semaphore(). */
SemaphoreHandle_t semaphore = NULL;

Config *current_config = NULL;

/* El bus I2C y su pinout son de la aplicación: es la única parte que sabe en
 * qué placa corre. El componente nebulaedge_i2c ya no los compila adentro. */
i2c_master_bus_handle_t bus_handle = NULL;
/* Expansor de IO de la im-v2. Vive acá por el mismo motivo que bus_handle: se
 * rehace en cada cambio de protocolo. Ver el bloque de la SD, más abajo. */
fxl6408_handle_t io_expander = NULL;
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
TaskHandle_t xHandleCollectInertial = NULL;       // Data_2, cada send_interval_s
TaskHandle_t xHandleCollectEnvironmental = NULL;  // Data_1, cada send_interval_s

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

/* Reintento de una productora cuando malloc() falla. Corto a propósito: es una
 * condición transitoria y volver a intentar enseguida es mejor que perder el
 * ritmo de muestreo. */
#define ALLOC_RETRY_MS 50

/* Antes de reiniciar por un protocol_conf inválido, para que el log alcance a
 * salir por el puerto serie. */
#define RESTART_LOG_FLUSH_MS 1000

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
#define GATE_INERTIAL_IDLE  (1 << 2)  // la task del flujo rápido (Data_2) ya está detenida
#define GATE_ENV_IDLE       (1 << 3)  // la task del flujo lento (Data_1) ya está detenida
/* Un bit por protocolo, no uno para los cuatro.
 *
 * Con un bit compartido la pausa no podía distinguir QUÉ task lo había puesto.
 * La del protocolo que se está abandonando se auto-suspende, y si alcanzaba a
 * poner el bit compartido después del resume, la pausa siguiente creía que la
 * task ACTIVA ya se había detenido y cerraba el transporte por debajo de ella.
 * Con un bit propio cada una, el estado de una task suspendida no le miente a
 * nadie. */
#define GATE_SEND_IDLE(id)  ((EventBits_t)1 << (4 + (id)))   // bits 4..7
#define GATE_RSP_IDLE(id)   ((EventBits_t)1 << (8 + (id)))   // bits 8..11
#define GATE_ALL_SEND_IDLE  ((EventBits_t)0x0F << 4)
#define GATE_ALL_RSP_IDLE   ((EventBits_t)0x0F << 8)

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

/* Escaneo de buses al arrancar: diagnóstico de bring-up, APAGADO por defecto.
 *
 * No queda encendido porque cuesta y porque mueve cosas: el barrido I2C son 112
 * sondeos con su timeout, varios segundos con el bus vacío, y el SPI baja chip
 * selects del expansor de a uno.
 *
 * BUS_SCAN_CS_MASK excluye el IO1 a propósito: en la im-v2 va a un RELÉ, y
 * bajarlo lo acciona de verdad. 0xFD son los otros siete IO. */
/* Persistencia local en microSD. ENCENDIDA.
 *
 * El pinout es el del bringup de la IM-V2 (CS en el IO0 del expansor FXL6408).
 * Encenderla dejaba el chip en un bucle de reset por INTERRUPT watchdog
 * (rst:0x8, TG1WDT_SYS_RST) hasta que se encontró la causa, que no era la
 * contención en el bus I2C sino un corrimiento indefinido dentro de SDSPI: está
 * contada en sd_mount(), en nebulaedge_microsd.c.
 *
 * Verificada en banco después del arreglo, con los cuatro protocolos en deep
 * sleep: 18 montajes, 250 líneas escritas, cero reinicios por watchdog y cero
 * fallos de montaje. El caso de control en modo continuo monta la tarjeta y no
 * escribe nada, que es lo que corresponde. Con la tarjeta sin responder, el
 * montaje falla con ESP_ERR_TIMEOUT, el firmware sigue transmitiendo sin
 * respaldo local y NO intenta formatear: format_if_mount_failed solo entra si
 * la tarjeta contesta pero no trae una FAT válida.
 *
 * OJO: el montaje va con .format_if_mount_failed en true, heredado del bringup.
 * Hace que una tarjeta virgen sirva sin prepararla a mano, pero también
 * formatea una que no monte por cualquier otro motivo. */
#define SD_PERSISTENCE      1

#define BUS_SCAN_AT_BOOT    0
#define BUS_SCAN_CS_MASK    0xFD

/* Prueba de banco de la microSD al arrancar. APAGADA.
 *
 * Monta, escribe, relee y desmonta antes de BLE, WiFi, sensores y tasks, con
 * nadie más usando los buses. Sirvió para acorralar el reset por interrupt
 * watchdog (con nada más corriendo, seguía cayéndose: eso descartó la
 * contención) y queda acá para validar una tarjeta nueva sin tener que levantar
 * el resto del firmware. */
#define SD_SELFTEST_AT_BOOT 0

#if SD_PERSISTENCE || SD_SELFTEST_AT_BOOT
/* Pinout de la microSD de esta placa. Lo piden el montaje del bucle de
 * protocolos y la prueba de banco, así que se arma en un solo lugar. Lee
 * io_expander al ser llamada: los dos llamadores lo inicializan antes. */
static sd_pins_t board_sd_pins(void) {
    return (sd_pins_t){
        .mosi_io          = PIN_NUM_MOSI,
        .clk_io           = PIN_NUM_CLK,
        .miso_io          = PIN_NUM_MISO,
        .spi_host         = SD_SPI_HOST,
        .max_freq_khz     = SD_MAX_FREQ_KHZ,
        .cs_expander      = io_expander,
        .cs_expander_pin  = SD_CS_EXPANDER_PIN,
        .format_if_mount_failed = true,
    };
}

/* Deja los otros dispositivos del bus SPI deseleccionados antes de hablarle a
 * la tarjeta. Qué cuelga de cada IO lo sabe la placa, no el componente. */
static void sd_deselect_other_spi_devices(void) {
    static const uint8_t disabled_pins[] = SPI_DISABLED_EXPANDER_PINS;
    for (size_t i = 0; i < sizeof(disabled_pins) / sizeof(disabled_pins[0]); i++) {
        fxl6408_config_output(io_expander, disabled_pins[i], true, false);
    }
}
#endif

#if SD_SELFTEST_AT_BOOT
/* Levanta el bus I2C y el expansor solo para la prueba, y los deja como los
 * encontró: el bucle de protocolos rehace el bus desde cero más adelante, y el
 * handle del expansor no sobrevive a que se destruya el bus. */
static void sd_selftest_at_boot(void) {
    ESP_ERROR_CHECK(i2c_master_init(&bus_handle, &board_i2c));

    if (fxl6408_init(bus_handle, FXL6408_I2C_ADDR, &io_expander) != ESP_OK) {
        ESP_LOGE(TAG, "Prueba de banco de la SD: el expansor de IO no responde");
        i2c_master_deinit(&bus_handle);
        return;
    }

    sd_deselect_other_spi_devices();

    const sd_pins_t sd_pins = board_sd_pins();
    sd_selftest(&sd_pins);

    fxl6408_del(io_expander);
    io_expander = NULL;
    i2c_master_deinit(&bus_handle);
}
#endif

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
 * esperando el intervalo entero: con send_interval_s en 10 s o más, pausar
 * tardaría eso en completarse. */
static void sensor_gate_sleep(uint32_t ms) {
    /* El +1 redondea hacia arriba, y con ms = 0 (modo sin espera) deja
     * exactamente un tick: lo mínimo para ceder la CPU. Ver
     * packet_interval_s() para por qué no puede ser cero de verdad. */
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
    if (send_task[id]) expected |= GATE_SEND_IDLE(id);
    pause_data_tasks(expected);
}

/* Deep sleep: lo llama la task de ENVÍO, así que detiene las productoras y la
 * task de respuesta. A la de envío no se la espera — es la que está acá. */
static void pause_for_deep_sleep(protocol_t id) {
    EventBits_t expected = collect_idle_bits();
    if (response_task[id]) expected |= GATE_RSP_IDLE(id);
    pause_data_tasks(expected);
}

/* Devuelve el permiso de producir. Limpia también los bits de "detenida" para
 * que la próxima pausa no los lea de la vuelta anterior. */
static void resume_collect_tasks(void) {
    if (sensor_gate == NULL) {
        return;
    }

    xEventGroupClearBits(sensor_gate, GATE_PAUSE_REQ | GATE_INERTIAL_IDLE |
                                  GATE_ENV_IDLE | GATE_ALL_SEND_IDLE | GATE_ALL_RSP_IDLE);
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
     * /data; en BLE, a la característica B.
     *
     * Devuelve esp_err_t en los cuatro. Antes era void porque cada transporte
     * reportaba distinto -esp_err_t en BLE, msg_id en MQTT, nada en UDP y
     * TCP-, así que un fallo de envío se veía en MQTT y era invisible en los
     * otros tres. */
    esp_err_t (*send_data)(const uint8_t *data, size_t size);

    /* Manda UN ACK de config ya serializado. El caller repite la llamada
     * `control_repeats` veces. En MQTT va al tópico /config/ack; en BLE, a la
     * característica D, que es distinta de la de telemetría. */
    esp_err_t (*send_ack)(const Config *cfg, const uint8_t *buf, size_t size);

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

static esp_err_t mqtt_send_data(const uint8_t *data, size_t size) {
    char topic_data[128];
    snprintf(topic_data, sizeof(topic_data), "/topic/nebulaedge/%s/data", current_config->id_device);

    /* esp-mqtt devuelve el msg_id, o negativo si falló. Se traduce a esp_err_t
     * para que los cuatro transportes hablen el mismo idioma. */
    int msg_id = mqtt_publish(topic_data, data, size, 0);
    if (msg_id < 0) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG_SEND_MQTT, "Paquete publicado por MQTT. msg_id=%d", msg_id);
    return ESP_OK;
}

static esp_err_t mqtt_send_ack(const Config *cfg, const uint8_t *buf, size_t size) {
    char topic_ack[128];
    snprintf(topic_ack, sizeof(topic_ack), "/topic/nebulaedge/%s/config/ack", cfg->id_device);
    return mqtt_publish(topic_ack, buf, size, 0) < 0 ? ESP_FAIL : ESP_OK;
}

/* La config llega cruda por la cola y se desempaqueta acá, igual que en BLE:
 * los componentes de transporte mueven bytes y no conocen el formato de cable. */
static Config *mqtt_recv_config(void) {
    packet_t pkt;

    if (xQueueReceive(xQueueConfig, &pkt, pdMS_TO_TICKS(TASK_POLL_MS)) != pdTRUE) {
        return NULL;    // nada todavía; la task vuelve a mirar la compuerta
    }

    if (pkt.data == NULL || pkt.size == 0) {
        free(pkt.data);
        return NULL;
    }

    Config *cfg = config__unpack(NULL, pkt.size, pkt.data);
    free(pkt.data);

    if (cfg == NULL) {
        ESP_LOGW(TAG_GET_RSP_MQTT, "Error al desempaquetar la configuración MQTT");
    }
    return cfg;
}

/* -------------------------------------------------------------------- UDP */

static esp_err_t udp_send_data(const uint8_t *data, size_t size) {
    return nebulaedge_udp_send(data, size);
}

static esp_err_t udp_send_ack(const Config *cfg, const uint8_t *buf, size_t size) {
    (void)cfg;    // UDP no direcciona dentro de la conexión
    return nebulaedge_udp_send(buf, size);
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

static esp_err_t tcp_send_data(const uint8_t *data, size_t size) {
    return nebulaedge_tcp_send(data, size);
}

static esp_err_t tcp_send_ack(const Config *cfg, const uint8_t *buf, size_t size) {
    (void)cfg;
    return nebulaedge_tcp_send(buf, size);
}

static Config *tcp_recv_config(void) {
    uint8_t buffer[CONFIG_RECV_BUF_BYTES];

    size_t len_recv = nebulaedge_tcp_receive(buffer, sizeof(buffer));
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

static esp_err_t ble_send_data(const uint8_t *data, size_t size) {
    return ble_set_char_with_notify(IDX_CHAR_VAL_B_BLE, data, size);
}

static esp_err_t ble_send_ack(const Config *cfg, const uint8_t *buf, size_t size) {
    (void)cfg;

    /* Char D, distinta de la de telemetría: el ACK queda ahí legible, así que
     * si se pierde la notificación el servidor lo reconcilia leyéndolo
     * (BleTransport.confirm_config_applied). */
    esp_err_t ret = ble_set_char_with_notify(IDX_CHAR_VAL_D_BLE, buf, size);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG_GET_RSP_BLE, "BLE: ACK no se pudo notificar (%s); queda legible en char D.",
                 esp_err_to_name(ret));
    }
    return ret;
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
        .recv_config = tcp_recv_config, .close = nebulaedge_tcp_close_socket,
        .control_repeats = CONTROL_PKT_REDUNDANCY, .ack_drain_ms = ACK_DRAIN_MS,
    },
    [PROTOCOL_BLE] = {
        .name = "BLE", .id = PROTOCOL_BLE,
        .send_data = ble_send_data, .send_ack = ble_send_ack,
        .send_tag = TAG_SEND_BLE, .rsp_tag = TAG_GET_RSP_BLE,
        .recv_config = ble_recv_config, .close = ble_close,
        /* Una sola vez: el link layer de BLE ya retransmite lo que se encoló, y
         * ble_set_char_with_notify() reintenta por su cuenta si el stack rechaza el
         * envío por congestión. */
        .control_repeats = 1,
        /* BANCO: en 0. Este delay se aplica ANTES de cada paquete en modo
         * discontinuo, así que en 1000 techaba el envío BLE a 1 paquete/s y
         * hacía ver "lecturas lentísimas" que no eran del sensor ni del
         * intervalo configurado. El respiro que sí hace falta es el de abajo,
         * que corre una sola vez por ventana. */
        .pre_send_delay_ms = 0,
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
    int flag_sent = 0;
    for (int i = 0; i < proto->control_repeats; i++) {
        if (proto->send_data((const uint8_t *)DEEP_SLEEP_FLAG, DEEP_SLEEP_FLAG_LEN) == ESP_OK) {
            flag_sent++;
        }
        vTaskDelay(pdMS_TO_TICKS(CONTROL_PKT_REDUNDANCY_DELAY_MS));
    }
    if (flag_sent == 0) {
        ESP_LOGW(TAG, "%s: el aviso de deep sleep no salió; el servidor va a ver una caída",
                 proto->name);
    }

    proto->close();

    config_store_save(current_config);
    
    /* Cada driver se saca del bus solo. Tiene que ser ANTES del
     * i2c_master_deinit: borrar el bus invalida los handles de sus slaves. */
    bmm350_deinit();
    bme688_deinit();
    bmi270_deinit();
    /* La SD también, y por el mismo motivo que en el cambio de protocolo: su
     * chip select cuelga del expansor, que a su vez cuelga de este bus. Acá
     * importa además cerrar la FAT antes de cortar la alimentación, para no
     * dejar el sistema de archivos marcado como sucio; los datos ya están en la
     * tarjeta porque sdstorage hace fflush() por línea. */
#if SD_PERSISTENCE
    sd_unmount();
    fxl6408_del(io_expander);
    io_expander = NULL;
#endif
    i2c_master_deinit(&bus_handle);
    
    vTaskDelay(pdMS_TO_TICKS(DEEP_SLEEP_SETTLE_MS));
    
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

    int sent = 0;
    for (int i = 0; i < proto->control_repeats; i++) {
        if (proto->send_ack(cfg, buf, size) == ESP_OK) {
            sent++;
        }
        vTaskDelay(pdMS_TO_TICKS(CONTROL_PKT_REDUNDANCY_DELAY_MS));
    }

    free(buf);

    /* Con que salga una copia alcanza; el servidor descarta las repetidas. Que
     * no salga ninguna sí importa: el servidor va a dar la config por no
     * aplicada y cerrar la sesión. */
    if (sent == 0) {
        ESP_LOGE(TAG, "%s: NINGUNA copia del ACK de config pudo enviarse", proto->name);
    }
    else {
        ESP_LOGI(TAG, "%s: ACK de config enviado (%d/%d copias).",
                 proto->name, sent, proto->control_repeats);
    }
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

/* Intervalo entre paquetes. Lo comparten las dos productoras: Data_1 y Data_2
 * salen al mismo ritmo, como en el contrato original.
 *
 * send_interval_s = 0 significa "sin espera": producir tan rápido como dejen
 * el bus I2C y el transporte. No es una espera de cero absoluto —
 * sensor_gate_sleep() siempre cede al menos un tick— porque una task de
 * prioridad 2 girando sin ceder la CPU mata a la idle de su core y dispara el
 * watchdog. Con CONFIG_FREERTOS_HZ=100 ese piso son 10 ms, o sea un techo de
 * ~100 paquetes/s por flujo; en la práctica manda antes la lectura de los
 * sensores o la contrapresión de xQueueData. */
static uint32_t packet_interval_s(void) {
    if (!current_config) {
        return 1;
    }
    return current_config->send_interval_s;
}

// GEN_DATA: BMI270 + BMM350 -> paquete Data_2, cada send_interval_s.
void vTaskCollectInertial(void *pvParameters) {
    for (;;) {
        // Punto seguro: acá no hay memoria reservada ni bus tomado.
        sensor_gate_wait(GATE_INERTIAL_IDLE);

        Data2 data_2 = DATA_2__INIT;
        data_2.id_device = device_id();
        data_2.config_version_applied = current_config ? current_config->config_version : 0;
        data_2.time_client = device_clock_now_s();

        /* Recogida de datos inerciales. Cada driver devuelve su propio tipo en
         * unidades físicas; traducirlo al mensaje protobuf es trabajo de acá,
         * que es la única parte que conoce el formato de cable.
         *
         * Ningún sensor es obligatorio: el que falla deja sus ejes en cero y
         * el paquete sale igual, como en el contrato original de Data_2. El
         * costo es que un cero no se distingue de una medición legítima de
         * cero, así que si hay un sensor caído hay que saberlo por el log.
         *
         * BANCO: esto importa en la IM-V2, que no tiene el BMI270 poblado. Con
         * el acelerómetro obligatorio no salía NINGÚN Data_2 y el flujo rápido
         * desaparecía entero. */
        bmi270_reading_t imu;
        if (bmi270_read(&imu) == ESP_OK) {
            data_2.acc_x = imu.acc_x_ms2;
            data_2.acc_y = imu.acc_y_ms2;
            data_2.acc_z = imu.acc_z_ms2;
            data_2.gyr_x = imu.gyr_x_rads;
            data_2.gyr_y = imu.gyr_y_rads;
            data_2.gyr_z = imu.gyr_z_rads;
        }

        bmm350_reading_t mag;
        if (bmm350_read(&mag) == ESP_OK) {
            data_2.mag_x = mag.mag_x_ut;
            data_2.mag_y = mag.mag_y_ut;
            data_2.mag_z = mag.mag_z_ut;
        }

        // Serializa el mensaje protobuf, con el byte de tipo por delante
        packet_t packet;
        packet.size = data_2__get_packed_size(&data_2) + 1;
        packet.data = malloc(packet.size);
        if (packet.data == NULL) {
            ESP_LOGE(TAG_COLLECT_INERTIAL, "Error: no se pudo reservar memoria para el paquete");
            vTaskDelay(pdMS_TO_TICKS(ALLOC_RETRY_MS));
            continue;
        }
        packet.data[0] = 0x02;
        data_2__pack(&data_2, packet.data + 1);
        ESP_LOGI(TAG_COLLECT_INERTIAL, "Paquete Data_2 generado");

        enqueue_packet(&packet, TAG_COLLECT_INERTIAL);

        // Ritma la producción. Despierta antes si se pide una pausa.
        sensor_gate_sleep(packet_interval_s() * 1000U);
    }
}

// GEN_DATA: BME688 -> paquete Data_1, cada send_interval_s.
void vTaskCollectEnvironmental(void *pvParameters) {
    for (;;) {
        // Punto seguro: acá no hay memoria reservada ni bus tomado.
        sensor_gate_wait(GATE_ENV_IDLE);

        Data1 data_1 = DATA_1__INIT;
        data_1.id_device = device_id();
        data_1.config_version_applied = current_config ? current_config->config_version : 0;
        data_1.time_client = device_clock_now_s();

        /* Recogida de datos ambientales. El driver devuelve su propio tipo en
         * unidades físicas; traducirlo al mensaje protobuf es trabajo de acá,
         * que es la única parte que conoce el formato de cable.
         *
         * Si la lectura falla el paquete sale igual, con los campos en cero,
         * igual que en la task rápida y que en el contrato original. El costo
         * es que ese cero no se distingue de una medición legítima de cero:
         * el warning de acá es la única señal de que el sensor no respondió. */
        bme688_reading_t ambient;
        if (bme688_read(&ambient) == ESP_OK) {
            data_1.temperature = ambient.temperature_c;
            data_1.press       = ambient.pressure_pa;
            data_1.hum         = ambient.humidity_pct;
            data_1.co          = ambient.gas_resistance_ohm;
        }
        else {
            ESP_LOGW(TAG_COLLECT_ENV, "Lectura del BME688 falló, el paquete va con ceros");
        }

        // Serializa el mensaje protobuf, con el byte de tipo por delante
        packet_t packet;
        packet.size = data_1__get_packed_size(&data_1) + 1;
        packet.data = malloc(packet.size);
        if (packet.data == NULL) {
            ESP_LOGE(TAG_COLLECT_ENV, "Error: no se pudo reservar memoria para el paquete");
            vTaskDelay(pdMS_TO_TICKS(ALLOC_RETRY_MS));
            continue;
        }
        packet.data[0] = 0x01;
        data_1__pack(&data_1, packet.data + 1);
        ESP_LOGI(TAG_COLLECT_ENV, "Paquete Data_1 generado");

        enqueue_packet(&packet, TAG_COLLECT_ENV);

        // Ritma la producción. Despierta antes si se pide una pausa.
        sensor_gate_sleep(packet_interval_s() * 1000U);
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
        sensor_gate_wait(GATE_SEND_IDLE(proto->id));

        /* GATE_RUN es un bit GLOBAL: resume_collect_tasks() lo levanta para
         * todas las tasks estacionadas, también para la de envío de un
         * protocolo que ya cerró su transporte. Sin esta guarda esa task volvía
         * a consumir de xQueueData —la misma cola que la nueva— y mandaba por un
         * socket cerrado: "socket is closed", "MQTT client is not initialized".
         * Y el paquete se perdía igual, porque lo liberaba de todos modos: con N
         * protocolos ya usados llegaba 1 de cada N.
         *
         * Antes del refactor esto no pasaba porque el cambio de protocolo hacía
         * vTaskSuspend() sobre la task de envío concreta y solo su propia rama
         * de app_main la reanudaba. Al pasar a la compuerta cooperativa se
         * perdió esa selectividad, porque el bit es uno para todas.
         *
         * Auto-suspenderse es seguro, igual que en la task de respuesta: ocurre
         * en un punto que esta misma task eligió y sin nada tomado.
         * start_protocol_tasks() la reanuda si su protocolo vuelve a activarse. */
        if (current_config && current_config->protocol_conf != proto->id) {
            /* Declarar el bit propio antes de dormirse no es una formalidad: la
             * task de respuesta reemplaza current_config ANTES de llamar a
             * pause_for_protocol_change(), así que esta guarda dispara mientras
             * esa pausa todavía no empezó. Una task suspendida no vuelve nunca
             * al punto seguro, así que sin esto la pausa esperaba en vano los
             * 5 s enteros de GATE_PAUSE_TIMEOUT_MS en cada cambio de protocolo.
             *
             * Y es cierto: suspendida es la forma más fuerte de estar detenida.
             * El bit es propio de este protocolo, así que no le habla por la
             * task activa. */
            xEventGroupSetBits(sensor_gate, GATE_SEND_IDLE(proto->id));
            ESP_LOGI(proto->send_tag, "Protocolo inactivo: se suspende la task de envío");
            vTaskSuspend(NULL);
            continue;
        }

        // Con timeout, para poder volver acá arriba si se pide una pausa.
        if (xQueueReceive(xQueueData, &packet, pdMS_TO_TICKS(TASK_POLL_MS)) != pdTRUE) {
            continue;
        }

        // Delay de precaución en modo discontinuo. Solo BLE lo declara.
        if (current_config->sleep_time_s > 0 && proto->pre_send_delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(proto->pre_send_delay_ms));
        }

        esp_err_t send_ret = proto->send_data(packet.data, packet.size);
        if (send_ret != ESP_OK) {
            ESP_LOGE(proto->send_tag, "Paquete perdido: el envío falló (%s)",
                     esp_err_to_name(send_ret));
        }

        // Respaldo local, solo en modo deep sleep.
        if (current_config->sleep_time_s > 0) {
            sdstorage_write_packet(packet.data, packet.size);
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
        sensor_gate_wait(GATE_RSP_IDLE(proto->id));

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







/****************************************************************/
/******************* DIAGNÓSTICO DE ARRANQUE ********************/
/****************************************************************/

/* Cada cuánto se vuelve a informar la memoria libre una vez el sistema está en
 * régimen. Sirve para cazar fugas: si el número baja despacio y no se
 * recupera, algo no se está liberando. Poner 0 apaga el reporte periódico. */
#define MEMORY_LOG_PERIOD_MS 60000

static const char *reset_reason_name(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON:  return "encendido";
        case ESP_RST_SW:       return "esp_restart()";
        case ESP_RST_PANIC:    return "PANIC o excepcion";
        case ESP_RST_INT_WDT:  return "watchdog de interrupciones";
        case ESP_RST_TASK_WDT: return "watchdog de tasks";
        case ESP_RST_WDT:      return "otro watchdog";
        case ESP_RST_DEEPSLEEP: return "salida de deep sleep";
        case ESP_RST_BROWNOUT: return "brownout (caida de tension)";
        case ESP_RST_EXT:      return "reset externo";
        default:               return "desconocida";
    }
}

/* Informa memoria libre en un punto del arranque.
 *
 * LOS TRES NÚMEROS NO SON EL MISMO
 *     libre     suma de todos los huecos del heap interno.
 *     mayor     el hueco contiguo más grande. Este es el que decide si un
 *               malloc grande entra: se puede tener 90 KB libres y que el
 *               mayor hueco sea de 20 KB si el heap está fragmentado.
 *     minimo    el valor más bajo que alcanzó `libre` desde el arranque.
 *               Es la marca de agua: dice cuán cerca se estuvo de quedarse
 *               sin memoria aunque ahora sobre.
 *
 * Se mide solo la RAM interna (MALLOC_CAP_INTERNAL): es la que compite de
 * verdad. Si algún día se agrega PSRAM, conviene informarla aparte. */
static void log_memory(const char *stage) {
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    ESP_LOGI(TAG, "MEM [%s] libre=%u B  mayor=%u B  minimo=%u B",
             stage,
             (unsigned)heap_caps_get_free_size(caps),
             (unsigned)heap_caps_get_largest_free_block(caps),
             (unsigned)heap_caps_get_minimum_free_size(caps));
}

static void memory_timer_cb(void *arg) {
    log_memory("regimen");
}

/* Arranca el reporte periódico de memoria.
 *
 * Va por esp_timer y no dentro de una task para no tocar ningún bucle de
 * envío ni de sensores: el callback corre en la task del timer. */
static void start_memory_monitor(void) {
    if (MEMORY_LOG_PERIOD_MS == 0) {
        return;
    }
    const esp_timer_create_args_t args = {
        .callback = memory_timer_cb,
        .name = "mem_log",
    };
    esp_timer_handle_t timer = NULL;
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        esp_timer_start_periodic(timer, (uint64_t)MEMORY_LOG_PERIOD_MS * 1000);
    }
}

/* Identifica el build que está corriendo, antes de cualquier otra cosa.
 *
 * POR QUÉ EXISTE
 *     Sin esto no hay forma de saber qué código tiene la placa. La versión
 *     sale de `git describe`, así que el log dice exactamente qué commit se
 *     flasheó: eso es lo que separa "no anda" de "no anda EN ESTE commit".
 *
 * El tamaño de flash que informa es el DETECTADO en el chip, que puede no
 * coincidir con el configurado en sdkconfig. Si el configurado es menor, la
 * tabla de particiones está desaprovechando el chip. */
static void log_boot_banner(void) {
    const esp_app_desc_t *app = esp_app_get_description();

    ESP_LOGI(TAG, "================ NebulaEdge ================");
    ESP_LOGI(TAG, "version  %s", app->version);
    ESP_LOGI(TAG, "build    %s %s", app->date, app->time);
    ESP_LOGI(TAG, "ESP-IDF  %s", app->idf_ver);
    ESP_LOGI(TAG, "reinicio por: %s", reset_reason_name(esp_reset_reason()));

    uint32_t flash_bytes = 0;
    if (esp_flash_get_size(NULL, &flash_bytes) == ESP_OK) {
        ESP_LOGI(TAG, "flash detectada %u KB (el build asume %s)",
                 (unsigned)(flash_bytes / 1024), CONFIG_ESPTOOLPY_FLASHSIZE);
    }

    log_memory("arranque");
    ESP_LOGI(TAG, "============================================");
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
    /* Las dos se reanudan de verdad y no solo por la compuerta: la de respuesta
     * porque se auto-suspende al salir de su protocolo, y la de envío por la
     * misma razón desde que tiene la guarda de protocolo inactivo. vTaskResume
     * sobre una task que no está suspendida no hace nada, así que sirve igual
     * la primera vez, cuando se acaban de crear. */
    vTaskResume(response_task[proto->id]);
    vTaskResume(send_task[proto->id]);

    /* Un punto de medida por protocolo activado. Acá ya levantaron WiFi y, si
     * corresponde, el cliente MQTT, así que es el número que de verdad importa:
     * lo que queda de heap con los radios arriba. */
    log_memory(proto->rsp_tag);
}

void app_main() {

    /****************************************************************/
    /***********************  INICIALIZACIÓN ************************/
    /****************************************************************/

    /* Lo primero de todo: qué build es este y con cuánta memoria arranca. */
    log_boot_banner();

#if SD_SELFTEST_AT_BOOT
    /* Diagnóstico, apagado por defecto. Ver el bloque de SD_SELFTEST_AT_BOOT. */
    sd_selftest_at_boot();
#endif

    // Inicializa NVS
    ESP_ERROR_CHECK(nvs_flash_init());

    // Obtiene MAC BT del dispositivo
    device_id_init();

    /* Semáforo binario, arranca cerrado. Lo libera la task de respuesta al
     * cambiar de protocolo, y lo espera app_main en cada rama del switch. */
    semaphore = xSemaphoreCreateBinary();

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
    xQueueConfig = xQueueCreate(5, sizeof(packet_t));
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
    log_memory("BLE arriba");

    /* Desde acá en adelante, un reporte de memoria cada MEMORY_LOG_PERIOD_MS
     * para poder ver si el heap baja con el tiempo. */
    start_memory_monitor();

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

    /* PERSISTENCIA EN SD
     *
     * El chip select cuelga del IO0 del expansor FXL6408 y el pinout es el del
     * bringup de la IM-V2, verificado contra esa placa.
     *
     * El montaje está más abajo, dentro del bucle de protocolos, y no acá: el
     * expansor cuelga del bus I2C, que se rehace en cada cambio de protocolo,
     * así que hay que volver a registrarlo igual que los tres sensores.
     *
     * El respaldo local solo se escribe en modo deep sleep: las tasks de envío
     * llaman a sdstorage_write_packet() cuando sleep_time_s > 0. Con
     * SD_PERSISTENCE en 0 esa llamada corta de inmediato en sd_is_mounted() y
     * no escribe nada, sin que el resto del código lo note. */

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
        /* La SD sale antes que el bus: su chip select vive en el expansor, que
         * a su vez cuelga del bus I2C que estamos por borrar. */
#if SD_PERSISTENCE
        sd_unmount();
        fxl6408_del(io_expander);
        io_expander = NULL;
#endif
        i2c_master_deinit(&bus_handle);

        // Inicializa el bus con el pinout de esta placa
        ESP_ERROR_CHECK(i2c_master_init(&bus_handle, &board_i2c));

#if BUS_SCAN_AT_BOOT
        /* Diagnóstico de bring-up, apagado por defecto. Ver nebulaedge_busscan.h.
         * El escaneo SPI mueve IO del expansor, y en la im-v2 el IO1 va a un
         * relé: por eso la máscara es explícita y excluye ese bit. */
        nebulaedge_busscan_i2c(bus_handle);
        nebulaedge_busscan_spi(bus_handle, BUS_SCAN_CS_MASK);
#endif

        /* Cada driver se agrega al bus y se configura. La dirección I2C la
         * conoce cada uno; acá solo van los parámetros de medición. */
        bmm350_init(bus_handle, 400, 4);
        bme688_init(bus_handle, current_config->bme688_sampling, current_config->bme688_sampling, current_config->bme688_sampling);
        bmi270_init(bus_handle, current_config->acc_sampling, 4, 8, 400, current_config->gyro_sensibility); 

        /* Expansor de IO y microSD. Poner SD_PERSISTENCE en 1 es todo lo que
         * hace falta para reactivar la persistencia local. */
#if SD_PERSISTENCE
        if (fxl6408_init(bus_handle, FXL6408_I2C_ADDR, &io_expander) != ESP_OK) {
            ESP_LOGW(TAG, "Expansor de IO no disponible, se continúa sin SD");
        } else {
            sd_deselect_other_spi_devices();

            const sd_pins_t sd_pins = board_sd_pins();
            esp_err_t sd_ret = sd_mount(&sd_pins);
            if (sd_ret != ESP_OK) {
                ESP_LOGW(TAG, "SD no disponible, se continúa sin persistencia local: %s", esp_err_to_name(sd_ret));
            }
        }
#endif
    
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
                vTaskDelay(pdMS_TO_TICKS(SERVER_READY_MS));

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
                nebulaedge_tcp_open_socket(&params);
                // Conecta
                if (nebulaedge_tcp_connect() != 0) {
                    // Cierra el socket
                    nebulaedge_tcp_close_socket();
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
                vTaskDelay(pdMS_TO_TICKS(SERVER_READY_MS));

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
                vTaskDelay(pdMS_TO_TICKS(RESTART_LOG_FLUSH_MS));
                esp_restart();
                break;
            }
        }
    }
}