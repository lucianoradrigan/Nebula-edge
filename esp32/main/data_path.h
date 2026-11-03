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

/* Pone en cero los números de secuencia de Data_1 y Data_2. Se llama en
 * arranque frío: a través del deep sleep los contadores siguen, para que un
 * salto de seq siga significando "paquete perdido". */
void data_path_reset_seq(void);

#endif
