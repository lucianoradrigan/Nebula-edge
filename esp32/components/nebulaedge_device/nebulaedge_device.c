#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nebulaedge_device.h"

static const char *TAG = "device";

/* ---------------------------------------------------------------- IDENTIDAD */

static char s_device_id[18] = "00:00:00:00:00:00";

void device_id_init(void) {
    uint8_t mac[6] = {0};

    esp_err_t ret = esp_read_mac(mac, ESP_MAC_BT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo leer MAC BT: %s", esp_err_to_name(ret));
        return;
    }

    snprintf(s_device_id, sizeof(s_device_id), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "ID del device detectado: %s", s_device_id);
}

char *device_id(void) {
    return s_device_id;
}

/* -------------------------------------------------------------------- RELOJ */

/* RTC_DATA_ATTR: única memoria que sobrevive al deep sleep. */
RTC_DATA_ATTR static uint32_t s_rtc_unix_time_s = 0;

void device_clock_set(int64_t unix_time_s) {
    if (unix_time_s <= 0) {
        ESP_LOGW(TAG, "time_client inválido: %lld", (long long)unix_time_s);
        return;
    }

    struct timeval tv = {
        .tv_sec = (time_t)unix_time_s,
        .tv_usec = 0,
    };

    if (settimeofday(&tv, NULL) != 0) {
        ESP_LOGW(TAG, "No se pudo ajustar la hora del sistema a %lld", (long long)unix_time_s);
        return;
    }

    /* La copia RTC se actualiza acá adentro y no desde afuera: así la
     * invariante "la memoria RTC refleja la última hora conocida" se sostiene
     * en un solo lugar. Antes el caller de la config inicial tenía que
     * acordarse de escribirla a mano después de poner el reloj. */
    s_rtc_unix_time_s = (uint32_t)unix_time_s;

    time_t now = 0;
    time(&now);
    ESP_LOGI(TAG, "Hora del sistema ajustada a %lld", (long long)now);
}

uint32_t device_clock_now_s(void) {
    time_t now_s = 0;
    time(&now_s);
    return now_s > 0 ? (uint32_t)now_s : 0;
}

void device_clock_save_before_deep_sleep(uint64_t sleep_us) {
    time_t now = 0;
    time(&now);

    // Redondea microsegundos a segundos y calcula epoch esperado al despertar.
    uint32_t sleep_s = (uint32_t)((sleep_us + 999999ULL) / 1000000ULL);
    s_rtc_unix_time_s = (uint32_t)((uint64_t)now + sleep_s);

    ESP_LOGI(TAG, "Hora guardada para restaurar tras deep sleep: %lu",
             (unsigned long)s_rtc_unix_time_s);
}

void device_clock_restore_after_deep_sleep(void) {
    if (s_rtc_unix_time_s == 0) {
        ESP_LOGW(TAG, "No hay hora RTC guardada para restaurar tras deep sleep");
        return;
    }

    device_clock_set(s_rtc_unix_time_s);
}
