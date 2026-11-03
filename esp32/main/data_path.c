/* Camino de datos: las dos productoras de telemetría y la task de envío.
 * Salió de main.c tal cual. */
#include <stdint.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nebulaedge_defs.h"
#include "nebulaedge_device.h"
#include "nebulaedge_sdstorage.h"
#include "bmm350.h"
#include "bme688.h"
#include "bmi270.h"
#include "app.h"
#include "protocols.h"
#include "gate.h"
#include "power.h"
#include "data_path.h"

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

/* NÚMERO DE SECUENCIA
 *
 * Uno por flujo, asignado al producir el paquete (Data_1.seq / Data_2.seq). Un
 * salto en el servidor es un paquete perdido en cualquier punto del camino,
 * también los que se descartan acá adentro: la cola llena, o la cola vaciada
 * en un cambio de protocolo (gate.c). Arranca en 1: el 0 es "sin seq".
 *
 * En memoria RTC para que siga a través del deep sleep; en arranque frío lo
 * pone en cero data_path_reset_seq(). */
RTC_DATA_ATTR static uint32_t s_seq_environmental = 0;
RTC_DATA_ATTR static uint32_t s_seq_inertial = 0;

void data_path_reset_seq(void) {
    s_seq_environmental = 0;
    s_seq_inertial = 0;
}

/* HORA DE ENVÍO
 *
 * Data_1.time_sent_us / Data_2.time_sent_us se escriben justo antes del send
 * del transporte, sobre el paquete ya serializado. Funciona porque el campo es
 * fixed64 y el de número más alto del mensaje: protobuf-c lo deja siempre en
 * los últimos 9 bytes, un byte de tag y 8 de valor little-endian
 * (test_contract.py lo verifica del lado del servidor).
 *
 * La productora lo empaqueta con un valor de relleno no nulo: proto3 omite los
 * campos en 0, y entonces esos 9 bytes no existirían. */
#define TIME_SENT_PLACEHOLDER   1ULL
#define TIME_SENT_TAG(field)    (((field) << 3) | 1)    // wire type 1 = 64 bits
#define TIME_SENT_DATA_1_TAG    TIME_SENT_TAG(9)        // 0x49
#define TIME_SENT_DATA_2_TAG    TIME_SENT_TAG(14)       // 0x71
#define TIME_SENT_TRAILER_LEN   9

/* Escribe la hora actual en el time_sent_us de un paquete [tipo][protobuf].
 * Antes de escribir verifica el tag: si no calza (otro tipo de paquete, o el
 * contrato cambió sin actualizar esto) no toca nada y lo loguea. Un reloj que
 * todavía no está en hora deja 0, que el servidor guarda como "sin dato". */
static void stamp_time_sent(uint8_t *data, size_t size, const char *tag) {
    if (size < 1 + TIME_SENT_TRAILER_LEN) {
        return;
    }
    uint8_t expected;
    if (data[0] == PACKET_TYPE__PACKET_TYPE_DATA_1) {
        expected = TIME_SENT_DATA_1_TAG;
    }
    else if (data[0] == PACKET_TYPE__PACKET_TYPE_DATA_2) {
        expected = TIME_SENT_DATA_2_TAG;
    }
    else {
        return;
    }

    uint8_t *trailer = data + size - TIME_SENT_TRAILER_LEN;
    if (trailer[0] != expected) {
        ESP_LOGE(tag, "No se pudo marcar la hora de envío: tag 0x%02x, se esperaba 0x%02x",
                 trailer[0], expected);
        return;
    }

    uint64_t now_us = device_clock_now_us();
    for (int i = 0; i < 8; i++) {
        trailer[1 + i] = (uint8_t)(now_us >> (8 * i));
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
        data_2.seq = ++s_seq_inertial;
        data_2.time_sent_us = TIME_SENT_PLACEHOLDER;     // lo reemplaza vTaskSendData

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

/* Recordatorio mientras el BME688 sigue sin responder. Ver el aviso en
 * vTaskCollectEnvironmental. */
#define BME688_FAIL_REMINDER_S 300

// GEN_DATA: BME688 -> paquete Data_1, cada send_interval_s.
void vTaskCollectEnvironmental(void *pvParameters) {
    uint32_t bme688_failures = 0;       // lecturas fallidas seguidas
    int64_t bme688_last_warn_us = 0;

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
        data_1.seq = ++s_seq_environmental;
        data_1.time_sent_us = TIME_SENT_PLACEHOLDER;     // lo reemplaza vTaskSendData

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
            if (bme688_failures > 0) {
                ESP_LOGI(TAG_COLLECT_ENV, "El BME688 responde de nuevo (tras %lu lecturas fallidas)",
                         (unsigned long)bme688_failures);
                bme688_failures = 0;
            }
        }
        else {
            /* Se avisa al cambiar de estado y después cada
             * BME688_FAIL_REMINDER_S, no en cada lectura: en una placa sin el
             * sensor eran un aviso por segundo, el 16 % del log (medido en la
             * placa con solo el BMI270). */
            bme688_failures++;
            int64_t now_us = esp_timer_get_time();
            if (bme688_failures == 1) {
                ESP_LOGW(TAG_COLLECT_ENV, "Lectura del BME688 falló: los Data_1 salen con ceros hasta que responda");
                bme688_last_warn_us = now_us;
            }
            else if (now_us - bme688_last_warn_us >= (int64_t)BME688_FAIL_REMINDER_S * 1000000) {
                ESP_LOGW(TAG_COLLECT_ENV, "El BME688 sigue sin responder (%lu lecturas fallidas)",
                         (unsigned long)bme688_failures);
                bme688_last_warn_us = now_us;
            }
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

        // Lo último antes de mandar: después del delay de precaución de BLE.
        stamp_time_sent(packet.data, packet.size, proto->send_tag);
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
