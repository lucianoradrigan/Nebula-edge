/* WiFi station connect */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sys.h"

#include "nebulaedge_wifi.h"
#include "nebulaedge_defs.h"

// FreeRTOS event group to signal when we are connected
static EventGroupHandle_t s_wifi_event_group;

// The event group allows multiple bits for each event, but we only care about two events:
// 0. we are connected to the AP with an IP
// 1. we failed to connect after the maximum amount of retries
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static const char *TAG = "nebulaedge_wifi";
static int s_retry_num = 0;

/**
 * @brief Event handler for Wi-Fi and IP events.
 *
 * This function handles events related to Wi-Fi and IP operations. It is 
 * triggered when specific events occur, such as changes in Wi-Fi connection 
 * status or IP address configuration.
 *
 * @param[in] arg        User-defined data passed to the event handler.
 * @param[in] event_base Base identifier for the event (e.g., Wi-Fi or IP events).
 * @param[in] event_id   Identifier for the specific event being handled.
 * @param[in] event_data Pointer to data associated with the event.
 */
static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    
    // Cast the argument to the custom Wi-Fi configuration structure
    global_wifi_config *config = (global_wifi_config *)arg;                 

    // Handle the event when the station starts
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        // Attempt to connect to the configured Wi-Fi network
        esp_wifi_connect();
    } 

    // Handle the event when the station is disconnected
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {

        // Check if the maximum number of retries has not been reached
        if (s_retry_num < config->max_retry) {
            ESP_LOGI(TAG, "Trying to connect to the AP: attempt %d", s_retry_num+1);

            // Attempt to reconnect
            esp_wifi_connect();

            // Log the delay before the next retry
            ESP_LOGI(TAG, "Failed. Retrying in %d milliseconds...", config->retry_delay_ms);
            vTaskDelay(pdMS_TO_TICKS(config->retry_delay_ms));

            // Increment the retry counter
            s_retry_num++;
        }
        else {
            // Set the failure bit in the event group if maximum retries are reached
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    }

    // Handle the event when the station gets an IP address
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {

        // Extract the IP address information from the event data
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;

        // Log the obtained IP address
        ESP_LOGI(TAG, "Got ip:" IPSTR, IP2STR(&event->ip_info.ip));

        // Reset the retry counter
        s_retry_num = 0;

        // Set the connected bit in the event group
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* Función para consultar si el wifi funciona. 
 * Retorna un bool si está conectado. */
bool wifi_check_connection(void) {
    EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
    bool connected = (bits & WIFI_CONNECTED_BIT) != 0;
    ESP_LOGI("nebulaedge_wifi", "wifi_check_connection: %s", connected ? "connected" : "NOT connected");
    return connected;
}

/**
 * @brief Initializes the Wi-Fi station (STA) mode with the provided configuration.
 *
 * This function sets up the Wi-Fi station mode using the given global Wi-Fi configuration.
 * Before calling this function, the following components must be initialized:
 * - Non-Volatile Storage (NVS) by calling `nvs_flash_init()`.
 * - Network interface (netif) by calling `esp_netif_init()`.
 * - Default event loop by calling `esp_event_loop_create_default()`.
 *
 * @param global_wifi_config Pointer to the global Wi-Fi configuration structure.
 */
void wifi_init_sta(global_wifi_config *global_wifi_config) {
    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");

    // Create an event group to handle Wi-Fi connection states
    s_wifi_event_group = xEventGroupCreate();

    // Create the default network interface for station mode
    esp_netif_create_default_wifi_sta();

    // Initial Wi-Fi configuration with default values
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Declaration of instances to handle events
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;

    // Register an event handler for any Wi-Fi event, and one for obtaining an IP
    // A pointer to the custom configuration structure is passed.
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        global_wifi_config,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        global_wifi_config,
                                                        &instance_got_ip));

    printf("pasó la configuración de ssid y pass\n");

    // Wi-Fi configuration
    wifi_config_t wifi_config = {
        .sta = {
        // Authmode threshold resets to WPA2 as default if password matches WPA2 standards (password len => 8).
        // If you want to connect the device to deprecated WEP/WPA networks, Please set the threshold value
        // to WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK and set the password with length and format matching to
        // WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK standards.
        .threshold.authmode = global_wifi_config->auth_mode,
        },
    };

    // Copy the SSID and password values from the custom structure to the Wi-Fi configuration
    strncpy((char *)wifi_config.sta.ssid, global_wifi_config->ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, global_wifi_config->password, sizeof(wifi_config.sta.password));
    

    // Set the Wi-Fi mode to station
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    // Apply the Wi-Fi configuration
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    // Start Wi-Fi
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Wi-Fi initialization finished.");

    // Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
    // number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above)
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    // xEventGroupWaitBits() returns the bits before the call returned, hence 
    // we can test which event actually happened.
    if (bits & WIFI_CONNECTED_BIT) {
        // Conexión exitosa a la red wifi
        ESP_LOGI(TAG, "Connection succesful. SSID:%s password:%s",
                 global_wifi_config->ssid, global_wifi_config->password);
    } 
    else if (bits & WIFI_FAIL_BIT) {
        // Error al conectar a wifi
        ESP_LOGI(TAG, "Failed to connect to SSID: %s, password: %s",
                 global_wifi_config->ssid, global_wifi_config->password);
        ESP_LOGI(TAG, "Restarting...");
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    } 
    else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}