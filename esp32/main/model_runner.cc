#include <math.h>
#include <new>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "model_runner.h"

static const char *TAG = "model_runner";

/* Tamaño de la arena: lo que haya, entre estos dos topes. Lo que de verdad usa
 * cada modelo lo informa model_runner_arena_used() y viaja en ModelStatus.
 *
 * No es un número fijo porque no hay uno que sirva: MEDIDO en la IM-V2 en INFER,
 * con WiFi, BLE y MQTT arriba, el bloque contiguo más grande es de 31.744 B, y
 * una arena fija de 32 KB no entraba por 1 KB. Las capas densas de estos
 * modelos tienen como mucho 256 activaciones; la arena se la come sobre todo la
 * contabilidad de TFLM, no los datos. */
/* 16 KB y no 32: el modelo que más pidió en banco usó 4.144 B (v1 int8), y lo
 * que no se lleva la arena lo necesita la fase C2, que recibe ventanas crudas
 * de 8 KB. Con 32 la arena se comía 27 de los 31 KB contiguos. */
#define ARENA_MAX_BYTES  (16 * 1024)
#define ARENA_MIN_BYTES  (8 * 1024)
#define ARENA_HEAP_SLACK (4 * 1024)     // lo que se le deja al resto del sistema en ese bloque
/* Después de cargar, la arena se achica a lo que el modelo usó más este margen
 * (ver shrink_arena()). TFLM no pide más arena al invocar: lo que usa queda
 * fijado en AllocateTensors. */
#define ARENA_MARGIN     1024

/* Las operaciones que usan los modelos de ml/: FULLY_CONNECTED, LOGISTIC y
 * SOFTMAX, más QUANTIZE/DEQUANTIZE en los int8. Registrar solo estas mantiene
 * el binario chico; un modelo con otra operación no carga (RUNNER_ERR_LOAD). */
#define N_OPS 5

/* Lo que se copia del manifiesto al cargar: el manifiesto lo libera el caller. */
#define MAX_FEATURES 64
#define MAX_CLASSES  16

static struct {
    uint8_t *arena;
    size_t arena_size;
    tflite::MicroMutableOpResolver<N_OPS> *resolver;
    tflite::MicroInterpreter *interpreter;
    int32_t version;
    uint32_t n_features;
    uint32_t n_classes;
    uint32_t i_sev;
    uint32_t i_fault;
    float mean[MAX_FEATURES];
    float std[MAX_FEATURES];
} r;

void model_runner_unload(void) {
    if (r.interpreter) {
        r.interpreter->~MicroInterpreter();
        heap_caps_free(r.interpreter);
        r.interpreter = nullptr;
    }
    r.version = 0;
}

static void free_arena(void) {
    heap_caps_free(r.arena);
    r.arena = nullptr;
    r.arena_size = 0;
}

/* Reserva la arena con lo que haya, entre ARENA_MIN_BYTES y ARENA_MAX_BYTES. */
static runner_err_t alloc_arena(void) {
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    size_t largest = heap_caps_get_largest_free_block(caps);
    size_t want = largest > ARENA_HEAP_SLACK ? largest - ARENA_HEAP_SLACK : 0;
    if (want > ARENA_MAX_BYTES) want = ARENA_MAX_BYTES;
    want &= ~(size_t)15;
    if (want < ARENA_MIN_BYTES) {
        ESP_LOGE(TAG, "Bloque contiguo de %u B: no alcanza para una arena de %d B",
                 (unsigned)largest, ARENA_MIN_BYTES);
        return RUNNER_ERR_ARENA;
    }
    r.arena = (uint8_t *)heap_caps_aligned_alloc(16, want, caps);
    if (!r.arena) {
        ESP_LOGE(TAG, "No se pudieron reservar %u B para la arena", (unsigned)want);
        return RUNNER_ERR_ARENA;
    }
    r.arena_size = want;
    ESP_LOGI(TAG, "Arena de %u B (bloque contiguo libre: %u B)", (unsigned)want, (unsigned)largest);
    return RUNNER_OK;
}

static bool build_interpreter(const tflite::Model *model) {
    void *mem = heap_caps_malloc(sizeof(tflite::MicroInterpreter), MALLOC_CAP_8BIT);
    if (!mem) {
        return false;
    }
    r.interpreter = new (mem) tflite::MicroInterpreter(model, *r.resolver, r.arena, r.arena_size);
    return true;
}

/* ACHICAR LA ARENA A LO QUE EL MODELO USA
 *
 * La arena se reserva grande porque antes de AllocateTensors no se sabe cuánto
 * pide el modelo. Pero lo que pide es poco: 2.528 B el v3, de 16 KB
 * reservados. Y esos 16 KB salían del bloque contiguo más grande, que en la
 * IM-V2 en INFER es de ~31 KB: quedaban 13,8 KB, y un pedido C2 necesita dos
 * bloques de ~8 KB (el mensaje MQTT y la ventana desempaquetada). Medido en la
 * prueba nocturna: la IM-V2 #1 descartó TODOS los pedidos C2 de cuatro rondas
 * ("Pedido de inferencia ilegible", "Sin memoria para un mensaje de 8216 B").
 *
 * Se libera la arena grande, se reserva lo usado más ARENA_MARGIN y se vuelve a
 * armar el intérprete. Si algo falla, se vuelve a la arena grande. */
static runner_err_t shrink_arena(const tflite::Model *model) {
    size_t target = (r.interpreter->arena_used_bytes() + ARENA_MARGIN + 15) & ~(size_t)15;
    if (target + ARENA_MARGIN >= r.arena_size) {
        return RUNNER_OK;       // no vale la pena
    }
    size_t big = r.arena_size;
    model_runner_unload();
    free_arena();

    r.arena = (uint8_t *)heap_caps_aligned_alloc(16, target, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (r.arena) {
        r.arena_size = target;
        if (build_interpreter(model) && r.interpreter->AllocateTensors() == kTfLiteOk) {
            ESP_LOGI(TAG, "Arena achicada de %u a %u B", (unsigned)big, (unsigned)target);
            return RUNNER_OK;
        }
        model_runner_unload();
        free_arena();
    }
    ESP_LOGW(TAG, "No se pudo achicar la arena a %u B: se vuelve a la grande", (unsigned)target);
    runner_err_t err = alloc_arena();
    if (err != RUNNER_OK) {
        return err;
    }
    if (!build_interpreter(model) || r.interpreter->AllocateTensors() != kTfLiteOk) {
        model_runner_unload();
        return RUNNER_ERR_ARENA;
    }
    return RUNNER_OK;
}

bool model_runner_loaded(void) {
    return r.interpreter != nullptr;
}

int32_t model_runner_version(void) {
    return r.version;
}

uint32_t model_runner_n_classes(void) {
    return r.interpreter ? r.n_classes : 0;
}

size_t model_runner_arena_used(void) {
    return r.interpreter ? r.interpreter->arena_used_bytes() : 0;
}

static bool dims_are(const TfLiteTensor *t, int d0, int d1) {
    return t && t->type == kTfLiteFloat32 && t->dims->size == 2
        && t->dims->data[0] == d0 && t->dims->data[1] == d1;
}

runner_err_t model_runner_load(const void *model_data, const ModelManifest *meta) {
    model_runner_unload();

    if (meta->n_features == 0 || meta->n_features > MAX_FEATURES
            || meta->n_classes == 0 || meta->n_classes > MAX_CLASSES
            || meta->n_feat_mean != meta->n_features || meta->n_feat_std != meta->n_features) {
        ESP_LOGE(TAG, "Manifiesto fuera de rango (features %lu, clases %lu)",
                 (unsigned long)meta->n_features, (unsigned long)meta->n_classes);
        return RUNNER_ERR_INCOMPATIBLE;
    }

    /* Cada carga parte de una arena grande nueva: la anterior quedó achicada
     * a la medida del modelo anterior, y el nuevo puede pedir más. */
    free_arena();
    runner_err_t aerr = alloc_arena();
    if (aerr != RUNNER_OK) {
        return aerr;
    }
    if (!r.resolver) {
        void *mem = heap_caps_malloc(sizeof(tflite::MicroMutableOpResolver<N_OPS>), MALLOC_CAP_8BIT);
        if (!mem) {
            return RUNNER_ERR_ARENA;
        }
        r.resolver = new (mem) tflite::MicroMutableOpResolver<N_OPS>();
        r.resolver->AddFullyConnected();
        r.resolver->AddLogistic();
        r.resolver->AddSoftmax();
        r.resolver->AddQuantize();
        r.resolver->AddDequantize();
    }

    const tflite::Model *model = tflite::GetModel(model_data);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "Esquema TFLite %lu, este firmware entiende %d",
                 (unsigned long)model->version(), TFLITE_SCHEMA_VERSION);
        return RUNNER_ERR_LOAD;
    }

    if (!build_interpreter(model)) {
        return RUNNER_ERR_ARENA;
    }

    if (r.interpreter->AllocateTensors() != kTfLiteOk) {
        // También falla así con una operación no registrada: el log de TFLM
        // dice cuál de los dos fue.
        ESP_LOGE(TAG, "AllocateTensors falló (arena de %u B, u operación no registrada)", (unsigned)r.arena_size);
        model_runner_unload();
        return RUNNER_ERR_ARENA;
    }

    /* Que el modelo tenga la forma que dice el manifiesto: una entrada
     * [1, n_features] y dos salidas, [1, 1] la severidad y [1, n_classes] las
     * clases, en las posiciones que convert.py verificó. */
    const int nf = (int)meta->n_features, nc = (int)meta->n_classes;
    bool ok = r.interpreter->inputs_size() == 1
        && r.interpreter->outputs_size() == 2
        && meta->output_severity_index < 2 && meta->output_fault_index < 2
        && meta->output_severity_index != meta->output_fault_index
        && dims_are(r.interpreter->input(0), 1, nf)
        && dims_are(r.interpreter->output(meta->output_severity_index), 1, 1)
        && dims_are(r.interpreter->output(meta->output_fault_index), 1, nc);
    if (!ok) {
        ESP_LOGE(TAG, "Las entradas/salidas del modelo no calzan con el manifiesto");
        model_runner_unload();
        return RUNNER_ERR_INCOMPATIBLE;
    }

    runner_err_t serr = shrink_arena(model);
    if (serr != RUNNER_OK) {
        return serr;
    }

    r.version = meta->model_version;
    r.n_features = meta->n_features;
    r.n_classes = meta->n_classes;
    r.i_sev = meta->output_severity_index;
    r.i_fault = meta->output_fault_index;
    memcpy(r.mean, meta->feat_mean, nf * sizeof(float));
    memcpy(r.std, meta->feat_std, nf * sizeof(float));

    ESP_LOGI(TAG, "v%ld cargado: arena usada %u de %u B",
             (long)r.version, (unsigned)r.interpreter->arena_used_bytes(), (unsigned)r.arena_size);
    return RUNNER_OK;
}

bool model_runner_invoke(const float *raw, float *severity, float *probs, int64_t *invoke_us) {
    if (!r.interpreter) {
        return false;
    }

    // La misma estandarización que train.py: (x - mean) / std.
    float *in = r.interpreter->input(0)->data.f;
    for (uint32_t i = 0; i < r.n_features; i++) {
        in[i] = (raw[i] - r.mean[i]) / r.std[i];
    }

    int64_t t0 = esp_timer_get_time();
    TfLiteStatus st = r.interpreter->Invoke();
    if (invoke_us) {
        *invoke_us = esp_timer_get_time() - t0;
    }
    if (st != kTfLiteOk) {
        ESP_LOGE(TAG, "Invoke falló");
        return false;
    }

    *severity = r.interpreter->output(r.i_sev)->data.f[0];
    memcpy(probs, r.interpreter->output(r.i_fault)->data.f, r.n_classes * sizeof(float));
    return true;
}

float model_runner_selftest(const ModelManifest *meta, int64_t *invoke_us) {
    if (!r.interpreter || meta->n_test_input != r.n_features
            || meta->n_test_output != 1 + r.n_classes) {
        ESP_LOGE(TAG, "Autotest: el manifiesto no trae un vector de prueba válido");
        return -1.0f;
    }

    float sev, probs[MAX_CLASSES];
    if (!model_runner_invoke(meta->test_input, &sev, probs, invoke_us)) {
        return -1.0f;
    }

    // test_output = [severidad, prob_0, ..., prob_{n-1}], de Python.
    float max_err = fabsf(sev - meta->test_output[0]);
    for (uint32_t i = 0; i < r.n_classes; i++) {
        float e = fabsf(probs[i] - meta->test_output[1 + i]);
        if (e > max_err) max_err = e;
    }

    ESP_LOGI(TAG, "Autotest: severidad %.5f (Python %.5f), mayor error %.2e",
             sev, meta->test_output[0], max_err);
    return max_err;
}
