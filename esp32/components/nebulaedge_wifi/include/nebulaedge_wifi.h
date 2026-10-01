#ifndef NEBULAEDGE_WIFI
#define NEBULAEDGE_WIFI

#include "esp_wifi.h"

/* Credenciales y política de reintento de la red a la que unirse.
 * Vivía en nebulaedge_defs.h; es parte de la API de este componente y de
 * nadie más. */
typedef struct {
    char *ssid;
    char *password;
    wifi_auth_mode_t auth_mode;
    int max_retry;
    int retry_delay_ms;
} global_wifi_config;

void wifi_start_if_needed(global_wifi_config *global_wifi_config);
void wifi_deinit_sta(void);

#endif