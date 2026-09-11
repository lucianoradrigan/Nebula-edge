"""Contrato de transporte: lo único que cambia de verdad entre protocolos.

Un `Transport` solo mueve bytes: abrir, recibir, enviar, cerrar. Todo lo
demás -timeouts, sondeo proactivo de la BD, comparación de versiones de
config, handshake de ACK, deep sleep- vive una sola vez en
`ProtocolSession` (classes.py), compartido por todos los protocolos.

La idea es que agregar un protocolo nuevo (CoAP, LoRa, el que sea) sea:

    1. escribir un Transport nuevo acá,
    2. `class CoAPDeviceSession(ProtocolSession): transport_cls = CoapTransport`,
    3. una línea en el dict `session_classes` de handle_protocol.

...en vez de copiar ~140 líneas de una sesión existente y ajustarlas. Ese
copy-paste ya costó caro en este repo: el `if/if/else` que dejaba la
revisión de config inalcanzable existía en BLE y parcialmente en UDP/TCP,
pero no en MQTT, porque las cuatro sesiones derivaron por separado.
"""
from __future__ import annotations
from abc import ABC, abstractmethod
import asyncio
import queue
import socket
import time

from models import ConfigData
from mqtt import mqtt_start, mqtt_publish, get_data_queue, get_ack_queue


class TransportClosed(Exception):
    """El enlace con el device se cortó (el otro extremo cerró, o avisó deep sleep).

    Distinto de un timeout: acá no es que no llegó nada, es que ya no hay por
    dónde recibir. Si el transporte declara `reopens = True`, la sesión vuelve
    a abrirlo y sigue esperando al device.
    """


class Transport(ABC):
    """Mueve bytes hacia/desde un device. Sin lógica de protocolo de aplicación."""

    # Nombre corto para los logs ("UDP", "TCP", "BLE", ...).
    name: str = "?"

    # Si el enlace se corta (TransportClosed), ¿tiene sentido reabrir y seguir
    # esperando al device en la misma sesión? TCP/BLE sí: el device se desconecta
    # al dormirse y vuelve. UDP/MQTT no tienen conexión que se corte.
    reopens: bool = False

    def __init__(self, config: ConfigData, connect_timeout_sec: float):
        self.config = config
        # Cuánto esperar en open() a que el device aparezca (accept, connect...).
        self.connect_timeout_sec = connect_timeout_sec

    @property
    def can_send(self) -> bool:
        """Si ahora mismo se le puede mandar algo al device.

        No siempre se puede: en UDP no se sabe a qué dirección responder hasta
        que el device manda su primer paquete. La sesión lo consulta antes de
        intentar empujar una config sin que haya llegado telemetría.
        """
        return True

    @abstractmethod
    async def open(self) -> bool:
        """Deja el transporte listo para recibir.

        Retorna False si no se llegó a establecer (p.ej. ningún device se
        conectó dentro de `connect_timeout_sec`): la sesión termina.
        """

    @abstractmethod
    async def close(self) -> None:
        """Libera lo que haya tomado open(). Debe tolerar llamarse dos veces."""

    @abstractmethod
    async def recv(self, timeout_sec: float) -> bytes | None:
        """Espera un paquete hasta `timeout_sec`. Retorna None si se venció.

        Lanza `TransportClosed` si el enlace se cortó.
        """

    @abstractmethod
    async def send(self, data: bytes) -> None:
        """Envía un paquete al device. Puede lanzar excepción si falla."""


class UdpTransport(Transport):
    """UDP: el server escucha en un puerto fijo por device (`config.udp_port`)
    y le responde a la última dirección desde la que ese device escribió.

    Esa dirección es estado interno del transporte: la sesión ya no tiene que
    arrastrar un `last_udp_addr` como hacía antes UDPDeviceSession.
    """
    name = "UDP"
    reopens = False     # sin conexión que se corte: el socket sigue escuchando

    def __init__(self, config: ConfigData, connect_timeout_sec: float):
        super().__init__(config, connect_timeout_sec)
        self._sock: socket.socket | None = None
        self._peer = None                            # última dirección vista
        self._pending: asyncio.Task | None = None    # recepción en curso entre llamadas a recv()

    @property
    def can_send(self) -> bool:
        return self._sock is not None and self._peer is not None

    async def open(self) -> bool:
        host = "0.0.0.0"                # Escucha en todas las interfaces
        port = self.config.udp_port

        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)  # reutiliza puerto si quedó abierto
        sock.setblocking(False)         # para no bloquear el event loop
        sock.bind((host, port))

        self._sock = sock
        print(f"Servidor UDP escuchando en {host}:{port}")
        return True

    async def close(self) -> None:
        if self._pending is not None:
            self._pending.cancel()
            self._pending = None
        if self._sock is not None:
            self._sock.close()
            self._sock = None

    async def recv(self, timeout_sec: float) -> bytes | None:
        """Espera un datagrama hasta `timeout_sec`. None si se venció.

        OJO con el patrón obvio `await asyncio.wait_for(loop.sock_recvfrom(...))`:
        pierde paquetes. Si el datagrama llega en la misma vuelta del event loop
        en que vence el timeout, wait_for cancela la recepción y descarta un dato
        que el socket YA sacó del buffer del kernel. Como la sesión llama a recv()
        en ventanas cortas (config_poll_sec) todo el tiempo, ese borde se cruza
        seguido: medido acá, ~1 de cada 3 paquetes que caen justo en el límite se
        perdía (y era telemetría de sensores que nunca llegaba a la BD).

        En vez de eso se mantiene UNA recepción pendiente entre llamadas: si el
        timeout vence, la tarea queda viva y se reusa en la siguiente llamada, así
        que ningún datagrama se descarta nunca.
        """
        loop = asyncio.get_running_loop()

        if self._pending is None:
            self._pending = asyncio.ensure_future(loop.sock_recvfrom(self._sock, 1024))

        done, _ = await asyncio.wait({self._pending}, timeout=timeout_sec)
        if not done:
            return None     # sigue pendiente; se retoma en la próxima llamada

        packet, peer = self._pending.result()
        self._pending = None

        # Recuerda a quién responderle (y habilita can_send).
        self._peer = peer
        return packet

    async def send(self, data: bytes) -> None:
        loop = asyncio.get_running_loop()
        await loop.sock_sendto(self._sock, data, self._peer)


class TcpTransport(Transport):
    """TCP: el server escucha en `config.tcp_port` y acepta UNA conexión del device.

    A diferencia de UDP hay una conexión real que se puede cortar: cuando el
    device se duerme o cierra, `recv()` lanza TransportClosed y la sesión reabre
    el socket para esperar a que vuelva (`reopens = True`).
    """
    name = "TCP"
    reopens = True

    def __init__(self, config: ConfigData, connect_timeout_sec: float):
        super().__init__(config, connect_timeout_sec)
        self._listen: socket.socket | None = None
        self._conn: socket.socket | None = None
        self._pending: asyncio.Task | None = None

    @property
    def can_send(self) -> bool:
        return self._conn is not None

    async def open(self) -> bool:
        loop = asyncio.get_running_loop()
        host = "0.0.0.0"
        port = self.config.tcp_port

        listen = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listen.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listen.setblocking(False)
        listen.bind((host, port))
        listen.listen()
        self._listen = listen
        print(f"Servidor TCP escuchando en {host}:{port}")

        try:
            conn, addr = await asyncio.wait_for(
                loop.sock_accept(listen),
                timeout=self.connect_timeout_sec,
            )
        except asyncio.TimeoutError:
            print(f"Timeout esperando conexion TCP ({self.connect_timeout_sec}s)")
            await self.close()
            return False

        conn.setblocking(False)
        self._conn = conn
        print(f"Conexión establecida desde {addr}")
        return True

    async def close(self) -> None:
        if self._pending is not None:
            self._pending.cancel()
            self._pending = None
        if self._conn is not None:
            self._conn.close()
            self._conn = None
        if self._listen is not None:
            self._listen.close()
            self._listen = None

    async def recv(self, timeout_sec: float) -> bytes | None:
        # Misma precaución que en UdpTransport.recv: no cancelar una recepción
        # que puede haber consumido datos, para no perder paquetes en el borde
        # del timeout.
        loop = asyncio.get_running_loop()

        if self._pending is None:
            self._pending = asyncio.ensure_future(loop.sock_recv(self._conn, 1024))

        done, _ = await asyncio.wait({self._pending}, timeout=timeout_sec)
        if not done:
            return None

        pending, self._pending = self._pending, None
        try:
            packet = pending.result()
        except Exception as e:
            raise TransportClosed(f"recepción TCP falló: {e}") from e

        if not packet:
            # recv vacío en TCP = el otro extremo cerró la conexión.
            raise TransportClosed("el device cerró la conexión")
        return packet

    async def send(self, data: bytes) -> None:
        loop = asyncio.get_running_loop()
        await loop.sock_sendall(self._conn, data)


class MqttTransport(Transport):
    """MQTT: el cliente es del proceso (ver mqtt.py), no de esta sesión.

    Dos diferencias con UDP/TCP:

    - No hay socket propio: mqtt.py mantiene UN cliente compartido y enruta
      por device a colas separadas (tópicos wildcard). `open()` solo se
      asegura de que el cliente esté arriba -mqtt_start() es idempotente- y
      toma las colas de este device. `close()` NO apaga el cliente: otros
      devices pueden estar usándolo.

    - Los ACK de config llegan por un tópico distinto (.../config/ack) que la
      telemetría (.../data), o sea por otra cola. Acá se fusionan en un solo
      stream de bytes, para que la sesión los vea igual que en un socket, sin
      lógica especial.
    """
    name = "MQTT"
    reopens = False     # no hay conexión por sesión que se pueda cortar

    # Cada cuánto se miran las colas mientras se espera. Son queue.Queue
    # (las llena el hilo de paho), así que no se pueden esperar con await.
    # Se sondean en vez de bloquear un hilo del pool de asyncio.to_thread por
    # cola y por device: ese pool es el mismo que usan las consultas a la BD,
    # y dejarlo sin hilos libres frenaría todas las sesiones.
    _QUEUE_POLL_SEC = 0.02

    def __init__(self, config: ConfigData, connect_timeout_sec: float):
        super().__init__(config, connect_timeout_sec)
        self._data_queue: queue.Queue | None = None
        self._ack_queue: queue.Queue | None = None
        self._config_topic = f"/topic/nebulaedge/{config.id_device}/config"

    async def open(self) -> bool:
        mqtt_start()    # idempotente: conecta el cliente compartido si hace falta
        self._data_queue = get_data_queue(self.config.id_device)
        self._ack_queue = get_ack_queue(self.config.id_device)
        return True

    async def close(self) -> None:
        # A propósito no se llama mqtt_shutdown(): el cliente es compartido
        # por todos los devices en MQTT y sobrevive a esta sesión.
        self._data_queue = None
        self._ack_queue = None

    async def recv(self, timeout_sec: float) -> bytes | None:
        deadline = time.monotonic() + timeout_sec
        while True:
            for q in (self._data_queue, self._ack_queue):
                try:
                    return q.get_nowait()
                except queue.Empty:
                    pass
            if time.monotonic() >= deadline:
                return None
            await asyncio.sleep(self._QUEUE_POLL_SEC)

    async def send(self, data: bytes) -> None:
        mqtt_publish(self._config_topic, data)
