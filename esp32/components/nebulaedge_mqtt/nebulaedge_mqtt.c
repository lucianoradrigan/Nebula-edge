#include <stdio.h>
#include <stdbool.h>
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

static const char *TAG = "nebulaedge_mqtt";

/* Tópicos suscritos, para poder restaurarlos al reconectar (ver
 * MQTT_EVENT_CONNECTED). Son pocos y fijos -hoy uno solo, el de configuración
 * de este device-, así que una tabla chica alcanza y evita reservar memoria en
 * el camino de un evento. */
#define MQTT_MAX_SUBS 6     // config + manifest + chunk + infer/req en INFER, con margen
#define MQTT_TOPIC_MAX 128

static char s_sub_topics[MQTT_MAX_SUBS][MQTT_TOPIC_MAX];
static int  s_sub_qos[MQTT_MAX_SUBS];
static int  s_sub_count = 0;

static void remember_subscription(const char *topic, int qos) {
    for (int i = 0; i < s_sub_count; i++) {
        if (strcmp(s_sub_topics[i], topic) == 0) {
            s_sub_qos[i] = qos;     // ya estaba: solo se actualiza el qos
            return;
        }
    }
    if (s_sub_count >= MQTT_MAX_SUBS) {
        ESP_LOGW(TAG, "No hay lugar para recordar la suscripcion a %s", topic);
        return;
    }
    snprintf(s_sub_topics[s_sub_count], MQTT_TOPIC_MAX, "%s", topic);
    s_sub_qos[s_sub_count] = qos;
    s_sub_count++;
}


/* Cola de configuraciones entrantes. La pone la aplicación con
 * mqtt_set_config_queue(); el componente no la crea ni la conoce por nombre. */
static QueueHandle_t s_config_queue = NULL;

void mqtt_set_config_queue(QueueHandle_t queue) {
    s_config_queue = queue;
}

/* Cola de lo que llega por /model/manifest y /model/chunk (INFER). */
static QueueHandle_t s_model_queue = NULL;

void mqtt_set_model_queue(QueueHandle_t queue) {
    s_model_queue = queue;
}

/* Variable global para el cliente MQTT */
static esp_mqtt_client_handle_t client = NULL;

/* REARMADO DE MENSAJES PARTIDOS
 *
 * esp-mqtt entrega un mensaje más grande que su buffer (1 KB por defecto) en
 * VARIOS MQTT_EVENT_DATA: el primero trae el tópico y current_data_offset 0,
 * los siguientes solo trozos de datos, y total_data_len dice cuánto mide el
 * mensaje entero. Antes cada evento se trataba como un mensaje completo. No se
 * notaba porque una config pesa menos de 1 KB, pero un trozo de modelo (2 KB)
 * llegaba cortado en dos mensajes inválidos.
 *
 * Los eventos de un mismo mensaje llegan seguidos y desde la única task del
 * cliente, así que alcanza con un buffer en curso. */
#define MQTT_RX_MAX_BYTES  (10 * 1024)   // un trozo de modelo, o una ventana cruda de 8 KB

static uint8_t *s_rx_buf = NULL;
static size_t   s_rx_total = 0;
static size_t   s_rx_got = 0;
static char     s_rx_topic[MQTT_TOPIC_MAX];

static bool topic_ends_with(const char *topic, const char *suffix) {
    size_t t = strlen(topic), s = strlen(suffix);
    return t >= s && strcmp(topic + t - s, suffix) == 0;
}

/* Entrega un mensaje completo. Toma posesión de `buf`.
 *
 * `buf` trae UN BYTE LIBRE al principio y el mensaje desde buf[1]: ahí va el
 * tipo de los mensajes de modelo sin tener que copiarlos. Una ventana cruda de
 * la fase C2 mide 8 KB, y copiarla para anteponerle un byte pedía otros 8 KB
 * contiguos justo cuando menos hay. */
static void rx_dispatch(const char *topic, uint8_t *buf, size_t len) {
    if (strncmp(topic, "/topic/nebulaedge/", 18) != 0) {
        free(buf);
        return;
    }

    if (topic_ends_with(topic, "/config")) {
        ESP_LOGI(TAG, "MQTT config topic: %s", topic);
        /* Se encolan los bytes crudos, sin desempaquetar. Este componente mueve
         * bytes y no tiene por qué conocer el formato de cable; la aplicación,
         * que sí lo conoce, hace el config__unpack en su recv_config. Es el
         * mismo contrato que ya usaba BLE. */
        if (s_config_queue == NULL) {
            ESP_LOGW(TAG, "Sin cola de config (mqtt_set_config_queue no fue llamada), se descarta");
            free(buf);
            return;
        }
        // La config va sin el byte de tipo: se corre el mensaje un lugar.
        memmove(buf, buf + 1, len);
        packet_t pkt = { .size = len, .data = buf };
        if (xQueueSend(s_config_queue, &pkt, 0) != pdTRUE) {
            ESP_LOGW(TAG, "Cola de config llena, configuración descartada");
            free(buf);
        }
        return;
    }

    char kind = 0;
    if (topic_ends_with(topic, "/model/manifest")) kind = MQTT_MODEL_MSG_MANIFEST;
    else if (topic_ends_with(topic, "/model/chunk")) kind = MQTT_MODEL_MSG_CHUNK;
    else if (topic_ends_with(topic, "/infer/req")) kind = MQTT_MODEL_MSG_INFER;

    if (kind == 0 || s_model_queue == NULL) {
        free(buf);
        return;
    }

    /* El tipo va en el primer byte, como en los paquetes de telemetría: la
     * cola es de packet_t y así no hace falta otra estructura. */
    buf[0] = (uint8_t)kind;
    packet_t pkt = { .size = len + 1, .data = buf };
    if (xQueueSend(s_model_queue, &pkt, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Cola de modelo llena, mensaje descartado");
        free(pkt.data);
    }
}

static void rx_fragment(esp_mqtt_event_handle_t event) {
    if (event->current_data_offset == 0) {
        // Empieza un mensaje nuevo. Uno anterior incompleto se descarta.
        free(s_rx_buf);
        s_rx_buf = NULL;
        s_rx_got = 0;
        s_rx_total = (size_t)event->total_data_len;

        snprintf(s_rx_topic, sizeof s_rx_topic, "%.*s", event->topic_len, event->topic);

        if (s_rx_total == 0) {
            ESP_LOGW(TAG, "Mensaje vacío en %s, se descarta", s_rx_topic);
            return;
        }
        if (s_rx_total > MQTT_RX_MAX_BYTES) {
            ESP_LOGW(TAG, "Mensaje de %u B en %s supera el tope de %u B, se descarta",
                     (unsigned)s_rx_total, s_rx_topic, (unsigned)MQTT_RX_MAX_BYTES);
            s_rx_total = 0;
            return;
        }
        s_rx_buf = malloc(s_rx_total + 1);    // +1: el byte libre de rx_dispatch()
        if (s_rx_buf == NULL) {
            ESP_LOGE(TAG, "Sin memoria para un mensaje de %u B", (unsigned)s_rx_total);
            s_rx_total = 0;
            return;
        }
    }

    if (s_rx_buf == NULL) {
        return;     // es parte de un mensaje que ya se decidió descartar
    }

    size_t off = (size_t)event->current_data_offset;
    size_t len = (size_t)event->data_len;
    if (off != s_rx_got || off + len > s_rx_total) {
        ESP_LOGW(TAG, "Fragmento fuera de orden en %s, se descarta el mensaje", s_rx_topic);
        free(s_rx_buf);
        s_rx_buf = NULL;
        return;
    }
    memcpy(s_rx_buf + 1 + off, event->data, len);
    s_rx_got += len;

    if (s_rx_got == s_rx_total) {
        uint8_t *done = s_rx_buf;
        s_rx_buf = NULL;
        rx_dispatch(s_rx_topic, done, s_rx_total);
    }
}

static void resubscribe_all(void) {
    for (int i = 0; i < s_sub_count; i++) {
        int id = esp_mqtt_client_subscribe(client, s_sub_topics[i], s_sub_qos[i]);
        ESP_LOGI(TAG, "Re-suscripcion a %s (msg_id=%d)", s_sub_topics[i], id);
    }
}

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
            /* Re-suscribe lo que ya estaba suscrito.
             *
             * Las suscripciones MQTT son POR SESIÓN y esp-mqtt no las restaura
             * al reconectar: solo vuelve a abrir la conexión. Antes esto no se
             * notaba porque el broker era externo y no se caía nunca; desde que
             * lo hospeda el servidor, el broker muere en cada reinicio del
             * servidor y el device reconectaba SIN suscripción. Publicar no la
             * necesita, así que la telemetría seguía saliendo y el fallo era
             * invisible: el device quedaba sordo a la configuración.
             *
             * Medido en banco: el servidor publicó la config 10 veces en un
             * tópico sin suscriptor (a QoS 0 el broker las descarta), el
             * handshake agotó sus reintentos y la config terminó entrando por
             * el canal de rescate BLE a los 50 s. */
            resubscribe_all();
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
            rx_fragment(event);
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
        remember_subscription(topic, qos);
        msg_id = esp_mqtt_client_subscribe(client, topic, qos); 
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    return msg_id;
}

/* Finaliza mqtt por completo. */
void mqtt_finish(void) {
    if (client != NULL) {
        esp_mqtt_client_stop(client);
        esp_mqtt_client_destroy(client);
        client = NULL;
        /* Las suscripciones recordadas viven lo que vive el cliente: el que
         * venga despues las va a pedir de nuevo en mqtt_open().
         *
         * Sin esto quedaba suscrito DOS veces al mismo topico tras un cambio de
         * configuracion: resubscribe_all() las restauraba al reconectar y
         * mqtt_open() volvia a pedir la suya. El broker entrega el mensaje
         * retenido una vez por suscripcion, asi que el device recibia la misma
         * config varias veces y respondia un ACK por cada una. No rompia nada
         * -una config repetida es idempotente- pero era ruido evitable. */
        s_sub_count = 0;
        // Un mensaje a medio rearmar no le sirve al cliente que venga después.
        free(s_rx_buf);
        s_rx_buf = NULL;
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