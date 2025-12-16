#ifndef NEBULAEDGE_UDP
#define NEBULAEDGE_UDP

#include "nebulaedge_defs.h"

void nebulaedge_udp_open_socket(udp_params_t *params);
void nebulaedge_udp_send(const uint8_t *data, size_t len);
size_t nebulaedge_udp_receive(uint8_t *data_recv, size_t len);
void nebulaedge_udp_close_socket(void);

#endif