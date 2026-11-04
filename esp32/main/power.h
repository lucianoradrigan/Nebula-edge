#ifndef NEBULAEDGE_POWER_H
#define NEBULAEDGE_POWER_H

#include <stdint.h>

/* Avisa al servidor, apaga todo y duerme sleep_time_s. La llama la task de
 * envío cuando la ventana despierta terminó: cada flujo produjo
 * sleep_window_size muestras y la cola quedó vacía (ver VENTANA DESPIERTA en
 * data_path.c). Sin config o sin deep sleep, no hace nada. */
void deep_sleep_if_needed(void);

#endif
