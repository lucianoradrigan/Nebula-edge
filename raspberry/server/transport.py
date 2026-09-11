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
import socket

from models import ConfigData


class Transport(ABC):
    """Mueve bytes hacia/desde un device. Sin lógica de protocolo de aplicación."""

    # Nombre corto para los logs ("UDP", "TCP", "BLE", ...).
    name: str = "?"

    def __init__(self, config: ConfigData):
        self.config = config

    @property
    def can_send(self) -> bool:
        """Si ahora mismo se le puede mandar algo al device.

        No siempre se puede: en UDP no se sabe a qué dirección responder
        hasta que el device manda su primer paquete, y en TCP/BLE hace
        falta una conexión establecida. La sesión lo consulta antes de
        intentar empujar una config sin que haya llegado telemetría.
        """
        return True

    async def __aenter__(self) -> "Transport":
        await self.open()
        return self

    async def __aexit__(self, exc_type, exc, tb) -> None:
        await self.close()

    @abstractmethod
    async def open(self) -> None:
        """Deja el transporte listo para recibir (abrir socket, conectar, etc.)."""

    @abstractmethod
    async def close(self) -> None:
        """Libera lo que haya tomado open()."""

    @abstractmethod
    async def recv(self, timeout_sec: float) -> bytes | None:
        """Espera un paquete hasta `timeout_sec`. Retorna None si se venció."""

    @abstractmethod
    async def send(self, data: bytes) -> None:
        """Envía un paquete al device. Puede lanzar excepción si falla."""


class UdpTransport(Transport):
    """UDP: el server escucha en un puerto fijo por device (`config.udp_port`)
    y le responde a la última dirección desde la que ese device escribió.

    Esa dirección es estado interno del transporte: la sesión ya no tiene
    que arrastrar un `last_udp_addr` como hacía antes UDPDeviceSession.
    """
    name = "UDP"

    def __init__(self, config: ConfigData):
        super().__init__(config)
        self._sock: socket.socket | None = None
        self._peer = None   # última dirección vista, a donde se responde
        self._pending: asyncio.Task | None = None   # recepción en curso entre llamadas a recv()

    @property
    def can_send(self) -> bool:
        return self._sock is not None and self._peer is not None

    async def open(self) -> None:
        host = "0.0.0.0"                # Escucha en todas las interfaces
        port = self.config.udp_port

        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)  # reutiliza puerto si quedó abierto
        sock.setblocking(False)         # para no bloquear el event loop
        sock.bind((host, port))

        self._sock = sock
        print(f"Servidor UDP escuchando en {host}:{port}")

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
