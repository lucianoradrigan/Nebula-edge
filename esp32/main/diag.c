/* Diagnóstico: el banner de arranque, el motivo del último reinicio y el
 * reporte de memoria libre. Salió de main.c tal cual. */
#include <stdint.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "app.h"
#include "diag.h"

/****************************************************************/
/******************* DIAGNÓSTICO DE ARRANQUE ********************/
/****************************************************************/

/* Cada cuánto se vuelve a informar la memoria libre una vez el sistema está en
 * régimen. Sirve para cazar fugas: si el número baja despacio y no se
 * recupera, algo no se está liberando. Poner 0 apaga el reporte periódico. */
#define MEMORY_LOG_PERIOD_MS 60000

static const char *reset_reason_name(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON:  return "encendido";
        case ESP_RST_SW:       return "esp_restart()";
        case ESP_RST_PANIC:    return "PANIC o excepcion";
        case ESP_RST_INT_WDT:  return "watchdog de interrupciones";
        case ESP_RST_TASK_WDT: return "watchdog de tasks";
        case ESP_RST_WDT:      return "otro watchdog";
        case ESP_RST_DEEPSLEEP: return "salida de deep sleep";
        case ESP_RST_BROWNOUT: return "brownout (caida de tension)";
        case ESP_RST_EXT:      return "reset externo";
        default:               return "desconocida";
    }
}

/* Informa memoria libre en un punto del arranque.
 *
 * LOS TRES NÚMEROS NO SON EL MISMO
 *     libre     suma de todos los huecos del heap interno.
 *     mayor     el hueco contiguo más grande. Este es el que decide si un
 *               malloc grande entra: se puede tener 90 KB libres y que el
 *               mayor hueco sea de 20 KB si el heap está fragmentado.
 *     minimo    el valor más bajo que alcanzó `libre` desde el arranque.
 *               Es la marca de agua: dice cuán cerca se estuvo de quedarse
 *               sin memoria aunque ahora sobre.
 *
 * Se mide solo la RAM interna (MALLOC_CAP_INTERNAL): es la que compite de
 * verdad. Si algún día se agrega PSRAM, conviene informarla aparte. */
void log_memory(const char *stage) {
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    ESP_LOGI(TAG, "MEM [%s] libre=%u B  mayor=%u B  minimo=%u B",
             stage,
             (unsigned)heap_caps_get_free_size(caps),
             (unsigned)heap_caps_get_largest_free_block(caps),
             (unsigned)heap_caps_get_minimum_free_size(caps));
}

static void memory_timer_cb(void *arg) {
    log_memory("regimen");
}

/* Arranca el reporte periódico de memoria.
 *
 * Va por esp_timer y no dentro de una task para no tocar ningún bucle de
 * envío ni de sensores: el callback corre en la task del timer. */
void start_memory_monitor(void) {
    if (MEMORY_LOG_PERIOD_MS == 0) {
        return;
    }
    const esp_timer_create_args_t args = {
        .callback = memory_timer_cb,
        .name = "mem_log",
    };
    esp_timer_handle_t timer = NULL;
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        esp_timer_start_periodic(timer, (uint64_t)MEMORY_LOG_PERIOD_MS * 1000);
    }
}

/* Identifica el build que está corriendo, antes de cualquier otra cosa.
 *
 * POR QUÉ EXISTE
 *     Sin esto no hay forma de saber qué código tiene la placa. La versión
 *     sale de `git describe`, así que el log dice exactamente qué commit se
 *     flasheó: eso es lo que separa "no anda" de "no anda EN ESTE commit".
 *
 * El tamaño de flash que informa es el DETECTADO en el chip, que puede no
 * coincidir con el configurado en sdkconfig. Si el configurado es menor, la
 * tabla de particiones está desaprovechando el chip. */
void log_boot_banner(void) {
    const esp_app_desc_t *app = esp_app_get_description();

    ESP_LOGI(TAG, "================ NebulaEdge ================");
    ESP_LOGI(TAG, "version  %s", app->version);
    ESP_LOGI(TAG, "build    %s %s", app->date, app->time);
    ESP_LOGI(TAG, "ESP-IDF  %s", app->idf_ver);
    ESP_LOGI(TAG, "reinicio por: %s", reset_reason_name(esp_reset_reason()));

    uint32_t flash_bytes = 0;
    if (esp_flash_get_size(NULL, &flash_bytes) == ESP_OK) {
        ESP_LOGI(TAG, "flash detectada %u KB (el build asume %s)",
                 (unsigned)(flash_bytes / 1024), CONFIG_ESPTOOLPY_FLASHSIZE);
    }

    log_memory("arranque");
    ESP_LOGI(TAG, "============================================");
}
