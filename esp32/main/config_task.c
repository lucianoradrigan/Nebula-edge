/* Recepción de configuraciones: la task de respuesta y lo que usa para
 * validar, comparar y confirmar cada config. Salió de main.c tal cual. */
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nebulaedge_device.h"
#include "nebulaedge_model_store.h"
#include "infer.h"
#include "schema.pb-c.h"
#include "app.h"
#include "protocols.h"
#include "gate.h"
#include "power.h"
#include "config_task.h"

/* Si la tabla de protocolos tiene una entrada para el protocol_conf de `c`.
 * Las configs con un protocolo desconocido se rechazan al llegar, y no al
 * intentar abrir el transporte. */
bool protocol_conf_is_valid(const Config *c) {
    return c->protocol_conf >= 0 && c->protocol_conf < PROTOCOL_COUNT;
}

// Recibe dos configuraciones y retorna un booleano si son diferentes o iguales
bool config_has_changed(Config *old, Config *new) {
    /* Sin config previa, cualquiera es nueva. El caller de más arriba ya
     * pregunta por NULL antes de comparar versiones, así que acá faltaba la
     * misma guarda: sin ella esto desreferencia old y revienta. */
    if (!old) {
        return new != NULL;
    }
    if (!new) {
        return false;
    }

    if (new->config_version > old->config_version) {
        ESP_LOGI(TAG, "Nueva configuración recibida. Versión %ld.", new->config_version);
        return true;
    }

    // Igual o más vieja: no hay nada que aplicar.
    return false;
}

/* Envía el ACK de una configuración recibida, por el protocolo `over`.
 *
 * OJO con por qué `over` es un parámetro en vez de leerse de
 * current_config->protocol_conf: cuando el ACK confirma un cambio DE
 * protocolo, current_config ya es la configuración nueva, pero el transporte
 * que sigue abierto es el viejo. El ACK tiene que salir por el viejo — es lo
 * último que se manda antes de cerrarlo. Cada task de respuesta pasa su propio
 * protocolo, que es justamente el que tiene abierto.
 *
 * Envío redundante: no se espera respuesta, así que se manda varias veces en
 * vez de pedir una confirmación (que agregaría otro flanco de pérdida en vez
 * de reducir el riesgo). Cuántas, lo dice la tabla de cada protocolo. */
static void send_config_ack(protocol_t over, const Config *cfg, bool applied) {
    if (!cfg || !cfg->id_device) return;

    const protocol_ops_t *proto = &PROTOCOLS[over];

    ConfigAck ack = CONFIG_ACK__INIT;
    ack.id_device = cfg->id_device;
    ack.config_version = cfg->config_version;
    ack.applied = applied;
    ack.time_client = device_clock_now_s();
    ack.model_version_active = model_store_active_version();

    size_t size = config_ack__get_packed_size(&ack);
    uint8_t *buf = malloc(size);
    if (!buf) {
        ESP_LOGE(TAG, "No hay memoria para el ACK de config");
        return;
    }
    config_ack__pack(&ack, buf);

    int sent = 0;
    for (int i = 0; i < proto->control_repeats; i++) {
        if (proto->send_ack(cfg, buf, size) == ESP_OK) {
            sent++;
        }
        vTaskDelay(pdMS_TO_TICKS(CONTROL_PKT_REDUNDANCY_DELAY_MS));
    }

    free(buf);

    /* Con que salga una copia alcanza; el servidor descarta las repetidas. Que
     * no salga ninguna sí importa: el servidor va a dar la config por no
     * aplicada y cerrar la sesión. */
    if (sent == 0) {
        ESP_LOGE(TAG, "%s: NINGUNA copia del ACK de config pudo enviarse", proto->name);
    }
    else {
        ESP_LOGI(TAG, "%s: ACK de config enviado (%d/%d copias).",
                 proto->name, sent, proto->control_repeats);
    }
}

/* Task de respuesta: espera configuraciones nuevas y las aplica. UNA sola
 * función, instanciada cuatro veces —una por protocolo— con su tabla por
 * pvParameters.
 *
 * Es la lógica delicada del firmware: comparar versiones, mandar el ACK,
 * reemplazar la config global, detener a todos, cerrar el transporte y avisarle
 * a app_main que cambie de protocolo. Estaba escrita cuatro veces, y las copias
 * ya habían divergido en el orden de las operaciones y en los delays.
 *
 * Lo único propio de cada protocolo es de dónde sale la config y cómo se cierra
 * el transporte: las dos cosas las pone la tabla. */
void vTaskGetResponse(void *pvParameters) {
    const protocol_ops_t *proto = pvParameters;

    for (;;) {
        // Punto seguro: acá no hay config a medio desempaquetar ni memoria viva.
        sensor_gate_wait(GATE_RSP_IDLE(proto->id));

        /* Espera una config, con timeout. Devuelve NULL si no llegó nada o si
         * lo que llegó no servía; cada protocolo ya logueó el motivo. */
        Config *new_config = proto->recv_config();

        /* Por dónde sale el ACK. Es igual al protocolo activo salvo cuando la
         * config entró por el canal de rescate, más abajo. */
        protocol_t ack_over = proto->id;

        /* CANAL DE RESCATE POR BLE
         *
         * ble_init() corre una sola vez en app_main y no se deshace nunca: el
         * anuncio BLE y la característica A quedan activos con cualquier
         * protocolo, así que el servidor puede escribir ahí cuando el
         * transporte principal dejó de responder. Es su única forma de
         * recuperar un device que ya no lo escucha.
         *
         * Lo que faltaba era que alguien vaciara la cola: solo la vaciaban
         * app_main al arrancar y la task de respuesta de BLE. Con UDP, TCP o
         * MQTT activos nadie la tocaba, se llenaba con cinco configs y el resto
         * se descartaba, así que el rescate no llegaba nunca. Medido en banco:
         * "cola BLE llena, descartando config" mientras el servidor esperaba
         * una conexión TCP que el device no iba a abrir.
         *
         * El sondeo va sin espera porque la de esta vuelta ya la hizo el
         * recv_config del protocolo activo. */
        if (!new_config && proto->id != PROTOCOL_BLE) {
            new_config = ble_config_from_queue(0);
            if (new_config) {
                ESP_LOGW(proto->rsp_tag, "Config recibida por BLE (canal de rescate)");
                /* El ACK tiene que volver por BLE: el servidor escribió en la
                 * característica A y está esperando en la D, no en el
                 * transporte activo. Mandarlo por el otro lado lo dejaría
                 * reintentando hasta darla por no aplicada. */
                ack_over = PROTOCOL_BLE;
            }
        }

        if (!new_config) {
            continue;
        }

        /* PONER EL RELOJ EN HORA, ANTES DE MIRAR LA VERSIÓN
         *
         * El servidor estampa `time_client` con la hora del momento en CADA
         * Config que arma (repository.py: time_cli = utc_epoch_now()), así que
         * cualquier config que llegue —nueva, repetida o vieja— trae hora
         * fresca. Antes solo se usaba la de la primera config por BLE, y las
         * demás se descartaban junto con la config: el reloj quedaba corriendo
         * libre desde el arranque, con una deriva medida en banco de ~0,75 s
         * por minuto (unos 45 s de atraso en una hora).
         *
         * Va acá arriba y no más abajo a propósito: si fuera después de las
         * comparaciones de versión, una config repetida —que es justamente el
         * caso de la resincronización periódica del servidor— se descartaría
         * sin que su hora llegara nunca al reloj.
         *
         * La excepción es una config RETENIDA de MQTT, que llega con
         * time_client = 0 (ver mqtt_recv_config): su hora es vieja. */
        if (new_config->time_client > 0) {
            device_clock_set(new_config->time_client);
        }

        /* Config más vieja que la aplicada: se rechaza con un ACK negativo, para
         * que el servidor sepa que llegó y que NO se aplicó. */
        if (current_config && new_config->config_version < current_config->config_version) {
            ESP_LOGW(proto->rsp_tag, "Config antigua: %ld < %ld",
                     (long)new_config->config_version, (long)current_config->config_version);
            send_config_ack(ack_over, new_config, false);
            config__free_unpacked(new_config, NULL);
            continue;
        }

        /* Protocolo fuera de la tabla: también se rechaza, y el device sigue
         * con la config que tiene. Antes se aplicaba, y app_main reiniciaba al
         * no encontrar el protocolo; como el reinicio borra la config de NVS,
         * el servidor volvía a mandar la misma por BLE y el device quedaba en
         * un loop de reinicios. */
        if (!protocol_conf_is_valid(new_config)) {
            ESP_LOGW(proto->rsp_tag, "Config v%ld con protocol_conf inválido (%ld): se rechaza",
                     (long)new_config->config_version, (long)new_config->protocol_conf);
            send_config_ack(ack_over, new_config, false);
            config__free_unpacked(new_config, NULL);
            continue;
        }

        /* Misma versión: se confirma y se descarta. No hay nada que cambiar. */
        if (!config_has_changed(current_config, new_config)) {
            ESP_LOGI(proto->rsp_tag, "La configuración recibida es la misma");
            send_config_ack(ack_over, new_config, true);
            config__free_unpacked(new_config, NULL);
            continue;
        }

        ESP_LOGI(proto->rsp_tag, "Configuración cambiada!");

        /* Antes de tocar current_config: la task de INFER corre en paralelo
         * y no debe seguir trabajando sobre un transporte que se va a cerrar.
         * Ver infer_freeze(). */
        infer_freeze();

        /* Reemplaza la configuración global y reinicia la ventana de deep sleep.
         *
         * La vieja NO se libera acá: la task de envío y las productoras siguen
         * corriendo hasta pause_for_protocol_change(), y leen current_config
         * sin lock (el tópico de MQTT, sleep_time_s, config_version). Si se
         * liberaba antes, podían leer memoria ya devuelta al heap. Se cambia
         * el puntero ahora —la task de envío necesita ver la config nueva para
         * notar el cambio de protocolo— y la vieja se libera después de la
         * pausa, cuando ya nadie la puede estar usando. */
        Config *old_config = current_config;
        current_config = new_config;
        data_window_count = 0;

        /* El ACK sale ANTES de detener nada, y por `proto->id` en vez de por el
         * protocolo de la config nueva: el transporte abierto sigue siendo
         * este, y el ACK es lo último que se manda antes de cerrarlo. */
        send_config_ack(ack_over, current_config, true);

        /* Detiene la task de envío y las productoras, y vacía la cola: los
         * paquetes del protocolo anterior no deben colarse al siguiente. */
        ESP_LOGI(proto->rsp_tag, "Se detienen las tasks del camino de datos");
        pause_for_protocol_change(proto->id);
        if (old_config) config__free_unpacked(old_config, NULL);

        // Le da tiempo al ACK de llegar antes de cerrar. BLE no cierra: 0.
        if (proto->ack_drain_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(proto->ack_drain_ms));
        }

        proto->close();

        // Señala a app_main que debe cambiar de protocolo.
        if (xSemaphoreGive(protocol_change_semaphore)) {
            ESP_LOGI(proto->rsp_tag, "Libera semáforo para cambio de protocolo");
        }

        /* Auto-suspensión hasta que este protocolo vuelva a activarse. Es
         * segura, a diferencia de suspender a otra task: la víctima es esta
         * misma, en un punto donde no tiene nada reservado ni tomado. */
        ESP_LOGI(proto->rsp_tag, "Suspendiendo task de respuesta");
        vTaskSuspend(NULL);
    }
}
