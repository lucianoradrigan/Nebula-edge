#include "sdkconfig.h"
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <errno.h>
#include <netdb.h>            // struct addrinfo
#include <arpa/inet.h>
#include "esp_netif.h"
#include "esp_log.h"
#include "nebulaedge_tcp.h"
#include "nebulaedge_defs.h"

static const char *TAG = "nebulaedge_tcp";

/* El descriptor de socket es definido globalmente en este script. */
static int sock = -1;

/* Variable global para guardar información del destinatario.
 * esta se configura al momento de abrir el socket. */
static struct sockaddr *dest_addr;

/* Función auxiliar que ayuda a buscar una interfaz de red dada su descripción. */
static bool netif_desc_matches_with(esp_netif_t *netif, void *ctx) {
    return strcmp(ctx, esp_netif_get_desc(netif)) == 0;
}

/* Función auxiliar que ayuda a buscar una interfaz de red dada su descripción. */
static esp_netif_t *get_netif_from_desc(const char *desc) {
    return esp_netif_find_if(netif_desc_matches_with, (void*)desc);
}

/* Abre socket TCP IPv4 o IPv6 y lo configura en base a los parámetros entregados. 
 * Luego, se setea la dirección IPv4 o IPv6 del host. Para cambiar de host 
 * hay que cerrar el socket y llamar de nuevo a esta función con los nuevos parámetros. */
void tcp_open_socket(tcp_params_t *params) {
    int addr_family = 0;
    int ip_protocol = 0;

    // Caso selección IPv4
    if (params->ip_version == IPV4) {

        // Pide memoria para guardar información de destinatario
        struct sockaddr_in *dest_addr_ipv4 = malloc(sizeof(struct sockaddr_in));
        if (dest_addr_ipv4 == NULL) {
            ESP_LOGE(TAG, "Failed to allocate memory for IPv4 address");
            return;
        }

        // Configura destinatario
        inet_pton(AF_INET, params->ip_host, &dest_addr_ipv4->sin_addr);
        dest_addr_ipv4->sin_family = AF_INET;
        dest_addr_ipv4->sin_port = htons(params->port);

        // Asigna destinatario a la variable global
        dest_addr = (struct sockaddr *)dest_addr_ipv4;

        // Asigna parámetros del socket
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
        inet_pton(AF_INET6, params->ip_host, &dest_addr_ipv6->sin6_addr);
        dest_addr_ipv6->sin6_family = AF_INET6;
        dest_addr_ipv6->sin6_port = htons(params->port);
        dest_addr_ipv6->sin6_scope_id = (uint32_t)get_netif_from_desc("sta");

        if (dest_addr_ipv6->sin6_scope_id == NETIF_NO_INDEX) {
            ESP_LOGE(TAG, "NETIF_NO_INDEX");
            return;
        }

        // Asigna destinatario a la variable global
        dest_addr = (struct sockaddr *)dest_addr_ipv6;

        // Asigna parámetros del socket
        addr_family = AF_INET6;
        ip_protocol = IPPROTO_TCP;
    }

    // Abre socket
    sock = socket(addr_family, SOCK_STREAM, ip_protocol);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d (%s)", errno, strerror(errno));
        return;
    }
    ESP_LOGI(TAG, "Socket created, connecting to %s:%d", params->ip_host, params->port);
}

/* Realiza conexión TCP con el host preconfigurado. Se debe haber abierto
 * el socket previamente. Retorna el número de socket */
int nebula_tcp_connect(void) {
// Conecta al server
    printf("socket is %d\n", sock);
    int err = connect(sock, dest_addr, sizeof(*dest_addr));
    if (err != 0) {
        ESP_LOGE(TAG, "Socket unable to connect: errno %d (%s)", errno, strerror(errno));
        return err;
    }
    ESP_LOGI(TAG, "Successful TCP connection to host");
    return err;
}

/* Envía un array de bytes de 8 bits (uint8_t *) a la dirección IP
 * de destino configurada al abrir el socket. Se tiene que especificar
 * el largo de bytes a enviar. */

// Esto aún no maneja el caso de conexión caída!! Se queda caída
// hasta reiniciar
void tcp_send(uint8_t *data, size_t len) {
    if (sock < 0) {
        ESP_LOGE(TAG, "Socket is closed");
        return;
    }

    int err = send(sock, data, len, 0);
    if (err < 0) {
        ESP_LOGE(TAG, "Error occurred during sending errno %d (%s)", errno, strerror(errno));
        return;
    }
    ESP_LOGI(TAG, "TCP packet sent");
}

/* Recibe bytes en el buffer entregado como argumento. El tamaño de este
 * buffer de recepción tiene que ser del mismo tamaño que el paquete de
 * bytes enviados, para evitar recortar el arreglo.
 * 
 * Retorna  cuando falla recepción, acompañado de un mensaje de error. */
/* TODO: testear esta función. */
size_t tcp_receive(uint8_t *buffer, size_t len) {

    // Receive TCP
    ssize_t len_recv = recv(sock, buffer, len, 0);

    // Error occurred during receiving
    if (len_recv < 0) {
        ESP_LOGE(TAG, "recv failed: errno %d (%s)", errno, strerror(errno));
        return 0;
    }

    if (len_recv == 0) {
        ESP_LOGW(TAG, "socket closed by peer");
        return 0;
    }

    // Data received
    ESP_LOGI(TAG, "received %d bytes", (int)len_recv);

    return (size_t)len_recv;
}


/* Cierra socket previamente abierto. No recibe parámetros
 * pues el socket está definido como variable global en este
 * script. */
void tcp_close_socket(void) {
    if (sock != -1) {
        shutdown(sock, 0);
        close(sock);
        ESP_LOGI(TAG, "Socket closed");
        sock = -1;
    }
    else {
        ESP_LOGW(TAG, "Socket is already closed");
    }
    free(dest_addr);
    dest_addr = NULL;
}