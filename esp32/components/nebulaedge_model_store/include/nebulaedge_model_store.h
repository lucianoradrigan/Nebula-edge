#ifndef NEBULAEDGE_MODEL_STORE
#define NEBULAEDGE_MODEL_STORE

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

/* Dónde vive el modelo de INFER, y cómo se reemplaza sin quedarse sin él.
 *
 * DOS RANURAS
 *     Las particiones model_a y model_b (partitions.csv). Una tiene el modelo
 *     ACTIVO; el nuevo se escribe en la otra. Solo cuando la otra está completa
 *     y su sha256 calza se cambia cuál es la activa. Un corte de luz en
 *     cualquier momento deja al device con el modelo anterior, nunca con uno
 *     a medio escribir. Es el mismo esquema que ota_0/ota_1 para el firmware.
 *
 * SE RETOMA DESPUÉS DE UN REINICIO
 *     El progreso (qué versión se está bajando y cuál es el siguiente trozo)
 *     se guarda en NVS después de cada trozo. Un reinicio en el trozo 300 de
 *     500 sigue en el 301.
 *
 * SOLO EN ORDEN
 *     La flash NOR no se puede reescribir sin borrar: escribir dos veces el
 *     mismo trozo hace un AND de los bits y lo corrompe. Por eso se acepta
 *     únicamente el trozo que sigue; uno repetido (QoS 1 puede duplicar) se
 *     ignora en vez de escribirse de nuevo. */

typedef enum {
    MODEL_BEGIN_ACTIVE,     // esa versión ya es la activa: no hay nada que bajar
    MODEL_BEGIN_RESUME,     // ya se venía bajando: seguir desde *next_chunk
    MODEL_BEGIN_FRESH,      // se preparó la ranura libre: empezar desde 0
    MODEL_BEGIN_TOO_BIG,    // no cabe en una ranura
    MODEL_BEGIN_FLASH_ERR,  // no se pudo borrar la ranura
} model_begin_t;

/* Busca las particiones y lee el estado de NVS. Llamar una vez, después de
 * nvs_flash_init(). */
esp_err_t model_store_init(void);

/* Prepara la recepción de un modelo. `next_chunk` sale con el primer trozo a
 * pedir (0 en FRESH). */
model_begin_t model_store_begin(int32_t version, uint32_t size, uint32_t chunk_size,
                                const uint8_t sha256[32], uint32_t *next_chunk);

/* Escribe un trozo. Solo acepta el que sigue (ver arriba): devuelve
 * ESP_ERR_INVALID_STATE si no es ese, sin tocar nada. */
esp_err_t model_store_write(uint32_t chunk_index, uint32_t offset, const uint8_t *data, size_t len);

/* Cuántos trozos ya están escritos de la transferencia en curso. */
uint32_t model_store_next_chunk(void);

/* True cuando ya se escribieron todos los bytes de la transferencia en curso. */
bool model_store_complete(void);

/* Verifica el sha256 de lo escrito. NO lo activa: entre esto y
 * model_store_activate() la aplicación prueba el modelo (TFLM y autotest).
 * ESP_ERR_INVALID_CRC si no calza: la transferencia se descarta y la próxima
 * empieza de cero. */
esp_err_t model_store_verify(void);

/* Deja como activo el modelo recién verificado, junto con su `meta`: el
 * manifiesto empaquetado, que trae lo necesario para usarlo (estandarización,
 * orden de salidas). Se guarda con el modelo para poder inferir después de un
 * reinicio sin depender de que el servidor lo vuelva a mandar. */
esp_err_t model_store_activate(const uint8_t *meta, size_t meta_len);

/* Abandona la transferencia en curso, si la hay. El activo no se toca. */
void model_store_abort(void);

/* Versión activa, 0 si no hay ninguna. */
int32_t model_store_active_version(void);

/* Mapea un modelo de flash a memoria, para que TFLM lo lea sin copiarlo a
 * RAM. `pending` = el recién verificado (todavía no activo); si no, el activo.
 * NULL si no hay. El puntero vale hasta que se vuelva a escribir esa ranura. */
const void *model_store_map(bool pending, size_t *size);

/* Copia el `meta` del modelo activo en `buf`. ESP_ERR_NOT_FOUND si no hay. */
esp_err_t model_store_active_meta(uint8_t *buf, size_t *len);

/* Guarda el `meta` del modelo activo. Para el que se activó sin él: el firmware
 * de la fase A activaba solo con el sha256. El caller garantiza que `meta` es
 * el manifiesto de ESE modelo (misma versión y sha). */
esp_err_t model_store_set_active_meta(const uint8_t *meta, size_t meta_len);

#endif
