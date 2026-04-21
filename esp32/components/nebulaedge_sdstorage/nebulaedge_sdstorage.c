/* Dependencias de esta componente: nebulaedge_microsd, nebulaedge_proto_schema (tipos Data1, Data2) y 
 * cJSON (en ESP-IDF). */

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

static const char *data1_path= "/sdcard/data_1.ndjson";
static const char *data2_path= "/sdcard/data_2.ndjson";
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

/* Escribe en un archivo los datos del tipo Data1 en formato .ndjson. */
static esp_err_t data1_to_ndjson(Data1 *d1, FILE *f) {
    if (!d1 || !f) return ESP_ERR_INVALID_ARG;

    cJSON *json = cJSON_CreateObject();
    if (!json) return ESP_ERR_NO_MEM;

    cJSON_AddStringToObject(json, "id_device", d1->id_device ? d1->id_device : "");
    cJSON_AddNumberToObject(json, "temperature", d1->temperature);
    cJSON_AddNumberToObject(json, "press", d1->press);
    cJSON_AddNumberToObject(json, "hum", d1->hum);
    cJSON_AddNumberToObject(json, "co", d1->co);
    cJSON_AddNumberToObject(json, "rms", d1->rms);
    cJSON_AddNumberToObject(json, "amp_x", d1->amp_x);
    cJSON_AddNumberToObject(json, "freq_x", d1->freq_x);
    cJSON_AddNumberToObject(json, "amp_y", d1->amp_y);
    cJSON_AddNumberToObject(json, "freq_y", d1->freq_y);
    cJSON_AddNumberToObject(json, "amp_z", d1->amp_z);
    cJSON_AddNumberToObject(json, "freq_z", d1->freq_z);
    cJSON_AddNumberToObject(json, "mag_x", d1->mag_x);
    cJSON_AddNumberToObject(json, "mag_y", d1->mag_y);
    cJSON_AddNumberToObject(json, "mag_z", d1->mag_z);
    cJSON_AddNumberToObject(json, "config_version_applied", d1->config_version_applied);

    esp_err_t ret = append_ndjson_line(f, json);
    cJSON_Delete(json);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "NDJSON write OK: type=0x01 path=%s", data1_path);
    }
    return ret;
}

static esp_err_t data2_to_ndjson(Data2 *d2, FILE *f) {
    if (!d2 || !f) return ESP_ERR_INVALID_ARG;

    cJSON *json = cJSON_CreateObject();
    if (!json) return ESP_ERR_NO_MEM;

    cJSON_AddStringToObject(json, "id_device", d2->id_device ? d2->id_device : "");
    cJSON_AddNumberToObject(json, "acc_x", d2->acc_x);
    cJSON_AddNumberToObject(json, "acc_y", d2->acc_y);
    cJSON_AddNumberToObject(json, "acc_z", d2->acc_z);
    cJSON_AddNumberToObject(json, "gyr_x", d2->gyr_x);
    cJSON_AddNumberToObject(json, "gyr_y", d2->gyr_y);
    cJSON_AddNumberToObject(json, "gyr_z", d2->gyr_z);
    cJSON_AddNumberToObject(json, "time_client", (double)d2->time_client);
    cJSON_AddNumberToObject(json, "config_version_applied", d2->config_version_applied);

    esp_err_t ret = append_ndjson_line(f, json);
    cJSON_Delete(json);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "NDJSON write OK: type=0x02 path=%s", data2_path);
    }
    return ret;
}

/* Recibe paquete de forma [tipo|payload protobuf] y lo guarda en NDJSON. */
esp_err_t data_to_sd(uint8_t *data, size_t size) {
    if (!data || size < 2) return ESP_ERR_INVALID_ARG;

    if (!is_sd_mounted()) {
        ESP_LOGI(TAG, "SD not mounted. Not writing.");
        return ESP_OK;
    }

    esp_err_t ret = format_sd_if_no_space();
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t type = data[0];
    const uint8_t *payload = data + 1;
    size_t payload_size = size - 1;

    if (type == 0x01) {
        FILE *f = open_append(data1_path);
        if (!f) return ESP_FAIL;

        Data1 *msg = data_1__unpack(NULL, payload_size, payload);
        if (!msg) {
            fclose(f);
            return ESP_FAIL;
        }

        ret = data1_to_ndjson(msg, f);
        data_1__free_unpacked(msg, NULL);
        fclose(f);
        return ret;
    }

    if (type == 0x02) {
        FILE *f = open_append(data2_path);
        if (!f) return ESP_FAIL;

        Data2 *msg = data_2__unpack(NULL, payload_size, payload);
        if (!msg) {
            fclose(f);
            return ESP_FAIL;
        }

        ret = data2_to_ndjson(msg, f);
        data_2__free_unpacked(msg, NULL);
        fclose(f);
        return ret;
    }

    return ESP_ERR_NOT_SUPPORTED;
}