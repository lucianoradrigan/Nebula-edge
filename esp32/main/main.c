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
#include "data_path.h"
#include "config_task.h"
#include "diag.h"

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
        config_apply_time(current_config);
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