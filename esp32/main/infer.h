#ifndef NEBULAEDGE_INFER
#define NEBULAEDGE_INFER

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* Modo INFER (protocol_conf = 4): el device baja un modelo por MQTT, lo verifica
 * en TFLM y responde pedidos de inferencia.
 *
 *   fase A  transferencia: la conduce el device, que con el manifiesto en la
 *           mano pide los trozos de a uno (nebulaedge_model_store los guarda)
 *   fase B  sha256, carga en TFLM y autotest antes de activar (model_runner)
 *   fase C  /infer/req: features ya calculadas (C1) o la ventana cruda, de la
 *           que el device calcula las features (C2, nebulaedge_features)
 *
 * La clasificación (classify() en infer.c) no depende de quién la pide. La
 * fuente de sensores (infer_source = 2, todavía sin implementar) se suma así:
 * algo llena una ventana de FEATURES_WINDOW_SIZE muestras a fs conocida, la
 * pasa a esta task, y esta la clasifica y publica el resultado con
 * request_id = 0 e input_kind = INFER_INPUT_SENSOR. El servidor ya guarda esos
 * resultados.
 *
 * Ver schema.proto para el contrato. */

/* `model_queue` es la que se le pasó a mqtt_set_model_queue(). Una vez, en el
 * arranque. */
void infer_init(QueueHandle_t model_queue);

/* Arranca o reanuda la task. Con el cliente MQTT ya conectado y suscrito. */
void infer_start(void);

/* La detiene en un punto seguro y espera a que lo esté. Antes de mqtt_finish(). */
void infer_stop(void);

/* Deja de procesar y de publicar, sin detenerse. Lo llama la task de respuesta
 * apenas llega una config nueva, antes de reemplazar current_config: desde ahí
 * hasta infer_stop() el transporte está por cerrarse. No hace nada si INFER no
 * está activo. Lo deshace el próximo infer_start(). */
void infer_freeze(void);

#endif
