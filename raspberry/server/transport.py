"""Contrato de transporte: lo único que cambia de verdad entre protocolos.

UTILIDAD PRINCIPAL
    Definir `Transport` -la interfaz mínima que un protocolo debe cumplir para
    mover bytes: abrir, recibir, enviar, cerrar- e implementar las cuatro
    variantes que usa el sistema:

        UdpTransport    socket sin conexión; escucha en udp_port
        TcpTransport    socket de escucha que acepta una conexión y la reabre
        MqttTransport   cliente compartido del proceso (mqtt.py), colas por device
        BleTransport    GATT sobre bleak; notificaciones en vez de sockets

    Todo lo demás -timeouts, sondeo de la BD, comparación de versiones de
    config, handshake del ACK, deep sleep- NO vive acá: vive una sola vez en
    `ProtocolSession` (sessions.py).

AGREGAR UN PROTOCOLO NUEVO (CoAP, LoRa, el que sea)
    1. escribir un Transport nuevo en este archivo,
    2. `class CoAPDeviceSession(ProtocolSession): transport_cls = CoapTransport`,
    3. una línea en el dict `session_classes` de dispatch.py.

PUNTOS DEL CONTRATO, Y QUÉ PROTOCOLO LOS PIDIÓ
    reopens                     el enlace se corta y hay que reabrirlo (TCP, BLE)
    can_send                    no siempre se puede responder (UDP sin peer aún)
    ack_window_sec              ventana de ACK propia del protocolo (BLE)
    confirm_config_applied()    confirmar por una segunda vía (BLE lee char D/A)

DETALLE IMPORTANTE DE recv()
    La tarea de lectura se guarda entre llamadas (`self._pending`) en vez de
    cancelarse cuando vence el timeout. Cancelarla descarta el paquete que
    llegó justo en el borde: `asyncio.wait_for` tira el resultado ya recibido
    si el timeout salta en la misma iteración del event loop. Es pérdida real
    de telemetría, medida en ~1 de cada 3 paquetes en ese caso límite.
"""
from __future__ import annotations
from abc import ABC, abstractmethod
import asyncio
import queue
import socket
import time

from bleak import BleakClient

from ble import UUID_CHAR_A, UUID_CHAR_B, UUID_CHAR_C, UUID_CHAR_D
from codec import DataCodec
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

    # Ventana de espera de un ACK por intento. None = usar `Timeouts.config_ack_sec`.
    # Un transporte puede necesitar otra escala: BLE espera más que un socket, y
    # el día que entre LoRa habrá que darle minutos (el downlink solo sale en la
    # ventana RX posterior a un uplink, con duty cycle de por medio).
    ack_window_sec: float | None = None

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

    async def confirm_config_applied(self, device_id: str, version: int) -> bool:
        """Segunda vía para confirmar que el device aplicó una config, si el ACK
        no llegó por el canal normal.

        Por defecto no hay ninguna: en un socket, si el ACK no llegó, no llegó.
        BLE sí puede preguntar, porque el device deja la última config y el
        último ACK legibles en sus características.
        """
        return False


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


class BleTransport(Transport):
    """BLE: conexión GATT persistente con el device.

    Es el más distinto de los cuatro:

    - Necesita más contexto que los demás (el BLEDevice de bleak, el adaptador,
      y los callbacks para pausar/reanudar el scanner), así que BLEDeviceSession
      lo construye con `_make_transport()` en vez del constructor por defecto.
    - Hay que pausar el escaneo para conectar (recomendación de bleak) y
      reanudarlo una vez conectado.
    - Los datos no se "reciben": llegan por notificaciones (char B telemetría,
      char D ACKs). Los callbacks los dejan en una cola que recv() consume, así
      la sesión los ve como un stream igual que en un socket. Ambas van a la
      MISMA cola a propósito: la sesión distingue ACK de telemetría por
      contenido, igual que en UDP/TCP.
    - El device deja la última config aplicada (char A) y el último ACK (char D)
      legibles, así que si se pierde la notificación del ACK se puede preguntar:
      eso es `confirm_config_applied()`.
    """
    name = "BLE"
    reopens = True      # el device corta el enlace al dormirse y vuelve al despertar

    def __init__(
        self,
        config: ConfigData,
        connect_timeout_sec: float,
        *,
        device,
        adapter: str | None = None,
        scanner_lock=None,
        scanner_stop=None,
        scanner_start=None,
        ack_window_sec: float | None = None,
    ):
        super().__init__(config, connect_timeout_sec)
        self.device = device
        self.adapter = adapter
        self.scanner_lock = scanner_lock
        self.scanner_stop = scanner_stop
        self.scanner_start = scanner_start
        self.ack_window_sec = ack_window_sec

        self._client: BleakClient | None = None
        self._queue: asyncio.Queue[bytes] = asyncio.Queue()
        self._pending: asyncio.Task | None = None
        self._notifying = False

    @property
    def can_send(self) -> bool:
        return self._client is not None

    async def _scanner(self, action) -> None:
        """Pausa/reanuda el escaneo BLE, si la sesión pasó esos callbacks."""
        if self.scanner_lock is None or action is None:
            return
        async with self.scanner_lock:
            await action()

    async def open(self) -> bool:
        print(f"BLE: Modo persistente. Intentando conectar al dispositivo {self.config.id_device}")

        # Conectar con el scanner corriendo da problemas (ver docs de bleak).
        await self._scanner(self.scanner_stop)
        try:
            client = BleakClient(self.device, adapter=self.adapter)
            await client.connect()
            if not client.is_connected:
                print(f"No se pudo conectar a {self.config.id_device} para RECIBIR DATOS.")
                return False
        except Exception as e:
            print(f"BLE: fallo conectando a {self.config.id_device}: {type(e).__name__}: {e}")
            return False
        finally:
            # El scanner vuelve pase lo que pase: si queda apagado, no se
            # descubre ningún otro device.
            await self._scanner(self.scanner_start)

        self._client = client

        loop = asyncio.get_running_loop()

        def _on_notify(_, data: bytearray):
            # Corre en el hilo/callback de bleak: hay que volver al event loop.
            loop.call_soon_threadsafe(self._queue.put_nowait, bytes(data))

        # Char A: configuración | Char B: telemetría | Char C: semáforo | Char D: ACKs
        await client.start_notify(UUID_CHAR_D, _on_notify)
        await client.start_notify(UUID_CHAR_B, _on_notify)
        self._notifying = True

        # Señal de inicio por característica C (libera el semáforo en la ESP32)
        await client.write_gatt_char(UUID_CHAR_C, b"start", response=True)

        print(f"BLE persistente iniciado correctamente para {self.config.id_device}")
        return True

    async def close(self) -> None:
        if self._pending is not None:
            self._pending.cancel()
            self._pending = None

        client, self._client = self._client, None
        if client is None:
            return

        if self._notifying:
            self._notifying = False
            for uuid in (UUID_CHAR_B, UUID_CHAR_D):
                try:
                    await client.stop_notify(uuid)
                except Exception:
                    pass    # se está cerrando igual; no vale la pena fallar acá
        try:
            await client.disconnect()
        except Exception:
            pass

        # Asegura que el scanner quede activo aunque algo haya fallado.
        await self._scanner(self.scanner_start)

    async def recv(self, timeout_sec: float) -> bytes | None:
        # Misma precaución que en los otros transportes: no cancelar un get()
        # que puede haber sacado un elemento de la cola.
        if self._pending is None:
            self._pending = asyncio.ensure_future(self._queue.get())

        done, _ = await asyncio.wait({self._pending}, timeout=timeout_sec)
        if not done:
            return None

        packet = self._pending.result()
        self._pending = None
        return packet

    async def send(self, data: bytes) -> None:
        await self._client.write_gatt_char(UUID_CHAR_A, data, response=True)

    async def confirm_config_applied(self, device_id: str, version: int) -> bool:
        """Pregunta al device si ya aplicó `version`, por si se perdió el notify.

        Lee el ACK que dejó en char D y, si eso no alcanza, la config que tiene
        aplicada en char A.
        """
        if self._client is None:
            return False

        window = self.ack_window_sec or self.connect_timeout_sec

        # Reconciliación: ACK persistente en D
        try:
            payload = await asyncio.wait_for(self._client.read_gatt_char(UUID_CHAR_D), timeout=window)
            ack = DataCodec.deserialize_config_ack(payload)
            if ack and ack.id_device == device_id and ack.config_version == version and ack.applied:
                return True
        except Exception:
            pass

        # Reconciliación: config persistente en A
        try:
            payload = await asyncio.wait_for(self._client.read_gatt_char(UUID_CHAR_A), timeout=window)
            cfg = DataCodec.deserialize_config(payload)
            if cfg and cfg.id_device == device_id and cfg.config_version == version:
                return True
        except Exception:
            pass

        return False
