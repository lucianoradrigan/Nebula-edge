#ifndef NEBULAEDGE_MQTT
#define NEBULAEDGE_MQTT

#include "nebulaedge_defs.h"

void mqtt_start(const mqtt_config_global *mqtt_config_global);
int mqtt_publish(const char *topic, const uint8_t *data, size_t len, int qos);
int mqtt_subscribe(const char *topic, int qos);
void mqtt_finish(void);

#endif