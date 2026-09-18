#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
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

/* Los paquetes de control (ACK de config, aviso de deep sleep) no esperan respuesta:
 * se mandan varias veces seguidas para bajar la probabilidad de que se pierdan, en
 * vez de implementar un segundo hop de confirmación (que agregaría otro flanco de
 * pérdida en vez de reducir el riesgo). */
#define CONTROL_PKT_REDUNDANCY 3
#define CONTROL_PKT_REDUNDANCY_DELAY_MS 50

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

/* COMPUERTA DE LAS TASKS DE SENSORES
 *
 * Antes las dos productoras se detenían con vTaskSuspend() desde afuera. Eso
 * congela a la víctima en la instrucción exacta en que iba: si justo estaba
 * dentro de malloc() o free(), se queda con el lock del heap tomado y la
 * siguiente reserva de CUALQUIER task del sistema se bloquea para siempre.
 * Las dos tasks hacen malloc() en cada paquete, así que la ventana existía de
 * verdad — es el cuelgue que se veía.
 *
 * El mutex I2C no tapaba ese hueco: solo cubría las transacciones del bus, no
 * el heap. Y para lo que decía proteger tampoco hacía falta, porque el driver
 * i2c_master de ESP-IDF ya serializa cada transacción con su propio lock
 * interno (bus_lock_mux).
 *
 * La solución es no interrumpirlas: se les PIDE que paren y ellas se detienen
 * solas en un punto donde no tienen nada tomado (el tope de su bucle). */
#define GATE_RUN            (1 << 0)  // permiso para producir
#define GATE_PAUSE_REQ      (1 << 1)  // hay una pausa pedida (despierta los sleeps)
#define GATE_INERTIAL_IDLE  (1 << 2)  // la task Inertial ya está detenida
#define GATE_ENV_IDLE       (1 << 3)  // la task Environmental ya está detenida

/* Cuánto se espera a que lleguen a la compuerta. Una lectura de sensor a medio
 * hacer puede tardar lo suyo si el bus está lento; pasado esto se sigue igual,
 * porque ya nadie queda suspendido y lo peor que pasa es un paquete de más. */
#define GATE_PAUSE_TIMEOUT_MS 5000

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

/* Pide a las dos productoras que se detengan y espera a que lleguen al punto
 * seguro. Reemplaza al antiguo vTaskSuspend() sobre las productoras.
 *
 * El drenado de la cola se hace acá adentro, y dos veces. Antes estaba afuera
 * y solo una vez, ANTES de detener las tasks: cualquier paquete producido
 * entre el drenado y la detención se colaba al protocolo siguiente. Ahora el
 * segundo drenado, ya con las dos detenidas, cierra esa ventana.
 *
 * El primero tampoco sobra: si la cola está llena y la task consumidora ya fue
 * suspendida, la productora está bloqueada en xQueueSend(portMAX_DELAY) y
 * nunca llegaría a la compuerta. Vaciar la cola es justo lo que la desbloquea. */
static void pause_collect_tasks(void) {
    if (sensor_gate == NULL) {
        return;
    }

    xEventGroupClearBits(sensor_gate, GATE_RUN);
    xEventGroupSetBits(sensor_gate, GATE_PAUSE_REQ);

    drain_and_free_data_queue();

    EventBits_t expected = 0;
    if (xHandleCollectInertial)      expected |= GATE_INERTIAL_IDLE;
    if (xHandleCollectEnvironmental) expected |= GATE_ENV_IDLE;

    // Todavía no existen: no hay nada a qué esperar.
    if (expected == 0) {
        return;
    }

    EventBits_t got = xEventGroupWaitBits(sensor_gate, expected, pdFALSE, pdTRUE,
                                          pdMS_TO_TICKS(GATE_PAUSE_TIMEOUT_MS));
    if ((got & expected) == expected) {
        ESP_LOGI(TAG, "Tasks de sensores detenidas en punto seguro");
    }
    else {
        ESP_LOGW(TAG, "Timeout esperando que las tasks de sensores se detengan (bits=0x%02X)",
                 (unsigned)got);
    }

    // Segundo drenado: lo que alcanzaron a encolar antes de detenerse.
    drain_and_free_data_queue();
}

/* Devuelve el permiso de producir. Limpia también los bits de "detenida" para
 * que la próxima pausa no los lea de la vuelta anterior. */
static void resume_collect_tasks(void) {
    if (sensor_gate == NULL) {
        return;
    }

    xEventGroupClearBits(sensor_gate, GATE_PAUSE_REQ | GATE_INERTIAL_IDLE | GATE_ENV_IDLE);
    xEventGroupSetBits(sensor_gate, GATE_RUN);
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
    
    // Detiene recogida de datos antes de dormir
    ESP_LOGI(TAG, "Deteniendo tasks de sensores");
    pause_collect_tasks();
    
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
        // config_store_clear();
    }
    
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

            /* Detiene las productoras y vacía la cola: los paquetes del protocolo
             * anterior no deben colarse al siguiente. */
            ESP_LOGI(TAG_GET_RSP_MQTT, "Se detienen tasks de sensores");
            pause_collect_tasks();

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
            /* Detiene las productoras y vacía la cola: los paquetes del protocolo
             * anterior no deben colarse al siguiente. */
            ESP_LOGI(TAG_GET_RSP_BLE, "Se detienen tasks de sensores");
            pause_collect_tasks();

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

            /* Detiene las productoras y vacía la cola: los paquetes del protocolo
             * anterior no deben colarse al siguiente. */
            ESP_LOGI(TAG_GET_RSP_UDP, "Se detienen tasks de sensores");
            pause_collect_tasks();

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
            /* Detiene las productoras y vacía la cola: los paquetes del protocolo
             * anterior no deben colarse al siguiente. */
            ESP_LOGI(TAG_GET_RSP_TCP, "Se detienen tasks de sensores");
            pause_collect_tasks();

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
                if (nebulaedge_tcp_connect() != 0) {
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