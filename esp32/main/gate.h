#ifndef NEBULAEDGE_GATE_H
#define NEBULAEDGE_GATE_H

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "app.h"

/* COMPUERTA DEL CAMINO DE DATOS
 *
 * Las cuatro tasks que tocan telemetría —las dos productoras, la de envío y la
 * de respuesta— se detenían con vTaskSuspend() desde otra task. Eso congela a
 * la víctima en la instrucción exacta en que iba: si justo estaba dentro de
 * malloc() o free(), queda con el lock del heap tomado y la siguiente reserva
 * de CUALQUIER task del sistema se bloquea para siempre. Las cuatro reservan o
 * liberan memoria en cada vuelta, así que la ventana existía de verdad — es el
 * cuelgue que se veía.
 *
 * El mutex I2C no tapaba ese hueco: solo cubría las transacciones del bus, no
 * el heap. Y para lo que decía proteger tampoco hacía falta, porque el driver
 * i2c_master de ESP-IDF ya serializa cada transacción con su propio lock
 * interno (bus_lock_mux).
 *
 * La solución es no interrumpirlas: se les PIDE que paren y ellas se detienen
 * solas en un punto donde no tienen nada tomado (el tope de su bucle). La
 * única suspensión que queda en el archivo es vTaskSuspend(NULL), en la task
 * de respuesta: suspenderse a sí misma sí es seguro, porque ocurre en un punto
 * que la propia task eligió. */
#define GATE_RUN            (1 << 0)  // permiso para correr
#define GATE_PAUSE_REQ      (1 << 1)  // hay una pausa pedida (despierta los sleeps)
#define GATE_INERTIAL_IDLE  (1 << 2)  // la task del flujo rápido (Data_2) ya está detenida
#define GATE_ENV_IDLE       (1 << 3)  // la task del flujo lento (Data_1) ya está detenida
/* Un bit por protocolo, no uno para los cuatro.
 *
 * Con un bit compartido la pausa no podía distinguir QUÉ task lo había puesto.
 * La del protocolo que se está abandonando se auto-suspende, y si alcanzaba a
 * poner el bit compartido después del resume, la pausa siguiente creía que la
 * task ACTIVA ya se había detenido y cerraba el transporte por debajo de ella.
 * Con un bit propio cada una, el estado de una task suspendida no le miente a
 * nadie. */
/* El ancho sale de PROTOCOL_COUNT y no de un 4 fijo: con el bloque de envío en
 * los bits 4..7, el bit de envío de un quinto protocolo caía encima del de
 * respuesta de MQTT. Un event group tiene 24 bits, así que entran hasta 10. */
#define GATE_PROTO_MASK     (((EventBits_t)1 << PROTOCOL_COUNT) - 1)
#define GATE_SEND_IDLE(id)  ((EventBits_t)1 << (4 + (id)))
#define GATE_RSP_IDLE(id)   ((EventBits_t)1 << (4 + PROTOCOL_COUNT + (id)))
#define GATE_ALL_SEND_IDLE  (GATE_PROTO_MASK << 4)
#define GATE_ALL_RSP_IDLE   (GATE_PROTO_MASK << (4 + PROTOCOL_COUNT))
_Static_assert(4 + 2 * PROTOCOL_COUNT <= 24, "los bits de la compuerta no entran en un event group");

/* Cuánto se espera a que lleguen a la compuerta. Una lectura de sensor a medio
 * hacer puede tardar lo suyo si el bus está lento; pasado esto se sigue igual,
 * porque ya nadie queda suspendido y lo peor que pasa es un paquete de más. */
#define GATE_PAUSE_TIMEOUT_MS 5000

/* Definida en gate.c; la crea app_main. La task de envío también levanta su
 * bit directamente (ver vTaskSendData). */
extern EventGroupHandle_t sensor_gate;

void sensor_gate_wait(EventBits_t idle_bit);
void sensor_gate_sleep(uint32_t ms);
void pause_for_protocol_change(protocol_t id);
void pause_for_deep_sleep(protocol_t id);
void resume_collect_tasks(void);

#endif
