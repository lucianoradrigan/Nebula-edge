#ifndef NEBULAEDGE_BLE
#define NEBULAEDGE_BLE

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

// Índices de características GATT
enum {
    // Índice de servicio
    IDX_SVC_BLE,

    // Char A
    IDX_CHAR_A_BLE,
    IDX_CHAR_VAL_A_BLE,

    // Char B
    IDX_CHAR_B_BLE,
    IDX_CHAR_VAL_B_BLE,
    IDX_CHAR_CFG_B_BLE,

    // Char C
    IDX_CHAR_C_BLE,
    IDX_CHAR_VAL_C_BLE,

    // Char D
    IDX_CHAR_D_BLE,
    IDX_CHAR_VAL_D_BLE,
    IDX_CHAR_CFG_D_BLE,

    IDX_NB_BLE,
};

/* Lo que el servidor GATT tiene que avisarle a la aplicación.
 *
 * Las dos las entrega la aplicación, que es la dueña; antes este componente
 * alcanzaba dos globales por su nombre (`extern QueueHandle_t xQueueConfigBle`
 * y `extern SemaphoreHandle_t semaphore`), así que copiarlo a otro proyecto
 * obligaba a ese proyecto a declarar globales con esos nombres exactos.
 *
 * Las dos existen porque bluedroid entrega por callback, desde su propia task:
 * ese callback no puede bloquearse esperando a nadie, así que deja el dato o la
 * señal y retorna. Si no se llaman, lo que llegue se descarta con un aviso. */

/* Cola donde dejar lo que el cliente escriba en la característica A (config),
 * como packet_t de bytes crudos. Desempaquetarlo es trabajo de la aplicación. */
void ble_set_config_queue(QueueHandle_t queue);

/* Semáforo que se libera cuando el cliente escribe en la característica C,
 * la señal de "ya estoy listo, arranca". */
void ble_set_start_semaphore(SemaphoreHandle_t sem);

void ble_init(void);
void ble_deinit(void);

size_t ble_get_char(uint8_t char_index, uint8_t *out_buffer, size_t max_len);
esp_err_t ble_set_char(uint8_t char_index, const uint8_t *value, uint16_t length);
esp_err_t ble_set_char_with_notify(uint8_t char_index, const uint8_t *value, uint16_t length);
esp_err_t ble_stop_advertising(void);

#endif