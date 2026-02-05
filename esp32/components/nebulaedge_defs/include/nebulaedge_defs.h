#ifndef NEBULAEDGE_DEFS_H
#define NEBULAEDGE_DEFS_H

#include "esp_wifi.h"

typedef struct {
    size_t size;
    uint8_t *data;
} packet_t;

typedef struct {
    char *ssid;
    char *password;
    wifi_auth_mode_t auth_mode;
    int max_retry;
    int retry_delay_ms;
} global_wifi_config;

// Estructura de configuración global MQTT esto se puede extender muchísimo, por ahora
// implementado así por simpleza. Mirar campos de la estructura esp_mqtt_client_config_t
typedef struct {
    const char *broker;
} mqtt_config_global;

typedef enum {
    IPV4,
    IPV6
} ip_version_t;

typedef struct {
    char *ip_host;            // Dirección IP (IPv4)
    int port;                 // Puerto
    ip_version_t ip_version;  // tipo. IPv4 o IPv6
} udp_params_t;

typedef struct {
    char *ip_host;            // Dirección IP (IPv4)
    int port;                 // Puerto
    ip_version_t ip_version;  // tipo. IPv4 o IPv6
} tcp_params_t;

extern SemaphoreHandle_t semaphore;
extern SemaphoreHandle_t semaphore_ble;


#endif