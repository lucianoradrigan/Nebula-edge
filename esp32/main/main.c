#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_sleep.h"
#include "esp_attr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/semphr.h"

#include "nebulaedge_wifi.h"
#include "nebulaedge_mqtt.h"
#include "nebulaedge_udp.h"
#include "nebulaedge_tcp.h"
#include "nebulaedge_ble.h"
#include "nebulaedge_defs.h"
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

/* Dos productoras de telemetría, una por ritmo. Los sensores tienen tiempos
 * naturales muy distintos: la temperatura cambia en minutos y el acelerómetro
 * en milisegundos. Con una sola task había que elegir un intervalo único, que
 * sobremuestreaba el ambiente o submuestreaba el movimiento. Ahora cada una
 * corre a lo suyo y ambas escriben en la misma xQueueData. */
TaskHandle_t xHandleCollectInertial = NULL;       // cada send_interval_s
TaskHandle_t xHandleCollectEnvironmental = NULL;  // cada env_interval_s

TaskHandle_t xHandleSendUDP = NULL;
TaskHandle_t xHandleGetResponseUDP = NULL;
TaskHandle_t xHandleSendTCP = NULL;
TaskHandle_t xHandleGetResponseTCP = NULL;
TaskHandle_t xHandleSendMQTT = NULL;
TaskHandle_t xHandleGetResponseMQTT = NULL;
TaskHandle_t xHandleSendBLE = NULL;
TaskHandle_t xHandleGetResponseBLE = NULL;

const char *TAG = "main_task";
const char *TAG_COLLECT_INERTIAL = "task_collect_inertial";
const char *TAG_COLLECT_ENV = "task_collect_env";

const char *TAG_SEND_MQTT = "task_send_mqtt";
const char *TAG_SEND_UDP = "task_send_udp";
const char *TAG_SEND_TCP = "task_send_tcp";
const char *TAG_SEND_BLE = "task_send_ble";

const char *TAG_GET_RSP_MQTT = "task_get_rsp_mqtt";
const char *TAG_GET_RSP_BLE = "task_get_rsp_ble";
const char *TAG_GET_RSP_TCP = "task_get_rsp_tcp";
const char *TAG_GET_RSP_UDP = "task_get_rsp_udp";

static uint32_t data_window_count = 0;
RTC_DATA_ATTR static uint32_t rtc_unix_time_s = 0;
static char this_device_id[18] = "00:00:00:00:00:00";

#define NVS_NAMESPACE "nebulaedge"
#define NVS_KEY_CONFIG "config_blob"

/* Los paquetes de control (ACK de config, aviso de deep sleep) no esperan respuesta:
 * se mandan varias veces seguidas para bajar la probabilidad de que se pierdan, en
 * vez de implementar un segundo hop de confirmación (que agregaría otro flanco de
 * pérdida en vez de reducir el riesgo). */
#define CONTROL_PKT_REDUNDANCY 3
#define CONTROL_PKT_REDUNDANCY_DELAY_MS 50

/* Suspende las dos tasks de sensores solo cuando el bus I2C está libre.
 * Evita pausar una task en mitad de una transacción I2C.
 *
 * Las dos se suspenden bajo el MISMO mutex tomado una sola vez: si se hiciera
 * en dos pasos, la que quedara viva podría empezar una transacción justo entre
 * medio y volveríamos al problema que esto evita. */
static void suspend_collect_tasks_when_i2c_idle(void) {
    // Si no existe ninguna todavía, no hay nada que suspender.
    if (!xHandleCollectInertial && !xHandleCollectEnvironmental) {
        return;
    }

    // Fallback defensivo: si el mutex aún no fue creado, suspende igual.
    if (i2c_bus_mutex == NULL) {
        ESP_LOGW(TAG, "i2c_bus_mutex es NULL, se suspenden tasks de sensores sin validación");
        if (xHandleCollectInertial) vTaskSuspend(xHandleCollectInertial);
        if (xHandleCollectEnvironmental) vTaskSuspend(xHandleCollectEnvironmental);
        return;
    }

    // Espera hasta tomar el mutex y suspende antes de liberarlo.
    // Así se evita la carrera entre "mutex libre" y "vTaskSuspend".
    while (1) {
        if (xSemaphoreTake(i2c_bus_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            if (xHandleCollectInertial) vTaskSuspend(xHandleCollectInertial);
            if (xHandleCollectEnvironmental) vTaskSuspend(xHandleCollectEnvironmental);
            xSemaphoreGive(i2c_bus_mutex);
            ESP_LOGI(TAG, "Tasks de sensores suspendidas con mutex I2C libre");
            return;
        }

        ESP_LOGW(TAG, "Esperando mutex I2C libre para suspender tasks de sensores...");
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* Reanuda las dos tasks de sensores. */
static void resume_collect_tasks(void) {
    if (xHandleCollectInertial) vTaskResume(xHandleCollectInertial);
    if (xHandleCollectEnvironmental) vTaskResume(xHandleCollectEnvironmental);
}

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

/* Ajusta la hora del sistema con epoch UNIX recibido en configuración. */
static void set_device_time_from_unix_s(int64_t unix_time_s) {
    if (unix_time_s <= 0) {
        ESP_LOGW(TAG, "time_client inválido: %lld", (long long)unix_time_s);
        return;
    }

    struct timeval tv = {
        .tv_sec = (time_t)unix_time_s,
        .tv_usec = 0,
    };

    if (settimeofday(&tv, NULL) != 0) {
        ESP_LOGW(TAG, "No se pudo ajustar la hora del sistema a %lld", (long long)unix_time_s);
        return;
    }

    time_t now = 0;
    time(&now);
    ESP_LOGI(TAG, "Hora del sistema ajustada a %lld", (long long)now);
}

/* Guarda la hora estimada de despertar para restaurarla tras deep sleep. */
static void save_device_time_before_deep_sleep(uint64_t sleep_us) {
    time_t now = 0;
    time(&now);

    // Redondea microsegundos a segundos y calcula epoch esperado al despertar.
    uint32_t sleep_s = (uint32_t)((sleep_us + 999999ULL) / 1000000ULL);
        rtc_unix_time_s = (uint32_t)((uint64_t)now + sleep_s);

    ESP_LOGI(TAG, "Hora guardada para restaurar tras deep sleep: %lu", (unsigned long)rtc_unix_time_s);
}

/* Restaura la hora del sistema al volver desde deep sleep. */
static void restore_device_time_after_deep_sleep(void) {
    if (rtc_unix_time_s == 0) {
        ESP_LOGW(TAG, "No hay hora RTC guardada para restaurar tras deep sleep");
        return;
    }

    set_device_time_from_unix_s(rtc_unix_time_s);
}

/* Obtiene la MAC BT y la deja en formato string como ID del dispositivo. */
static void get_bt_mac(void) {
    uint8_t mac[6] = {0};

    esp_err_t ret = esp_read_mac(mac, ESP_MAC_BT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo leer MAC BT: %s", esp_err_to_name(ret));
        return;
    }

    snprintf(this_device_id, sizeof(this_device_id), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "ID del device detectado: %s", this_device_id);
}

/* Borra los datos de la NVS. */
static void nvs_clear_config(void) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    nvs_erase_key(nvs, NVS_KEY_CONFIG);
    nvs_commit(nvs);
    nvs_close(nvs);
}

/* Guarda un paquete Config en la NVS */
static void nvs_save_config(const Config *cfg) {
    if (!cfg) return;

    size_t size = config__get_packed_size(cfg);
    if (size == 0) return;

    uint8_t *buf = malloc(size);
    if (!buf) {
        ESP_LOGE(TAG, "No hay memoria para guardar config en NVS");
        return;
    }
    config__pack(cfg, buf);

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        if (nvs_set_blob(nvs, NVS_KEY_CONFIG, buf, size) == ESP_OK) {
            nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    free(buf);
}

/* Carga un paquete Config desde la NVS. */
static Config *nvs_load_config(void) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return NULL;
    }

    size_t size = 0;
    if (nvs_get_blob(nvs, NVS_KEY_CONFIG, NULL, &size) != ESP_OK || size == 0) {
        nvs_close(nvs);
        return NULL;
    }

    uint8_t *buf = malloc(size);
    if (!buf) {
        nvs_close(nvs);
        return NULL;
    }

    Config *cfg = NULL;
    if (nvs_get_blob(nvs, NVS_KEY_CONFIG, buf, &size) == ESP_OK) {
        cfg = config__unpack(NULL, size, buf);
    }

    free(buf);
    nvs_close(nvs);
    return cfg;
}

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
    
    // Suspende recogida de datos
    ESP_LOGI(TAG, "Suspendiendo tasks de sensores");
    suspend_collect_tasks_when_i2c_idle();
    
    // En MQTT
    if (current_config->protocol_conf == 0) {
        // Suspende porque envío de flag cierra socket
        if (xHandleGetResponseMQTT) {
            vTaskSuspend(xHandleGetResponseMQTT);
        }
        // Avisa al server que se va a dormir, antes de cerrar. No se espera
        // respuesta, así que se manda varias veces por si se pierde alguna.
        char topic_data[128];
        snprintf(topic_data, sizeof(topic_data), "/topic/nebulaedge/%s/data", current_config->id_device);
        for (int i = 0; i < CONTROL_PKT_REDUNDANCY; i++) {
            mqtt_publish(topic_data, (uint8_t *)DEEP_SLEEP_FLAG, DEEP_SLEEP_FLAG_LEN, 0);
            vTaskDelay(pdMS_TO_TICKS(CONTROL_PKT_REDUNDANCY_DELAY_MS));
        }
        mqtt_finish();
    }

    // En UDP
    else if (current_config->protocol_conf == 1) {
        // Suspende porque envío de flag cierra socket
        if (xHandleGetResponseUDP) {
            vTaskSuspend(xHandleGetResponseUDP);
        }
        // Avisa al server que se va a dormir, antes de cerrar el socket.
        for (int i = 0; i < CONTROL_PKT_REDUNDANCY; i++) {
            nebulaedge_udp_send((uint8_t *)DEEP_SLEEP_FLAG, DEEP_SLEEP_FLAG_LEN);
            vTaskDelay(pdMS_TO_TICKS(CONTROL_PKT_REDUNDANCY_DELAY_MS));
        }
        nebulaedge_udp_close_socket();
    }

    // En TCP
    else if (current_config->protocol_conf == 2) {
        // Suspende para luego cerrar socket
        if (xHandleGetResponseTCP) {
            vTaskSuspend(xHandleGetResponseTCP);
        }
        // Avisa al server que se va a dormir, antes de cerrar el socket.
        for (int i = 0; i < CONTROL_PKT_REDUNDANCY; i++) {
            tcp_send((uint8_t *)DEEP_SLEEP_FLAG, DEEP_SLEEP_FLAG_LEN);
            vTaskDelay(pdMS_TO_TICKS(CONTROL_PKT_REDUNDANCY_DELAY_MS));
        }
        tcp_close_socket();
    }

    // En BLE
    else if (current_config->protocol_conf == 3) {
        if (xHandleGetResponseBLE) {
            vTaskSuspend(xHandleGetResponseBLE);
        }
        /* Una sola vez: ver el comentario en send_config_ack_ble(). Acá importa
         * más todavía, porque cada reenvío son milisegundos despierto antes de
         * dormir. */
        set_char_with_notify(IDX_CHAR_VAL_B_BLE, (uint8_t *)DEEP_SLEEP_FLAG, DEEP_SLEEP_FLAG_LEN);
        // nvs_clear_config();
    }
    
    nvs_save_config(current_config);
    
    // Deinicializa slaves sensores BMM350, BME688, BMI270
    i2c_slave_deinit(&device_bmm350);
    i2c_slave_deinit(&device_bme688);
    i2c_slave_deinit(&device_bmi270);
    i2c_master_deinit(&bus_handle);
    
    // Delay de precaución
    vTaskDelay(pdMS_TO_TICKS(3000));
    
    ESP_LOGI(TAG, "Entrando deep sleep por %lu s", (unsigned long)current_config->sleep_time_s);
    save_device_time_before_deep_sleep(sleep_us);
    
    esp_sleep_enable_timer_wakeup(sleep_us);
    esp_deep_sleep_start();
}

// Recibe dos configuraciones y retorna un booleano si son diferentes o iguales
bool config_has_changed(Config *old, Config *new) {
    if (new->config_version > old->config_version) {
        ESP_LOGI(TAG, "Nueva configuración recibida. Versión %ld.", new->config_version);
        return true;
    }
    if (new->config_version < old->config_version) {
        return false;
    }
    return false;
}

/* Envía ACK en protocolo MQTT después de recibir una nueva configuración del servidor. */
static void send_config_ack_mqtt(const Config *cfg, bool applied) {
    if (!cfg || !cfg->id_device) return;
    ConfigAck ack = CONFIG_ACK__INIT;
    time_t now_s = 0;
    time(&now_s);
    ack.id_device = cfg->id_device;
    ack.config_version = cfg->config_version;
    ack.applied = applied;
    ack.time_client = now_s > 0 ? (uint32_t)now_s : 0;

    size_t size = config_ack__get_packed_size(&ack);
    uint8_t *buf = malloc(size);
    if (!buf) {
        ESP_LOGE(TAG, "No hay memoria para ACK MQTT");
        return;
    }
    config_ack__pack(&ack, buf);

    char topic_ack[128];
    snprintf(topic_ack, sizeof(topic_ack), "/topic/nebulaedge/%s/config/ack", cfg->id_device);
    // Envío redundante: no se espera respuesta, así que se manda varias veces
    // en vez de esperar una confirmación (que agregaría otro flanco de pérdida).
    for (int i = 0; i < CONTROL_PKT_REDUNDANCY; i++) {
        mqtt_publish(topic_ack, buf, size, 0);
        vTaskDelay(pdMS_TO_TICKS(CONTROL_PKT_REDUNDANCY_DELAY_MS));
    }
    free(buf);
    ESP_LOGI(TAG, "MQTT: ACK de config enviado.");
}

/* Envía ACK en protocolo BLE después de recibir una nueva configuración del servidor. */
static void send_config_ack_ble(const Config *cfg, bool applied) {
    if (!cfg || !cfg->id_device) return;
    ConfigAck ack = CONFIG_ACK__INIT;
    time_t now_s = 0;
    time(&now_s);
    ack.id_device = cfg->id_device;
    ack.config_version = cfg->config_version;
    ack.applied = applied;
    ack.time_client = now_s > 0 ? (uint32_t)now_s : 0;

    size_t size = config_ack__get_packed_size(&ack);
    uint8_t *buf = malloc(size);
    if (!buf) {
        ESP_LOGE(TAG, "No hay memoria para ACK BLE");
        return;
    }
    config_ack__pack(&ack, buf);

    /* Una sola vez, a diferencia de UDP: en BLE el link layer ya retransmite
     * lo que se encoló, y set_char_with_notify() reintenta por su cuenta si el
     * stack rechaza el envío por congestión. Además el ACK queda legible en
     * char D, así que si se cae la conexión el servidor lo reconcilia
     * leyéndolo (BleTransport.confirm_config_applied). */
    esp_err_t ack_ret = set_char_with_notify(IDX_CHAR_VAL_D_BLE, buf, size);
    free(buf);

    if (ack_ret == ESP_OK) {
        ESP_LOGI(TAG, "BLE: ACK de config enviado.");
    } else {
        ESP_LOGW(TAG, "BLE: ACK no se pudo notificar (%s); queda legible en char D.",
                 esp_err_to_name(ack_ret));
    }
}

/* Envía ACK en protocolo UDP después de recibir una nueva configuración del servidor. */
static void send_config_ack_udp(const Config *cfg, bool applied) {
    if (!cfg || !cfg->id_device) return;
    ConfigAck ack = CONFIG_ACK__INIT;
    time_t now_s = 0;
    time(&now_s);
    ack.id_device = cfg->id_device;
    ack.config_version = cfg->config_version;
    ack.applied = applied;
    ack.time_client = now_s > 0 ? (uint32_t)now_s : 0;

    size_t size = config_ack__get_packed_size(&ack);
    uint8_t *buf = malloc(size);
    if (!buf) {
        ESP_LOGE(TAG, "No hay memoria para ACK UDP");
        return;
    }
    config_ack__pack(&ack, buf);
    for (int i = 0; i < CONTROL_PKT_REDUNDANCY; i++) {
        nebulaedge_udp_send(buf, size);
        vTaskDelay(pdMS_TO_TICKS(CONTROL_PKT_REDUNDANCY_DELAY_MS));
    }
    free(buf);
    ESP_LOGI(TAG, "UDP: ACK de config enviado.");
}

/* Envía ACK en protocolo TCP después de recibir una nueva configuración del servidor. */
static void send_config_ack_tcp(const Config *cfg, bool applied) {
    if (!cfg || !cfg->id_device) return;
    ConfigAck ack = CONFIG_ACK__INIT;
    time_t now_s = 0;
    time(&now_s);
    ack.id_device = cfg->id_device;
    ack.config_version = cfg->config_version;
    ack.applied = applied;
    ack.time_client = now_s > 0 ? (uint32_t)now_s : 0;

    size_t size = config_ack__get_packed_size(&ack);
    uint8_t *buf = malloc(size);
    if (!buf) {
        ESP_LOGE(TAG, "No hay memoria para ACK TCP");
        return;
    }
    config_ack__pack(&ack, buf);
    for (int i = 0; i < CONTROL_PKT_REDUNDANCY; i++) {
        tcp_send(buf, size);
        vTaskDelay(pdMS_TO_TICKS(CONTROL_PKT_REDUNDANCY_DELAY_MS));
    }
    free(buf);
    ESP_LOGI(TAG, "TCP: ACK de config enviado.");
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

/* Epoch actual en segundos, o 0 si el reloj todavía no está puesto en hora. */
static uint32_t now_unix_s(void) {
    time_t now_s = 0;
    time(&now_s);
    return now_s > 0 ? (uint32_t)now_s : 0;
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
        Inertial inertial = INERTIAL__INIT;
        inertial.id_device = this_device_id;
        inertial.config_version_applied = current_config ? current_config->config_version : 0;
        inertial.time_client = now_unix_s();

        // Recogida de datos de sensores inerciales
        readout_data_bmi270(&inertial);
        readout_data_bmm350(&inertial);

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

        // Ritma la producción
        uint32_t interval_s = current_config ? current_config->send_interval_s : 1;
        vTaskDelay(pdMS_TO_TICKS(interval_s * 1000U) + 1);
    }
}

// GEN_DATA (lento): BME688 -> paquete Environmental, cada env_interval_s.
void vTaskCollectEnvironmental(void *pvParameters) {
    for (;;) {
        Environmental env = ENVIRONMENTAL__INIT;
        env.id_device = this_device_id;
        env.config_version_applied = current_config ? current_config->config_version : 0;
        env.time_client = now_unix_s();

        // Recogida de datos ambientales
        readout_data_bme688(&env);

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

        // Ritma la producción
        vTaskDelay(pdMS_TO_TICKS(environmental_interval_s() * 1000U) + 1);
    }
}

// MQTT: Si aún no hay datos, send espera a gen_rand_data para
// recibir datos.
void vTaskSendMQTT(void *pvParameters) {
    packet_t packet;
    for (;;) {

        int msg_id;

        // Espera a que haya datos disponibles
        int receive = xQueueReceive(xQueueData, &packet, portMAX_DELAY);

        // Caso recepción correcta desde la cola
        if (receive == pdTRUE) {

            char topic_data[128];
            snprintf(topic_data, sizeof(topic_data), "/topic/nebulaedge/%s/data", current_config->id_device);
            msg_id = mqtt_publish(topic_data, packet.data, packet.size, 0);
            // Logs
            if (msg_id < 0) {
                ESP_LOGE(TAG_SEND_MQTT, "Error al publicar por MQTT. msg_id=%d", msg_id);
            } 
            else {
                ESP_LOGI(TAG_SEND_MQTT, "Paquete publicado por MQTT. msg_id=%d", msg_id);
            }

            // Escribe en SD si está en modo deep sleep
            if (current_config->sleep_time_s > 0) {
                data_to_sd(packet.data, packet.size);
            }

            // Libera memoria de paquete
            free(packet.data);
            deep_sleep_if_needed();
        }

        // Caso en que haya error al recibir desde la cola
        else if (receive == pdFALSE) {
            ESP_LOGW(TAG_SEND_MQTT, "No se recibió ningún paquete de la xQueue");
        }
        vTaskDelay(1);
    }
}

// MQTT: Espera recepción de datos de comunicación.
void vTaskGetResponseMQTT(void *pvParameters) {

    Config *new_config;
    for (;;) {

        // Con portMAX_DELAY espera a que haya un elemento en la cola
        xQueueReceive(xQueueConfig, &new_config, portMAX_DELAY);

        if (!new_config) {
            continue;
        }

        if (current_config && new_config->config_version < current_config->config_version) {
            ESP_LOGW(TAG_GET_RSP_MQTT, "Config MQTT antigua: %ld < %ld", (long)new_config->config_version, (long)current_config->config_version);
            send_config_ack_mqtt(new_config, false);
            config__free_unpacked(new_config, NULL);
            continue;
        }

        if (config_has_changed(current_config, new_config)) {
            ESP_LOGI(TAG_GET_RSP_MQTT, "Configuración cambiada!");

            if (current_config) config__free_unpacked(current_config, NULL);
            current_config = new_config;
            data_window_count = 0;

            if (xHandleSendMQTT) {     
                vTaskSuspend(xHandleSendMQTT);
                ESP_LOGI(TAG_GET_RSP_MQTT, "Se suspende vTaskSendMQTT");
            }

            // Se resetea queue para que quede vacía
            drain_and_free_data_queue();
            ESP_LOGI(TAG_GET_RSP_MQTT, "Se suspenden tasks de sensores");
            suspend_collect_tasks_when_i2c_idle();

            send_config_ack_mqtt(current_config, true);
            vTaskDelay(4000 / portTICK_PERIOD_MS);

            mqtt_finish();

            if (xSemaphoreGive(semaphore)) {
                ESP_LOGI(TAG_GET_RSP_MQTT, "vTaskGetResponseMQTT libera semáforo para cambio de protocolo");
            }

            // Auto-suspende esta task hasta que el protocolo MQTT vuelva a activarse
            ESP_LOGI(TAG_GET_RSP_MQTT, "Suspendiendo vTaskGetResponseMQTT");  
            vTaskSuspend(NULL);
        } 
        else {
            ESP_LOGI(TAG_GET_RSP_MQTT, "La configuración recibida es la misma");
            send_config_ack_mqtt(new_config, true);
            config__free_unpacked(new_config, NULL);
        }
    }
}

// BLE
void vTaskSendBLE(void *pvParameters) {
    packet_t packet;
    for (;;) {

        int receive = xQueueReceive(xQueueData, &packet, portMAX_DELAY);
        if (receive == pdTRUE) {

            // Delay de precaución en caso de modo discontinuo
            // Escribe en SD si está en modo deep sleep
            if (current_config->sleep_time_s > 0) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                data_to_sd(packet.data, packet.size);
            }

            set_char_with_notify(IDX_CHAR_VAL_B_BLE, packet.data, packet.size);
            free(packet.data);

            // Para evitar que se pierda el último paquete
            if (data_window_count + 1 == (uint32_t)current_config->sleep_window_size) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            }
            deep_sleep_if_needed();
        }
    }
}

// BLE response
void vTaskGetResponseBLE(void *pvParameters) {
    packet_t pkt;

    for (;;) {
        if (xQueueReceive(xQueueConfigBle, &pkt, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (pkt.data == NULL || pkt.size == 0) {
            continue;
        }

        // Desempaqueta la configuración recibida
        Config *new_config = config__unpack(NULL, pkt.size, pkt.data);
        free(pkt.data);
        if (new_config == NULL) {
            ESP_LOGW(TAG_GET_RSP_BLE, "Error al desempaquetar el mensaje protobuf de configuración");
            continue;
        }

        ESP_LOGI(TAG_GET_RSP_BLE, "BLE: Se recibió información de configuración!");

        if (current_config && new_config->config_version < current_config->config_version) {
            ESP_LOGW(TAG_GET_RSP_BLE, "Config BLE antigua: %ld < %ld", (long)new_config->config_version, (long)current_config->config_version);
            send_config_ack_ble(new_config, false);
            config__free_unpacked(new_config, NULL);
            continue;
        }

        if (config_has_changed(current_config, new_config)) {
            ESP_LOGI(TAG_GET_RSP_BLE, "Configuración cambiada!");

            // Reemplaza la configuración global
            if (current_config) config__free_unpacked(current_config, NULL);
            current_config = new_config;
            data_window_count = 0;

            send_config_ack_ble(current_config, true);

            if (xHandleSendBLE) {
                ESP_LOGI(TAG_GET_RSP_BLE, "Suspendiendo vTaskSendBLE");
                vTaskSuspend(xHandleSendBLE);
            }
            // Se resetea queue para que quede vacía
            drain_and_free_data_queue();
            ESP_LOGI(TAG_GET_RSP_BLE, "Suspendiendo tasks de sensores");
            suspend_collect_tasks_when_i2c_idle();

            // Señala a app_main que debe cambiar de protocolo
            if (xSemaphoreGive(semaphore)) {
                ESP_LOGI(TAG_GET_RSP_BLE, "BLE libera semáforo para cambio de protocolo");
            }

            ESP_LOGI(TAG_GET_RSP_BLE, "Suspendiendo vTaskGetResponseBLE");
            vTaskSuspend(NULL);
        } 
        else {
            ESP_LOGI(TAG_GET_RSP_BLE, "La configuración recibida es la misma");
            send_config_ack_ble(new_config, true);
            config__free_unpacked(new_config, NULL);
        }
    }
}

// UDP: Si aún no hay datos, send espera a gen_rand_data para
// recibir datos.
void vTaskSendUDP(void *pvParameters) {
    packet_t packet;
    for (;;) {
        if (xQueueReceive(xQueueData, &packet, portMAX_DELAY) == pdTRUE) {
            nebulaedge_udp_send(packet.data, packet.size);
            // Escribe en SD si está en modo deep sleep
            if (current_config->sleep_time_s > 0) {
                data_to_sd(packet.data, packet.size);
            }
            free(packet.data);
            deep_sleep_if_needed();
        }
    }
}

// UDP: Pide configuración a la Raspberry por UDP. Hay que liberar el puntero Config *!!
void vTaskGetResponseUDP(void *pvParameters) {

    size_t len = 256;
    uint8_t buffer[len];

    for (;;) {
        // Se queda bloqueado en esta llamada (y cede la cpu) hasta recibir algo
        size_t len_recv = nebulaedge_udp_receive(buffer, len);

        if (len_recv == 0) {
            ESP_LOGI(TAG_GET_RSP_UDP, "no han llegado nuevos datos de configuración");
            vTaskDelay(1);
            continue;
        }

        Config *new_config = config__unpack(NULL, len_recv, buffer);
        if (!new_config) {
            ESP_LOGI(TAG_GET_RSP_UDP, "error al desempaquetar");
            continue;
        }

        // Compara configuración actual versus recibida
        if (current_config && new_config->config_version < current_config->config_version) {
            ESP_LOGW(TAG_GET_RSP_UDP, "Config UDP antigua: %ld < %ld", (long)new_config->config_version, (long)current_config->config_version);
            send_config_ack_udp(new_config, false);
            config__free_unpacked(new_config, NULL);
            continue;
        }

        if (config_has_changed(current_config, new_config)) {
            ESP_LOGI(TAG_GET_RSP_UDP, "configuración cambiada!");

            if (current_config) config__free_unpacked(current_config, NULL);
            current_config = new_config;
            data_window_count = 0;

            if (xHandleSendUDP) {
                ESP_LOGI(TAG_GET_RSP_UDP, "Suspendiendo vTaskSendUDP");
                vTaskSuspend(xHandleSendUDP);
            }

            // Se resetea queue para que quede vacía
            drain_and_free_data_queue();
            ESP_LOGI(TAG_GET_RSP_UDP, "Suspendiendo tasks de sensores");
            suspend_collect_tasks_when_i2c_idle();

            send_config_ack_udp(current_config, true);
            vTaskDelay(2000 / portTICK_PERIOD_MS);

            // Se cierra el socket UDP
            nebulaedge_udp_close_socket();

            // Libera el semáforo para que main elimine las tareas UDP y cambie de protocolo
            if (xSemaphoreGive(semaphore)) {
                ESP_LOGI(TAG_GET_RSP_UDP, "UDP libera semáforo para cambio de protocolo");
            }

            ESP_LOGI(TAG_GET_RSP_UDP, "Suspendiendo vTaskGetResponseUDP");
            vTaskSuspend(NULL);
        } 
        else {
            ESP_LOGI(TAG_GET_RSP_UDP, "UDP: Config igual, se descarta");
            send_config_ack_udp(new_config, true);
            config__free_unpacked(new_config, NULL);
        }
    }
}

// TCP: Si aún no hay datos, send espera a gen_rand_data para
// recibir datos.
void vTaskSendTCP(void *pvParameters) {
    packet_t packet;
    for (;;) {
        if (xQueueReceive(xQueueData, &packet, portMAX_DELAY) == pdTRUE) {
            tcp_send(packet.data, packet.size);
            // Escribe en SD si está en modo deep sleep
            if (current_config->sleep_time_s > 0) {
                data_to_sd(packet.data, packet.size);
            }
            free(packet.data);
            deep_sleep_if_needed();
        }
    }
}

// TCP: Pide configuración a la Raspberry por TCP. Hay que liberar el puntero Config *!!
void vTaskGetResponseTCP(void *pvParameters) {

    size_t len = 256;
    uint8_t buffer[len];

    for (;;) {

        // Espera recepción de datos de configuración
        size_t len_recv = tcp_receive(buffer, len);
        if (len_recv == 0) {
            // sin datos/timeout
            vTaskDelay(1);
            continue;
        }

        Config *new_config = config__unpack(NULL, len_recv, buffer);   // Esta memoria hay que liberarla!
        
        if (!new_config) {
            ESP_LOGI(TAG_GET_RSP_TCP, "TCP: error al desempaquetar");
            continue;
        }

        if (current_config && new_config->config_version < current_config->config_version) {
            ESP_LOGW(TAG_GET_RSP_TCP, "Config TCP antigua: %ld < %ld", (long)new_config->config_version, (long)current_config->config_version);
            send_config_ack_tcp(new_config, false);
            config__free_unpacked(new_config, NULL);
            continue;
        }

        if (config_has_changed(current_config, new_config)) {
            ESP_LOGI(TAG_GET_RSP_TCP, "TCP: Configuración cambiada!");

            // Libera la configuración anterior
            if (current_config) config__free_unpacked(current_config, NULL);

            // Actualiza la configuración global
            current_config = new_config;
            data_window_count = 0;

            if (xHandleSendTCP) {
                ESP_LOGI(TAG_GET_RSP_TCP, "Suspendiendo vTaskSendTCP");
                vTaskSuspend(xHandleSendTCP);
            }
            // Se resetea queue para que quede vacía
            drain_and_free_data_queue();
            ESP_LOGI(TAG_GET_RSP_TCP, "Suspendiendo tasks de sensores");
            suspend_collect_tasks_when_i2c_idle();

            send_config_ack_tcp(current_config, true);
            vTaskDelay(2000 / portTICK_PERIOD_MS);

            // Cierra el socket
            tcp_close_socket();

            // Se libera semáforo para pasar a otro protocolo
            if (xSemaphoreGive(semaphore)) {
                ESP_LOGI(TAG_GET_RSP_TCP, "TCP libera semáforo para cambio de protocolo");
            }

            vTaskSuspend(NULL);
        }
        else {
            ESP_LOGI(TAG_GET_RSP_TCP, "TCP: Config igual, se descarta");
            send_config_ack_tcp(new_config, true);
            config__free_unpacked(new_config, NULL);
        }    
    }
}

void app_main() {

    /****************************************************************/
    /***********************  INICIALIZACIÓN ************************/
    /****************************************************************/

    // Inicializa NVS
    ESP_ERROR_CHECK(nvs_flash_init());

    // Obtiene MAC BT del dispositivo
    get_bt_mac();

    // Inicializa semáforos binario. Inician cerrados.
    semaphore = xSemaphoreCreateBinary();
    semaphore_ble = xSemaphoreCreateBinary();

    // Crea queues para pasar datos entre tasks
    xQueueData = xQueueCreate(100, sizeof(packet_t));
    xQueueConfig = xQueueCreate(5, sizeof(Config *));
    xQueueConfigBle = xQueueCreate(5, sizeof(packet_t));

    /********************************************************************/
    /********************* CONFIG INICIAL (NVS/BLE) *********************/
    /********************************************************************/

    // Recoge la información de la NVS solo si despierta de un deep sleep.
    // En caso de reinicio espera configuración vía BLE.
    esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();
    if (wake_cause == ESP_SLEEP_WAKEUP_TIMER) {
        current_config = nvs_load_config();
        if (current_config) {
            ESP_LOGI(TAG, "Config cargada desde NVS (wake-up por deep sleep)");
            restore_device_time_after_deep_sleep();
            data_window_count = 0;
        }
    }
    else {
        nvs_clear_config();
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
            set_device_time_from_unix_s(current_config->time_client);
            rtc_unix_time_s = (uint32_t)current_config->time_client;
            data_window_count = 0;
            ESP_LOGI(TAG, "configuración leída correctamente");
        }
    } 

    // Monta la tarjeta SD en /sdcard y abre archivos para guardar data en deep sleep
    // AL MONTAR OCUPANDO GPIO 1 (im-v2) FALLA BRUTALMENTE
    // esp_err_t sd_ret = mount_sd();
    // if (sd_ret != ESP_OK) {
    //     ESP_LOGW(TAG, "SD no disponible, se continúa sin persistencia local: %s", esp_err_to_name(sd_ret));
    // }

    /****************************************************************/
    /********** ITERACIÓN QUE MANEJA DE CAMBIOS DE PROTOCOLO ********/
    /****************************************************************/
    while (1) {
        // Deinicializa master y slave al cambiar de protocolo (en la primera iteración no ocurre nada).
        // Asegura transmisión i2c limpia.
        i2c_slave_deinit(&device_bmi270);
        i2c_slave_deinit(&device_bme688);
        i2c_slave_deinit(&device_bmm350);
        i2c_master_deinit(&bus_handle);
        
        // Inicializa master bus
        ESP_ERROR_CHECK(i2c_master_init(&bus_handle));

        // Inicializa slaves sensores BMM350, BME688, BMI270
        ESP_ERROR_CHECK(i2c_slave_init(&bus_handle, &device_bmm350, BMM350_SLAVE_ADDR, I2C_MASTER_FREQ_HZ));
        ESP_ERROR_CHECK(i2c_slave_init(&bus_handle, &device_bme688, BME688_SLAVE_ADDR, I2C_MASTER_FREQ_HZ));
        ESP_ERROR_CHECK(i2c_slave_init(&bus_handle, &device_bmi270, BMI270_SLAVE_ADDR, I2C_MASTER_FREQ_HZ));

        // Inicializa sensores BME688, BMM350, BMI270 con respectivas configuraciones
        bmm350_init(400, 4);
        bme688_init(current_config->bme688_sampling, current_config->bme688_sampling, current_config->bme688_sampling);
        bmi270_init(current_config->acc_sampling, 4, 8, 400, current_config->gyro_sensibility); 
    
        switch (current_config->protocol_conf) {

            /****************************************************************/
            /**************************  MQTT *******************************/
            /****************************************************************/
            case 0: {
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
                
                // Crea una sola vez las tasks
                if (!xHandleCollectInertial)
                    xTaskCreate(vTaskCollectInertial, TAG_COLLECT_INERTIAL, 4096, NULL, 2, &xHandleCollectInertial);
                if (!xHandleCollectEnvironmental)
                    xTaskCreate(vTaskCollectEnvironmental, TAG_COLLECT_ENV, 4096, NULL, 2, &xHandleCollectEnvironmental);
                if (!xHandleSendMQTT)
                    xTaskCreate(vTaskSendMQTT, TAG_SEND_MQTT, 4096, NULL, 2, &xHandleSendMQTT);
                if (!xHandleGetResponseMQTT) 
                    xTaskCreate(vTaskGetResponseMQTT, TAG_GET_RSP_MQTT, 4096, NULL, 3, &xHandleGetResponseMQTT);

                resume_collect_tasks();
                vTaskResume(xHandleSendMQTT);
                vTaskResume(xHandleGetResponseMQTT);

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
            case 1: {
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

                // Crea una sola vez las tasks
                if (!xHandleCollectInertial)
                    xTaskCreate(vTaskCollectInertial, TAG_COLLECT_INERTIAL, 4096, NULL, 2, &xHandleCollectInertial);
                if (!xHandleCollectEnvironmental)
                    xTaskCreate(vTaskCollectEnvironmental, TAG_COLLECT_ENV, 4096, NULL, 2, &xHandleCollectEnvironmental);
                if (!xHandleSendUDP)
                    xTaskCreate(vTaskSendUDP, TAG_SEND_UDP, 4096, NULL, 2, &xHandleSendUDP);
                if (!xHandleGetResponseUDP) 
                    xTaskCreate(vTaskGetResponseUDP, TAG_GET_RSP_UDP, 4096, NULL, 3, &xHandleGetResponseUDP);

                resume_collect_tasks();
                vTaskResume(xHandleSendUDP);
                vTaskResume(xHandleGetResponseUDP);

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
            case 2: {
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
                if (nebula_tcp_connect() != 0) {
                    // Cierra el socket
                    tcp_close_socket();
                    ESP_LOGI(TAG, "Retrying TCP connection...");
                    wifi_deinit_sta();
                    continue;
                }

                // Crea una sola vez las tasks
                if (!xHandleCollectInertial)
                    xTaskCreate(vTaskCollectInertial, TAG_COLLECT_INERTIAL, 4096, NULL, 2, &xHandleCollectInertial);
                if (!xHandleCollectEnvironmental)
                    xTaskCreate(vTaskCollectEnvironmental, TAG_COLLECT_ENV, 4096, NULL, 2, &xHandleCollectEnvironmental);
                if (!xHandleSendTCP)
                    xTaskCreate(vTaskSendTCP, TAG_SEND_TCP, 4096, NULL, 2, &xHandleSendTCP);
                if (!xHandleGetResponseTCP) 
                    xTaskCreate(vTaskGetResponseTCP, TAG_GET_RSP_TCP, 4096, NULL, 3, &xHandleGetResponseTCP);

                resume_collect_tasks();
                vTaskResume(xHandleSendTCP);
                vTaskResume(xHandleGetResponseTCP);

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
            case 3: {
                
                ESP_LOGI(TAG, "esperando conexión BLE para iniciar tasks...");

                // Semáforo se libera cuando se escribe configuración en charact. C de BLE.
                // Mientras tanto queda bloqueado aquí.
                if (xSemaphoreTake(semaphore, portMAX_DELAY)) {
                    ESP_LOGI(TAG, "toma semáforo, empieza recepción BLE");          
                }

                // Da tiempo a la Raspberry para estar lista
                vTaskDelay(5000 / portTICK_PERIOD_MS);

                // Crea una sola vez las tasks
                if (!xHandleCollectInertial)
                    xTaskCreate(vTaskCollectInertial, TAG_COLLECT_INERTIAL, 4096, NULL, 2, &xHandleCollectInertial);
                if (!xHandleCollectEnvironmental)
                    xTaskCreate(vTaskCollectEnvironmental, TAG_COLLECT_ENV, 4096, NULL, 2, &xHandleCollectEnvironmental);
                if (!xHandleSendBLE)
                    xTaskCreate(vTaskSendBLE, TAG_SEND_BLE, 4096, NULL, 2, &xHandleSendBLE);
                if (!xHandleGetResponseBLE) 
                    xTaskCreate(vTaskGetResponseBLE, TAG_GET_RSP_BLE, 4096, NULL, 3, &xHandleGetResponseBLE);

                resume_collect_tasks();
                vTaskResume(xHandleSendBLE);
                vTaskResume(xHandleGetResponseBLE);

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