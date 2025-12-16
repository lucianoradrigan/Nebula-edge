#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "freertos/semphr.h"

#include "nebulaedge_wifi.h"
#include "nebulaedge_mqtt.h"
#include "nebulaedge_udp.h"
#include "nebulaedge_tcp.h"
#include "nebulaedge_ble.h"
#include "nebulaedge_defs.h"

#include "schema.pb-c.h"

QueueHandle_t xQueueConfig;
QueueHandle_t xQueueData;

Config *current_config = NULL;
TaskHandle_t xHandleGenRandData = NULL;

TaskHandle_t xHandleSendUDP = NULL;
TaskHandle_t xHandleGetResponseUDP = NULL;
TaskHandle_t xHandleSendTCP = NULL;
TaskHandle_t xHandleGetResponseTCP = NULL;
TaskHandle_t xHandleSendMQTT = NULL;
TaskHandle_t xHandleGetResponseMQTT = NULL;
TaskHandle_t xHandleSendBLE = NULL;
TaskHandle_t xHandleGetResponseBLE = NULL;



const char *TAG = "main_task";
const char *TAG_RAND_DATA = "task_rand_data"; 

const char *TAG_SEND_MQTT = "task_send_mqtt";
const char *TAG_SEND_UDP = "task_send_udp";
const char *TAG_SEND_TCP = "task_send_tcp";
const char *TAG_SEND_BLE = "task_send_ble";

const char *TAG_GET_RSP_MQTT = "task_get_rsp_mqtt";
const char *TAG_GET_RSP_BLE = "task_get_rsp_ble";
const char *TAG_GET_RSP_TCP = "task_get_rsp_tcp";
const char *TAG_GET_RSP_UDP = "task_get_rsp_udp";


// ACORDARSE DEL DEEP SLEEP

void busy_wait_10s_ticks(void) {
    TickType_t start = xTaskGetTickCount();
    TickType_t target = start + pdMS_TO_TICKS(10000);
    while (xTaskGetTickCount() < target) {
        // esp_task_wdt_reset(); // si necesario
    }
}

// Imprime los tasks activos
void print_active_tasks(void) {
    char buffer[1024];
    vTaskList(buffer);
    printf("Nombre      Estado Prio Stack Num\n");
    printf("%s\n", buffer);
}


// config: Recibe dos configuraciones y retorna un booleano si son diferentes o iguales
// esta responsabilidad se le puede pasar a la raspberry
bool config_has_changed(Config *old, Config *new) {
    if (old == NULL || new == NULL) return true;
    // Compara los campos relevantes
    if (old->protocol_conf != new->protocol_conf) return true;
    if (strcmp(old->host_ip_addr, new->host_ip_addr) != 0) return true;
    if (old->tcp_port != new->tcp_port) return true;
    if (old->udp_port != new->udp_port) return true;
    // ...agrega más campos si lo necesitas...
    return false;
}

// GEN_DATA: Genera datos random y los inserta en una xQueue.
void vTaskGenRandData(void *pvParameters) {

    for (;;) {
        Data1 data_1 = DATA_1__INIT;
        data_1.id_device   = rand() % 100;                            // id entre 0 y 99
        data_1.temperature = rand() % 100;                            // temperatura entre 0 y 99
        data_1.press       = rand() % 1100;                           // presión entre 0 y 1099
        data_1.co          = ((float)rand() / RAND_MAX) * 10.0f;      // CO entre 0.0 y 10.0
        data_1.rms         = ((float)rand() / RAND_MAX) * 5.0f;       // RMS entre 0.0 y 5.0
        data_1.amp_x       = ((float)rand() / RAND_MAX) * 2.0f;       // amp_x entre 0.0 y 2.0
        data_1.freq_x      = ((float)rand() / RAND_MAX) * 100.0f;     // freq_x entre 0.0 y 100.0
        data_1.amp_y       = ((float)rand() / RAND_MAX) * 2.0f;
        data_1.freq_y      = ((float)rand() / RAND_MAX) * 100.0f;
        data_1.amp_z       = ((float)rand() / RAND_MAX) * 2.0f;
        data_1.freq_z      = ((float)rand() / RAND_MAX) * 100.0f;

        packet_t packet;

        // Prueba a generar datos cada 10 segundos (lento)
        // vTaskDelay(pdMS_TO_TICKS(10000));
        busy_wait_10s_ticks();

        // Serializa el mensaje protobuf
        packet.size = data_1__get_packed_size(&data_1);
        packet.data = malloc(packet.size);
        if (packet.data == NULL) {
            ESP_LOGI(TAG_RAND_DATA, "Error: no se pudo reservar memoria para el paquete");
            continue;
        }
        data_1__pack(&data_1, packet.data);
        ESP_LOGI(TAG_RAND_DATA, "Paquete random generado");

        // Importante sobre queues: xQSend bloquea la task por x ms cuando 
        // la Queue está llena, y cuando pasa el tiempo retorna aunque esté
        // llena o no
        int send = xQueueSend(xQueueData, &packet, 100 / portTICK_PERIOD_MS);
        
        // Caso inserción correcta en la xQueue
        if (send == pdTRUE) {
            ESP_LOGI(TAG_RAND_DATA, "Se ha insertado correctamente en la xQueue");
        }

        // Caso xQueue llena: descarta el paquete generado
        else if (send == errQUEUE_FULL) {
            ESP_LOGW(TAG_RAND_DATA, "xQueueSend: QUEUE FULL, paquete descartado y memoria liberada");
            free(packet.data);
        }
    }
}

// MQTT: Si aún no hay datos, send espera a gen_rand_data para
// recibir datos.
void vTaskSendMQTT(void *pvParameters) {
    
    packet_t packet;
    for (;;) {

        int msg_id;
        // Chequea conexión wifi
        // wifi_check_connection();

        // Recibe un paquete desde la xQueue. Con portMAX_DELAY espera a 
        // que haya un elemento en la cola
        int receive = xQueueReceive(xQueueData, &packet, portMAX_DELAY);

        // Caso recepción correcta desde la cola
        if (receive == pdTRUE) {

            msg_id = mqtt_publish("/topic/nebulaedge/data", packet.data, packet.size, 0);
            // Logs
            if (msg_id < 0) {
                ESP_LOGE(TAG_SEND_MQTT, "Error al publicar por MQTT. msg_id=%d", msg_id);
            } 
            else {
                ESP_LOGI(TAG_SEND_MQTT, "Paquete publicado por MQTT. msg_id=%d", msg_id);
            }

            // Libera memoria de paquete
            free(packet.data);
        }

        // Caso en que haya error al recibir desde la cola
        else if (receive == pdFALSE) {
            ESP_LOGW(TAG_SEND_MQTT, "No se recibió ningún paquete de la xQueue");
        }
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

        if (config_has_changed(current_config, new_config)) {
            ESP_LOGI(TAG_GET_RSP_MQTT, "Configuración cambiada!");

            if (current_config) config__free_unpacked(current_config, NULL);
            current_config = new_config;

            print_active_tasks();
            
            if (xHandleSendMQTT) {     
                vTaskSuspend(xHandleSendMQTT);
                ESP_LOGI(TAG_GET_RSP_MQTT, "Se suspende vTaskSendMQTT");
            }

            if (xHandleGenRandData) { 
                ESP_LOGI(TAG_GET_RSP_MQTT, "entra en el if al menos");
                
                // Se resetea queue para que quede vacía
                xQueueReset(xQueueData);

                vTaskSuspend(xHandleGenRandData); // Puede que tenga que ver con la memoria
                                                    // guardada en rand_data

                ESP_LOGI(TAG_GET_RSP_MQTT, "Se suspende vTaskGenRandData");  
            }

            print_active_tasks();
            mqtt_finish();

            
            // El semáforo aquí queda tomado
            if (xSemaphoreGive(semaphore)) {
                ESP_LOGI(TAG_GET_RSP_MQTT, "vTaskGetResponseMQTT libera semáforo para cambio de protocolo");
            }

            // Auto-suspende esta task hasta que el protocolo MQTT vuelva a activarse
            ESP_LOGI(TAG_GET_RSP_MQTT, "Suspendiendo vTaskGetResponseMQTT");  
            vTaskSuspend(NULL);
        } 
        else {
            ESP_LOGI(TAG_GET_RSP_MQTT, "La configuración recibida es la misma");
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
            set_char_b(packet.data, packet.size);
            free(packet.data);
        }
    }
}

// BLE response
void vTaskGetResponseBLE(void *pvParameters) {

    const int buffer_len = 256;
    uint8_t buffer[buffer_len];
    const int time_to_wait_ms = 200;

    for (;;) {
        // Evita busy loop
        vTaskDelay(pdMS_TO_TICKS(time_to_wait_ms));

        // Lee la característica A (config)
        int read_bytes = get_char_a(buffer, buffer_len);
        if (read_bytes <= 0) {
            continue;
        }

        // Desempaqueta la configuración recibida
        Config *new_config = config__unpack(NULL, read_bytes, buffer);
        if (new_config == NULL) {
            ESP_LOGW(TAG_GET_RSP_BLE, "Error al desempaquetar el mensaje protobuf de configuración");
            continue;
        }

        ESP_LOGI(TAG_GET_RSP_BLE, "BLE: Se recibió información de configuración!");

        if (config_has_changed(current_config, new_config)) {
            ESP_LOGI(TAG_GET_RSP_BLE, "Configuración cambiada!");

            // Reemplaza la configuración global
            if (current_config) config__free_unpacked(current_config, NULL);
            current_config = new_config;

            if (xHandleSendBLE) {
                ESP_LOGI(TAG_GET_RSP_BLE, "Suspendiendo vTaskSendBLE");
                vTaskSuspend(xHandleSendBLE);
            }
            if (xHandleGenRandData) {
                // Se resetea queue para que quede vacía
                xQueueReset(xQueueData);
                ESP_LOGI(TAG_GET_RSP_BLE, "Suspendiendo vTaskGenRandData");
                vTaskSuspend(xHandleGenRandData);
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
        }
    }
}

// UDP: Pide configuración a la Raspberry por UDP. Hay que liberar el puntero Config *!!
void vTaskGetResponseUDP(void *pvParameters) {

    size_t len = 256;
    uint8_t buffer[len];

    for (;;) {
        // Se queda bloqueado en esta llamada (y cede la cpu) hasta 
        // recibir algo

        print_active_tasks();
        size_t len_recv = nebulaedge_udp_receive(buffer, len);

        if (len_recv == 0) {
            ESP_LOGI(TAG_GET_RSP_UDP, "no han llegado nuevos datos de configuración");
            continue;
        }

        Config *new_config = config__unpack(NULL, len_recv, buffer);
        if (!new_config) {
            ESP_LOGI(TAG_GET_RSP_UDP, "error al desempaquetar");
            continue;
        }

        if (config_has_changed(current_config, new_config)) {
            ESP_LOGI(TAG_GET_RSP_UDP, "configuración cambiada!");

            if (current_config) config__free_unpacked(current_config, NULL);
            current_config = new_config;

            if (xHandleSendUDP) {
                ESP_LOGI(TAG_GET_RSP_UDP, "Suspendiendo vTaskSendUDP");
                vTaskSuspend(xHandleSendUDP);
            }

            if (xHandleGenRandData) {
                // Se resetea queue para que quede vacía
                xQueueReset(xQueueData);
                ESP_LOGI(TAG_GET_RSP_UDP, "Suspendiendo vTaskGenRandData");
                vTaskSuspend(xHandleGenRandData);
            }

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
        }
    }
}

// TCP: Pide configuración a la Raspberry por TCP. Hay que liberar el puntero Config *!!
void vTaskGetResponseTCP(void *pvParameters) {

    size_t len = 256;
    uint8_t buffer[len];
    // int time_to_wait_ms = 10;

    for (;;) {

        // Espera recepción de datos de configuración
        size_t len_recv = tcp_receive(buffer, len);
        if (len_recv == 0) {
            // sin datos/timeout
            continue;
        }

        Config *new_config = config__unpack(NULL, len_recv, buffer);   // Esta memoria hay que liberarla!
        
        if (!new_config) {
            ESP_LOGI(TAG_GET_RSP_TCP, "TCP: error al desempaquetar");
            continue;
        }

        if (config_has_changed(current_config, new_config)) {
            ESP_LOGI(TAG_GET_RSP_TCP, "TCP: Configuración cambiada!");

            // Libera la configuración anterior
            if (current_config) config__free_unpacked(current_config, NULL);

            // Actualiza la configuración global
            current_config = new_config;

            if (xHandleSendTCP) {
                ESP_LOGI(TAG_GET_RSP_TCP, "Suspendiendo vTaskSendTCP");
                vTaskSuspend(xHandleSendTCP);
            }
            if (xHandleGenRandData) {
                // Se resetea queue para que quede vacía
                xQueueReset(xQueueData);
                ESP_LOGI(TAG_GET_RSP_TCP, "Suspendiendo vTaskGenRandData");
                vTaskSuspend(xHandleGenRandData);
            }

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
            config__free_unpacked(new_config, NULL);
        }    
    }
}

void app_main() {

    /* ***************************************************************/
    /* *************************  COMUNES ****************************/
    /* ***************************************************************/

    srand((unsigned)time(NULL));

    // Inicializa NVS
    ESP_ERROR_CHECK(nvs_flash_init());

    // Inicializa semáforo binario. Parte cerrado.
    semaphore = xSemaphoreCreateBinary();

    // Crea queue para pasar datos entre tasks
    xQueueData = xQueueCreate(100, sizeof(packet_t));
    xQueueConfig = xQueueCreate(5, sizeof(Config *));


    /* ***************************************************************/
    /* ***********************  BLE INICIO  **************************/
    /* ***************************************************************/

    // Inicializa BLE
    ble_init();

    // Semáforo se libera cuando se escribe configuración en charact. C de BLE.
    // Mientras tanto queda bloqueado aquí.
    if (xSemaphoreTake(semaphore, portMAX_DELAY)) {
        ESP_LOGI(TAG, "toma semáforo, continúa ejecución principal");
    }

    // Buffer de datos donde se recibirá información vía BLE
    int buffer_len = 256;
    uint8_t buffer[buffer_len];

    // Lee característica A de BLE y guarda en buffer
    int read_bytes = get_char_a(buffer, buffer_len);

    // NO OLVIDAR LOS LOGS DE VUELTA

    // Desempaqueta el mensaje protobuf
    current_config = config__unpack(NULL, read_bytes, buffer);
    if (current_config == NULL) {
        ESP_LOGI(TAG, "Error al desempaquetar el mensaje protobuf");
    }

    /* ***************************************************************/
    /* *************************  WIFI *******************************/
    /* ***************************************************************/

    // OBS: no se ha considerado el caso en que haya que autenticarse a
    // una red wifi con username -> caso muy específico

    // Se inicializa el wifi una sola vez, se use o no; por tema de simplicidad. Se puede mejorar.
    // Igual hay que reiniciarlo pues las nuevas configuraciones pueden entregar una nueva red wifi

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Estructura de configuración de wifi
    global_wifi_config wifi_config = {
        .ssid = current_config->ssid,
        .password = current_config->passwd,

        // Hacer enums para simplificar
        .auth_mode = WIFI_AUTH_WPA_WPA2_PSK,
        .max_retry = 10,
        .retry_delay_ms = 5000,
    };

    wifi_init_sta(&wifi_config);

    while (1) {

        // Pequeño delay precaución
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    

        /* ***************************************************************/
        /* *************************  MQTT *******************************/
        /* ***************************************************************/
        switch (current_config->protocol_conf) {

            case 0: {

                // Valores de configuración (por ahora solo campo broker)
                char *broker = "mqtt://broker.hivemq.com:1883";

                // Estructura de configuración MQTT: debe ser visible desde main
                mqtt_config_global mqtt_config = {
                    .broker = broker,
                };

                // Empieza la conexión mqtt en el broker configurado: visibilidad main
                mqtt_start(&mqtt_config);

                // Da tiempo a la Raspberry para conectarse al broker
                vTaskDelay(5000 / portTICK_PERIOD_MS);

                // Suscribe al tópico de configuración
                mqtt_subscribe("/topic/nebulaedge/config", 0);
                
                // Crea una sola vez las tasks
                if (!xHandleGenRandData)
                    xTaskCreatePinnedToCore(vTaskGenRandData, TAG_RAND_DATA, 4096, NULL, 1, &xHandleGenRandData, 0);
                if (!xHandleSendMQTT)
                    xTaskCreatePinnedToCore(vTaskSendMQTT, TAG_SEND_MQTT, 4096, NULL, 1, &xHandleSendMQTT, 0);
                if (!xHandleGetResponseMQTT) 
                    xTaskCreatePinnedToCore(vTaskGetResponseMQTT, TAG_GET_RSP_MQTT, 4096, NULL, 2, &xHandleGetResponseMQTT, 0);

                vTaskResume(xHandleGenRandData);
                vTaskResume(xHandleSendMQTT);
                vTaskResume(xHandleGetResponseMQTT);

                // Punto de bloqueo
                if (xSemaphoreTake(semaphore, portMAX_DELAY)) {
                    ESP_LOGI(TAG, "Se sale de MQTT (cambio de protocolo)");
                }

                break;
            }

            /* ***************************************************************/
            /* *************************  UDP  *******************************/
            /* ***************************************************************/

            case 1: {
                
                udp_params_t params = {
                    .ip_host = current_config->host_ip_addr,
                    .port = current_config->udp_port,
                    .ip_version = IPV4,
                };

                nebulaedge_udp_open_socket(&params);

                // Crea una sola vez las tasks
                if (!xHandleGenRandData)
                    xTaskCreatePinnedToCore(vTaskGenRandData, TAG_RAND_DATA, 4096, NULL, 4, &xHandleGenRandData, 1);
                if (!xHandleSendUDP)
                    xTaskCreatePinnedToCore(vTaskSendUDP, TAG_SEND_UDP, 4096, NULL, 2, &xHandleSendUDP, 0);
                if (!xHandleGetResponseUDP) 
                    xTaskCreatePinnedToCore(vTaskGetResponseUDP, TAG_GET_RSP_UDP, 4096, NULL, 3, &xHandleGetResponseUDP, 0);

                vTaskResume(xHandleGenRandData);
                vTaskResume(xHandleSendUDP);
                vTaskResume(xHandleGetResponseUDP);

                // Punto de bloqueo
                if (xSemaphoreTake(semaphore, portMAX_DELAY)) {
                    ESP_LOGI(TAG, "se libera semáforo, se sale de UDP correctamente");
                }

                break;
            }


            /* ***************************************************************/
            /* *************************  TCP  *******************************/
            /* ***************************************************************/

            case 2: {

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
                    continue;
                }

                // Crea una sola vez las tasks
                if (!xHandleGenRandData)
                    xTaskCreatePinnedToCore(vTaskGenRandData, TAG_RAND_DATA, 4096, NULL, 1, &xHandleGenRandData, 1);
                if (!xHandleSendTCP)
                    xTaskCreatePinnedToCore(vTaskSendTCP, TAG_SEND_TCP, 4096, NULL, 1, &xHandleSendTCP, 0);
                if (!xHandleGetResponseTCP) 
                    xTaskCreatePinnedToCore(vTaskGetResponseTCP, TAG_GET_RSP_TCP, 4096, NULL, 2, &xHandleGetResponseTCP, 0);

                vTaskResume(xHandleGenRandData);
                vTaskResume(xHandleSendTCP);
                vTaskResume(xHandleGetResponseTCP);

                // Punto de bloqueo
                if (xSemaphoreTake(semaphore, portMAX_DELAY)) {
                    ESP_LOGI(TAG, "Se sale de TCP correctamente");
                }

                break;
            }
            
            /* ***************************************************************/
            /* *************************  BLE  *******************************/
            /* ***************************************************************/
            
            case 3: {
                
                ESP_LOGI(TAG, "Esperando conexión BLE para iniciar tasks...");
    
                // Semáforo se libera cuando se escribe configuración en charact. C de BLE.
                // Mientras tanto queda bloqueado aquí.
                if (xSemaphoreTake(semaphore, portMAX_DELAY)) {
                    ESP_LOGI(TAG, "toma semáforo, empieza recepción BLE");
                }

                // Crea una sola vez las tasks
                if (!xHandleGenRandData)
                    xTaskCreatePinnedToCore(vTaskGenRandData, TAG_RAND_DATA, 4096, NULL, 1, &xHandleGenRandData, 0);
                if (!xHandleSendBLE)
                    xTaskCreatePinnedToCore(vTaskSendBLE, TAG_SEND_BLE, 4096, NULL, 1, &xHandleSendBLE, 0);
                if (!xHandleGetResponseBLE) 
                    xTaskCreatePinnedToCore(vTaskGetResponseBLE, TAG_GET_RSP_BLE, 4096, NULL, 2, &xHandleGetResponseBLE, 0);

                vTaskResume(xHandleGenRandData);
                vTaskResume(xHandleSendBLE);
                vTaskResume(xHandleGetResponseBLE);

                // Punto de bloqueo
                if (xSemaphoreTake(semaphore, portMAX_DELAY)) {
                    ESP_LOGI(TAG, "se libera semáforo, se sale de BLE correctamente");
                }

                break;
            }

            /* Protocolo inválido */
            default: {
                ESP_LOGE(TAG, "Selección de protocolo inválida: %ld", current_config->protocol_conf);
                vTaskDelay(pdMS_TO_TICKS(1000));
                break;
            }
        }
    }
}