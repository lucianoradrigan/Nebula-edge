/* Deep sleep del modo discontinuo. Salió de main.c tal cual. */
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "nebulaedge_defs.h"
#include "nebulaedge_config_store.h"
#include "nebulaedge_device.h"
#include "nebulaedge_i2c.h"
#include "nebulaedge_microsd.h"
#include "bmm350.h"
#include "bme688.h"
#include "bmi270.h"
#include "app.h"
#include "protocols.h"
#include "gate.h"
#include "power.h"

uint32_t data_window_count = 0;     // ver power.h

/* Deep sleep helper para modo discontinuo.
 * Se llama en la función de envío de cada protocolo. */
void deep_sleep_if_needed(void) {

    // Si no existe configuración actual
    if (!current_config) {
        return;
    }

    data_window_count++;

    // Caso en que deep sleep está desactivado
    if (current_config->sleep_time_s <= 0) {
        return;
    }

    int window_size = current_config->sleep_window_size;
    if (window_size <= 0) {
        window_size = 1;
    }

    // Aún no se envía toda la ventana, retorna
    if (data_window_count < (uint32_t)window_size) {
        return;
    }
    data_window_count = 0;

    // Acá se tiene condición de ventana completa, ya se puede entrar en deep sleep
    uint64_t sleep_us = (uint64_t)current_config->sleep_time_s * 1000000ULL;
    
    const protocol_ops_t *proto = &PROTOCOLS[current_config->protocol_conf];

    /* Detiene las productoras y la task de respuesta antes de dormir. La de
     * respuesta hay que pararla porque el aviso de deep sleep cierra el
     * transporte y no tiene sentido que siga esperando config sobre él.
     *
     * A la task de ENVÍO no se la espera: es la que está ejecutando esto. */
    ESP_LOGI(TAG, "Deteniendo tasks antes de dormir");
    pause_for_deep_sleep(proto->id);

    /* Avisa al server que se va a dormir, antes de cerrar. No se espera
     * respuesta, así que se repite lo que diga la tabla: tres veces en los de
     * socket, una sola en BLE — acá eso importa más todavía, porque cada
     * reenvío son milisegundos despierto antes de dormir.
     *
     * Va por send_data() y no por send_ack(): el flag viaja por el mismo canal
     * que la telemetría (tópico /data en MQTT, característica B en BLE), no por
     * el de los ACKs. */
    int flag_sent = 0;
    for (int i = 0; i < proto->control_repeats; i++) {
        if (proto->send_data((const uint8_t *)DEEP_SLEEP_FLAG, DEEP_SLEEP_FLAG_LEN) == ESP_OK) {
            flag_sent++;
        }
        vTaskDelay(pdMS_TO_TICKS(CONTROL_PKT_REDUNDANCY_DELAY_MS));
    }
    if (flag_sent == 0) {
        ESP_LOGW(TAG, "%s: el aviso de deep sleep no salió; el servidor va a ver una caída",
                 proto->name);
    }

    proto->close();

    config_store_save(current_config);
    
    /* Cada driver se saca del bus solo. Tiene que ser ANTES del
     * i2c_master_deinit: borrar el bus invalida los handles de sus slaves. */
    bmm350_deinit();
    bme688_deinit();
    bmi270_deinit();
    /* La SD también, y por el mismo motivo que en el cambio de protocolo: su
     * chip select cuelga del expansor, que a su vez cuelga de este bus. Acá
     * importa además cerrar la FAT antes de cortar la alimentación, para no
     * dejar el sistema de archivos marcado como sucio; los datos ya están en la
     * tarjeta porque sdstorage hace fflush() por línea. */
#if SD_PERSISTENCE
    sd_unmount();
    fxl6408_del(io_expander);
    io_expander = NULL;
#endif
    i2c_master_deinit(&bus_handle);
    
    vTaskDelay(pdMS_TO_TICKS(DEEP_SLEEP_SETTLE_MS));
    
    ESP_LOGI(TAG, "Entrando deep sleep por %lu s", (unsigned long)current_config->sleep_time_s);
    device_clock_save_before_deep_sleep(sleep_us);
    
    esp_sleep_enable_timer_wakeup(sleep_us);
    esp_deep_sleep_start();
}
