#include <stdlib.h>
#include "nvs.h"
#include "esp_log.h"
#include "nebulaedge_config_store.h"

static const char *TAG = "config_store";

/* Namespace y clave dentro de la NVS. Son detalle interno: nadie de afuera
 * necesita saber dónde ni con qué nombre queda guardado el blob. */
#define NVS_NAMESPACE  "nebulaedge"
#define NVS_KEY_CONFIG "config_blob"

void config_store_clear(void) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    nvs_erase_key(nvs, NVS_KEY_CONFIG);
    nvs_commit(nvs);
    nvs_close(nvs);
}

void config_store_save(const Config *cfg) {
    if (!cfg) return;

    size_t size = config__get_packed_size(cfg);
    if (size == 0) return;

    uint8_t *buf = malloc(size);
    if (!buf) {
        ESP_LOGE(TAG, "No hay memoria para guardar config en NVS");
        return;
    }
    config__pack(cfg, buf);

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        if (nvs_set_blob(nvs, NVS_KEY_CONFIG, buf, size) == ESP_OK) {
            nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    free(buf);
}

Config *config_store_load(void) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return NULL;
    }

    size_t size = 0;
    if (nvs_get_blob(nvs, NVS_KEY_CONFIG, NULL, &size) != ESP_OK || size == 0) {
        nvs_close(nvs);
        return NULL;
    }

    uint8_t *buf = malloc(size);
    if (!buf) {
        nvs_close(nvs);
        return NULL;
    }

    Config *cfg = NULL;
    if (nvs_get_blob(nvs, NVS_KEY_CONFIG, buf, &size) == ESP_OK) {
        cfg = config__unpack(NULL, size, buf);
    }

    free(buf);
    nvs_close(nvs);
    return cfg;
}
