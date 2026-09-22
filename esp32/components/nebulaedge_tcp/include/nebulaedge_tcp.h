#ifndef NEBULAEDGE_TCP
#define NEBULAEDGE_TCP

#include "esp_err.h"
#include "nebulaedge_defs.h"

void tcp_open_socket(tcp_params_t *params);
/* OJO con el nombre: NO se puede llamar `tcp_connect`. lwIP, la pila TCP/IP
 * de ESP-IDF, ya exporta un `tcp_connect` global (su API raw) y el enlace
 * falla con simbolo duplicado. El prefijo del componente es obligatorio. */
int nebulaedge_tcp_connect(void);
/* Manda un mensaje con su prefijo de largo. ESP_OK si salió entero;
 * ESP_ERR_INVALID_STATE si el socket está cerrado, ESP_ERR_INVALID_SIZE si el
 * largo está fuera de rango, ESP_ERR_NO_MEM si no se pudo armar el frame,
 * ESP_FAIL si send() cortó a medias. El `data` es const, como en UDP: antes
 * era uint8_t* y obligaba a un cast en cada llamada. */
esp_err_t tcp_send(const uint8_t *data, size_t len);
size_t tcp_receive(uint8_t *buffer, size_t len);
void tcp_close_socket(void);

#endif