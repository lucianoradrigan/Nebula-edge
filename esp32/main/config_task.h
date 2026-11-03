#ifndef NEBULAEDGE_CONFIG_TASK_H
#define NEBULAEDGE_CONFIG_TASK_H

#include <stdbool.h>
#include "schema.pb-c.h"

/* Si la tabla de protocolos tiene una entrada para el protocol_conf de `c`.
 * La usan la task de respuesta y app_main, al recibir la config inicial. */
bool protocol_conf_is_valid(const Config *c);

/* Pone el reloj con la hora que trae la config: time_server_us si viene (µs),
 * si no time_client (s, un servidor anterior). Una config sin hora —una
 * retenida de MQTT, ver mqtt_recv_config()— no toca el reloj. La usan la task
 * de respuesta y app_main. */
void config_apply_time(const Config *c);

/* Task de respuesta de un protocolo: espera configs nuevas y las aplica. La
 * crea start_protocol_tasks(), en main.c, con su protocol_ops_t en
 * pvParameters. */
void vTaskGetResponse(void *pvParameters);

#endif
