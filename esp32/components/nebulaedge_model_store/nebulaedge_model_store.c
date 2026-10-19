#include <string.h>
#include <stdbool.h>
#include "esp_log.h"
#include "esp_partition.h"
#include "nvs.h"
#include "mbedtls/sha256.h"
#include "nebulaedge_model_store.h"

static const char *TAG = "model_store";

#define NVS_NS          "model"
#define KEY_ACT_SLOT    "act_slot"      // u8: 0 = model_a, 1 = model_b, 0xFF = ninguna
#define KEY_ACT_VER     "act_ver"       // i32
#define KEY_ACT_SHA     "act_sha"       // blob 32
#define KEY_ACT_META    "act_meta"      // blob: ModelManifest empaquetado del activo
#define KEY_XFER_VER    "xfer_ver"      // i32; 0 = no hay transferencia en curso
#define KEY_XFER_SIZE   "xfer_size"     // u32
#define KEY_XFER_CHUNK  "xfer_chunk"    // u32: chunk_size de esa transferencia
#define KEY_XFER_SHA    "xfer_sha"      // blob 32
#define KEY_XFER_NEXT   "xfer_next"     // u32: siguiente trozo a escribir

#define SLOT_NONE 0xFF

static const esp_partition_t *s_slots[2];

static uint8_t  s_act_slot = SLOT_NONE;
static int32_t  s_act_ver = 0;
static uint8_t  s_act_sha[32];

static int32_t  s_xfer_ver = 0;
static uint32_t s_xfer_size = 0;
static uint32_t s_xfer_chunk = 0;
static uint8_t  s_xfer_sha[32];
static uint32_t s_xfer_next = 0;
static bool     s_verified = false;     // la transferencia en curso pasó el sha256

/* Un mapeo por ranura. Se deshace antes de borrar o escribir esa ranura: la
 * caché de flash serviría los bytes viejos. */
static esp_partition_mmap_handle_t s_map_handle[2];
static const void *s_map_ptr[2];

static void unmap_slot(uint8_t slot) {
    if (s_map_ptr[slot]) {
        esp_partition_munmap(s_map_handle[slot]);
        s_map_ptr[slot] = NULL;
    }
}

/* La ranura que NO es la activa: ahí va siempre el modelo nuevo. */
static uint8_t free_slot(void) {
    return s_act_slot == 0 ? 1 : 0;
}

static void persist_xfer(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo abrir NVS para guardar el progreso");
        return;
    }
    nvs_set_i32(h, KEY_XFER_VER, s_xfer_ver);
    nvs_set_u32(h, KEY_XFER_SIZE, s_xfer_size);
    nvs_set_u32(h, KEY_XFER_CHUNK, s_xfer_chunk);
    nvs_set_blob(h, KEY_XFER_SHA, s_xfer_sha, sizeof s_xfer_sha);
    nvs_set_u32(h, KEY_XFER_NEXT, s_xfer_next);
    nvs_commit(h);
    nvs_close(h);
}

/* Solo el puntero de progreso: es lo único que cambia trozo a trozo. */
static void persist_next(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u32(h, KEY_XFER_NEXT, s_xfer_next);
    nvs_commit(h);
    nvs_close(h);
}

static void clear_xfer(void) {
    s_verified = false;
    s_xfer_ver = 0;
    s_xfer_size = 0;
    s_xfer_chunk = 0;
    s_xfer_next = 0;
    memset(s_xfer_sha, 0, sizeof s_xfer_sha);
    persist_xfer();
}

esp_err_t model_store_init(void) {
    s_slots[0] = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "model_a");
    s_slots[1] = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "model_b");
    if (!s_slots[0] || !s_slots[1]) {
        ESP_LOGE(TAG, "Faltan las particiones model_a/model_b: ¿se flasheó la tabla de particiones nueva?");
        return ESP_ERR_NOT_FOUND;
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof s_act_sha;
        nvs_get_u8(h, KEY_ACT_SLOT, &s_act_slot);
        nvs_get_i32(h, KEY_ACT_VER, &s_act_ver);
        nvs_get_blob(h, KEY_ACT_SHA, s_act_sha, &len);

        len = sizeof s_xfer_sha;
        nvs_get_i32(h, KEY_XFER_VER, &s_xfer_ver);
        nvs_get_u32(h, KEY_XFER_SIZE, &s_xfer_size);
        nvs_get_u32(h, KEY_XFER_CHUNK, &s_xfer_chunk);
        nvs_get_blob(h, KEY_XFER_SHA, s_xfer_sha, &len);
        nvs_get_u32(h, KEY_XFER_NEXT, &s_xfer_next);
        nvs_close(h);
    }
    // Sin NVS todavía (primer arranque): quedan los valores por defecto.

    if (s_act_slot > 1) {
        s_act_slot = SLOT_NONE;
        s_act_ver = 0;
    }

    ESP_LOGI(TAG, "Ranuras de %u KB. Activo: v%ld (%s). En curso: v%ld, trozo %lu",
             (unsigned)(s_slots[0]->size / 1024), (long)s_act_ver,
             s_act_slot == SLOT_NONE ? "ninguna" : s_slots[s_act_slot]->label,
             (long)s_xfer_ver, (unsigned long)s_xfer_next);
    return ESP_OK;
}

model_begin_t model_store_begin(int32_t version, uint32_t size, uint32_t chunk_size,
                                const uint8_t sha256[32], uint32_t *next_chunk) {
    *next_chunk = 0;

    if (s_act_ver == version && s_act_slot != SLOT_NONE && memcmp(s_act_sha, sha256, 32) == 0) {
        return MODEL_BEGIN_ACTIVE;
    }

    const esp_partition_t *slot = s_slots[free_slot()];
    if (size == 0 || chunk_size == 0 || size > slot->size) {
        return MODEL_BEGIN_TOO_BIG;
    }

    /* La misma transferencia que ya venía: mismo modelo y mismo troceado. Si
     * cambió el chunk_size los offsets ya no calzan y hay que empezar de nuevo. */
    if (s_xfer_ver == version && s_xfer_size == size && s_xfer_chunk == chunk_size
            && memcmp(s_xfer_sha, sha256, 32) == 0) {
        *next_chunk = s_xfer_next;
        return MODEL_BEGIN_RESUME;
    }

    s_verified = false;
    unmap_slot(free_slot());

    // Borrado redondeado a sector: es la unidad mínima de la flash.
    uint32_t erase_len = (size + slot->erase_size - 1) / slot->erase_size * slot->erase_size;
    esp_err_t err = esp_partition_erase_range(slot, 0, erase_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo borrar %s: %s", slot->label, esp_err_to_name(err));
        return MODEL_BEGIN_FLASH_ERR;
    }

    s_xfer_ver = version;
    s_xfer_size = size;
    s_xfer_chunk = chunk_size;
    memcpy(s_xfer_sha, sha256, 32);
    s_xfer_next = 0;
    persist_xfer();

    ESP_LOGI(TAG, "Recibiendo v%ld (%lu B) en %s", (long)version, (unsigned long)size, slot->label);
    return MODEL_BEGIN_FRESH;
}

esp_err_t model_store_write(uint32_t chunk_index, uint32_t offset, const uint8_t *data, size_t len) {
    if (s_xfer_ver == 0 || chunk_index != s_xfer_next) {
        return ESP_ERR_INVALID_STATE;
    }
    /* El offset lo manda el servidor, pero se recalcula y se compara: un
     * offset que no calce con el índice escribiría en otro lado de la ranura. */
    if (offset != chunk_index * s_xfer_chunk || offset + len > s_xfer_size || len == 0) {
        ESP_LOGW(TAG, "Trozo %lu fuera de forma (offset %lu, %u B)",
                 (unsigned long)chunk_index, (unsigned long)offset, (unsigned)len);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = esp_partition_write(s_slots[free_slot()], offset, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Fallo al escribir el trozo %lu: %s", (unsigned long)chunk_index, esp_err_to_name(err));
        return err;
    }

    s_xfer_next++;
    persist_next();
    return ESP_OK;
}

uint32_t model_store_next_chunk(void) {
    return s_xfer_next;
}

bool model_store_complete(void) {
    return s_xfer_ver != 0 && s_xfer_chunk != 0
        && (uint64_t)s_xfer_next * s_xfer_chunk >= s_xfer_size;
}

esp_err_t model_store_verify(void) {
    const esp_partition_t *slot = s_slots[free_slot()];
    uint8_t buf[512];
    uint8_t digest[32];

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    for (uint32_t off = 0; off < s_xfer_size; off += sizeof buf) {
        size_t n = s_xfer_size - off < sizeof buf ? s_xfer_size - off : sizeof buf;
        if (esp_partition_read(slot, off, buf, n) != ESP_OK) {
            mbedtls_sha256_free(&ctx);
            clear_xfer();
            return ESP_FAIL;
        }
        mbedtls_sha256_update(&ctx, buf, n);
    }
    mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);

    if (memcmp(digest, s_xfer_sha, 32) != 0) {
        ESP_LOGE(TAG, "sha256 de v%ld no calza: se descarta", (long)s_xfer_ver);
        clear_xfer();
        return ESP_ERR_INVALID_CRC;
    }

    s_verified = true;
    ESP_LOGI(TAG, "sha256 de v%ld verificado en %s", (long)s_xfer_ver, slot->label);
    return ESP_OK;
}

esp_err_t model_store_activate(const uint8_t *meta, size_t meta_len) {
    if (s_xfer_ver == 0 || !s_verified) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Recién acá cambia cuál es la activa, y en un solo commit de NVS: si se
     * corta la luz antes, sigue mandando la anterior. */
    uint8_t new_slot = free_slot();
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return ESP_FAIL;
    }
    nvs_set_u8(h, KEY_ACT_SLOT, new_slot);
    nvs_set_i32(h, KEY_ACT_VER, s_xfer_ver);
    nvs_set_blob(h, KEY_ACT_SHA, s_xfer_sha, 32);
    nvs_set_blob(h, KEY_ACT_META, meta, meta_len);
    nvs_set_i32(h, KEY_XFER_VER, 0);
    nvs_set_u32(h, KEY_XFER_NEXT, 0);
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo activar v%ld: %s", (long)s_xfer_ver, esp_err_to_name(err));
        return err;
    }

    s_act_slot = new_slot;
    s_act_ver = s_xfer_ver;
    memcpy(s_act_sha, s_xfer_sha, 32);
    s_xfer_ver = 0;
    s_xfer_next = 0;
    s_verified = false;

    ESP_LOGI(TAG, "v%ld activo en %s", (long)s_act_ver, s_slots[new_slot]->label);
    return ESP_OK;
}

void model_store_abort(void) {
    s_verified = false;
    if (s_xfer_ver != 0) {
        clear_xfer();
    }
}

const void *model_store_map(bool pending, size_t *size) {
    uint8_t slot;
    if (pending) {
        if (s_xfer_ver == 0 || !s_verified) return NULL;
        slot = free_slot();
        *size = s_xfer_size;
    }
    else {
        if (s_act_slot == SLOT_NONE) return NULL;
        slot = s_act_slot;
        *size = s_slots[slot]->size;    // el tamaño exacto lo sabe el flatbuffer
    }

    if (s_map_ptr[slot] == NULL) {
        esp_err_t err = esp_partition_mmap(s_slots[slot], 0, s_slots[slot]->size,
                                           ESP_PARTITION_MMAP_DATA, &s_map_ptr[slot], &s_map_handle[slot]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "No se pudo mapear %s: %s", s_slots[slot]->label, esp_err_to_name(err));
            s_map_ptr[slot] = NULL;
            return NULL;
        }
    }
    return s_map_ptr[slot];
}

esp_err_t model_store_active_meta(uint8_t *buf, size_t *len) {
    if (s_act_slot == SLOT_NONE) {
        return ESP_ERR_NOT_FOUND;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = nvs_get_blob(h, KEY_ACT_META, buf, len);
    nvs_close(h);
    return err;
}

esp_err_t model_store_set_active_meta(const uint8_t *meta, size_t meta_len) {
    if (s_act_slot == SLOT_NONE) {
        return ESP_ERR_INVALID_STATE;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, KEY_ACT_META, meta, meta_len);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

int32_t model_store_active_version(void) {
    return s_act_slot == SLOT_NONE ? 0 : s_act_ver;
}
