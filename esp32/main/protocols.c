/* Las piezas propias de cada protocolo y la tabla que las junta. Ver
 * protocols.h. Salió de main.c tal cual. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nebulaedge_mqtt.h"
#include "nebulaedge_udp.h"
#include "nebulaedge_tcp.h"
#include "nebulaedge_ble.h"
#include "nebulaedge_defs.h"
#include "infer.h"
#include "schema.pb-c.h"
#include "app.h"
#include "protocols.h"

/* Tamaño del buffer donde se recibe una config cruda en UDP y TCP. Vive en la
 * pila de la task de respuesta (4096 B), así que hay espacio de sobra. */
#define CONFIG_RECV_BUF_BYTES 256



/* ------------------------------------------------------------------- MQTT */

static esp_err_t mqtt_send_data(const uint8_t *data, size_t size) {
    char topic_data[128];
    mqtt_topic(topic_data, sizeof topic_data, current_config->id_device, MQTT_TOPIC_DATA);

    /* esp-mqtt devuelve el msg_id, o negativo si falló. Se traduce a esp_err_t
     * para que los cuatro transportes hablen el mismo idioma. */
    int msg_id = mqtt_publish(topic_data, data, size, 0);
    if (msg_id < 0) {
        return ESP_FAIL;
    }

    ESP_LOGD(TAG_SEND_MQTT, "Paquete publicado por MQTT. msg_id=%d", msg_id);
    return ESP_OK;
}

static esp_err_t mqtt_send_ack(const Config *cfg, const uint8_t *buf, size_t size) {
    char topic_ack[128];
    mqtt_topic(topic_ack, sizeof topic_ack, cfg->id_device, MQTT_TOPIC_CONFIG_ACK);
    return mqtt_publish(topic_ack, buf, size, 0) < 0 ? ESP_FAIL : ESP_OK;
}

/* La config llega cruda por la cola y se desempaqueta acá, igual que en BLE:
 * los componentes de transporte mueven bytes y no conocen el formato de cable.
 *
 * Por MQTT llega como [retenida][protobuf] (ver rx_dispatch() en
 * nebulaedge_mqtt.c). De una retenida se borra time_client: su hora es la de
 * cuando se publicó, no la de ahora, y no debe tocar el reloj. */
static Config *mqtt_recv_config(void) {
    packet_t pkt;

    if (xQueueReceive(xQueueConfig, &pkt, pdMS_TO_TICKS(TASK_POLL_MS)) != pdTRUE) {
        return NULL;    // nada todavía; la task vuelve a mirar la compuerta
    }

    if (pkt.data == NULL || pkt.size < 2) {
        free(pkt.data);
        return NULL;
    }

    bool retained = pkt.data[0] != 0;
    Config *cfg = config__unpack(NULL, pkt.size - 1, pkt.data + 1);
    free(pkt.data);

    if (cfg == NULL) {
        ESP_LOGW(TAG_GET_RSP_MQTT, "Error al desempaquetar la configuración MQTT");
    }
    else if (retained) {
        ESP_LOGI(TAG_GET_RSP_MQTT, "Config v%ld retenida: no se usa su hora", (long)cfg->config_version);
        cfg->time_client = 0;
    }
    return cfg;
}

/* Conecta al broker, espera a que el servidor llegue también y se suscribe al
 * tópico de configuración de este device. */
static bool mqtt_open(void) {
    mqtt_config_global mqtt_config = {
        .broker = current_config->mqtt_broker,
    };
    mqtt_start(&mqtt_config);

    // Le da tiempo a la Raspberry para conectarse al broker antes de enviar.
    vTaskDelay(pdMS_TO_TICKS(SERVER_READY_MS));

    char topic_cfg[128];
    mqtt_topic(topic_cfg, sizeof topic_cfg, current_config->id_device, MQTT_TOPIC_CONFIG);
    ESP_LOGI(TAG_SEND_MQTT, "topic_cfg: %s", topic_cfg);
    mqtt_subscribe(topic_cfg, 0);

    return true;
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

/* UDP no conecta: abrir el socket es todo lo que hace falta. */
static bool udp_open(void) {
    udp_params_t params = {
        .ip_host = current_config->host_ip_addr,
        .port = current_config->udp_port,
        .ip_version = IPV4,
    };
    nebulaedge_udp_open_socket(&params);
    return true;
}

/* -------------------------------------------------------------------- TCP */

static esp_err_t tcp_send_data(const uint8_t *data, size_t size) {
    return nebulaedge_tcp_send(data, size);
}

static esp_err_t tcp_send_ack(const Config *cfg, const uint8_t *buf, size_t size) {
    (void)cfg;
    return nebulaedge_tcp_send(buf, size);
}

/* Espera entre reintentos de reconexión TCP. Tiene que quedar holgadamente
 * por debajo de los 5 s de escape de pause_for_protocol_change(): esta espera
 * corre en la task de respuesta, que es la que tiene que levantar su bit de
 * "detenida" cuando se pide una pausa. */
#define TCP_RECONNECT_DELAY_MS  1000

static Config *tcp_recv_config(void) {
    uint8_t buffer[CONFIG_RECV_BUF_BYTES];

    /* Enlace caído: hay que reconectar acá, porque nebulaedge_tcp_receive()
     * devuelve 0 igual que cuando no llegó nada y la task volvería a llamar de
     * inmediato. Antes de esto el device giraba a ~76 recv fallidos por segundo
     * y solo se recuperaba con un reset físico. */
    if (nebulaedge_tcp_link_is_down()) {
        vTaskDelay(pdMS_TO_TICKS(TCP_RECONNECT_DELAY_MS));
        if (nebulaedge_tcp_reconnect() != 0) {
            return NULL;
        }
        ESP_LOGI(TAG_GET_RSP_TCP, "TCP: enlace recuperado");
    }

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

/* El único que puede fallar al abrir: si el servidor no está escuchando, el
 * connect() no entra y hay que reintentar el protocolo entero. */
static bool tcp_open(void) {
    tcp_params_t params = {
        .ip_host = current_config->host_ip_addr,
        .port = current_config->tcp_port,
        .ip_version = IPV4,
    };

    nebulaedge_tcp_open_socket(&params);

    if (nebulaedge_tcp_connect() != 0) {
        nebulaedge_tcp_close_socket();
        return false;
    }
    return true;
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

/* En BLE la config llega cruda por la cola y hay que desempaquetarla acá.
 *
 * `wait_ms` en 0 la convierte en un sondeo que no bloquea, que es como la usa
 * el canal de rescate de vTaskGetResponse() cuando el protocolo activo es otro:
 * ahí la espera ya la hizo el recv_config de ese protocolo y sumarle otra
 * alargaría cada vuelta del bucle. */
Config *ble_config_from_queue(uint32_t wait_ms) {
    packet_t pkt;

    if (xQueueReceive(xQueueConfigBle, &pkt, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
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

/* La que va en la tabla de protocolos, con la espera normal del bucle. */
static Config *ble_recv_config(void) {
    return ble_config_from_queue(TASK_POLL_MS);
}

/* El enlace BLE no se cierra al cambiar de protocolo: queda activo siempre. */
static void ble_close(void) { }

/* BLE no abre nada: el stack y el anuncio ya están arriba desde app_main y no
 * se deshacen nunca. Lo que hace es ESPERAR a que el servidor escriba en la
 * característica C, que es lo que libera el semáforo. Es el único protocolo
 * cuyo open() se bloquea. */
static bool ble_open(void) {
    ESP_LOGI(TAG, "esperando conexión BLE para iniciar tasks...");

    if (xSemaphoreTake(ble_start_semaphore, portMAX_DELAY)) {
        ESP_LOGI(TAG, "toma semáforo, empieza recepción BLE");
    }

    // Le da tiempo a la Raspberry para estar lista.
    vTaskDelay(pdMS_TO_TICKS(SERVER_READY_MS));
    return true;
}

/* ------------------------------------------------------------------ INFER */

/* MQTT de siempre (config, ACK, canal de rescate) más los dos tópicos del
 * modelo y la task que los atiende. */
static bool infer_open(void) {
    if (!mqtt_open()) {
        return false;
    }

    char topic[128];
    /* QoS 1: un trozo perdido se vuelve a pedir igual, pero así casi nunca
     * hace falta. */
    mqtt_topic(topic, sizeof topic, current_config->id_device, MQTT_TOPIC_MODEL_MANIFEST);
    mqtt_subscribe(topic, 1);
    mqtt_topic(topic, sizeof topic, current_config->id_device, MQTT_TOPIC_MODEL_CHUNK);
    mqtt_subscribe(topic, 1);
    mqtt_topic(topic, sizeof topic, current_config->id_device, MQTT_TOPIC_INFER_REQ);
    mqtt_subscribe(topic, 1);

    infer_start();
    return true;
}

static void infer_close(void) {
    infer_stop();       // antes que el cliente: la task publica por él
    mqtt_finish();
}

/* En INFER las productoras de telemetría no producen. Siguen existiendo
 * (son de todos los protocolos) y respetan la compuerta, pero no leen
 * sensores ni encolan. */
bool telemetry_enabled(void) {
    return current_config && current_config->protocol_conf != PROTOCOL_INFER;
}

/* ----------------------------------------------------------- LA TABLA */

const protocol_ops_t PROTOCOLS[] = {
    [PROTOCOL_MQTT] = {
        .name = "MQTT", .id = PROTOCOL_MQTT,
        .send_data = mqtt_send_data, .send_ack = mqtt_send_ack,
        .send_tag = TAG_SEND_MQTT, .rsp_tag = TAG_GET_RSP_MQTT,
        .open = mqtt_open, .needs_wifi = true,
        .recv_config = mqtt_recv_config, .close = mqtt_finish,
        .control_repeats = CONTROL_PKT_REDUNDANCY, .ack_drain_ms = ACK_DRAIN_MS,
    },
    [PROTOCOL_UDP] = {
        .name = "UDP", .id = PROTOCOL_UDP,
        .send_data = udp_send_data, .send_ack = udp_send_ack,
        .send_tag = TAG_SEND_UDP, .rsp_tag = TAG_GET_RSP_UDP,
        .open = udp_open, .needs_wifi = true,
        .recv_config = udp_recv_config, .close = nebulaedge_udp_close_socket,
        .control_repeats = CONTROL_PKT_REDUNDANCY, .ack_drain_ms = ACK_DRAIN_MS,
    },
    [PROTOCOL_TCP] = {
        .name = "TCP", .id = PROTOCOL_TCP,
        .send_data = tcp_send_data, .send_ack = tcp_send_ack,
        .send_tag = TAG_SEND_TCP, .rsp_tag = TAG_GET_RSP_TCP,
        .open = tcp_open, .needs_wifi = true,
        .recv_config = tcp_recv_config, .close = nebulaedge_tcp_close_socket,
        .control_repeats = CONTROL_PKT_REDUNDANCY, .ack_drain_ms = ACK_DRAIN_MS,
    },
    [PROTOCOL_BLE] = {
        .name = "BLE", .id = PROTOCOL_BLE,
        .send_data = ble_send_data, .send_ack = ble_send_ack,
        .send_tag = TAG_SEND_BLE, .rsp_tag = TAG_GET_RSP_BLE,
        .open = ble_open, .needs_wifi = false,
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
    [PROTOCOL_INFER] = {
        .name = "INFER", .id = PROTOCOL_INFER,
        /* send_data no se usa: sin telemetría la cola de datos queda vacía.
         * Está para que la tabla no tenga huecos. */
        .send_data = mqtt_send_data, .send_ack = mqtt_send_ack,
        .send_tag = TAG_SEND_INFER, .rsp_tag = TAG_GET_RSP_INFER,
        .open = infer_open, .needs_wifi = true,
        .recv_config = mqtt_recv_config, .close = infer_close,
        .control_repeats = CONTROL_PKT_REDUNDANCY, .ack_drain_ms = ACK_DRAIN_MS,
    },
};
_Static_assert(sizeof PROTOCOLS / sizeof PROTOCOLS[0] == PROTOCOL_COUNT,
               "falta un protocolo en la tabla");
