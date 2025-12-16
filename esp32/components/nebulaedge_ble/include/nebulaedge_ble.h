#ifndef NEBULAEDGE_BLE
#define NEBULAEDGE_BLE

void ble_init(void);

size_t get_char_a(uint8_t *out_buffer, size_t max_len);
esp_err_t set_char_b(const uint8_t *value, uint16_t length);
// bool ble_wait_connected(TickType_t ticks_to_wait);
// bool ble_is_connected(void);

#endif