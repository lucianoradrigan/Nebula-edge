#ifndef NEBULAEDGE_UDP
#define NEBULAEDGE_UDP

#include "esp_err.h"
#include "nebulaedge_defs.h"

/* Destino del datagrama. Vivía en nebulaedge_defs.h; es parte de la API de
 * este componente. `ip_version_t` sí se queda en defs: lo comparten UDP y TCP. */
typedef struct {
    char *ip_host;            // Dirección IP (IPv4)
    int port;                 // Puerto
    ip_version_t ip_version;  // tipo. IPv4 o IPv6
} udp_params_t;

void nebulaedge_udp_open_socket(udp_params_t *params);
/* Manda un datagrama. ESP_OK si salió; ESP_ERR_INVALID_STATE si el socket
 * está cerrado, ESP_FAIL si sendto() falló. Devolver el resultado y no void
 * es lo que permite que quien envía sepa que se perdió un paquete. */
esp_err_t nebulaedge_udp_send(const uint8_t *data, size_t len);
size_t nebulaedge_udp_receive(uint8_t *data_recv, size_t len);
void nebulaedge_udp_close_socket(void);

#endif