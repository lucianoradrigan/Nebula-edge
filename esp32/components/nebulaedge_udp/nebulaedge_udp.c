#include <string.h>
#include <errno.h>
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

/* Cuánto espera recvfrom() antes de devolver vacío. Tiene que ser corto: es
 * lo que marca cada cuánto la task de respuesta puede atender una pausa. */
#define UDP_RECV_TIMEOUT_MS 250

static const char *TAG = "nebulaedge_udp";

/* El descriptor de socket es definido globalmente en este script. */
static int sock = -1;

/* Destinatario, fijado al abrir el socket. Es solo IPv4: ver
 * nebulaedge_udp_open_socket(). */
static struct sockaddr_in dest_addr;

/* Abre el socket UDP y fija el destinatario. Para cambiar de destinatario hay
 * que cerrar el socket y llamar de nuevo a esta función.
 *
 * SOLO IPv4. Había una rama IPv6 que nunca se usó y que no funcionaba: ponía
 * en sin6_scope_id un puntero a la interfaz en vez de su índice, y sendto()
 * pasaba sizeof(struct sockaddr) (16 B) en vez de los 28 B de sockaddr_in6.
 * Se quitó en vez de arreglarla sin poder probarla. */
void nebulaedge_udp_open_socket(udp_params_t *params) {
    if (params->ip_version != IPV4) {
        ESP_LOGE(TAG, "Solo IPv4 está soportado");
        return;
    }

    memset(&dest_addr, 0, sizeof dest_addr);
    dest_addr.sin_addr.s_addr = inet_addr(params->ip_host);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(params->port);

    // ESP32 abre socket
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        return;
    }
    ESP_LOGI(TAG, "Socket created successfully. Destiny: %s:%d", params->ip_host, params->port);

    /* Timeout de recepción. Estaba en cero, que en POSIX/lwIP significa "sin
     * timeout": el recvfrom() quedaba bloqueado para siempre y la task de
     * respuesta no volvía nunca a mirar la compuerta cooperativa, así que toda
     * pausa que la esperara agotaba su timeout de escape.
     *
     * UDP es de datagramas, así que cortar acá es seguro: no hay mensajes a
     * medio leer como en el stream de TCP. */
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = UDP_RECV_TIMEOUT_MS * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
}

/* Envía un array de bytes de 8 bits (uint8_t *) a la dirección IP
 * de destino configurada al abrir el socket. */
esp_err_t nebulaedge_udp_send(const uint8_t *data, size_t len) {
    if (sock < 0) {
        ESP_LOGE(TAG, "Socket is closed");
        return ESP_ERR_INVALID_STATE;
    }

    // Envía payload a una dirección IP y puerto
    int err = sendto(sock, data, len, 0, (struct sockaddr *)&dest_addr, sizeof dest_addr);
    if (err < 0) {
        ESP_LOGE(TAG, "Error occurred during sending: %s", strerror(errno));
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "UDP packet sent");
    return ESP_OK;
}

/* Recibe datos del socket preconfigurado. MODO BLOQUEANTE. */
size_t nebulaedge_udp_receive(uint8_t *data_recv, size_t len) {

    // Large enough for both IPv4 or IPv6
    struct sockaddr_storage source_addr;        
    socklen_t socklen = sizeof(source_addr);

    // Espera que lleguen datos al socket, hasta UDP_RECV_TIMEOUT_MS.
    int len_recv = recvfrom(sock, data_recv, len, 0, (struct sockaddr *)&source_addr, &socklen);

    // Error occurred during receiving
    if (len_recv < 0) {
        /* El timeout no es un error: es la vía normal para que el caller vuelva
         * a mirar la compuerta. Loguearlo sería una línea cada
         * UDP_RECV_TIMEOUT_MS, o cuatro por segundo. */
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            ESP_LOGW(TAG, "recvfrom failed: %s", strerror(errno));
        }
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
}
