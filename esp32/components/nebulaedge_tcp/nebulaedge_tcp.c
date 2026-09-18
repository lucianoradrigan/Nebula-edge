#include "sdkconfig.h"
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/socket.h>
#include <errno.h>
#include <netdb.h>            // struct addrinfo
#include <arpa/inet.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nebulaedge_tcp.h"
#include "nebulaedge_defs.h"

static const char *TAG = "nebulaedge_tcp";

/* FRAMING: TCP NO CONSERVA LOS LÍMITES DE MENSAJE
 *
 * TCP es un stream de bytes, no de paquetes. Dos paquetes enviados seguidos
 * pueden llegar juntos en un solo recv() del otro extremo, y uno grande puede
 * llegar partido en dos. Sin marcar dónde termina cada mensaje, el servidor
 * le pasa dos protobuf pegados (o medio) al parser y los descarta: se pierden
 * ambos.
 *
 * Por eso cada mensaje viaja como [largo: 2 bytes big-endian][payload].
 * El servidor hace exactamente lo mismo en TcpTransport
 * (raspberry/server/transport.py). Si el formato cambia acá, hay que
 * cambiarlo allá.
 *
 * UDP no necesita nada de esto: el datagrama llega entero o no llega. */
#define TCP_LENGTH_PREFIX_BYTES 2
#define TCP_MAX_FRAME_BYTES     4096

/* El descriptor de socket es definido globalmente en este script. */
static int sock = -1;

/* Variable global para guardar información del destinatario.
 * esta se configura al momento de abrir el socket. */
static struct sockaddr *dest_addr;

/* Largo real de la estructura apuntada por dest_addr. Hace falta para
 * connect(): sizeof(*dest_addr) devuelve el de `struct sockaddr` genérico,
 * que solo por casualidad coincide con el de sockaddr_in (16 bytes). */
static socklen_t dest_addr_len = 0;

/* Dos tasks escriben en el mismo socket: vTaskSendTCP (telemetría) y
 * vTaskGetResponseTCP (ACK de config), esta última con más prioridad.
 * Con framing, una escritura interrumpida a la mitad por la otra no corrompe
 * un paquete sino que desincroniza el stream completo, porque el servidor
 * leería el cuerpo de un mensaje como si fuera el header del siguiente.
 * El mutex serializa el envío del frame entero. */
static SemaphoreHandle_t tcp_send_mutex = NULL;

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
        dest_addr_len = sizeof(struct sockaddr_in);

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
        dest_addr_len = sizeof(struct sockaddr_in6);

        // Asigna parámetros del socket
        addr_family = AF_INET6;
        ip_protocol = IPPROTO_TCP;
    }

    // El mutex de envío vive lo que el proceso; se crea en la primera apertura.
    if (tcp_send_mutex == NULL) {
        tcp_send_mutex = xSemaphoreCreateMutex();
        if (tcp_send_mutex == NULL) {
            ESP_LOGE(TAG, "No se pudo crear el mutex de envío TCP");
        }
    }

    // Abre socket
    sock = socket(addr_family, SOCK_STREAM, ip_protocol);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d (%s)", errno, strerror(errno));
        return;
    }

    int opt = 1;

    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        ESP_LOGW(TAG, "Failed to set SO_REUSEADDR: errno %d (%s)", errno, strerror(errno));
    }

    ESP_LOGI(TAG, "Socket created, connecting to %s:%d", params->ip_host, params->port);
}

/* Realiza conexión TCP con el host preconfigurado. Se debe haber abierto
 * el socket previamente. Retorna el número de socket */
int nebulaedge_tcp_connect(void) {
    // Conecta al server
    ESP_LOGI(TAG, "socket is %d\n", sock);

    int err = connect(sock, dest_addr, dest_addr_len);
    if (err != 0) {
        ESP_LOGE(TAG, "Socket unable to connect: errno %d (%s)", errno, strerror(errno));
        return err;
    }
    ESP_LOGI(TAG, "Successful TCP connection to host");
    return err;
}

/* Envía un array de bytes de 8 bits (uint8_t *) a la dirección IP
 * de destino configurada al abrir el socket. Se tiene que especificar
 * el largo de bytes a enviar.
 *
 * Antepone el prefijo de largo (ver el bloque FRAMING arriba) para que el
 * servidor pueda separar un mensaje del siguiente. */

// Esto aún no maneja el caso de conexión caída!! Se queda caída
// hasta reiniciar
void tcp_send(uint8_t *data, size_t len) {
    if (sock < 0) {
        ESP_LOGE(TAG, "Socket is closed");
        return;
    }

    if (len == 0 || len > TCP_MAX_FRAME_BYTES) {
        ESP_LOGE(TAG, "Largo de paquete fuera de rango: %u B", (unsigned)len);
        return;
    }

    /* Header y payload en un solo buffer. Con dos send() separados, una
     * escritura parcial dejaría el header sin su mensaje detrás y el servidor
     * leería el paquete siguiente como si fuera la cola de este. */
    size_t frame_len = TCP_LENGTH_PREFIX_BYTES + len;
    uint8_t *frame = malloc(frame_len);
    if (frame == NULL) {
        ESP_LOGE(TAG, "Sin memoria para el frame TCP (%u B)", (unsigned)frame_len);
        return;
    }
    frame[0] = (uint8_t)((len >> 8) & 0xFF);    // big-endian (orden de red)
    frame[1] = (uint8_t)(len & 0xFF);
    memcpy(frame + TCP_LENGTH_PREFIX_BYTES, data, len);

    if (tcp_send_mutex != NULL) {
        xSemaphoreTake(tcp_send_mutex, portMAX_DELAY);
    }

    /* send() puede entregar menos bytes de los pedidos: hay que insistir hasta
     * vaciar el frame, o el mensaje queda cortado en el cable. */
    size_t sent = 0;
    bool failed = false;
    while (sent < frame_len) {
        int n = send(sock, frame + sent, frame_len - sent, 0);
        if (n < 0) {
            ESP_LOGE(TAG, "Error occurred during sending errno %d (%s)", errno, strerror(errno));
            failed = true;
            break;
        }
        sent += (size_t)n;
    }

    if (tcp_send_mutex != NULL) {
        xSemaphoreGive(tcp_send_mutex);
    }

    free(frame);

    if (!failed) {
        ESP_LOGI(TAG, "TCP packet sent (%u B de payload)", (unsigned)len);
    }
}

/* Lee exactamente `n` bytes del socket. Retorna true si los completó.
 *
 * recv() puede devolver menos de lo pedido: un mensaje puede llegar repartido
 * en varios segmentos TCP. Sin este bucle, el resto del mensaje se leería como
 * si fuera el comienzo del siguiente. */
static bool recv_exact(uint8_t *buffer, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(sock, buffer + got, n - got, 0);
        if (r < 0) {
            ESP_LOGE(TAG, "recv failed: errno %d (%s)", errno, strerror(errno));
            return false;
        }
        if (r == 0) {
            ESP_LOGW(TAG, "socket closed by peer");
            return false;
        }
        got += (size_t)r;
    }
    return true;
}

/* Consume y descarta `n` bytes del socket. Sirve para no dejar el stream
 * desincronizado cuando el mensaje anunciado no cabe en el buffer del caller. */
static bool recv_discard(size_t n) {
    uint8_t scratch[64];
    while (n > 0) {
        size_t chunk = (n < sizeof(scratch)) ? n : sizeof(scratch);
        if (!recv_exact(scratch, chunk)) {
            return false;
        }
        n -= chunk;
    }
    return true;
}

/* Recibe UN mensaje completo en el buffer entregado como argumento y retorna
 * su largo en bytes, o 0 si la recepción falló.
 *
 * Lee primero el prefijo de largo y después exactamente esa cantidad de bytes
 * (ver el bloque FRAMING arriba), así que el buffer nunca queda con dos
 * mensajes pegados ni con medio mensaje. */
size_t tcp_receive(uint8_t *buffer, size_t len) {
    uint8_t header[TCP_LENGTH_PREFIX_BYTES];

    if (!recv_exact(header, sizeof(header))) {
        return 0;
    }

    size_t frame_len = ((size_t)header[0] << 8) | (size_t)header[1];

    if (frame_len == 0 || frame_len > TCP_MAX_FRAME_BYTES) {
        ESP_LOGE(TAG, "Largo anunciado fuera de rango (%u B): stream desincronizado",
                 (unsigned)frame_len);
        return 0;
    }

    if (frame_len > len) {
        /* No cabe, pero igual hay que sacarlo del socket: si se dejara ahí, el
         * siguiente tcp_receive() leería el cuerpo de este mensaje creyendo que
         * es un header. */
        ESP_LOGE(TAG, "Mensaje de %u B no cabe en el buffer de %u B, se descarta",
                 (unsigned)frame_len, (unsigned)len);
        recv_discard(frame_len);
        return 0;
    }

    if (!recv_exact(buffer, frame_len)) {
        return 0;
    }

    ESP_LOGI(TAG, "received %u bytes", (unsigned)frame_len);
    return frame_len;
}


/* Cierra socket previamente abierto. No recibe parámetros
 * pues el socket está definido como variable global en este
 * script. */
void tcp_close_socket(void) {
    if (sock != -1) {
        shutdown(sock, SHUT_RDWR);
        close(sock);
        ESP_LOGI(TAG, "Socket closed");
        sock = -1;
    }
    else {
        ESP_LOGW(TAG, "Socket is already closed");
    }
    free(dest_addr);
    dest_addr = NULL;
    dest_addr_len = 0;
}