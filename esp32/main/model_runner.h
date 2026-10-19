#ifndef NEBULAEDGE_MODEL_RUNNER
#define NEBULAEDGE_MODEL_RUNNER

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "schema.pb-c.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Corre un modelo de INFER con TensorFlow Lite Micro.
 *
 * Es C++ por dentro (TFLM lo es) y C por fuera, para que infer.c no tenga que
 * saber nada de TFLM. Hay un solo modelo cargado a la vez.
 *
 * El modelo no se copia: TFLM lee los pesos directamente desde la flash
 * mapeada (model_store_map). En RAM solo vive la arena, el buffer donde TFLM
 * guarda las activaciones intermedias y su propia contabilidad. */

/* Resultado de cargar, en los términos de ModelError del contrato. */
typedef enum {
    RUNNER_OK = 0,
    RUNNER_ERR_LOAD,            // flatbuffer ilegible o con una operación no registrada
    RUNNER_ERR_ARENA,           // AllocateTensors no cupo en la arena
    RUNNER_ERR_INCOMPATIBLE,    // las formas de entrada o salida no calzan con el manifiesto
} runner_err_t;

/* Carga `model` con los parámetros de `meta` (estandarización, orden de
 * salidas, n_features, n_classes). Descarga lo que hubiera antes. Copia lo que
 * necesita de `meta`: se puede liberar al volver. */
runner_err_t model_runner_load(const void *model, const ModelManifest *meta);

void model_runner_unload(void);

bool model_runner_loaded(void);
int32_t model_runner_version(void);
uint32_t model_runner_n_classes(void);    // del modelo cargado; 0 si no hay

/* Bytes de arena que de verdad usó AllocateTensors. */
size_t model_runner_arena_used(void);

/* Infiere sobre features SIN estandarizar (el runner aplica mean/std).
 * `probs` debe tener lugar para n_classes floats. Devuelve false si no hay
 * modelo cargado o TFLM falló. `invoke_us` puede ser NULL. */
bool model_runner_invoke(const float *raw_features, float *severity, float *probs, int64_t *invoke_us);

/* Corre el autotest del manifiesto: test_input -> compara con test_output.
 * Devuelve el mayor error absoluto entre las salidas, o -1 si no pudo correr. */
float model_runner_selftest(const ModelManifest *meta, int64_t *invoke_us);

#ifdef __cplusplus
}
#endif

#endif
