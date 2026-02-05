#ifndef NEBULAEDGE_WIFI
#define NEBULAEDGE_WIFI

#include "nebulaedge_defs.h"

void wifi_init_sta(global_wifi_config *global_wifi_config);
void wifi_start_if_needed(global_wifi_config *global_wifi_config);
void wifi_deinit_sta(void);
bool wifi_check_connection(void);

#endif