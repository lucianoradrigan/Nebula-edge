#ifndef NEBULAEDGE_WIFI
#define NEBULAEDGE_WIFI

#include "nebulaedge_defs.h"

void wifi_init_sta(global_wifi_config *global_wifi_config);
bool wifi_check_connection(void);

#endif