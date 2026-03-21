#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_sleep.h"
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

#include "schema.pb-c.h"

QueueHandle_t xQueueConfig = NULL;
QueueHandle_t xQueueData = NULL;
QueueHandle_t xQueueConfigBle = NULL;

Config *current_config = NULL;
TaskHandle_t xHandleCollectSensorData = NULL;

TaskHandle_t xHandleSendUDP = NULL;
TaskHandle_t xHandleGetResponseUDP = NULL;
TaskHandle_t xHandleSendTCP = NULL;
TaskHandle_t xHandleGetResponseTCP = NULL;
TaskHandle_t xHandleSendMQTT = NULL;
TaskHandle_t xHandleGetResponseMQTT = NULL;
TaskHandle_t xHandleSendBLE = NULL;
TaskHandle_t xHandleGetResponseBLE = NULL;



const char *TAG = "main_task";
const char *TAG_COLLECT_DATA = "task_rand_data"; 

const char *TAG_SEND_MQTT = "task_send_mqtt";
const char *TAG_SEND_UDP = "task_send_udp";
const char *TAG_SEND_TCP = "task_send_tcp";
const char *TAG_SEND_BLE = "task_send_ble";

const char *TAG_GET_RSP_MQTT = "task_get_rsp_mqtt";
const char *TAG_GET_RSP_BLE = "task_get_rsp_ble";
const char *TAG_GET_RSP_TCP = "task_get_rsp_tcp";
const char *TAG_GET_RSP_UDP = "task_get_rsp_udp";

static uint32_t data_window_count = 0;
static char this_device_id[18] = "00:00:00:00:00:00";

#define NVS_NAMESPACE "nebulaedge"
#define NVS_KEY_CONFIG "config_blob"

/* Obtiene dirección MAC bluetooth. */
static void get_device_id(void) {
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

static void nvs_clear_config(void) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    nvs_erase_key(nvs, NVS_KEY_CONFIG);
    nvs_commit(nvs);
    nvs_close(nvs);
}

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
    if (!current_config) {
        return;
    }
    data_window_count++;
    if (current_config->discontinuous_sleep_time <= 0) {
        return;
    }

    int window_size = current_config->discontinuous_window_size;
    if (window_size <= 0) {
        window_size = 1;
    }

    if (data_window_count < (uint32_t)window_size) {
        return;
    }
    data_window_count = 0;

    uint64_t sleep_us = (uint64_t)current_config->discontinuous_sleep_time * 1000ULL;
    ESP_LOGI(TAG, "Entrando deep sleep por %ld ms", (long)current_config->discontinuous_sleep_time);


    // En MQTT
    if (current_config->protocol_conf == 0) {
        // Suspende porque envío de flag cierra socket 
        if (xHandleGetResponseMQTT) {
            vTaskSuspend(xHandleGetResponseMQTT);
        }
        char topic_data[128];
        snprintf(topic_data, sizeof(topic_data), "/topic/nebulaedge/%s/data", current_config->id_device);
        mqtt_publish(topic_data, DEEP_SLEEP_FLAG, DEEP_SLEEP_FLAG_LEN, 0);
    }
    

    // En UDP
    if (current_config->protocol_conf == 1) {
        // Suspende porque envío de flag cierra socket 
        if (xHandleGetResponseUDP) {
            vTaskSuspend(xHandleGetResponseUDP);
        }
        nebulaedge_udp_send((uint8_t *)DEEP_SLEEP_FLAG, DEEP_SLEEP_FLAG_LEN);
    }

    // En TCP
    if (current_config->protocol_conf == 2) {
        // Suspende porque envío de flag cierra socket 
        if (xHandleGetResponseTCP) {
            vTaskSuspend(xHandleGetResponseTCP);
        }
        tcp_send((uint8_t *)DEEP_SLEEP_FLAG, DEEP_SLEEP_FLAG_LEN);
    }
    
    // En BLE: no persistir config, partir desde cero al despertar
    if (current_config->protocol_conf == 3) {
        nvs_clear_config();
    } 
    else {
        nvs_save_config(current_config);
    }

    // Delay de precaución
    vTaskDelay(pdMS_TO_TICKS(3000));

    esp_sleep_enable_timer_wakeup(sleep_us);
    esp_deep_sleep_start();
}

// Recibe dos configuraciones y retorna un booleano si son diferentes o iguales
bool config_has_changed(Config *old, Config *new) {
    if (new->config_version > old->config_version) return true;
    if (new->config_version < old->config_version) return false;
    return false;
}

static void send_config_ack_mqtt(const Config *cfg, bool applied) {
    if (!cfg || !cfg->id_device) return;
    ConfigAck ack = CONFIG_ACK__INIT;
    ack.id_device = cfg->id_device;
    ack.config_version = cfg->config_version;
    ack.applied = applied;

    size_t size = config_ack__get_packed_size(&ack);
    uint8_t *buf = malloc(size);
    if (!buf) {
        ESP_LOGE(TAG, "No hay memoria para ACK MQTT");
        return;
    }
    config_ack__pack(&ack, buf);

    char topic_ack[128];
    snprintf(topic_ack, sizeof(topic_ack), "/topic/nebulaedge/%s/config/ack", cfg->id_device);
    mqtt_publish(topic_ack, buf, size, 0);
    free(buf);
}

static void send_config_ack_ble(const Config *cfg, bool applied) {
    if (!cfg || !cfg->id_device) return;
    ConfigAck ack = CONFIG_ACK__INIT;
    ack.id_device = cfg->id_device;
    ack.config_version = cfg->config_version;
    ack.applied = applied;

    size_t size = config_ack__get_packed_size(&ack);
    uint8_t *buf = malloc(size);
    if (!buf) {
        ESP_LOGE(TAG, "No hay memoria para ACK BLE");
        return;
    }
    config_ack__pack(&ack, buf);
    set_char_with_notify(IDX_CHAR_VAL_D_BLE, buf, size);
    free(buf);
}

static void send_config_ack_udp(const Config *cfg, bool applied) {
    if (!cfg || !cfg->id_device) return;
    ConfigAck ack = CONFIG_ACK__INIT;
    ack.id_device = cfg->id_device;
    ack.config_version = cfg->config_version;
    ack.applied = applied;

    size_t size = config_ack__get_packed_size(&ack);
    uint8_t *buf = malloc(size);
    if (!buf) {
        ESP_LOGE(TAG, "No hay memoria para ACK UDP");
        return;
    }
    config_ack__pack(&ack, buf);
    nebulaedge_udp_send(buf, size);
    free(buf);
}

static void send_config_ack_tcp(const Config *cfg, bool applied) {
    if (!cfg || !cfg->id_device) return;
    ConfigAck ack = CONFIG_ACK__INIT;
    ack.id_device = cfg->id_device;
    ack.config_version = cfg->config_version;
    ack.applied = applied;

    size_t size = config_ack__get_packed_size(&ack);
    uint8_t *buf = malloc(size);
    if (!buf) {
        ESP_LOGE(TAG, "No hay memoria para ACK TCP");
        return;
    }
    config_ack__pack(&ack, buf);
    tcp_send(buf, size);
    free(buf);
}

// GEN_DATA: Lee datos de sensores, los empaqueta y los inserta en una xQueue.
void vTaskCollectSensorData(void *pvParameters) {
    for (;;) {
        Data1 data_1 = DATA_1__INIT;
        data_1.id_device = this_device_id;
        data_1.config_version_applied = current_config ? current_config->config_version : 0;

        Data2 data_2 = DATA_2__INIT;
        data_2.id_device = this_device_id;
        data_2.config_version_applied = current_config ? current_config->config_version : 0;

        packet_t packet_1;
        packet_t packet_2;

        // Recogida de datos de sensores
        readout_data_bmm350(&data_1, false, true);
        readout_data_bme688(&data_1, false, true, true, true, true, 2, 16, 1); 
        readout_data_bmi270(&data_2, false, true, true);
        
        // Serializa el mensaje protobuf
        packet_1.size = data_1__get_packed_size(&data_1) + 1;
        packet_1.data = malloc(packet_1.size);
        if (packet_1.data == NULL) {
            ESP_LOGI(TAG_COLLECT_DATA, "Error: no se pudo reservar memoria para el paquete");
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        packet_1.data[0] = 0x01;
        data_1__pack(&data_1, packet_1.data + 1);
        ESP_LOGI(TAG_COLLECT_DATA, "Paquete Data1 generado");

        // Inserción en la queue
        int send = xQueueSend(xQueueData, &packet_1, portMAX_DELAY);
        if (send == pdTRUE) {
            ESP_LOGI(TAG_COLLECT_DATA, "Se ha insertado correctamente en la xQueue");
        }
        else {
            ESP_LOGW(TAG_COLLECT_DATA, "xQueueSend falló (code=%d), paquete descartado y memoria liberada", send);
            free(packet_1.data);
        }
        
        // Ritma la producción
        vTaskDelay(1);

        packet_2.size = data_2__get_packed_size(&data_2) + 1;
        packet_2.data = malloc(packet_2.size);
        if (packet_2.data == NULL) {
            ESP_LOGI(TAG_COLLECT_DATA, "Error: no se pudo reservar memoria para el paquete");
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        packet_2.data[0] = 0x02;
        data_2__pack(&data_2, packet_2.data + 1);
        ESP_LOGI(TAG_COLLECT_DATA, "Paquete Data2 generado");

        // Inserción en la queue
        xQueueSend(xQueueData, &packet_2, portMAX_DELAY);
        if (send == pdTRUE) {
            ESP_LOGI(TAG_COLLECT_DATA, "Se ha insertado correctamente en la xQueue");
        }
        else {
            ESP_LOGW(TAG_COLLECT_DATA, "xQueueSend falló (code=%d), paquete descartado y memoria liberada", send);
            free(packet_2.data);
        }

        // Ritma la producción
        vTaskDelay(1);
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

            // Libera memoria de paquete
            free(packet.data);
            vTaskDelay(pdMS_TO_TICKS((uint32_t)current_config->send_interval_ms) + 1);
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

            send_config_ack_mqtt(current_config, true);
            
            if (xHandleSendMQTT) {     
                vTaskSuspend(xHandleSendMQTT);
                ESP_LOGI(TAG_GET_RSP_MQTT, "Se suspende vTaskSendMQTT");
            }

            if (xHandleCollectSensorData) { 
                // Se resetea queue para que quede vacía
                xQueueReset(xQueueData);
                vTaskSuspend(xHandleCollectSensorData);
                ESP_LOGI(TAG_GET_RSP_MQTT, "Se suspende vTaskCollectSensorData");  
            }

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

            // Delay entre paquetes
            vTaskDelay(pdMS_TO_TICKS((uint32_t)current_config->send_interval_ms) + 1);

            // Delay de precaución en caso de modo discontinuo
            if (current_config->discontinuous_sleep_time > 0) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }

            set_char_with_notify(IDX_CHAR_VAL_B_BLE, packet.data, packet.size);
            free(packet.data);

            // Para evitar que se pierda el último paquete
            if (data_window_count + 1 == (uint32_t)current_config->discontinuous_window_size) {
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
            if (xHandleCollectSensorData) {
                // Se resetea queue para que quede vacía
                xQueueReset(xQueueData);
                ESP_LOGI(TAG_GET_RSP_BLE, "Suspendiendo vTaskCollectSensorData");
                vTaskSuspend(xHandleCollectSensorData);
            }

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
            free(packet.data);
            vTaskDelay(pdMS_TO_TICKS((uint32_t)current_config->send_interval_ms) + 1);

            // // Delay de precaución en caso de modo discontinuo
            // if (current_config->discontinuous_sleep_time > 0) {
            //     vTaskDelay(pdMS_TO_TICKS(1000));
            // }

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

            if (xHandleCollectSensorData) {
                // Se resetea queue para que quede vacía
                xQueueReset(xQueueData);
                ESP_LOGI(TAG_GET_RSP_UDP, "Suspendiendo vTaskCollectSensorData");
                vTaskSuspend(xHandleCollectSensorData);
            }

            send_config_ack_udp(current_config, true);

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
            free(packet.data);
            vTaskDelay(pdMS_TO_TICKS((uint32_t)current_config->send_interval_ms) + 1);
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
            if (xHandleCollectSensorData) {
                // Se resetea queue para que quede vacía
                xQueueReset(xQueueData);
                ESP_LOGI(TAG_GET_RSP_TCP, "Suspendiendo vTaskCollectSensorData");
                vTaskSuspend(xHandleCollectSensorData);
            }

            send_config_ack_tcp(current_config, true);

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
    /******************** INICIALIZACIÓN SENSORES *******************/
    /****************************************************************/

    // Inicializa master bus
    ESP_ERROR_CHECK(i2c_master_init(&bus_handle));

    // Inicializa slaves BMM350, BME688, BMI270
    ESP_ERROR_CHECK(i2c_slave_init(&bus_handle, &device_bmm350, BMM350_SLAVE_ADDR, I2C_MASTER_FREQ_HZ));
    ESP_ERROR_CHECK(i2c_slave_init(&bus_handle, &device_bme688, BME688_SLAVE_ADDR, I2C_MASTER_FREQ_HZ));
    ESP_ERROR_CHECK(i2c_slave_init(&bus_handle, &device_bmi270, BMI270_SLAVE_ADDR, I2C_MASTER_FREQ_HZ));



    /****************************************************************/
    /**************************  COMUNES ****************************/
    /****************************************************************/

    srand((unsigned)time(NULL));
    get_device_id();

    // Inicializa NVS
    ESP_ERROR_CHECK(nvs_flash_init());

    // Inicializa semáforos binario. Parten cerrados.
    semaphore = xSemaphoreCreateBinary();
    semaphore_ble = xSemaphoreCreateBinary();

    // Crea queue para pasar datos entre tasks
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
            data_window_count = 0;
            ESP_LOGI(TAG, "configuración leída correctamente");
        }
    }

    // Inicializa sensores BMM350, BME688, BMI270 con respectivas configuraciones
    bmm350_init(400, 4);                // bmm350_init(ODR = 4, AVG = 4);
    bme688_init();                      // bme688_init();
    bmi270_init(400, 4, 8, 400, 500);   // bmi270_init(ACC_ODR, ACC_AVG, ACC_RANGE, GYR_ODR, GYR_RANGE);

    /****************************************************************/
    /********** ITERACIÓN QUE MANEJA DE CAMBIOS DE PROTOCOLO ********/
    /****************************************************************/
    while (1) {
        // Precaución
        vTaskDelay(3000 / portTICK_PERIOD_MS);
    
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
                    .max_retry = 15,                           // Después de 10 intentos reinicia la ESP
                    .retry_delay_ms = 2500,
                };
                wifi_start_if_needed(&wifi_config);

                // Broker MQTT provisto por configuración (fallback a valor por defecto)
                const char *broker = (current_config && current_config->mqtt_broker && current_config->mqtt_broker[0])
                    ? current_config->mqtt_broker
                    : "mqtt://broker.hivemq.com:1883";

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
                if (!xHandleCollectSensorData)
                    xTaskCreate(vTaskCollectSensorData, TAG_COLLECT_DATA, 4096, NULL, 2, &xHandleCollectSensorData);
                if (!xHandleSendMQTT)
                    xTaskCreate(vTaskSendMQTT, TAG_SEND_MQTT, 4096, NULL, 2, &xHandleSendMQTT);
                if (!xHandleGetResponseMQTT) 
                    xTaskCreate(vTaskGetResponseMQTT, TAG_GET_RSP_MQTT, 4096, NULL, 3, &xHandleGetResponseMQTT);

                vTaskResume(xHandleCollectSensorData);
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
                    .retry_delay_ms = 2500,
                };
                wifi_start_if_needed(&wifi_config);
                
                udp_params_t params = {
                    .ip_host = current_config->host_ip_addr,
                    .port = current_config->udp_port,
                    .ip_version = IPV4,
                };

                nebulaedge_udp_open_socket(&params);

                // Crea una sola vez las tasks
                if (!xHandleCollectSensorData)
                    xTaskCreate(vTaskCollectSensorData, TAG_COLLECT_DATA, 4096, NULL, 2, &xHandleCollectSensorData);
                if (!xHandleSendUDP)
                    xTaskCreate(vTaskSendUDP, TAG_SEND_UDP, 4096, NULL, 2, &xHandleSendUDP);
                if (!xHandleGetResponseUDP) 
                    xTaskCreate(vTaskGetResponseUDP, TAG_GET_RSP_UDP, 4096, NULL, 3, &xHandleGetResponseUDP);

                vTaskResume(xHandleCollectSensorData);
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
                    .retry_delay_ms = 2500,
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
                if (!xHandleCollectSensorData)
                    xTaskCreate(vTaskCollectSensorData, TAG_COLLECT_DATA, 4096, NULL, 2, &xHandleCollectSensorData);
                if (!xHandleSendTCP)
                    xTaskCreate(vTaskSendTCP, TAG_SEND_TCP, 4096, NULL, 2, &xHandleSendTCP);
                if (!xHandleGetResponseTCP) 
                    xTaskCreate(vTaskGetResponseTCP, TAG_GET_RSP_TCP, 4096, NULL, 3, &xHandleGetResponseTCP);

                vTaskResume(xHandleCollectSensorData);
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
                if (!xHandleCollectSensorData)
                    xTaskCreate(vTaskCollectSensorData, TAG_COLLECT_DATA, 4096, NULL, 2, &xHandleCollectSensorData);
                if (!xHandleSendBLE)
                    xTaskCreate(vTaskSendBLE, TAG_SEND_BLE, 4096, NULL, 2, &xHandleSendBLE);
                if (!xHandleGetResponseBLE) 
                    xTaskCreate(vTaskGetResponseBLE, TAG_GET_RSP_BLE, 4096, NULL, 3, &xHandleGetResponseBLE);

                vTaskResume(xHandleCollectSensorData);
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