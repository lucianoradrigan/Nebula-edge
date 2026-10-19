#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "infer.h"
#include "nebulaedge_defs.h"
#include "nebulaedge_device.h"
#include "nebulaedge_model_store.h"
#include "nebulaedge_mqtt.h"
#include "schema.pb-c.h"
#include "model_runner.h"
#include "nebulaedge_features.h"

#define TAG_INFER "task_infer"

extern Config *current_config;

/* Lo que este firmware sabe calcular. Un modelo entrenado con otras features
 * se rechaza (MODEL_ERROR_INCOMPATIBLE) en vez de inferir basura. Tiene que
 * calzar con FEATURE_SET_VERSION y N_FEATURES de ml/features.py. */
#define FIRMWARE_FEATURE_SET_VERSION FEATURES_SET_VERSION
#define FIRMWARE_N_FEATURES          FEATURES_COUNT

/* Cada cuánto se manda ModelStatus aunque nada cambie. En INFER no hay
 * telemetría: esto es lo que le prueba al servidor que el device sigue vivo,
 * así que tiene que quedar bien por debajo de su no_data_grace_sec (50 s). */
#define INFER_STATUS_PERIOD_MS  10000

/* Si el trozo pedido no llegó en este tiempo, se vuelve a pedir. El pedido o
 * la respuesta pueden perderse (QoS 1 reintenta, pero no a través de una
 * reconexión), y sin esto la transferencia quedaría esperando para siempre. */
#define INFER_CHUNK_TIMEOUT_MS  3000

/* Vuelta del bucle: cada cuánto se miran los timeouts y la orden de parar. */
#define INFER_POLL_MS 250

static QueueHandle_t s_queue = NULL;
static TaskHandle_t s_task = NULL;
static SemaphoreHandle_t s_stopped = NULL;
static volatile bool s_active = false;

/* Copia de lo que esta task necesita de la config, tomada en infer_start().
 *
 * NO se lee current_config desde acá: al aplicar una config nueva,
 * vTaskGetResponse() libera la vieja antes de asignar la nueva, y esta task
 * corre en paralelo. Leerla era usar memoria ya liberada. */
static char    s_device_id[64];
static int32_t s_config_version;

/* Lo levanta infer_freeze() desde la task de respuesta en cuanto llega una
 * config nueva, ANTES de tocar current_config. Congelada, la task descarta lo
 * que llegue y no publica: el transporte está por cerrarse. Medido en banco
 * antes de esto: en los 4 s entre el ACK y el cierre la task alcanzaba a bajar
 * 16 trozos de un modelo nuevo por la conexión que se iba a cortar. */
static volatile bool s_frozen = false;

/* Estado de la sesión INFER en curso. Se reinicia en cada infer_start(). */
static struct {
    int32_t   version;          // la del manifiesto vigente; 0 = ninguno
    uint32_t  size;
    uint32_t  chunk_size;
    uint32_t  n_chunks;
    bool      receiving;
    ModelState state;
    ModelError error;
    int64_t   requested_at_us;  // cuándo se pidió el trozo que se espera
    uint32_t  retries;          // pedidos repetidos del trozo actual
    int64_t   status_at_us;     // último ModelStatus enviado
    int64_t   started_at_us;    // comienzo de la transferencia, para el log
    uint32_t  chunks_done;      // para el status: los escritos, o todos si ya está

    /* El manifiesto vigente, en las dos formas: desempaquetado para usarlo y
     * empaquetado para guardarlo con el modelo al activarlo (model_store_activate). */
    ModelManifest *manifest;
    uint8_t   *manifest_raw;
    size_t     manifest_len;

    float     selftest_err;     // del último autotest; 0 si no hubo
    int64_t   invoke_us;
} s;

/* sha256 del último modelo que falló el autotest o no cargó. Fuera de `s` a
 * propósito: sobrevive a infer_start(). Sin esto, cada reconexión volvía a
 * bajar el mismo modelo malo para rechazarlo otra vez. Si el servidor quiere
 * reintentarlo, tiene que mandar otro modelo. La clave es versión Y sha: dos
 * versiones pueden traer los mismos bytes con distinto manifiesto (un autotest
 * mal armado, por ejemplo), y rechazar una no dice nada de la otra. */
static uint8_t s_rejected_sha[32];
static int32_t s_rejected_version = 0;
static ModelError s_rejected_error = MODEL_ERROR__MODEL_ERROR_NONE;

/* Diferencia aceptada en el autotest entre la salida del device y la de
 * Python. En float32 la medida es del orden de 1e-7; en int8 puede moverse un
 * paso de cuantización de la salida (1/256 ~ 0,004) porque los kernels de
 * esp-nn no son los del conversor de TensorFlow. 0,01 deja pasar eso y no un
 * modelo que dé otra cosa. */
#define SELFTEST_TOLERANCE 0.01f

static void free_manifest(void) {
    if (s.manifest) {
        model_manifest__free_unpacked(s.manifest, NULL);
        s.manifest = NULL;
    }
    free(s.manifest_raw);
    s.manifest_raw = NULL;
    s.manifest_len = 0;
}

static const char *state_name(ModelState st) {
    switch (st) {
        case MODEL_STATE__MODEL_STATE_NONE:      return "NONE";
        case MODEL_STATE__MODEL_STATE_RECEIVING: return "RECEIVING";
        case MODEL_STATE__MODEL_STATE_VERIFYING: return "VERIFYING";
        case MODEL_STATE__MODEL_STATE_READY:     return "READY";
        case MODEL_STATE__MODEL_STATE_FAILED:    return "FAILED";
        default:                                 return "?";
    }
}

/* Publica [tipo][protobuf] en /topic/nebulaedge/{id}/<path>, a QoS 1. */
static void publish_typed(const char *path, uint8_t type, const uint8_t *pb, size_t len) {
    char topic[128];
    snprintf(topic, sizeof topic, "/topic/nebulaedge/%s/%s", s_device_id, path);

    uint8_t *buf = malloc(len + 1);
    if (!buf) {
        ESP_LOGE(TAG_INFER, "Sin memoria para publicar en %s", topic);
        return;
    }
    buf[0] = type;
    memcpy(buf + 1, pb, len);
    if (mqtt_publish(topic, buf, len + 1, 1) < 0) {
        ESP_LOGW(TAG_INFER, "No se pudo publicar en %s", topic);
    }
    free(buf);
}

static void publish_status(void) {
    ModelStatus st = MODEL_STATUS__INIT;
    st.id_device = s_device_id;
    st.model_version = s.version;
    st.state = s.state;
    st.chunks_received = s.receiving ? model_store_next_chunk() : s.chunks_done;
    st.error = s.error;
    st.config_version_applied = s_config_version;
    st.time_client = device_clock_now_s();
    st.model_version_active = model_store_active_version();
    st.selftest_max_error = s.selftest_err;
    st.arena_used_bytes = model_runner_arena_used();
    st.invoke_us = (uint32_t)s.invoke_us;

    uint8_t buf[128];   // ModelStatus empaquetado: id de hasta ~40 B + 8 enteros
    size_t len = model_status__get_packed_size(&st);
    if (len > sizeof buf) {
        ESP_LOGE(TAG_INFER, "ModelStatus de %u B no cabe en el buffer", (unsigned)len);
        return;
    }
    model_status__pack(&st, buf);
    publish_typed("model/status", 0x05, buf, len);
    s.status_at_us = esp_timer_get_time();
}

static void set_state(ModelState st, ModelError err) {
    if (st != s.state || err != s.error) {
        ESP_LOGI(TAG_INFER, "Estado %s -> %s (error %d)", state_name(s.state), state_name(st), (int)err);
    }
    s.state = st;
    s.error = err;
    publish_status();
}

static void request_chunk(uint32_t index) {
    ModelChunkRequest req = MODEL_CHUNK_REQUEST__INIT;
    req.id_device = s_device_id;
    req.model_version = s.version;
    req.chunk_index = index;

    uint8_t buf[64];
    size_t len = model_chunk_request__get_packed_size(&req);
    if (len > sizeof buf) {
        return;
    }
    model_chunk_request__pack(&req, buf);
    publish_typed("model/chunk/req", 0x06, buf, len);
    s.requested_at_us = esp_timer_get_time();
}

static ModelError runner_to_model_error(runner_err_t e) {
    switch (e) {
        case RUNNER_ERR_ARENA:        return MODEL_ERROR__MODEL_ERROR_ARENA;
        case RUNNER_ERR_INCOMPATIBLE: return MODEL_ERROR__MODEL_ERROR_INCOMPATIBLE;
        case RUNNER_ERR_LOAD:
        default:                      return MODEL_ERROR__MODEL_ERROR_LOAD;
    }
}

/* Carga en TFLM un modelo ya en flash y corre su autotest. Es la verificación
 * que el sha256 no puede hacer: el sha dice que llegó lo que se mandó, esto
 * dice que este firmware lo sabe correr y que da lo mismo que en Python. */
static ModelError load_and_selftest(const void *model, const ModelManifest *meta) {
    runner_err_t rerr = model_runner_load(model, meta);
    if (rerr != RUNNER_OK) {
        s.selftest_err = 0;
        return runner_to_model_error(rerr);
    }

    float err = model_runner_selftest(meta, &s.invoke_us);
    s.selftest_err = err;
    if (err < 0 || err > SELFTEST_TOLERANCE) {
        ESP_LOGW(TAG_INFER, "Autotest de v%ld falló: error %.4f (tolerancia %.2f)",
                 (long)meta->model_version, err, SELFTEST_TOLERANCE);
        model_runner_unload();
        return MODEL_ERROR__MODEL_ERROR_SELFTEST;
    }
    ESP_LOGI(TAG_INFER, "v%ld en TFLM: arena %u B, inferencia %lld us, autotest err %.2e",
             (long)meta->model_version, (unsigned)model_runner_arena_used(),
             (long long)s.invoke_us, err);
    return MODEL_ERROR__MODEL_ERROR_NONE;
}

/* Deja cargado en TFLM el modelo ACTIVO, con el manifiesto que se guardó con
 * él. Se usa al entrar a INFER y cuando un modelo nuevo se rechaza: el activo
 * tiene que seguir sirviendo. */
static void ensure_active_loaded(void) {
    int32_t active = model_store_active_version();
    if (active == 0 || model_runner_version() == active) {
        return;
    }

    size_t size;
    const void *model = model_store_map(false, &size);
    uint8_t *raw = malloc(2048);
    size_t len = 2048;
    if (!model || !raw) {
        ESP_LOGW(TAG_INFER, "No se pudo mapear el modelo activo v%ld", (long)active);
        free(raw);
        return;
    }
    if (model_store_active_meta(raw, &len) != ESP_OK) {
        /* Activado sin manifiesto (lo hacía el firmware de la fase A). Si el
         * que acaba de llegar es el suyo, sirve: model_store_begin() ya
         * comprobó versión y sha256 antes de decir ACTIVE. Se guarda para la
         * próxima vez. */
        free(raw);
        raw = NULL;
        if (s.manifest && s.manifest->model_version == active && s.manifest_raw) {
            ESP_LOGW(TAG_INFER, "v%ld no tenía manifiesto guardado: se usa y guarda el recibido", (long)active);
            model_store_set_active_meta(s.manifest_raw, s.manifest_len);
            raw = malloc(s.manifest_len);
            if (raw) {
                memcpy(raw, s.manifest_raw, s.manifest_len);
                len = s.manifest_len;
            }
        }
        if (!raw) {
            ESP_LOGW(TAG_INFER, "No se pudo recuperar el manifiesto del modelo activo v%ld", (long)active);
            return;
        }
    }
    ModelManifest *meta = model_manifest__unpack(NULL, len, raw);
    free(raw);
    if (!meta) {
        ESP_LOGW(TAG_INFER, "El manifiesto guardado de v%ld es ilegible", (long)active);
        return;
    }
    ModelError err = load_and_selftest(model, meta);
    if (err != MODEL_ERROR__MODEL_ERROR_NONE) {
        ESP_LOGE(TAG_INFER, "El modelo activo v%ld no carga (error %d)", (long)active, (int)err);
    }
    model_manifest__free_unpacked(meta, NULL);
}

static void reject(ModelError err) {
    /* Se recuerda solo lo que es culpa del MODELO. Sin arena o con la flash
     * fallando, el mismo modelo puede servir en el próximo intento. */
    if (err != MODEL_ERROR__MODEL_ERROR_ARENA && err != MODEL_ERROR__MODEL_ERROR_FLASH) {
        memcpy(s_rejected_sha, s.manifest->sha256.data, 32);
        s_rejected_version = s.manifest->model_version;
        s_rejected_error = err;
    }
    model_store_abort();
    ensure_active_loaded();     // el anterior sigue: que quede cargado
    set_state(MODEL_STATE__MODEL_STATE_FAILED, err);
}

/* Transferencia completa: sha256, TFLM, autotest y recién ahí activar. */
static void finish_transfer(void) {
    s.receiving = false;
    s.chunks_done = s.n_chunks;
    set_state(MODEL_STATE__MODEL_STATE_VERIFYING, MODEL_ERROR__MODEL_ERROR_NONE);

    esp_err_t err = model_store_verify();
    if (err != ESP_OK) {
        reject(err == ESP_ERR_INVALID_CRC ? MODEL_ERROR__MODEL_ERROR_HASH : MODEL_ERROR__MODEL_ERROR_FLASH);
        return;
    }

    size_t size;
    const void *model = model_store_map(true, &size);
    if (!model) {
        reject(MODEL_ERROR__MODEL_ERROR_FLASH);
        return;
    }

    ModelError merr = load_and_selftest(model, s.manifest);
    if (merr != MODEL_ERROR__MODEL_ERROR_NONE) {
        reject(merr);
        return;
    }

    if (model_store_activate(s.manifest_raw, s.manifest_len) != ESP_OK) {
        reject(MODEL_ERROR__MODEL_ERROR_FLASH);
        return;
    }

    ESP_LOGI(TAG_INFER, "Modelo v%ld listo: %lu B en %.1f s",
             (long)s.version, (unsigned long)s.size,
             (esp_timer_get_time() - s.started_at_us) / 1e6);
    set_state(MODEL_STATE__MODEL_STATE_READY, MODEL_ERROR__MODEL_ERROR_NONE);
}

/* Toma posesión de `m` y de `raw`: quedan en s.manifest hasta el próximo. */
static void on_manifest(ModelManifest *m, uint8_t *raw, size_t raw_len) {
    free_manifest();
    s.manifest = m;
    s.manifest_raw = raw;
    s.manifest_len = raw_len;

    s.receiving = false;
    s.version = m->model_version;
    s.chunks_done = 0;

    if (m->model_version == 0) {
        ESP_LOGI(TAG_INFER, "Manifiesto vacío: no hay modelo que bajar (activo v%ld)",
                 (long)model_store_active_version());
        ensure_active_loaded();
        set_state(model_store_active_version() ? MODEL_STATE__MODEL_STATE_READY
                                               : MODEL_STATE__MODEL_STATE_NONE,
                  MODEL_ERROR__MODEL_ERROR_NONE);
        return;
    }

    ESP_LOGI(TAG_INFER, "Manifiesto v%ld: %lu B en %lu trozos de %lu B, %lu features, %lu clases",
             (long)m->model_version, (unsigned long)m->size_bytes, (unsigned long)m->n_chunks,
             (unsigned long)m->chunk_size, (unsigned long)m->n_features, (unsigned long)m->n_classes);

    // Que el manifiesto sea coherente consigo mismo, antes de tocar la flash.
    bool well_formed = m->sha256.len == 32 && m->chunk_size > 0
        && m->n_chunks == (m->size_bytes + m->chunk_size - 1) / m->chunk_size;
    bool compatible = m->n_features == FIRMWARE_N_FEATURES
        && m->feature_set_version == FIRMWARE_FEATURE_SET_VERSION;
    if (!well_formed || !compatible) {
        ESP_LOGW(TAG_INFER, "Modelo rechazado: %s (features %lu v%lu; este firmware: %d v%d)",
                 well_formed ? "incompatible" : "manifiesto mal formado",
                 (unsigned long)m->n_features, (unsigned long)m->feature_set_version,
                 FIRMWARE_N_FEATURES, FIRMWARE_FEATURE_SET_VERSION);
        ensure_active_loaded();
        set_state(MODEL_STATE__MODEL_STATE_FAILED, MODEL_ERROR__MODEL_ERROR_INCOMPATIBLE);
        return;
    }

    s.size = m->size_bytes;
    s.chunk_size = m->chunk_size;
    s.n_chunks = m->n_chunks;

    if (s_rejected_error != MODEL_ERROR__MODEL_ERROR_NONE && s_rejected_version == m->model_version
            && memcmp(s_rejected_sha, m->sha256.data, 32) == 0) {
        ESP_LOGW(TAG_INFER, "v%ld ya se rechazó en este arranque (error %d): no se vuelve a bajar",
                 (long)m->model_version, (int)s_rejected_error);
        ensure_active_loaded();
        set_state(MODEL_STATE__MODEL_STATE_FAILED, s_rejected_error);
        return;
    }

    uint32_t next = 0;
    switch (model_store_begin(m->model_version, m->size_bytes, m->chunk_size, m->sha256.data, &next)) {
        case MODEL_BEGIN_ACTIVE:
            ESP_LOGI(TAG_INFER, "v%ld ya es el modelo activo: no se baja de nuevo", (long)m->model_version);
            s.chunks_done = s.n_chunks;
            ensure_active_loaded();
            set_state(model_runner_version() == m->model_version ? MODEL_STATE__MODEL_STATE_READY
                                                                 : MODEL_STATE__MODEL_STATE_FAILED,
                      model_runner_version() == m->model_version ? MODEL_ERROR__MODEL_ERROR_NONE
                                                                 : MODEL_ERROR__MODEL_ERROR_LOAD);
            return;
        case MODEL_BEGIN_TOO_BIG:
            set_state(MODEL_STATE__MODEL_STATE_FAILED, MODEL_ERROR__MODEL_ERROR_TOO_BIG);
            return;
        case MODEL_BEGIN_FLASH_ERR:
            set_state(MODEL_STATE__MODEL_STATE_FAILED, MODEL_ERROR__MODEL_ERROR_FLASH);
            return;
        case MODEL_BEGIN_RESUME:
            ESP_LOGI(TAG_INFER, "Se retoma v%ld desde el trozo %lu", (long)m->model_version, (unsigned long)next);
            break;
        case MODEL_BEGIN_FRESH:
            break;
    }

    s.receiving = true;
    s.retries = 0;
    s.started_at_us = esp_timer_get_time();
    set_state(MODEL_STATE__MODEL_STATE_RECEIVING, MODEL_ERROR__MODEL_ERROR_NONE);

    if (model_store_complete()) {
        finish_transfer();      // se cortó justo entre el último trozo y la verificación
    }
    else {
        request_chunk(next);
    }
}

static void on_chunk(const ModelChunk *c) {
    if (!s.receiving || c->model_version != s.version) {
        return;     // de una transferencia que ya no es la vigente
    }

    esp_err_t err = model_store_write(c->chunk_index, c->offset, c->data.data, c->data.len);
    if (err == ESP_ERR_INVALID_STATE) {
        return;     // repetido o adelantado: QoS 1 puede duplicar; se ignora
    }
    if (err != ESP_OK) {
        s.receiving = false;
        model_store_abort();
        set_state(MODEL_STATE__MODEL_STATE_FAILED,
                  err == ESP_ERR_INVALID_ARG ? MODEL_ERROR__MODEL_ERROR_INCOMPATIBLE
                                             : MODEL_ERROR__MODEL_ERROR_FLASH);
        return;
    }

    s.retries = 0;
    uint32_t next = model_store_next_chunk();
    ESP_LOGI(TAG_INFER, "Trozo %lu/%lu escrito (%u B)",
             (unsigned long)next, (unsigned long)s.n_chunks, (unsigned)c->data.len);

    if (model_store_complete()) {
        finish_transfer();
    }
    else {
        request_chunk(next);
    }
}


/* ----------------------------------------------------- fase C: inferencia */

#define MAX_CLASSES 16

/* Responde un InferRequest. Siempre responde -también cuando no puede
 * inferir, con el motivo en `status`-: el servidor espera una respuesta por
 * pedido, y sin ella lo daría por perdido y confundiría un error con una
 * pérdida de red. */
static void on_infer_request(const InferRequest *req) {
    InferResult res = INFER_RESULT__INIT;
    res.id_device = s_device_id;
    res.request_id = req->request_id;
    res.model_version = model_runner_version();
    res.config_version_applied = s_config_version;

    float probs[MAX_CLASSES];
    float feats[FEATURES_COUNT];        // C2: las que se calculan acá
    float severity = 0;
    int64_t invoke_us = 0;
    int64_t t_feat = esp_timer_get_time();
    /* Del modelo CARGADO, no de s.manifest: si el último manifiesto fue de un
     * modelo rechazado, el que corre sigue siendo el anterior. */
    uint32_t n_classes = model_runner_n_classes();

    if (!model_runner_loaded()) {
        res.status = INFER_STATUS__INFER_STATUS_NO_MODEL;
    }
    else if (req->model_version != model_runner_version()) {
        res.status = INFER_STATUS__INFER_STATUS_WRONG_MODEL;
    }
    else if (n_classes == 0 || n_classes > MAX_CLASSES
             || (req->n_features != FIRMWARE_N_FEATURES && req->n_window != FEATURES_WINDOW_SIZE)) {
        res.status = INFER_STATUS__INFER_STATUS_BAD_INPUT;
    }
    else if (req->n_window == FEATURES_WINDOW_SIZE
             && features_extract(req->window, req->n_window, req->fs_hz, req->rpm, feats) != 0) {
        res.status = INFER_STATUS__INFER_STATUS_BAD_INPUT;
    }
    else if (!model_runner_invoke(req->n_window ? feats : req->features, &severity, probs, &invoke_us)) {
        res.status = INFER_STATUS__INFER_STATUS_FAILED;
    }
    else {
        res.status = INFER_STATUS__INFER_STATUS_OK;
        res.severity = severity;
        res.n_probs = n_classes;
        res.probs = probs;
        uint32_t best = 0;
        for (uint32_t i = 1; i < n_classes; i++) {
            if (probs[i] > probs[best]) best = i;
        }
        res.pred_fault = best;
        res.inference_us = (uint32_t)invoke_us;
        if (req->n_window) {
            /* El tiempo total menos el del modelo es el de las features. Y se
             * devuelven, para comparar en el servidor contra las de Python. */
            res.feature_us = (uint32_t)(esp_timer_get_time() - t_feat - invoke_us);
            res.n_features = FEATURES_COUNT;
            res.features = feats;
        }
        s.invoke_us = invoke_us;
    }
    res.time_client = device_clock_now_s();

    if (res.status != INFER_STATUS__INFER_STATUS_OK) {
        ESP_LOGW(TAG_INFER, "Pedido %llu sin inferir (estado %d)", (unsigned long long)req->request_id, (int)res.status);
    }

    size_t len = infer_result__get_packed_size(&res);
    uint8_t *buf = malloc(len);
    if (!buf) {
        ESP_LOGE(TAG_INFER, "Sin memoria para el InferResult");
        return;
    }
    infer_result__pack(&res, buf);
    publish_typed("infer/result", 0x07, buf, len);
    free(buf);
}

static void handle(const packet_t *pkt) {
    if (pkt->size < 2) {
        return;
    }
    const uint8_t *pb = pkt->data + 1;
    size_t len = pkt->size - 1;

    if (pkt->data[0] == MQTT_MODEL_MSG_MANIFEST) {
        ModelManifest *m = model_manifest__unpack(NULL, len, pb);
        uint8_t *raw = malloc(len);
        if (!m || !raw) {
            ESP_LOGW(TAG_INFER, "Manifiesto ilegible o sin memoria, se descarta");
            if (m) model_manifest__free_unpacked(m, NULL);
            free(raw);
            return;
        }
        memcpy(raw, pb, len);
        on_manifest(m, raw, len);
    }
    else if (pkt->data[0] == MQTT_MODEL_MSG_INFER) {
        InferRequest *req = infer_request__unpack(NULL, len, pb);
        if (!req) {
            ESP_LOGW(TAG_INFER, "Pedido de inferencia ilegible, se descarta");
            return;
        }
        on_infer_request(req);
        infer_request__free_unpacked(req, NULL);
    }
    else if (pkt->data[0] == MQTT_MODEL_MSG_CHUNK) {
        ModelChunk *c = model_chunk__unpack(NULL, len, pb);
        if (!c) {
            ESP_LOGW(TAG_INFER, "Trozo ilegible, se descarta");
            return;
        }
        on_chunk(c);
        model_chunk__free_unpacked(c, NULL);
    }
}

static void drain_queue(void) {
    packet_t pkt;
    while (xQueueReceive(s_queue, &pkt, 0) == pdTRUE) {
        free(pkt.data);
    }
}

static void vTaskInfer(void *pv) {
    for (;;) {
        /* Inactiva: se avisa que se llegó al punto seguro y se espera la
         * próxima infer_start(). Con notificación y no con vTaskSuspend: una
         * notificación dada antes de empezar a esperar no se pierde. */
        if (!s_active) {
            drain_queue();
            xSemaphoreGive(s_stopped);
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        packet_t pkt;
        if (xQueueReceive(s_queue, &pkt, pdMS_TO_TICKS(INFER_POLL_MS)) == pdTRUE) {
            if (!s_frozen) {
                handle(&pkt);
            }
            free(pkt.data);
        }
        if (s_frozen) {
            continue;
        }

        int64_t now = esp_timer_get_time();

        if (s.receiving && now - s.requested_at_us >= INFER_CHUNK_TIMEOUT_MS * 1000LL) {
            s.retries++;
            ESP_LOGW(TAG_INFER, "Trozo %lu no llegó en %d ms: se pide de nuevo (reintento %lu)",
                     (unsigned long)model_store_next_chunk(), INFER_CHUNK_TIMEOUT_MS,
                     (unsigned long)s.retries);
            request_chunk(model_store_next_chunk());
        }

        if (now - s.status_at_us >= INFER_STATUS_PERIOD_MS * 1000LL) {
            publish_status();
        }
    }
}

void infer_init(QueueHandle_t model_queue) {
    s_queue = model_queue;
    s_stopped = xSemaphoreCreateBinary();
}

void infer_start(void) {
    free_manifest();
    memset(&s, 0, sizeof s);
    s.state = model_store_active_version() ? MODEL_STATE__MODEL_STATE_READY
                                           : MODEL_STATE__MODEL_STATE_NONE;
    /* Se anuncia de inmediato: el servidor ve que el device entró a INFER y con
     * qué modelo activo, antes de que llegue el manifiesto. */
    s.status_at_us = 0;

    snprintf(s_device_id, sizeof s_device_id, "%s", current_config->id_device);
    s_config_version = current_config->config_version;

    // Lo que haya quedado de la sesión anterior no corresponde a esta.
    xSemaphoreTake(s_stopped, 0);
    s_frozen = false;
    s_active = true;

    if (s_task == NULL) {
        xTaskCreate(vTaskInfer, TAG_INFER, 8192, NULL, 3, &s_task);   // TFLM corre acá: más pila
    }
    else {
        xTaskNotifyGive(s_task);
    }
}

void infer_stop(void) {
    if (s_task == NULL || !s_active) {
        return;
    }
    s_active = false;
    if (xSemaphoreTake(s_stopped, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGW(TAG_INFER, "La task de INFER no se detuvo a tiempo");
    }
    /* Una transferencia a medias se deja como está: el progreso queda en NVS y
     * se retoma si el device vuelve a INFER con el mismo modelo. */
}

void infer_freeze(void) {
    s_frozen = true;
}
