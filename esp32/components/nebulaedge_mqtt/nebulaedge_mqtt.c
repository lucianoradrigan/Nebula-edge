#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "esp_system.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "nebulaedge_mqtt.h"
#include "nebulaedge_defs.h"

#include "schema.pb-c.h"

static const char *TAG = "nebulaedge_mqtt";
extern QueueHandle_t xQueueConfig;

/* Variable global para el cliente MQTT */
static esp_mqtt_client_handle_t client = NULL;

/**
 * @brief Logs an error message if the provided error code is non-zero
 * 
 * This utility function checks if an error code is non-zero and logs an error
 * message with the error code in hexadecimal format if an error is detected.
 * 
 * @param message Descriptive message to include in the error log
 * @param error_code Error code to check (0 indicates no error)
 */
static void log_error_if_nonzero(const char *message, int error_code) {
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

/*
 * @brief Event handler registered to receive MQTT events
 *
 *  This function is called by the MQTT client event loop.
 *
 * @param handler_args user data registered to the event.
 * @param base Event base for the handler(always MQTT Base in this example).
 * @param event_id The id for the received event.
 * @param event_data The data for the event, esp_mqtt_event_handle_t.
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    // Debug log indicating the event received from the MQTT event loop
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32 "", base, event_id);

    // MQTT event handler
    esp_mqtt_event_handle_t event = event_data;

    // Switch to handle the different types of MQTT events
    switch ((esp_mqtt_event_id_t)event_id) {

        // MQTT_EVENT_BEFORE_CONNECT: additional event

        // The client has successfully established a connection to the broker. 
        // Client is now ready to send and receive data.
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "CONNECTED: succesfully conected to broker");
            break;

        // The client has aborted the connection due to being unable to read 
        // or write data, e.g., because the server is unavailable.
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "DISCONNECTED: connection aborted");
            break;

        // The broker has acknowledged the client's subscribe request. 
        // The event data contains the message ID of the subscribe message.
        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "SUBSCRIBE, ACK from broker, msg_id=%d", event->msg_id);
            break;
        
        // The broker has acknowledged the client's unsubscribe request. 
        // The event data contains the message ID of the unsubscribe message.
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(TAG, "UNSUSBCRIBE: ACK from broker, msg_id=%d", event->msg_id);
            break;

        // The broker has acknowledged the client's publish message. This is 
        // only posted for QoS level 1 and 2, as level 0 does not use acknowledgements. 
        // The event data contains the message ID of the publish message.
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "PUBLISH: ACK from broker, msg_id=%d", event->msg_id);
            break;

        // The client has received a publish message. The event data contains: message ID, 
        // name of the topic it was published to, received data and its length. For data 
        // that exceeds the internal buffer, multiple MQTT_EVENT_DATA events are posted and
        // current_data_offset and total_data_len from event data updated to keep track of 
        // the fragmented message.
        case MQTT_EVENT_DATA:

        // Verifica si el tópico es el de configuración
            if (strncmp(event->topic, "/topic/nebulaedge/config", event->topic_len) == 0) {
                Config *new_config = config__unpack(NULL, event->data_len, (uint8_t *)event->data);

                if (new_config == NULL) {
                    ESP_LOGE(TAG, "Error al desempaquetar configuración MQTT");
                } 
                else {
                    ESP_LOGI(TAG, "Configuración MQTT desempaquetada correctamente");
                    // Envía el puntero a la queue para que main lo procese
                    if (xQueueSend(xQueueConfig, &new_config, 0) != pdTRUE) {
                        ESP_LOGW(TAG, "xConfigQueue FULL, configuración descartada");
                        config__free_unpacked(new_config, NULL);
                    }
                }
            }
            break;

        // The client has found an error. The field error_handle in the event data 
        // contains error_type that can be used to identify the error. The type of 
        // error determines which parts of the error_handle struct is filled.
        case MQTT_EVENT_ERROR:
            ESP_LOGI(TAG, "ERROR: error found");
            if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
                log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
                log_error_if_nonzero("captured as transport's socket errno",  event->error_handle->esp_transport_sock_errno);
                ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
            }
            break;
        default:
            ESP_LOGI(TAG, "Other event id:%d", event->event_id);
            break;
    }
}

/**
 * @brief Initializes and starts the MQTT client with the provided configuration.
 * 
 * This function sets up the MQTT client using the broker configuration passed as parameter.
 * It configures logging levels for various ESP-IDF components, initializes the MQTT client
 * with the specified broker URI, registers event handlers, and starts the MQTT client.
 * 
 * @param mqtt_config_global Pointer to the global MQTT configuration structure containing
 *                          the broker information and other connection parameters.
 * 
 * @note This function sets the global client variable and should be called after
 *       network connectivity is established.
 * 
 * @warning The function modifies global logging levels for multiple ESP-IDF components.
 * 
 * @see mqtt_event_handler
 * @see esp_mqtt_client_init
 * @see esp_mqtt_client_start
 */
void mqtt_start(const mqtt_config_global *mqtt_config_global) {

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("mqtt_client", ESP_LOG_VERBOSE);
    esp_log_level_set("mqtt_example", ESP_LOG_VERBOSE);
    esp_log_level_set("transport_base", ESP_LOG_VERBOSE);
    esp_log_level_set("esp-tls", ESP_LOG_VERBOSE);
    esp_log_level_set("transport", ESP_LOG_VERBOSE);
    esp_log_level_set("outbox", ESP_LOG_VERBOSE);
    
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = mqtt_config_global->broker,
    };

    // Assigns the global variable
    client = esp_mqtt_client_init(&mqtt_cfg);

    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
}

/**
 * @brief Publishes a byte array to a specified MQTT topic.
 *
 * This function sends a message to the specified MQTT topic with the given
 * Quality of Service (QoS) level. It returns the message ID of the published
 * message or -1 if the MQTT client is not initialized.
 *
 * @param topic The MQTT topic to publish the message to.
 * @param data A pointer to the byte array containing the message payload.
 * @param qos The Quality of Service level for the message (0, 1, or 2).
 * @return The message ID of the published message, -1 on failure or
 *         -2 in case of full outbox.
 */
int mqtt_publish(const char *topic, const uint8_t *data, size_t len, int qos) {
    // Checks if the client is initialized
    if (client == NULL) {
        ESP_LOGE(TAG, "MQTT client is not initialized");
        return -1;
    }

    // Publishes the message
    int msg_id = esp_mqtt_client_publish(client, topic, (char*)data, len, qos, 0);
    return msg_id;
}

/**
 * @brief Subscribes to a specific MQTT topic with a given QoS level.
 *
 * This function allows the MQTT client to subscribe to a specified topic
 * with the desired Quality of Service (QoS) level. It returns the message
 * ID of the subscription request, which can be used to track the status
 * of the subscription.
 *
 * @param topic The MQTT topic to subscribe to. Must be a valid string.
 * @param qos The Quality of Service level for the subscription. Valid values are:
 *            - 0: At most once
 *            - 1: At least once
 *            - 2: Exactly once
 * @return The message ID of the subscription request on success, or -1 if the
 *         MQTT client is not initialized.
 *
 * @note Ensure that the MQTT client is properly initialized before calling
 *       this function. If the client is not initialized, an error will be
 *       logged, and the function will return -1.
 */
int mqtt_subscribe(const char *topic, int qos) {
    // Checks if the client is initialized
    if (client == NULL) {
        ESP_LOGE(TAG, "MQTT client is not initialized");
        return -1;
    }

    // Reintenta hasta poder suscribir correctamente
    int msg_id = -1;
    while (msg_id < 0) {
        msg_id = esp_mqtt_client_subscribe(client, topic, qos); 
        vTaskDelay(200 / portTICK_PERIOD_MS);
    }

    return msg_id;
}

/* Finaliza mqtt por completo. */
void mqtt_finish(void) {
    if (client != NULL) {
        esp_mqtt_client_stop(client);
        esp_mqtt_client_destroy(client);
        client = NULL;
        ESP_LOGI(TAG, "MQTT client cerrado correctamente.");
    }
}

// Publica datos continuamente vía MQTT, un test packet.
// mode: 0 para modo discontinuo y 1 para modo continuo
// ESTO IRÁ DESPUÉS EN MAIN
// static const int ID_DEVICE = 12345;
// int mqtt_publish(int mode) {
//     int msg_id;

//     // Inicialización de estructura protobuf
//     Measure packet = MEASURE__INIT;


//     // Loop de envío de datos
//     while (1) {
//         // Se asignan los valores a enviar en la estructura protobuf
//         packet.id_device = ID_DEVICE;
//         packet.timestamp_esp = esp_log_timestamp();
//         printf("time in milliseconds is %ld\n", packet.timestamp_esp);

//         // Serializar la estructura (en variable buffer)
//         size_t packed_size = measure__get_packed_size(&packet);     // Obtiene el tamaño del empaquetado
//         uint8_t buffer[packed_size];
//         printf("%d bytes a enviar\n", packed_size); 
//         measure__pack(&packet, buffer);                             // Empaqueta

//         // Publicar mensaje en broker MQTT
//         msg_id = esp_mqtt_client_publish(client, "/topic/nebulaedge", (char*)buffer, packed_size, 1, 0);
//         ESP_LOGI(TAG, "sent publish successful, msg_id=%d, timestamp: %ld", msg_id, packet.timestamp_esp);
        
//         // Entra en modo deep sleep si mode == 0
//         if (mode == 0) {
//             ESP_LOGI(TAG, "Entering deep sleep for %d seconds", 10);
//             esp_deep_sleep(1000000LL * 10);
//         }
//         else {
//             // Delay 500 ms between packets
//             vTaskDelay(500 / portTICK_PERIOD_MS); 
//         }
//     }
// }