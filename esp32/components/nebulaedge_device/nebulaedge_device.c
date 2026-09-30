#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
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

/* RTC_DATA_ATTR: única memoria que sobrevive al deep sleep.
 *
 * En MICROSEGUNDOS a propósito. Guardarla en segundos costaba un truncamiento
 * por ciclo, y como truncar siempre redondea hacia abajo el error no se
 * compensa nunca: se acumula. Medido en banco con ciclos de ~37 s, el reloj
 * perdía ~1 s por ciclo y llegó a 70 s de atraso en tres cuartos de hora. */
RTC_DATA_ATTR static uint64_t s_rtc_unix_us = 0;

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
    s_rtc_unix_us = (uint64_t)unix_time_s * 1000000ULL;

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
    /* gettimeofday() y no time(): este último devuelve segundos enteros, así
     * que tiraba la fracción en curso en cada ciclo. */
    struct timeval tv = {0};
    gettimeofday(&tv, NULL);
    uint64_t now_us = (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;

    s_rtc_unix_us = now_us + sleep_us;

    ESP_LOGI(TAG, "Hora guardada para restaurar tras deep sleep: %llu us",
             (unsigned long long)s_rtc_unix_us);
}

void device_clock_restore_after_deep_sleep(void) {
    if (s_rtc_unix_us == 0) {
        ESP_LOGW(TAG, "No hay hora RTC guardada para restaurar tras deep sleep");
        return;
    }

    /* s_rtc_unix_us es la hora que iba a ser al TERMINAR el sueño, no la de
     * ahora: entre que el chip despierta y llega acá ya corrió el arranque
     * (bootloader, init de flash, NVS). esp_timer arranca de cero al salir del
     * deep sleep, así que esp_timer_get_time() mide justo ese pedazo.
     *
     * Va todo en microsegundos y NO pasa por device_clock_set(), que toma
     * segundos y pone tv_usec en 0: ese redondeo era otra fracción perdida por
     * ciclo, siempre hacia abajo. */
    uint64_t now_us = s_rtc_unix_us + (uint64_t)esp_timer_get_time();

    struct timeval tv = {
        .tv_sec  = (time_t)(now_us / 1000000ULL),
        .tv_usec = (suseconds_t)(now_us % 1000000ULL),
    };

    if (settimeofday(&tv, NULL) != 0) {
        ESP_LOGW(TAG, "No se pudo restaurar la hora tras deep sleep");
        return;
    }

    s_rtc_unix_us = now_us;
    ESP_LOGI(TAG, "Hora restaurada tras deep sleep: %lld", (long long)tv.tv_sec);
}
