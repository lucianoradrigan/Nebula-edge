#ifndef NEBULAEDGE_DEFS
#define NEBULAEDGE_DEFS

#include "esp_wifi.h"
#include "driver/gpio.h"

/*****************************************************************/
/************************* PROTOCOLOS ****************************/
/*****************************************************************/

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

#define DEEP_SLEEP_FLAG_LEN 3U
extern const uint8_t DEEP_SLEEP_FLAG[DEEP_SLEEP_FLAG_LEN];

extern SemaphoreHandle_t semaphore;
extern SemaphoreHandle_t semaphore_ble;


/*****************************************************************/
/*************************** SENSORES ****************************/
/*****************************************************************/

/* Frecuencia MASTER */
#define I2C_MASTER_FREQ_HZ              100000

/* Pines I2C */
#define I2C_MASTER_SCL_IO				GPIO_NUM_2			    // GPIO pin I2C master GPIO_NUM_47 imv1 GPIO_NUM_2 imv2
#define I2C_MASTER_SDA_IO				GPIO_NUM_42				// GPIO pin I2C master GPIO_NUM_48 imv1 GPIO_NUM_42 imv2

/* Las direcciones I2C de los sensores viven ahora en el header de su driver
 * (BME688_SLAVE_ADDR, BMI270_SLAVE_ADDR, BMM350_SLAVE_ADDR): son propiedad del
 * integrado, no de la placa. */

/* Pines SPI (microsd)*/
#define PIN_NUM_CS                          GPIO_NUM_1         // GPIO pin GPIO_NUM_1 im-v1
#define PIN_NUM_MOSI                        GPIO_NUM_21        // GPIO pin GPIO_NUM_2 im-v1
#define PIN_NUM_CLK                         GPIO_NUM_38        // GPIO pin GPIO_NUM_43 im-v1
#define PIN_NUM_MISO                        GPIO_NUM_47        // GPIO pin GPIO_NUM_44 im-v1

/* SD */
#define FORMAT_IF_MOUNT_FAILED              true

/* Los Output Data Rate de cada sensor viven ahora en el header de su driver
 * (BMI270_ODR_*, BMM350_ODR_*): son propiedad del chip, no de la placa, y
 * tenerlos acá obligaba a cada driver a depender de este archivo. */

/* CONCAT_BYTES vive ahora en bme688.h, su único usuario. */

#endif