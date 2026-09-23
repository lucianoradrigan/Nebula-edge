#ifndef NEBULAEDGE_DEFS
#define NEBULAEDGE_DEFS

#include <stddef.h>
#include <stdint.h>

/* El vocabulario que los componentes se pasan entre sí.
 *
 * QUÉ VA ACÁ Y QUÉ NO
 *     Solo lo que necesita MÁS DE UN componente y no es propiedad de ninguno.
 *     Es el mismo rol que models.py en el servidor: todos lo importan, él no
 *     importa a nadie, y así no hay ciclos.
 *
 *     Lo que usa un solo componente es parte de SU API y va en SU header.
 *     Este archivo venía siendo un cajón de sastre: tenía además las structs
 *     de configuración de los cuatro transportes (se fueron a nebulaedge_wifi.h,
 *     nebulaedge_mqtt.h, nebulaedge_udp.h y nebulaedge_tcp.h), el semáforo de
 *     arranque de la aplicación (se fue a main.c) y el pinout de la placa (se
 *     fue a main/board_pinout.h).
 */

/* Un paquete ya serializado, listo para viajar. Lo producen las tasks de
 * sensores en main.c y lo consumen las colas de MQTT y BLE. */
typedef struct {
    size_t size;
    uint8_t *data;
} packet_t;

/* Compartido por nebulaedge_udp y nebulaedge_tcp: los dos abren socket y los
 * dos tienen que decidir entre IPv4 e IPv6. */
typedef enum {
    IPV4,
    IPV6
} ip_version_t;

/* Marca de deep sleep que el device manda al servidor antes de dormirse, para
 * que no dé la sesión por caída. Es parte del protocolo, no de un transporte:
 * viaja igual por los cuatro. */
#define DEEP_SLEEP_FLAG_LEN 3U
extern const uint8_t DEEP_SLEEP_FLAG[DEEP_SLEEP_FLAG_LEN];

#endif
