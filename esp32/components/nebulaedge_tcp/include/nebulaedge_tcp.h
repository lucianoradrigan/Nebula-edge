#ifndef NEBULAEDGE_TCP
#define NEBULAEDGE_TCP

#include "nebulaedge_defs.h"

void tcp_open_socket(tcp_params_t *params);
/* OJO con el nombre: NO se puede llamar `tcp_connect`. lwIP, la pila TCP/IP
 * de ESP-IDF, ya exporta un `tcp_connect` global (su API raw) y el enlace
 * falla con simbolo duplicado. El prefijo del componente es obligatorio. */
int nebulaedge_tcp_connect(void);
void tcp_send(uint8_t *data, size_t len);
size_t tcp_receive(uint8_t *buffer, size_t len);
void tcp_close_socket(void);

#endif