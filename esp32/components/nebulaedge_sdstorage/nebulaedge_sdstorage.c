/* Dependencias de esta componente: nebulaedge_microsd, nebulaedge_proto_schema
 * (tipos Environmental, Inertial) y cJSON (en ESP-IDF). */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <cJSON.h>
#include "esp_log.h"
#include "esp_err.h"
#include "schema.pb-c.h"
#include "nebulaedge_microsd.h"

static const char *environmental_path = "/sdcard/environmental.ndjson";
static const char *inertial_path      = "/sdcard/inertial.ndjson";
static const char *TAG = "nebulaedge_sdstorage";

/* Abre en append (crea si no existe). */
static FILE *open_append(const char *path) {
    FILE *f = fopen(path, "ab");
    if (!f) {
        ESP_LOGE(TAG, "cannot open %s: errno=%d (%s)", path, errno, strerror(errno));
    }
    return f;
}

/* Escribe una línea NDJSON: {"...":...}\n */
static esp_err_t append_ndjson_line(FILE *f, cJSON *json) {
    if (!f || !json) {
        return ESP_ERR_INVALID_ARG;
    } 

    char *line = cJSON_PrintUnformatted(json);
    if (!line) {
        return ESP_ERR_NO_MEM;
    } 

    size_t n = strlen(line);
    if (fwrite(line, 1, n, f) != n) {
        cJSON_free(line);
        return ESP_FAIL;
    }
    if (fputc('\n', f) == EOF) {
        cJSON_free(line);
        return ESP_FAIL;
    }
    cJSON_free(line);

    return fflush(f) == 0 ? ESP_OK : ESP_FAIL;
}

/* Escribe en un archivo los datos ambientales en formato .ndjson. */
static esp_err_t environmental_to_ndjson(Environmental *env, FILE *f) {
    if (!env || !f) return ESP_ERR_INVALID_ARG;

    cJSON *json = cJSON_CreateObject();
    if (!json) return ESP_ERR_NO_MEM;

    cJSON_AddStringToObject(json, "id_device", env->id_device ? env->id_device : "");
    cJSON_AddNumberToObject(json, "temperature", env->temperature);
    cJSON_AddNumberToObject(json, "press", env->press);
    cJSON_AddNumberToObject(json, "hum", env->hum);
    cJSON_AddNumberToObject(json, "co", env->co);
    cJSON_AddNumberToObject(json, "time_client", (double)env->time_client);
    cJSON_AddNumberToObject(json, "config_version_applied", env->config_version_applied);

    esp_err_t ret = append_ndjson_line(f, json);
    cJSON_Delete(json);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "NDJSON write OK: type=0x01 path=%s", environmental_path);
    }
    return ret;
}

/* Escribe en un archivo los datos inerciales en formato .ndjson. */
static esp_err_t inertial_to_ndjson(Inertial *ine, FILE *f) {
    if (!ine || !f) return ESP_ERR_INVALID_ARG;

    cJSON *json = cJSON_CreateObject();
    if (!json) return ESP_ERR_NO_MEM;

    cJSON_AddStringToObject(json, "id_device", ine->id_device ? ine->id_device : "");
    cJSON_AddNumberToObject(json, "acc_x", ine->acc_x);
    cJSON_AddNumberToObject(json, "acc_y", ine->acc_y);
    cJSON_AddNumberToObject(json, "acc_z", ine->acc_z);
    cJSON_AddNumberToObject(json, "gyr_x", ine->gyr_x);
    cJSON_AddNumberToObject(json, "gyr_y", ine->gyr_y);
    cJSON_AddNumberToObject(json, "gyr_z", ine->gyr_z);
    cJSON_AddNumberToObject(json, "mag_x", ine->mag_x);
    cJSON_AddNumberToObject(json, "mag_y", ine->mag_y);
    cJSON_AddNumberToObject(json, "mag_z", ine->mag_z);
    cJSON_AddNumberToObject(json, "time_client", (double)ine->time_client);
    cJSON_AddNumberToObject(json, "config_version_applied", ine->config_version_applied);

    esp_err_t ret = append_ndjson_line(f, json);
    cJSON_Delete(json);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "NDJSON write OK: type=0x02 path=%s", inertial_path);
    }
    return ret;
}

/* Recibe paquete de forma [tipo|payload protobuf] y lo guarda en NDJSON. */
esp_err_t sdstorage_write_packet(uint8_t *data, size_t size) {
    if (!data || size < 2) return ESP_ERR_INVALID_ARG;

    if (!sd_is_mounted()) {
        ESP_LOGI(TAG, "SD not mounted. Not writing.");
        return ESP_OK;
    }

    esp_err_t ret = sd_format_if_no_space();
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t type = data[0];
    const uint8_t *payload = data + 1;
    size_t payload_size = size - 1;

    if (type == 0x01) {
        FILE *f = open_append(environmental_path);
        if (!f) return ESP_FAIL;

        Environmental *msg = environmental__unpack(NULL, payload_size, payload);
        if (!msg) {
            fclose(f);
            return ESP_FAIL;
        }

        ret = environmental_to_ndjson(msg, f);
        environmental__free_unpacked(msg, NULL);
        fclose(f);
        return ret;
    }

    if (type == 0x02) {
        FILE *f = open_append(inertial_path);
        if (!f) return ESP_FAIL;

        Inertial *msg = inertial__unpack(NULL, payload_size, payload);
        if (!msg) {
            fclose(f);
            return ESP_FAIL;
        }

        ret = inertial_to_ndjson(msg, f);
        inertial__free_unpacked(msg, NULL);
        fclose(f);
        return ret;
    }

    return ESP_ERR_NOT_SUPPORTED;
}