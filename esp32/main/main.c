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
#include "nebulaedge_model_store.h"
#include "infer.h"

#include "schema.pb-c.h"
#include "app.h"
#include "protocols.h"
#include "gate.h"
#include "power.h"

QueueHandle_t xQueueConfig = NULL;
QueueHandle_t xQueueData = NULL;
QueueHandle_t xQueueConfigBle = NULL;
QueueHandle_t xQueueModel = NULL;       // manifiesto y trozos del modelo (INFER)

/* Dos semáforos binarios, con dueños distintos.
 *
 * ble_start_semaphore: lo da nebulaedge_ble cuando el servidor escribe en la
 * característica C, y lo toma ble_open() antes de arrancar las tasks de BLE.
 * El componente lo recibe inyectado con ble_set_start_semaphore().
 *
 * protocol_change_semaphore: lo da la task de respuesta al aplicar una config
 * nueva, y lo toma app_main para pasar al protocolo siguiente.
 *
 * Antes eran uno solo. Con BLE corriendo, una reconexión del servidor vuelve a
 * escribir la característica C, y ese "start" despertaba a app_main como si
 * fuera un cambio de protocolo: rehacía el bus I2C con las tasks vivas y
 * después se quedaba bloqueado para siempre en ble_open(). */
SemaphoreHandle_t ble_start_semaphore = NULL;
SemaphoreHandle_t protocol_change_semaphore = NULL;

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



/* Si la tabla de protocolos tiene una entrada para el protocol_conf de `c`.
 * Las configs con un protocolo desconocido se rechazan al llegar, y no al
 * intentar abrir el transporte. */
static bool protocol_conf_is_valid(const Config *c) {
    return c->protocol_conf >= 0 && c->protocol_conf < PROTOCOL_COUNT;
}

/* Escaneo de buses al arrancar: diagnóstico de bring-up, APAGADO por defecto.
 *
 * No queda encendido porque cuesta y porque mueve cosas: el barrido I2C son 112
 * sondeos con su timeout, varios segundos con el bus vacío, y el SPI baja chip
 * selects del expansor de a uno.
 *
 * BUS_SCAN_CS_MASK deja fuera DOS IO a propósito, y los dos por motivos
 * medidos en banco:
 *
 *     IO1  va a un RELÉ en la im-v2, y bajarlo lo acciona de verdad.
 *     IO0  es el chip select de la microSD. Incluirlo hace que el escaneo le
 *          hable a la tarjeta y la deje a mitad de comando: el montaje
 *          posterior falla con ESP_ERR_INVALID_CRC (0x109) y la SD queda sin
 *          servir hasta el siguiente arranque. Verificado: con 0xFD no monta,
 *          con 0xFC monta. Para diagnosticar la tarjeta está sd_selftest(),
 *          que es lo que corresponde.
 *
 * Quedan los seis IO restantes: 0xFC. */

#define BUS_SCAN_AT_BOOT    0
#define BUS_SCAN_CS_MASK    0xFC

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
    ack.model_version_active = model_store_active_version();

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
        ESP_LOGD(tag, "Se ha insertado correctamente en la xQueue");
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

        if (!telemetry_enabled()) {
            sensor_gate_sleep(TASK_POLL_MS);
            continue;
        }

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
        packet.data[0] = PACKET_TYPE__PACKET_TYPE_DATA_2;
        data_2__pack(&data_2, packet.data + 1);
        ESP_LOGD(TAG_COLLECT_INERTIAL, "Paquete Data_2 generado");

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

        if (!telemetry_enabled()) {
            sensor_gate_sleep(TASK_POLL_MS);
            continue;
        }

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
        packet.data[0] = PACKET_TYPE__PACKET_TYPE_DATA_1;
        data_1__pack(&data_1, packet.data + 1);
        ESP_LOGD(TAG_COLLECT_ENV, "Paquete Data_1 generado");

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

        /* Por dónde sale el ACK. Es igual al protocolo activo salvo cuando la
         * config entró por el canal de rescate, más abajo. */
        protocol_t ack_over = proto->id;

        /* CANAL DE RESCATE POR BLE
         *
         * ble_init() corre una sola vez en app_main y no se deshace nunca: el
         * anuncio BLE y la característica A quedan activos con cualquier
         * protocolo, así que el servidor puede escribir ahí cuando el
         * transporte principal dejó de responder. Es su única forma de
         * recuperar un device que ya no lo escucha.
         *
         * Lo que faltaba era que alguien vaciara la cola: solo la vaciaban
         * app_main al arrancar y la task de respuesta de BLE. Con UDP, TCP o
         * MQTT activos nadie la tocaba, se llenaba con cinco configs y el resto
         * se descartaba, así que el rescate no llegaba nunca. Medido en banco:
         * "cola BLE llena, descartando config" mientras el servidor esperaba
         * una conexión TCP que el device no iba a abrir.
         *
         * El sondeo va sin espera porque la de esta vuelta ya la hizo el
         * recv_config del protocolo activo. */
        if (!new_config && proto->id != PROTOCOL_BLE) {
            new_config = ble_config_from_queue(0);
            if (new_config) {
                ESP_LOGW(proto->rsp_tag, "Config recibida por BLE (canal de rescate)");
                /* El ACK tiene que volver por BLE: el servidor escribió en la
                 * característica A y está esperando en la D, no en el
                 * transporte activo. Mandarlo por el otro lado lo dejaría
                 * reintentando hasta darla por no aplicada. */
                ack_over = PROTOCOL_BLE;
            }
        }

        if (!new_config) {
            continue;
        }

        /* PONER EL RELOJ EN HORA, ANTES DE MIRAR LA VERSIÓN
         *
         * El servidor estampa `time_client` con la hora del momento en CADA
         * Config que arma (repository.py: time_cli = utc_epoch_now()), así que
         * cualquier config que llegue —nueva, repetida o vieja— trae hora
         * fresca. Antes solo se usaba la de la primera config por BLE, y las
         * demás se descartaban junto con la config: el reloj quedaba corriendo
         * libre desde el arranque, con una deriva medida en banco de ~0,75 s
         * por minuto (unos 45 s de atraso en una hora).
         *
         * Va acá arriba y no más abajo a propósito: si fuera después de las
         * comparaciones de versión, una config repetida —que es justamente el
         * caso de la resincronización periódica del servidor— se descartaría
         * sin que su hora llegara nunca al reloj.
         *
         * La excepción es una config RETENIDA de MQTT, que llega con
         * time_client = 0 (ver mqtt_recv_config): su hora es vieja. */
        if (new_config->time_client > 0) {
            device_clock_set(new_config->time_client);
        }

        /* Config más vieja que la aplicada: se rechaza con un ACK negativo, para
         * que el servidor sepa que llegó y que NO se aplicó. */
        if (current_config && new_config->config_version < current_config->config_version) {
            ESP_LOGW(proto->rsp_tag, "Config antigua: %ld < %ld",
                     (long)new_config->config_version, (long)current_config->config_version);
            send_config_ack(ack_over, new_config, false);
            config__free_unpacked(new_config, NULL);
            continue;
        }

        /* Protocolo fuera de la tabla: también se rechaza, y el device sigue
         * con la config que tiene. Antes se aplicaba, y app_main reiniciaba al
         * no encontrar el protocolo; como el reinicio borra la config de NVS,
         * el servidor volvía a mandar la misma por BLE y el device quedaba en
         * un loop de reinicios. */
        if (!protocol_conf_is_valid(new_config)) {
            ESP_LOGW(proto->rsp_tag, "Config v%ld con protocol_conf inválido (%ld): se rechaza",
                     (long)new_config->config_version, (long)new_config->protocol_conf);
            send_config_ack(ack_over, new_config, false);
            config__free_unpacked(new_config, NULL);
            continue;
        }

        /* Misma versión: se confirma y se descarta. No hay nada que cambiar. */
        if (!config_has_changed(current_config, new_config)) {
            ESP_LOGI(proto->rsp_tag, "La configuración recibida es la misma");
            send_config_ack(ack_over, new_config, true);
            config__free_unpacked(new_config, NULL);
            continue;
        }

        ESP_LOGI(proto->rsp_tag, "Configuración cambiada!");

        /* Antes de tocar current_config: la task de INFER corre en paralelo
         * y no debe seguir trabajando sobre un transporte que se va a cerrar.
         * Ver infer_freeze(). */
        infer_freeze();

        /* Reemplaza la configuración global y reinicia la ventana de deep sleep.
         *
         * La vieja NO se libera acá: la task de envío y las productoras siguen
         * corriendo hasta pause_for_protocol_change(), y leen current_config
         * sin lock (el tópico de MQTT, sleep_time_s, config_version). Si se
         * liberaba antes, podían leer memoria ya devuelta al heap. Se cambia
         * el puntero ahora —la task de envío necesita ver la config nueva para
         * notar el cambio de protocolo— y la vieja se libera después de la
         * pausa, cuando ya nadie la puede estar usando. */
        Config *old_config = current_config;
        current_config = new_config;
        data_window_count = 0;

        /* El ACK sale ANTES de detener nada, y por `proto->id` en vez de por el
         * protocolo de la config nueva: el transporte abierto sigue siendo
         * este, y el ACK es lo último que se manda antes de cerrarlo. */
        send_config_ack(ack_over, current_config, true);

        /* Detiene la task de envío y las productoras, y vacía la cola: los
         * paquetes del protocolo anterior no deben colarse al siguiente. */
        ESP_LOGI(proto->rsp_tag, "Se detienen las tasks del camino de datos");
        pause_for_protocol_change(proto->id);
        if (old_config) config__free_unpacked(old_config, NULL);

        // Le da tiempo al ACK de llegar antes de cerrar. BLE no cierra: 0.
        if (proto->ack_drain_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(proto->ack_drain_ms));
        }

        proto->close();

        // Señala a app_main que debe cambiar de protocolo.
        if (xSemaphoreGive(protocol_change_semaphore)) {
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

/* Credenciales con las que está levantada la WiFi en este momento.
 *
 * Se guardan para poder responder una sola pregunta al cerrar el ciclo de
 * protocolo: ¿la conexión que ya está arriba sirve para la configuración que
 * sigue? Nunca salen por el log, ni siquiera la longitud. */
static char s_wifi_ssid_en_uso[33];
static char s_wifi_pass_en_uso[65];

static void wifi_remember_credentials(const Config *cfg) {
    snprintf(s_wifi_ssid_en_uso, sizeof s_wifi_ssid_en_uso, "%s", cfg->ssid ? cfg->ssid : "");
    snprintf(s_wifi_pass_en_uso, sizeof s_wifi_pass_en_uso, "%s", cfg->passwd ? cfg->passwd : "");
}

static bool wifi_credentials_changed(const Config *cfg) {
    return strcmp(s_wifi_ssid_en_uso, cfg->ssid ? cfg->ssid : "") != 0
        || strcmp(s_wifi_pass_en_uso, cfg->passwd ? cfg->passwd : "") != 0;
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

    // Los dos semáforos arrancan cerrados. Ver su declaración, arriba.
    ble_start_semaphore = xSemaphoreCreateBinary();
    protocol_change_semaphore = xSemaphoreCreateBinary();

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
    /* Pocos lugares a propósito: el device pide un trozo a la vez, así que en
     * régimen nunca hay más de uno o dos esperando. */
    xQueueModel = xQueueCreate(4, sizeof(packet_t));

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
    mqtt_set_model_queue(xQueueModel);
    ble_set_config_queue(xQueueConfigBle);

    /* El modelo vive en su propia partición y su estado en NVS; sobrevive a
     * cualquier reinicio, a diferencia de la config. */
    model_store_init();
    infer_init(xQueueModel);
    ble_set_start_semaphore(ble_start_semaphore);

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

    /* Espera recepción de paquete si no se recogió ninguno desde la NVS.
     *
     * Una config que no sirve se descarta y se sigue esperando, en vez de
     * reiniciar: el reinicio no arregla nada, porque el servidor vuelve a
     * mandar la misma apenas el device anuncia de nuevo. */
    while (!current_config) {

        ESP_LOGI(TAG, "esperando configuración vía BLE");
        packet_t init_pkt;
        if (xQueueReceive(xQueueConfigBle, &init_pkt, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (init_pkt.data == NULL || init_pkt.size == 0) {
            ESP_LOGW(TAG, "configuración BLE vacía, se descarta");
            free(init_pkt.data);
            continue;
        }
        current_config = config__unpack(NULL, init_pkt.size, init_pkt.data);
        free(init_pkt.data);
        if (current_config == NULL) {
            ESP_LOGW(TAG, "error al desempaquetar paquete de configuración, se descarta");
            continue;
        }
        if (!protocol_conf_is_valid(current_config)) {
            ESP_LOGW(TAG, "config v%ld con protocol_conf inválido (%ld), se descarta",
                     (long)current_config->config_version, (long)current_config->protocol_conf);
            config__free_unpacked(current_config, NULL);
            current_config = NULL;
            continue;
        }
        device_clock_set(current_config->time_client);
        data_window_count = 0;
        ESP_LOGI(TAG, "configuración leída correctamente");
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
        const busscan_spi_pins_t scan_spi_pins = {
            .host    = SD_SPI_HOST,
            .mosi_io = PIN_NUM_MOSI,
            .miso_io = PIN_NUM_MISO,
            .sclk_io = PIN_NUM_CLK,
        };
        nebulaedge_busscan_i2c(bus_handle);
        nebulaedge_busscan_spi(bus_handle, &scan_spi_pins, BUS_SCAN_CS_MASK);
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
    
        /* El protocolo lo resuelve la tabla, no un switch. Lo único que cambia
         * entre los cuatro es cómo se abre el transporte, y eso es proto->open();
         * el resto del ciclo -WiFi arriba, arrancar tasks, bloquearse hasta que
         * la task de respuesta avise un cambio, WiFi abajo- es idéntico. */
        /* No debería pasar: una config con protocolo inválido se rechaza al
         * llegar. Queda como red de seguridad (p.ej. una NVS corrupta). */
        if (!protocol_conf_is_valid(current_config)) {
            ESP_LOGE(TAG, "Selección de protocolo inválida: %ld", current_config->protocol_conf);
            vTaskDelay(pdMS_TO_TICKS(RESTART_LOG_FLUSH_MS));
            esp_restart();
        }

        const protocol_ops_t *proto = &PROTOCOLS[current_config->protocol_conf];

        if (proto->needs_wifi) {
            global_wifi_config wifi_config = {
                .ssid = current_config->ssid,
                .password = current_config->passwd,
                .auth_mode = WIFI_AUTH_WPA_WPA2_PSK,
                .max_retry = WIFI_MAX_RETRY,
                .retry_delay_ms = 0,
            };
            wifi_start_if_needed(&wifi_config);
            wifi_remember_credentials(current_config);
        }

        if (!proto->open()) {
            ESP_LOGI(TAG, "%s: no se pudo abrir el transporte, se reintenta", proto->name);
            if (proto->needs_wifi) {
                wifi_deinit_sta();
            }
            continue;
        }

        start_protocol_tasks(proto);

        /* Punto de bloqueo: lo libera la task de respuesta cuando aplica una
         * configuración que cambia de protocolo. */
        if (xSemaphoreTake(protocol_change_semaphore, portMAX_DELAY)) {
            ESP_LOGI(TAG, "%s: se sale por cambio de protocolo", proto->name);
        }

        /* WiFi abajo SOLO si el protocolo que sigue no la necesita, o si
         * cambió la red.
         *
         * Antes se bajaba siempre, y eso se pagaba en cada cambio de
         * configuración aunque el protocolo no cambiara: la WiFi se destruía
         * (wifi:state run -> init) y se volvía a asociar con la MISMA red.
         * wifi_start_if_needed() ya es idempotente -mira s_wifi_active y
         * vuelve-, así que dejarla levantada no cuesta nada y el ciclo
         * siguiente la reusa.
         *
         * MEDIDO EN BANCO, cambio UDP -> UDP, hueco del camino de datos (de un
         * "Paquete Data_N generado" al siguiente): 9,4-10,1 s antes, 8,1 s
         * después. El rebuild desaparece del log -queda
         * "wifi_start_if_needed: already active"- pero el ahorro es de ~1,4 s,
         * no de los ~3,8 s que tarda el rebuild en pared, porque parte se
         * solapaba con la reinicialización de los sensores.
         *
         * Lo que queda del hueco NO es WiFi: 4,2 s son ACK_DRAIN_MS (deliberado,
         * para que el ACK salga antes de cerrar el socket) y 3,2 s son los tres
         * reintentos de softreset del BMI270, que en esta placa no está montado.
         * Ahí no se toca nada: los drivers de sensores los lleva otra persona.
         *
         * Las credenciales se comparan porque si cambió la red hay que
         * reasociarse de verdad: reusar la conexión vieja dejaría al device
         * mandando a una red que ya no es la del servidor. Y en UDP eso no da
         * error, así que no se arreglaría solo. */
        if (proto->needs_wifi) {
            bool next_needs_wifi =
                protocol_conf_is_valid(current_config)
                && PROTOCOLS[current_config->protocol_conf].needs_wifi;

            if (!next_needs_wifi || wifi_credentials_changed(current_config)) {
                wifi_deinit_sta();
            }
            else {
                ESP_LOGI(TAG, "WiFi se mantiene arriba: %s la sigue usando con la misma red",
                         PROTOCOLS[current_config->protocol_conf].name);
            }
        }
    }
}