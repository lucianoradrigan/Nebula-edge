#ifndef NEBULAEDGE_MQTT
#define NEBULAEDGE_MQTT

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "nebulaedge_defs.h"

/* Cola donde dejar las configuraciones que lleguen por el tópico /config.
 *
 * La entrega la aplicación, que es la dueña de la cola; antes este componente
 * alcanzaba una global de main.c con `extern QueueHandle_t xQueueConfig`, así
 * que copiarlo a otro proyecto obligaba a ese proyecto a declarar una global
 * con ese nombre exacto.
 *
 * Si no se llama, las configuraciones entrantes se descartan con un aviso en
 * el log: el componente sigue publicando y suscribiendo igual. */
void mqtt_set_config_queue(QueueHandle_t queue);

/* Broker al que conectarse. Vivía en nebulaedge_defs.h; es parte de la API de
 * este componente. Se puede extender muchísimo: mirar los campos de
 * esp_mqtt_client_config_t. Por ahora, por simpleza, solo el broker. */
typedef struct {
    const char *broker;
} mqtt_config_global;

void mqtt_start(const mqtt_config_global *mqtt_config_global);
int mqtt_publish(const char *topic, const uint8_t *data, size_t len, int qos);
int mqtt_subscribe(const char *topic, int qos);
void mqtt_finish(void);

#endif