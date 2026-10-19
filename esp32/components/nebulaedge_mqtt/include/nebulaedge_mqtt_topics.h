#ifndef NEBULAEDGE_MQTT_TOPICS
#define NEBULAEDGE_MQTT_TOPICS

#include <stdio.h>

/* TÓPICOS MQTT DE NEBULAEDGE
 *
 * Todos cuelgan de /topic/nebulaedge/{id}/. El servidor arma los mismos en
 * raspberry/server/mqtt_client.py: un .proto no puede declarar textos, así que
 * hay una copia por lado, y si cambia una tiene que cambiar la otra.
 *
 * Antes cada tópico estaba escrito entero con snprintf en el lugar donde se
 * usaba (main.c, infer.c, y el prefijo en el ruteo de este componente). */
#define MQTT_TOPIC_PREFIX           "/topic/nebulaedge/"

#define MQTT_TOPIC_DATA             "data"              // device -> servidor, telemetría
#define MQTT_TOPIC_CONFIG           "config"            // servidor -> device, retenida
#define MQTT_TOPIC_CONFIG_ACK       "config/ack"        // device -> servidor
#define MQTT_TOPIC_MODEL_MANIFEST   "model/manifest"    // servidor -> device, retenido (INFER)
#define MQTT_TOPIC_MODEL_CHUNK      "model/chunk"       // servidor -> device (INFER)
#define MQTT_TOPIC_MODEL_STATUS     "model/status"      // device -> servidor, tipado (INFER)
#define MQTT_TOPIC_MODEL_CHUNK_REQ  "model/chunk/req"   // device -> servidor, tipado (INFER)
#define MQTT_TOPIC_INFER_REQ        "infer/req"         // servidor -> device (INFER)
#define MQTT_TOPIC_INFER_RESULT     "infer/result"      // device -> servidor, tipado (INFER)

/* Escribe en `buf` el tópico `suffix` del device `device_id`. Devuelve lo mismo
 * que snprintf. */
static inline int mqtt_topic(char *buf, size_t size, const char *device_id, const char *suffix) {
    return snprintf(buf, size, MQTT_TOPIC_PREFIX "%s/%s", device_id, suffix);
}

#endif
