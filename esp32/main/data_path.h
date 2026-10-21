#ifndef NEBULAEDGE_DATA_PATH_H
#define NEBULAEDGE_DATA_PATH_H

/* Las tasks del camino de datos. Las crea start_protocol_tasks(), en main.c.
 *
 * Las dos productoras leen los sensores a su ritmo y escriben en xQueueData; la
 * de envío saca de esa cola y manda por el protocolo que recibe en
 * pvParameters (un protocol_ops_t de PROTOCOLS[]). */
void vTaskCollectInertial(void *pvParameters);
void vTaskCollectEnvironmental(void *pvParameters);
void vTaskSendData(void *pvParameters);

#endif
