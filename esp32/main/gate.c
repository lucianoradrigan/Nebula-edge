/* La compuerta cooperativa del camino de datos. Ver gate.h. Salió de main.c
 * tal cual. */
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "nebulaedge_defs.h"
#include "app.h"
#include "gate.h"

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

/* La compuerta. La crea app_main; ver gate.h. */
EventGroupHandle_t sensor_gate = NULL;

/* Punto seguro. Va al tope del bucle de cada productora: si hay una pausa
 * pedida, la task levanta su bit de "detenida" y se bloquea acá hasta que
 * vuelva el permiso. */
void sensor_gate_wait(EventBits_t idle_bit) {
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
 * esperando el intervalo entero: con send_interval_s en 10 s o más, pausar
 * tardaría eso en completarse. */
void sensor_gate_sleep(uint32_t ms) {
    /* El +1 redondea hacia arriba, y con ms = 0 (modo sin espera) deja
     * exactamente un tick: lo mínimo para ceder la CPU. Ver
     * packet_interval_s() para por qué no puede ser cero de verdad. */
    TickType_t ticks = pdMS_TO_TICKS(ms) + 1;

    if (sensor_gate == NULL) {
        vTaskDelay(ticks);
        return;
    }

    xEventGroupWaitBits(sensor_gate, GATE_PAUSE_REQ, pdFALSE, pdTRUE, ticks);
}

/* Pide a las tasks del camino de datos que se detengan y espera a que lleguen
 * al punto seguro. Reemplaza al antiguo vTaskSuspend() sobre ellas.
 *
 * `expected` dice a CUÁLES esperar, porque el alcance no es el mismo siempre:
 * al cambiar de protocolo hay que detener también la task de envío, pero al
 * entrar en deep sleep esa misma task es la que está llamando acá y esperarla
 * sería esperarse a sí misma. Los envoltorios de abajo arman cada caso.
 *
 * El drenado de la cola se hace adentro, y dos veces. Antes estaba afuera y
 * solo una vez, ANTES de detener las tasks: cualquier paquete producido entre
 * el drenado y la detención se colaba al protocolo siguiente. El segundo
 * drenado, ya con todas detenidas, cierra esa ventana.
 *
 * El primero tampoco sobra: si la cola está llena y la consumidora ya se
 * detuvo, la productora está bloqueada en xQueueSend(portMAX_DELAY) y nunca
 * llegaría a la compuerta. Vaciar la cola es justo lo que la desbloquea. */
static void pause_data_tasks(EventBits_t expected) {
    if (sensor_gate == NULL) {
        return;
    }

    xEventGroupClearBits(sensor_gate, GATE_RUN);
    xEventGroupSetBits(sensor_gate, GATE_PAUSE_REQ);

    drain_and_free_data_queue();

    // Todavía no existe ninguna: no hay nada a qué esperar.
    if (expected == 0) {
        return;
    }

    EventBits_t got = xEventGroupWaitBits(sensor_gate, expected, pdFALSE, pdTRUE,
                                          pdMS_TO_TICKS(GATE_PAUSE_TIMEOUT_MS));
    if ((got & expected) == expected) {
        ESP_LOGI(TAG, "Tasks detenidas en punto seguro (bits=0x%02X)", (unsigned)expected);
    }
    else {
        ESP_LOGW(TAG, "Timeout esperando que las tasks se detengan (esperado=0x%02X, got=0x%02X)",
                 (unsigned)expected, (unsigned)got);
    }

    // Segundo drenado: lo que alcanzaron a encolar antes de detenerse.
    drain_and_free_data_queue();
}

/* Bits de las dos productoras, que hay que detener en todos los casos. */
static EventBits_t collect_idle_bits(void) {
    EventBits_t bits = 0;
    if (xHandleCollectInertial)      bits |= GATE_INERTIAL_IDLE;
    if (xHandleCollectEnvironmental) bits |= GATE_ENV_IDLE;
    return bits;
}

/* Cambio de protocolo: lo llama la task de respuesta, así que detiene las
 * productoras y la task de envío (a sí misma no: se auto-suspende después, que
 * es seguro). */
void pause_for_protocol_change(protocol_t id) {
    EventBits_t expected = collect_idle_bits();
    if (send_task[id]) expected |= GATE_SEND_IDLE(id);
    pause_data_tasks(expected);
}

/* Deep sleep: lo llama la task de ENVÍO, así que detiene las productoras y la
 * task de respuesta. A la de envío no se la espera — es la que está acá. */
void pause_for_deep_sleep(protocol_t id) {
    EventBits_t expected = collect_idle_bits();
    if (response_task[id]) expected |= GATE_RSP_IDLE(id);
    pause_data_tasks(expected);
}

/* Devuelve el permiso de producir. Limpia también los bits de "detenida" para
 * que la próxima pausa no los lea de la vuelta anterior. */
void resume_collect_tasks(void) {
    if (sensor_gate == NULL) {
        return;
    }

    xEventGroupClearBits(sensor_gate, GATE_PAUSE_REQ | GATE_INERTIAL_IDLE |
                                  GATE_ENV_IDLE | GATE_ALL_SEND_IDLE | GATE_ALL_RSP_IDLE);
    xEventGroupSetBits(sensor_gate, GATE_RUN);
}
