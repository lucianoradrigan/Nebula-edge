"""El bucle de sesión de un dispositivo, compartido por los cuatro protocolos.

UTILIDAD PRINCIPAL
    Una vez que el device ya tiene su configuración y está hablando por algún
    protocolo, acá vive todo lo que pasa después:

        recibir paquete -> clasificarlo -> persistir telemetría
                        -> comparar la versión de config contra la BD
                        -> si cambió: empujarla y esperar el ACK

    Ese flujo está escrito UNA sola vez, en `ProtocolSession`. Lo único que
    distingue a MQTT de UDP, TCP o BLE es cómo se mueven los bytes, y eso vive
    detrás de `Transport` (transport.py). Por eso las cuatro sesiones concretas
    son de dos líneas:

        class UDPDeviceSession(ProtocolSession):
            transport_cls = UdpTransport

    BLE es la única que sobreescribe `_make_transport()`, porque su transporte
    necesita contexto extra: el BLEDevice, el adaptador y el control del scanner.

REPARTO INTERNO
    DeviceSession     estado del device (config, repositorio, timeouts) y el
                      heartbeat periódico que escribe en la tabla `log`.
    ProtocolSession   el bucle de arriba, el handshake de ACK con reintentos,
                      y la reapertura del enlace cuando el transporte declara
                      `reopens` (TCP y BLE se caen cuando el device se duerme).

QUÉ DEVUELVE run()
    Una ConfigData cuando la sesión termina porque hay que reconfigurar -el
    caller (protocol_dispatch.py) decide con ella qué sesión abrir- o None cuando
    la sesión se acabó del todo y el device tiene que volver a descubrirse.
"""
from __future__ import annotations
import asyncio
import time
from typing import Callable, Any


from models import BleContext, Timeouts, ConfigData, Log
from codec import DataCodec
from system import utc_epoch_now, log
from config_resolver import ConfigResolver, ConfigDecision
from packet_router import PacketRouter, PacketOutcome
from repository import DatabaseRepository
from transport import (
    Transport, TransportClosed, DeviceWentToSleep,
    UdpTransport, TcpTransport, MqttTransport, BleTransport,
)


class DeviceSession:
    """Clase base para sesiones de protocolo (MQTT/UDP/TCP/BLE)."""
    def __init__(
        self,
        device_id: str,                     # ID del device: cómo lo llama la base
        initial_config: ConfigData,         # Configuración inicial del dispositivo
        database_repo: DatabaseRepository,  # Repositorio general de base de datos
        timeouts: Timeouts | None = None,   # Timeouts centralizados
        ble: BleContext | None = None,      # Solo lo usa BLEDeviceSession; ver models.py
    ):
        """Inicializa contexto de dispositivo y repositorios.

        La firma tiene cinco parámetros y no diez porque todo lo específico de
        BLE -el BLEDevice, el adaptador, los callbacks del scanner y la
        conexión heredada del descubrimiento- viaja junto en `ble`. MQTT, UDP
        y TCP lo reciben como None y no lo miran.
        """
        self.device_id = device_id                      # ID del dispositivo
        self.database_repo = database_repo              # Repositorio compartido para config y telemetría
        self.config = initial_config                    # Configuración actual en memoria del device
        self.timeouts = Timeouts() if timeouts is None else timeouts  # Timeouts centralizados
        self.ble = ble                                  # Contexto BLE, o None en los otros tres
        self._router = PacketRouter(database_repo)      # Decodifica + persiste paquetes de telemetría
        self._last_client_time: int | None = None       # Último time_client recibido (Data_1/Data_2)
        self._last_client_seen_at: float | None = None  # Cuándo llegó, en reloj monótono del server

    def _update_last_client_time(self, data: Any):
        """Actualiza el último timestamp de cliente observado en paquetes de datos.

        Guarda también CUÁNDO llegó, con el reloj monótono del server. Hace
        falta para saber si el dato sigue fresco sin usar el reloj del device:
        ese se atrasa con cada deep sleep, así que restarlo contra el del
        server no dice nada sobre la antigüedad del paquete.
        """
        ts = getattr(data, "time_client", None)
        if isinstance(ts, int):
            self._last_client_time = ts
            self._last_client_seen_at = time.monotonic()

    async def _protocol_heartbeat_loop(self, interval_sec: float = 10.0):
        """Inserta un log periódico mientras el protocolo de sesión está activo.

        `time_client` sale solo si hay telemetría reciente. Antes se escribía
        siempre el último valor visto, así que mientras el device dormía se
        repetía la misma hora en varias filas con `time_server` distinto: la
        resta entre ambas columnas parecía desfase de reloj cuando en realidad
        era la antigüedad del último paquete. Con NULL, esa resta o es una
        comparación de relojes válida o no existe, pero nunca es un número
        equivocado.
        """
        while True:
            await asyncio.sleep(interval_sec)
            if self._last_client_time is None:
                continue
            server_time = utc_epoch_now()
            seen_at = self._last_client_seen_at
            fresh = seen_at is not None and (time.monotonic() - seen_at) <= interval_sec
            try:
                await self.database_repo.insert_log_async(
                    Log(
                        id_device=self.device_id,
                        status_report=2,
                        protocol_report=self.config.protocol_conf,
                        batt_level=100,
                        time_client=self._last_client_time if fresh else None,
                        time_server=server_time,
                    )
                )
            except Exception as e:
                log(f"No se pudo insertar heartbeat para {self.device_id}: {e}")

    def _sleep_timeout_sec(self) -> float:
        """Calcula timeout de recepción según `send_interval_s` (s) y `sleep_time_s` (s)."""
        send_interval_s = max(0, self.config.send_interval_s or 0)
        discontinuous_sleep_s = max(0, self.config.sleep_time_s or 0)
        send_interval = float(send_interval_s)
        discontinuous_sleep = float(discontinuous_sleep_s)
        base = max(send_interval, discontinuous_sleep)

        if base > 0:
            grace = max(self.timeouts.no_data_grace_sec, 1.2 * base) # default 2 * base
            return base + grace
        return self.timeouts.no_data_grace_sec

    # Último reenvío de hora, por device y NO por sesión.
    #
    # POR QUÉ NO ES UN ATRIBUTO DE INSTANCIA
    #     En deep sleep, TCP y BLE cierran el enlace al dormirse y la sesión se
    #     construye de nuevo en cada despertar (Transport.reopens). Un contador
    #     por sesión se reiniciaría en cada ciclo y la resincronización no
    #     llegaría nunca justo en el modo donde más falta hace, que es el que
    #     acumula error de reloj al dormir.
    _clock_resync_at: dict[str, float] = {}

    async def _resync_device_clock(self, tx: Transport, db_config: "ConfigData") -> None:
        """Reenvía la config VIGENTE, sin cambiarle la versión, para poner el
        reloj del device en hora.

        POR QUÉ HACE FALTA
            La ESP32 no tiene RTC con batería: su hora entra en el campo
            `time_client` de la config y después corre sola. Y corre mal: el
            atraso medido en banco es de ~0,75 s por minuto, unos 45 s en una
            hora. La única hora confiable que el device puede ver es la que le
            mandamos nosotros.

        POR QUÉ REENVIAR LA MISMA VERSIÓN Y NO INVENTAR UNA NUEVA
            `get_config()` estampa `time_client` con la hora del momento en
            cada config que arma, así que el mismo mensaje que ya existe sirve
            de reloj sin tocar nada del protocolo. Subirle la versión para
            forzar una reaplicación ensuciaría el espacio de versiones y haría
            que el device rearme tasks y transporte por nada.

            Del lado del firmware, la hora se aplica ANTES de comparar
            versiones justamente para que una config repetida sirva de
            sincronización; ver vTaskGetResponse() en main.c.

        RECIBE LA CONFIG DE LA BASE, NO self.config
            `self.config` es la que quedó fija al abrir la sesión, con la hora
            de ese momento: reenviarla sincronizaría el reloj a un instante ya
            pasado. La que llega por parámetro sale de get_config_async() en la
            misma vuelta del bucle, así que su `time_client` es de ahora.

        NO ESPERA EL ACK
            El device contesta un ACK que acá no le sirve a nadie: llega al
            bucle de datos y el router lo descarta como IGNORED. Esperarlo
            bloquearía la recepción de telemetría por una tarea accesoria.
        """
        try:
            await tx.send(DataCodec.serialize_config(db_config))
        except Exception as e:
            # Que falle no es grave: el device sigue con su hora vieja y se
            # reintenta en la próxima vuelta.
            log(f"{tx.name}: no se pudo reenviar la hora a {self.device_id}: {e}")
            return
        log(f"{tx.name}: hora reenviada a {self.device_id} (config v{db_config.config_version})")

    async def _proactive_config_push(self, push_and_wait: Callable[["ConfigData"], Any]) -> "ConfigData | None":
        """Consulta la BD sin haber recibido un paquete y empuja la config si cambió.

        Se llama entre reintentos de espera (cada `config_poll_sec`) para no depender
        de que llegue telemetría para detectar un cambio de config. `push_and_wait(db_config)`
        debe enviar la config por el canal correspondiente y esperar su ACK, retornando
        True si se confirmó.
        """
        db_config = await self.database_repo.get_config_async(self.device_id)
        if db_config is None or db_config.config_version <= self.config.config_version:
            return None
        applied = await push_and_wait(db_config)
        return db_config if applied else None

class ProtocolSession(DeviceSession):
    """Sesión genérica: el flujo es idéntico para todos los protocolos.

    Lo único que cambia entre uno y otro es cómo se mueven los bytes, y eso
    vive en un `Transport` (transport.py). Una sesión concreta solo declara
    cuál usar:

        class UDPDeviceSession(ProtocolSession):
            transport_cls = UdpTransport

    Los cuatro protocolos (MQTT/UDP/TCP/BLE) corren por acá; BLE además
    sobreescribe `_make_transport()` porque necesita más contexto (el
    BLEDevice, el adaptador, los callbacks del scanner).
    """

    transport_cls: type[Transport]

    def _make_transport(self) -> Transport:
        """Construye el transporte de esta sesión.

        La mayoría solo necesita la config y cuánto esperar a que el device
        aparezca. BLE necesita más contexto (el BLEDevice, el adaptador, los
        callbacks del scanner) y sobreescribe esto.
        """
        return self.transport_cls(self.config, self._sleep_timeout_sec())

    async def _wait_ack(self, tx: Transport, db_config: "ConfigData") -> bool:
        """Manda la config y espera su ACK, reenviándola en cada reintento.

        Antes la config se mandaba UNA sola vez, desde _push_and_wait, y las
        `config_ack_retries` vueltas de acá solo volvían a esperar. Si ese
        único mensaje no se procesaba, el server esperaba
        `config_ack_sec * config_ack_retries` sin reintentar nada y cerraba la
        sesión. Y eso pasa seguido: en UDP el datagrama se puede perder sin
        más, y al despertar de deep sleep el device solo escucha config
        mientras no complete su ventana de envío -en cuanto la completa,
        deep_sleep_if_needed() suspende su task de respuesta y cierra el
        socket-, ventana que con send_interval_s=1 y sleep_window_size=10 son
        unos 5 segundos contra los 20 que esperaba el server.

        Mientras espera sigue procesando la telemetría que llegue (no se
        descarta data por estar en medio de un cambio de config).
        """
        ack_window = tx.ack_window_sec or self.timeouts.config_ack_sec

        for attempt in range(1, self.timeouts.config_ack_retries + 1):
            if attempt > 1:
                log(f"{tx.name}: reenviando config v{db_config.config_version} a {self.device_id} (intento {attempt})")
            try:
                await tx.send(DataCodec.serialize_config(db_config))
            except Exception as e:
                log(f"Error enviando config {tx.name}: {e}")
                return False

            deadline = time.monotonic() + ack_window
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break

                pkt = await tx.recv(remaining)
                if pkt is None:
                    break

                # La telemetría y el aviso de deep sleep llevan byte de tipo y
                # nunca son un ACK: pasarlos igual por deserialize_config_ack()
                # solo llenaba el log de "Error al desempaquetar el ACK" por
                # cada paquete que llegaba durante la espera.
                if DataCodec.is_typed_packet(pkt):
                    routed = await self._router.route(pkt, self.device_id, source=tx.name)
                    if routed.outcome == PacketOutcome.TELEMETRY:
                        self._update_last_client_time(routed.data)
                    continue

                ack = DataCodec.deserialize_config_ack(pkt)
                if (
                    ack
                    and ack.id_device == self.device_id
                    and ack.config_version == db_config.config_version
                    and ack.applied
                ):
                    log(f"ACK {tx.name} recibido para {self.device_id} v{db_config.config_version}")
                    return True

            # Se acabó la ventana sin ACK. Algunos transportes (BLE) pueden
            # preguntarle al device si igual la aplicó, por si se perdió el aviso.
            if await tx.confirm_config_applied(self.device_id, db_config.config_version):
                log(f"{tx.name}: {self.device_id} confirma v{db_config.config_version} aplicada (sin ACK directo)")
                return True

        log(f"ACK {tx.name} de config v{db_config.config_version} no recibido para {self.device_id}. Cerrando sesión.")
        return False

    async def _push_and_wait(self, tx: Transport, db_config: "ConfigData") -> bool:
        """Envía una config nueva al device y espera su ACK.

        El envío en sí lo hace _wait_ack, que lo repite en cada reintento.
        """
        log(f"Cambio de protocolo: {self.config.protocol_conf} -> {db_config.protocol_conf} para {self.device_id}")
        return await self._wait_ack(tx, db_config)

    # Entre dos intentos de reabrir el enlace después de un corte. Corto a
    # propósito: cada intento ya cuesta lo suyo (BleTransport.open() deja el
    # scanner apagado mientras conecta), así que esto solo evita el bucle
    # caliente, igual que el asyncio.sleep(0.2) que tenía BLEDeviceSession.
    _REOPEN_RETRY_SEC = 1.0

    # Piso de la pausa tras un aviso de deep sleep, para no reconectar con el
    # device todavía encendido. Cubre lo que le queda por hacer después de
    # avisar: nvs_save_config(), el deinit de los sensores y el
    # vTaskDelay(3000) final de deep_sleep_if_needed() (main.c).
    _DEEP_SLEEP_SHUTDOWN_SEC = 5.0

    async def run(self) -> "ConfigData | None":
        """Abre el transporte y corre la sesión; lo reabre si el enlace se corta.

        Los transportes con conexión (TCP, BLE) se cortan cuando el device se
        duerme o se desconecta: si declaran `reopens`, se vuelve a abrir y se
        sigue esperando al device dentro de la misma sesión, igual que hacía el
        while exterior de TCPDeviceSession. Los sin conexión (UDP, MQTT) nunca
        lanzan TransportClosed.

        Reabrir puede fallar varias veces antes de lograrse, y eso es normal:
        con deep sleep el device avisa y se va, así que el primer intento cae
        mientras todavía está apagando sensores, y los siguientes mientras
        duerme. Por eso se reintenta hasta `_sleep_timeout_sec()`, que ya
        contempla `sleep_time_s`, en vez de cerrar la sesión al primer fallo.
        """
        # None mientras no haya habido un corte: un fallo en la PRIMERA
        # apertura sí cierra la sesión, porque el enlace no se estableció nunca.
        reopen_deadline: float | None = None

        while True:
            tx = self._make_transport()

            if not await tx.open():
                await tx.close()

                if reopen_deadline is None:
                    # No se llegó a establecer (p.ej. ningún device se conectó).
                    return None

                if time.monotonic() >= reopen_deadline:
                    log(f"{tx.name}: {self.device_id} no volvió a aparecer. Cerrando sesión.")
                    return None

                await asyncio.sleep(self._REOPEN_RETRY_SEC)
                continue

            reopen_deadline = None

            try:
                return await self._session_loop(tx)
            except TransportClosed as e:
                if not tx.reopens:
                    log(f"{tx.name}: enlace cortado con {self.device_id} ({e}). Cerrando sesión.")
                    return None
                log(f"{tx.name}: {e}. Reabriendo para esperar al device.")
                pause = self._reopen_pause_sec(e)
                if pause > 0:
                    log(f"{tx.name}: esperando {pause:.0f}s a que {self.device_id} duerma y vuelva.")
                    await asyncio.sleep(pause)
                reopen_deadline = time.monotonic() + self._sleep_timeout_sec()
            finally:
                await tx.close()

    def _reopen_pause_sec(self, closed: TransportClosed) -> float:
        """Cuánto esperar antes del PRIMER intento de reabrir el enlace.

        Solo importa tras un aviso de deep sleep. El device manda el aviso y
        sigue encendido varios segundos más (ver DeviceWentToSleep), así que
        reconectar enseguida se conecta a la instancia que está por
        reiniciarse: la escritura de "start" en char C se pierde con el reset
        y el device queda esperando en su xSemaphoreTake() mientras la sesión
        espera datos que no van a llegar, hasta agotar el timeout completo.

        Se espera lo que el device va a estar ausente -su sleep_time_s-, con
        un piso que cubre el apagado. No se suman los dos: pasarse tampoco
        sirve, porque al volver todavía tarda en levantar el GATT y los
        reintentos normales se encargan de esa parte.
        """
        if not isinstance(closed, DeviceWentToSleep):
            return 0.0
        return max(self._DEEP_SLEEP_SHUTDOWN_SEC, float(self.config.sleep_time_s or 0))

    async def _session_loop(self, tx: Transport) -> "ConfigData | None":
        """Recibe telemetría y aplica cambios de config sobre un transporte ya abierto."""
        while True:
            # Espera un paquete, sondeando la BD proactivamente en ventanas
            # cortas mientras no llega nada (no depende de que llegue
            # telemetría para enterarse de un cambio de config).
            timeout_sec = self._sleep_timeout_sec()
            deadline = time.monotonic() + timeout_sec
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    log(f"Timeout {tx.name} ({timeout_sec}s) sin datos de {self.device_id}. " "Cerrando sesión para permitir reconexión.")
                    return None

                packet = await tx.recv(min(remaining, self.timeouts.config_poll_sec))
                if packet is not None:
                    break

                # Sin paquete: aprovecha de revisar si cambió la config en BD.
                if tx.can_send:
                    new_cfg = await self._proactive_config_push(
                        lambda cfg: self._push_and_wait(tx, cfg)
                    )
                    if new_cfg is not None:
                        return new_cfg

            routed = await self._router.route(packet, self.device_id, source=tx.name)
            if routed.outcome == PacketOutcome.IGNORED:
                continue
            if routed.outcome == PacketOutcome.DEEP_SLEEP:
                if tx.reopens:
                    # El device cierra el enlace al dormirse: hay que reabrirlo.
                    raise DeviceWentToSleep(f"{self.device_id} avisó deep sleep")
                log(f"{tx.name}: Se detectó deep sleep de {self.device_id}, se sigue escuchando")
                continue
            data = routed.data
            self._update_last_client_time(data)

            # Obtiene configuración desde DB
            db_config = await self.database_repo.get_config_async(self.device_id)
            if db_config is None:
                continue

            # Compara versiones actuales de config DEVICE vs versión BD
            applied_version = data.config_version_applied
            decision = ConfigResolver.evaluate(applied_version, db_config, self.config)
            if decision.decision == ConfigDecision.APPLIED_NEWER:
                log(f"Config aplicada detectada en {tx.name} ({applied_version}) para {self.device_id}. " "Cerrando sesión para reconfigurar.")
                return decision.db_config
            elif decision.decision == ConfigDecision.ALREADY_SENT:
                # Ya se envió esta config en el cambio de protocolo; espera que el device la aplique
                continue
            elif decision.decision == ConfigDecision.PUSH:
                applied = await self._push_and_wait(tx, decision.db_config)
                return decision.db_config if applied else None

            # El device está al día de config: es el momento tranquilo para
            # ponerle el reloj en hora. Ver _resync_device_clock().
            if tx.can_send and decision.decision == ConfigDecision.UP_TO_DATE:
                now = time.monotonic()
                last = self._clock_resync_at.get(self.device_id)
                if last is None:
                    # Primera vez que se ve este device en este proceso: solo se
                    # anota el instante, sin mandar nada. El reenvío sirve para
                    # corregir la deriva acumulada, y todavía no hubo tiempo de
                    # acumular ninguna. El costo es que un device que ya venía
                    # corriendo espera un intervalo antes de su primer ajuste.
                    self._clock_resync_at[self.device_id] = now
                elif now - last >= self.timeouts.clock_resync_sec:
                    self._clock_resync_at[self.device_id] = now
                    await self._resync_device_clock(tx, db_config)

class MQTTDeviceSession(ProtocolSession):
    """Sesión MQTT: toda la lógica está en ProtocolSession, solo cambia el transporte."""
    transport_cls = MqttTransport

class UDPDeviceSession(ProtocolSession):
    """Sesión UDP: toda la lógica está en ProtocolSession, solo cambia el transporte."""
    transport_cls = UdpTransport

class TCPDeviceSession(ProtocolSession):
    """Sesión TCP: toda la lógica está en ProtocolSession, solo cambia el transporte."""
    transport_cls = TcpTransport

class BLEDeviceSession(ProtocolSession):
    """Sesión BLE: la lógica está en ProtocolSession; el transporte necesita
    contexto extra (device, adaptador y control del scanner)."""
    transport_cls = BleTransport

    def _make_transport(self) -> Transport:
        # La conexión que trae el descubrimiento sirve una sola vez: si el
        # enlace se cae y la sesión reabre, ese cliente ya no vale y el
        # transporte tiene que conectar por su cuenta.
        client, self.ble.client = self.ble.client, None
        return BleTransport(
            self.config,
            self._sleep_timeout_sec(),
            device=self.ble.device,
            adapter=self.ble.adapter,
            scanner_lock=self.ble.scanner_lock,
            scanner_stop=self.ble.scanner_stop,
            scanner_start=self.ble.scanner_start,
            ack_window_sec=self.timeouts.ble_ack_short_sec,
            client=client,
        )
