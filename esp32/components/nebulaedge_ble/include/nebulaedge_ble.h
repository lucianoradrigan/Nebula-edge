#ifndef NEBULAEDGE_BLE
#define NEBULAEDGE_BLE

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

void ble_init(void);
void ble_deinit(void);

size_t get_char(uint8_t char_index, uint8_t *out_buffer, size_t max_len);
esp_err_t set_char(uint8_t char_index, const uint8_t *value, uint16_t length);
esp_err_t set_char_with_notify(uint8_t char_index, const uint8_t *value, uint16_t length);
esp_err_t ble_stop_advertising(void);

#endif