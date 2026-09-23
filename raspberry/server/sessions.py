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
from system import utc_epoch_now
from config_resolver import ConfigResolver, ConfigDecision
from packet_router import PacketRouter, PacketOutcome
from repository import DatabaseRepository
from transport import (
    Transport, TransportClosed,
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
        self._last_client_time: int | None = None       # Último time_client recibido (Environmental/Inertial)

    def _update_last_client_time(self, data: Any):
        """Actualiza el último timestamp de cliente observado en paquetes de datos."""
        ts = getattr(data, "time_client", None)
        if isinstance(ts, int):
            self._last_client_time = ts

    async def _protocol_heartbeat_loop(self, interval_sec: float = 10.0):
        """Inserta un log periódico mientras el protocolo de sesión está activo."""
        while True:
            await asyncio.sleep(interval_sec)
            if self._last_client_time is None:
                continue
            server_time = utc_epoch_now()
            try:
                await self.database_repo.insert_log_async(
                    Log(
                        id_device=self.device_id,
                        status_report=2,
                        protocol_report=self.config.protocol_conf,
                        batt_level=100,
                        time_client=self._last_client_time,
                        time_server=server_time,
                    )
                )
            except Exception as e:
                print(f"No se pudo insertar heartbeat para {self.device_id}: {e}")

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
        """Espera un ACK de config válido. Retorna True si el device la aplicó.

        Mientras espera sigue procesando la telemetría que llegue (no se
        descarta data por estar en medio de un cambio de config).
        """
        ack_window = tx.ack_window_sec or self.timeouts.config_ack_sec

        for _ in range(self.timeouts.config_ack_retries):
            deadline = time.monotonic() + ack_window
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break

                pkt = await tx.recv(remaining)
                if pkt is None:
                    break

                ack = DataCodec.deserialize_config_ack(pkt)
                if (
                    ack
                    and ack.id_device == self.device_id
                    and ack.config_version == db_config.config_version
                    and ack.applied
                ):
                    print(f"ACK {tx.name} recibido para {self.device_id} v{db_config.config_version}")
                    return True

                # No era ACK: si es telemetría se inserta y se sigue esperando.
                routed = await self._router.route(pkt, self.device_id, source=tx.name)
                if routed.outcome != PacketOutcome.TELEMETRY:
                    continue
                self._update_last_client_time(routed.data)

            # Se acabó la ventana sin ACK. Algunos transportes (BLE) pueden
            # preguntarle al device si igual la aplicó, por si se perdió el aviso.
            if await tx.confirm_config_applied(self.device_id, db_config.config_version):
                print(f"{tx.name}: {self.device_id} confirma v{db_config.config_version} aplicada (sin ACK directo)")
                return True

        print(f"ACK {tx.name} de config v{db_config.config_version} no recibido para {self.device_id}. Cerrando sesión.")
        return False

    async def _push_and_wait(self, tx: Transport, db_config: "ConfigData") -> bool:
        """Envía una config nueva al device y espera su ACK."""
        print(f"Cambio de protocolo: {self.config.protocol_conf} -> {db_config.protocol_conf} para {self.device_id}")
        try:
            await tx.send(DataCodec.serialize_config(db_config))
        except Exception as e:
            print(f"Error enviando config {tx.name}: {e}")
            return False
        return await self._wait_ack(tx, db_config)

    async def run(self) -> "ConfigData | None":
        """Abre el transporte y corre la sesión; lo reabre si el enlace se corta.

        Los transportes con conexión (TCP, BLE) se cortan cuando el device se
        duerme o se desconecta: si declaran `reopens`, se vuelve a abrir y se
        sigue esperando al device dentro de la misma sesión, igual que hacía el
        while exterior de TCPDeviceSession. Los sin conexión (UDP, MQTT) nunca
        lanzan TransportClosed.
        """
        while True:
            tx = self._make_transport()

            if not await tx.open():
                # No se llegó a establecer (p.ej. ningún device se conectó).
                return None

            try:
                return await self._session_loop(tx)
            except TransportClosed as e:
                if not tx.reopens:
                    print(f"{tx.name}: enlace cortado con {self.device_id} ({e}). Cerrando sesión.")
                    return None
                print(f"{tx.name}: {e}. Reabriendo para esperar al device.")
            finally:
                await tx.close()

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
                    print(f"Timeout {tx.name} ({timeout_sec}s) sin datos de {self.device_id}. " "Cerrando sesión para permitir reconexión.")
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
                    raise TransportClosed(f"{self.device_id} avisó deep sleep")
                print(f"{tx.name}: Se detectó deep sleep de {self.device_id}, se sigue escuchando")
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
                print(f"Config aplicada detectada en {tx.name} ({applied_version}) para {self.device_id}. " "Cerrando sesión para reconfigurar.")
                return decision.db_config
            elif decision.decision == ConfigDecision.ALREADY_SENT:
                # Ya se envió esta config en el cambio de protocolo; espera que el device la aplique
                continue
            elif decision.decision == ConfigDecision.PUSH:
                applied = await self._push_and_wait(tx, decision.db_config)
                return decision.db_config if applied else None

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
