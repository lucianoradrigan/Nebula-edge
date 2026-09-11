#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>
#include "nebulaedge_udp.h"
#include "nebulaedge_defs.h"

static const char *TAG = "nebulaedge_udp";

/* El descriptor de socket es definido globalmente en este script. */
static int sock = -1;

/* Variable global para guardar información del destinatario.
 * esta se configura al momento de abrir el socket. */
static struct sockaddr *dest_addr;

/* Realiza un print de todos los descriptores de red abiertos. */
static void print_all_netif_descriptions(void) {
    esp_netif_t *netif = NULL;
    while ((netif = esp_netif_next_unsafe(netif)) != NULL) {
        ESP_LOGI("netif", "Interface description: %s", esp_netif_get_desc(netif));
    }
}

/* Función auxiliar que ayuda a buscar una interfaz de red dada su descripción. */
static bool netif_desc_matches_with(esp_netif_t *netif, void *ctx) {
    return strcmp(ctx, esp_netif_get_desc(netif)) == 0;
}

/* Función auxiliar que ayuda a buscar una interfaz de red dada su descripción. */
static esp_netif_t *get_netif_from_desc(const char *desc) {
    return esp_netif_find_if(netif_desc_matches_with, (void*)desc);
}

/* Abre socket UDP IPv4 o IPv6 y lo configura en base a los parámetros entregados. 
 * Luego, asigna la dirección IPv4 o IPv6 de destino cambiando la variable global 
 * dest_addr. Para cambiar de destinatario hay que cerrar el socket y llamar de nuevo
 * a esta función con nuevos parámetros. */

 /* SEPARAR RESPONSABILIDADES de ABRIR Y PREPARAR DESTINO (pensarlo igual) */
void nebulaedge_udp_open_socket(udp_params_t *params) {
    int addr_family = 0;
    int ip_protocol = 0;

    // Caso selección IPv4
    if (params->ip_version == IPV4) {

        // Pide memoria para guardar información de destinatario.
        struct sockaddr_in *dest_addr_ipv4 = malloc(sizeof(struct sockaddr_in));
        if (dest_addr_ipv4 == NULL) {
            ESP_LOGE(TAG, "Failed to allocate memory for IPv4 address");
            return;
        }
        
        // Configura destinatario
        dest_addr_ipv4->sin_addr.s_addr = inet_addr(params->ip_host);
        dest_addr_ipv4->sin_family = AF_INET;
        dest_addr_ipv4->sin_port = htons(params->port);

        // Asigna destinatario a la variable global
        dest_addr = (struct sockaddr *)dest_addr_ipv4;

        // Parámetros del socket
        addr_family = AF_INET;
        ip_protocol = IPPROTO_IP;
    }

    // Caso selección IPv6
    else if (params->ip_version == IPV6) {
    
        // Pide memoria para guardar información de destinatario.
        struct sockaddr_in6 *dest_addr_ipv6 = malloc(sizeof(struct sockaddr_in6));
        if (dest_addr_ipv6 == NULL) {
            ESP_LOGE(TAG, "Failed to allocate memory for IPv6 address");
            return;
        }

        // Configura destinatario
        memset(dest_addr_ipv6, 0, sizeof(struct sockaddr_in6));
        inet6_aton(params->ip_host, &dest_addr_ipv6->sin6_addr);
        dest_addr_ipv6->sin6_family = AF_INET6;
        dest_addr_ipv6->sin6_port = htons(params->port);

        // Print de descripciones de socket
        print_all_netif_descriptions();

        // El nombre por default es sta: este se obtiene del componente
        // nebulaedge_wifi
        dest_addr_ipv6->sin6_scope_id = (uint32_t)get_netif_from_desc("sta");
        if (dest_addr_ipv6->sin6_scope_id == NETIF_NO_INDEX) {
            ESP_LOGE(TAG, "NETIF_NO_INDEX");
            return;
        }

        dest_addr = (struct sockaddr *)dest_addr_ipv6;

        addr_family = AF_INET6;
        ip_protocol = IPPROTO_IPV6;
    }

    // ESP32 abre socket
    sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        return;
    }
    ESP_LOGI(TAG, "Socket created successfully. Destiny: %s:%d", params->ip_host, params->port);

    // Set timeout del socket
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
}

/* Envía un array de bytes de 8 bits (uint8_t *) a la dirección IP
 * de destino configurada al abrir el socket. */
void nebulaedge_udp_send(const uint8_t *data, size_t len) {
    if (sock < 0) {
        ESP_LOGE(TAG, "Socket is closed");

        // MANEJAR ESTE CASO
        return;
    }

    // Envía payload a una dirección IP y puerto
    int err = sendto(sock, data, len, 0, dest_addr, sizeof(*dest_addr));
    if (err < 0) {
        ESP_LOGE(TAG, "Error occurred during sending: %s", strerror(errno));
        return;
    }
    ESP_LOGI(TAG, "UDP packet sent");
}

/* Recibe datos del socket preconfigurado. MODO BLOQUEANTE. */
size_t nebulaedge_udp_receive(uint8_t *data_recv, size_t len) {

    // Large enough for both IPv4 or IPv6
    struct sockaddr_storage source_addr;        
    socklen_t socklen = sizeof(source_addr);

    // Espera que lleguen datos al socket
    ESP_LOGI(TAG, "Waiting to receive data");
    int len_recv = recvfrom(sock, data_recv, len, 0, (struct sockaddr *)&source_addr, &socklen);

    // Error occurred during receiving
    if (len_recv < 0) {
        ESP_LOGW(TAG, "recvfrom failed: %s", strerror(errno));
        return 0;
    }

    // Data received
    ESP_LOGI(TAG, "Received %d bytes", len_recv);

    return (size_t)len_recv;
}

/* Cierra socket previamente abierto. No recibe parámetros
 * pues el socket está definido como variable global en este
 * script. */
void nebulaedge_udp_close_socket(void) {
    if (sock != -1) {
        shutdown(sock, SHUT_RDWR);
        close(sock);
        ESP_LOGI(TAG, "Socket closed");
        sock = -1;
    }
    else {
        ESP_LOGE(TAG, "Socket is already closed");
    }
    free(dest_addr);
    dest_addr = NULL;
}