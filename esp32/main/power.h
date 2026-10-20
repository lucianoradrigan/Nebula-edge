#ifndef NEBULAEDGE_POWER_H
#define NEBULAEDGE_POWER_H

#include <stdint.h>

/* Paquetes mandados en la ventana despierta actual. Lo sube
 * deep_sleep_if_needed() y lo ponen en 0 quien aplica una config nueva y
 * app_main al arrancar. */
extern uint32_t data_window_count;

/* Si la ventana despierta se completó, avisa al servidor, apaga todo y duerme
 * sleep_time_s. Si no, solo cuenta el paquete. La llama la task de envío. */
void deep_sleep_if_needed(void);

#endif
