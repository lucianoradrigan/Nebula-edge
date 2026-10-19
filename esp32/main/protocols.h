#ifndef NEBULAEDGE_PROTOCOLS_H
#define NEBULAEDGE_PROTOCOLS_H

/* TABLA DE PROTOCOLOS (protocol_ops_t)
 *
 * Los cuatro protocolos hacen lo mismo con piezas distintas: mandar
 * telemetría, mandar un ACK, esperar una config, cerrar el transporte. Eso
 * estaba escrito cuatro veces —una task de envío y una de respuesta por
 * protocolo— y las copias fueron divergiendo: órdenes distintos, delays
 * distintos, logs que estaban en una y no en otra.
 *
 * Acá cada protocolo describe SOLO sus piezas propias. Las tasks son una sola
 * función instanciada cuatro veces: xTaskCreate() recibe la tabla por
 * pvParameters, que hasta ahora estaba declarado y sin usar en las ocho.
 *
 * Es el equivalente en C de lo que el servidor ya tiene con la clase Transport:
 * agregar un protocolo es una entrada más en la tabla, no cuatro funciones
 * nuevas repartidas por el archivo. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "app.h"

typedef struct {
    const char *name;            // para los logs: "MQTT", "UDP", "TCP", "BLE"
    protocol_t  id;              // el mismo valor que protocol_conf del Config

    /* Manda un paquete de telemetría ya serializado. En MQTT va al tópico
     * /data; en BLE, a la característica B.
     *
     * Devuelve esp_err_t en los cuatro. Antes era void porque cada transporte
     * reportaba distinto -esp_err_t en BLE, msg_id en MQTT, nada en UDP y
     * TCP-, así que un fallo de envío se veía en MQTT y era invisible en los
     * otros tres. */
    esp_err_t (*send_data)(const uint8_t *data, size_t size);

    /* Manda UN ACK de config ya serializado. El caller repite la llamada
     * `control_repeats` veces. En MQTT va al tópico /config/ack; en BLE, a la
     * característica D, que es distinta de la de telemetría. */
    esp_err_t (*send_ack)(const Config *cfg, const uint8_t *buf, size_t size);

    /* Bloquea hasta que llegue una config nueva y la devuelve desempaquetada,
     * o NULL si lo que llegó no servía. El caller la libera.
     *
     * Esta es la firma que hace que todo lo demás encaje. Los cuatro reciben
     * en formas incompatibles: MQTT saca un Config* ya desempaquetado de una
     * cola, BLE saca un packet_t crudo de otra, UDP y TCP bloquean en recv()
     * sobre un buffer. Devolver Config* absorbe las tres diferencias, y todo
     * lo que viene después pasa a existir una sola vez. */
    /* Deja el transporte listo para que arranquen las tasks. Devuelve false si
     * no se pudo: el bucle de protocolos lo reintenta.
     *
     * POR QUÉ ESTÁ EN LA TABLA
     *     Faltaba, y era lo único que mantenía vivo el switch de cuatro ramas
     *     en app_main(): 153 líneas que repetían el mismo esqueleto -levantar
     *     WiFi, abrir, arrancar tasks, bloquearse en el semáforo, bajar WiFi-
     *     con tres de las cuatro copias idénticas salvo el medio. */
    bool (*open)(void);

    /* Si el protocolo necesita la radio WiFi arriba. Tres de los cuatro sí;
     * BLE no, y por eso tampoco se le baja al salir. */
    bool needs_wifi;

    Config *(*recv_config)(void);

    /* Cierra el transporte antes de cambiar de protocolo o de dormir.
     * En BLE no hace nada: el enlace no se cierra desde acá. */
    void (*close)(void);

    const char *send_tag;        // tag de log de la task de envío
    const char *rsp_tag;         // tag de log de la task de respuesta

    /* Veces que se repite un paquete de control. 3 en los de socket, donde
     * nadie confirma; 1 en BLE, donde el link layer ya retransmite. */
    uint8_t control_repeats;

    /* Espera entre el ACK y el cierre del transporte, para que el ACK alcance
     * a salir. 0 en BLE: no cierra nada, así que no hay nada que drenar. */
    uint32_t ack_drain_ms;

    /* Delays propios de BLE, acá como datos en vez de escondidos en el cuerpo
     * de su task. 0 en los otros tres. */
    uint32_t pre_send_delay_ms;      // aire al enlace antes de mandar, en modo discontinuo
    uint32_t last_packet_delay_ms;   // respiro antes del último paquete de la ventana
} protocol_ops_t;

/* Una entrada por protocol_t. Ver protocols.c. */
extern const protocol_ops_t PROTOCOLS[PROTOCOL_COUNT];

/* Si las productoras de telemetría deben leer sensores: en INFER no. */
bool telemetry_enabled(void);

/* La próxima config que haya llegado por BLE, ya desempaquetada, o NULL.
 * `wait_ms` en 0 es un sondeo sin espera (canal de rescate). */
Config *ble_config_from_queue(uint32_t wait_ms);

#endif
