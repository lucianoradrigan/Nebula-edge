"""Tests de integración de la sesión TCP (ProtocolSession + TcpTransport).

Corren la sesión real contra un "device" simulado que se conecta por TCP a
loopback. No tocan Postgres (FakeRepo) ni BLE (FakeBLEDevice).

    cd raspberry/server && python -m unittest discover -s tests
"""
from __future__ import annotations
import asyncio
import socket
import unittest

import sessions
from codec import DataCodec
from models import Timeouts
from tests.fakes import (
    DEEP_SLEEP_PACKET, DEVICE_ID, FakeBLEDevice, FakeRepo,
    config_ack_packet, data_1_packet, free_port, make_config, tcp_framed,
)


def quick_timeouts(**overrides) -> Timeouts:
    base = dict(
        config_poll_sec=0.2,
        no_data_grace_sec=8.0,
        config_ack_sec=1.0,
        config_ack_retries=3,
    )
    base.update(overrides)
    return Timeouts(**base)


class TcpSessionTests(unittest.IsolatedAsyncioTestCase):

    def _build_session(self, repo, port, timeouts):
        return sessions.TCPDeviceSession(
            FakeBLEDevice().address,
            make_config(1, tcp_port=port),
            repo,
            timeouts,
        )

    async def _connect_device(self, port, timeout=5.0):
        """Simula al ESP32 conectándose al server, reintentando hasta que escuche."""
        loop = asyncio.get_running_loop()
        deadline = loop.time() + timeout
        while loop.time() < deadline:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.setblocking(False)
            try:
                await loop.sock_connect(sock, ("127.0.0.1", port))
            except (ConnectionRefusedError, OSError):
                sock.close()
                await asyncio.sleep(0.05)
                continue
            self.addCleanup(sock.close)
            return sock
        self.fail("el servidor TCP nunca aceptó la conexión")

    async def _wait_until(self, predicate, timeout=5.0):
        loop = asyncio.get_running_loop()
        deadline = loop.time() + timeout
        while loop.time() < deadline:
            if predicate():
                return True
            await asyncio.sleep(0.05)
        return False

    async def _recv_frame(self, sock, timeout=5.0) -> bytes:
        """Lee un mensaje completo del server, como hace tcp_receive() del firmware.

        Lee el prefijo de largo y después exactamente esa cantidad de bytes, en
        vez de asumir que un recv() trae un mensaje entero.
        """
        loop = asyncio.get_running_loop()
        deadline = loop.time() + timeout
        buf = b""
        while True:
            if len(buf) >= 2:
                size = int.from_bytes(buf[:2], "big")
                if len(buf) >= 2 + size:
                    return buf[2:2 + size]
            remaining = deadline - loop.time()
            if remaining <= 0:
                self.fail("no llegó un mensaje completo desde el servidor")
            buf += await asyncio.wait_for(loop.sock_recv(sock, 2048), timeout=remaining)

    async def _reconnect_and_send_until(self, port, payload, predicate, timeout=8.0):
        """Reconecta y reenvía hasta que se cumpla `predicate`.

        Igual que hace el firmware al despertar: si el connect cae justo
        mientras el server está reabriendo su socket de escucha, esa conexión
        se pierde y hay que reintentar (app_main hace exactamente eso con
        `nebula_tcp_connect()` -> "Retrying TCP connection...").
        """
        loop = asyncio.get_running_loop()
        deadline = loop.time() + timeout
        while loop.time() < deadline:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.setblocking(False)
            try:
                await loop.sock_connect(sock, ("127.0.0.1", port))
                await loop.sock_sendall(sock, payload)
            except OSError:
                sock.close()
                await asyncio.sleep(0.2)
                continue
            self.addCleanup(sock.close)
            if await self._wait_until(predicate, timeout=1.0):
                return sock
            await asyncio.sleep(0.2)
        return None

    def test_tcp_session_uses_generic_session(self):
        self.assertTrue(issubclass(sessions.TCPDeviceSession, sessions.ProtocolSession))
        self.assertEqual(sessions.TCPDeviceSession.transport_cls.__name__, "TcpTransport")

    async def test_telemetry_is_inserted_and_session_stays_alive(self):
        port = free_port(socket.SOCK_STREAM)
        repo = FakeRepo(db_version=1, tcp_port=port)
        task = asyncio.create_task(self._build_session(repo, port, quick_timeouts()).run())
        self.addCleanup(task.cancel)

        dev = await self._connect_device(port)
        loop = asyncio.get_running_loop()
        await loop.sock_sendall(dev, tcp_framed(data_1_packet(applied_version=1)))

        self.assertTrue(await self._wait_until(lambda: len(repo.data_1) >= 1),
                        "no se insertó la telemetría recibida por TCP")
        self.assertEqual(repo.data_1[0].id_device, DEVICE_ID)
        self.assertFalse(task.done(), "la sesión no debía cerrar con la config al día")

    async def test_two_packets_in_one_segment_are_both_inserted(self):
        """El caso que TCP sin framing perdía: las dos tasks de sensores encolan
        sus paquetes de forma independiente, así que dos pueden viajar en el
        mismo segmento. Sin prefijo de largo el server leía el tipo del primero
        y le pasaba los dos protobuf pegados al parser, perdiendo ambos."""
        port = free_port(socket.SOCK_STREAM)
        repo = FakeRepo(db_version=1, tcp_port=port)
        task = asyncio.create_task(self._build_session(repo, port, quick_timeouts()).run())
        self.addCleanup(task.cancel)

        dev = await self._connect_device(port)
        loop = asyncio.get_running_loop()

        # Un solo sendall con los dos mensajes: llegan juntos sí o sí
        await loop.sock_sendall(dev, tcp_framed(data_1_packet(applied_version=1))
                                     + tcp_framed(data_1_packet(applied_version=1)))

        self.assertTrue(await self._wait_until(lambda: len(repo.data_1) >= 2),
                        f"se esperaban 2 inserciones, hubo {len(repo.data_1)}")
        self.assertFalse(task.done())

    async def test_packet_split_across_segments_is_reassembled(self):
        """El reverso: un mensaje partido en dos segmentos tiene que esperar a
        estar completo, no procesarse a medias ni descartarse."""
        port = free_port(socket.SOCK_STREAM)
        repo = FakeRepo(db_version=1, tcp_port=port)
        task = asyncio.create_task(self._build_session(repo, port, quick_timeouts()).run())
        self.addCleanup(task.cancel)

        dev = await self._connect_device(port)
        loop = asyncio.get_running_loop()

        frame = tcp_framed(data_1_packet(applied_version=1))
        cut = len(frame) // 2

        await loop.sock_sendall(dev, frame[:cut])
        # Con media telemetría en el buffer no se debe insertar nada todavía
        await asyncio.sleep(0.5)
        self.assertEqual(len(repo.data_1), 0,
                         "se procesó un mensaje incompleto")

        await loop.sock_sendall(dev, frame[cut:])
        self.assertTrue(await self._wait_until(lambda: len(repo.data_1) >= 1),
                        "el mensaje partido nunca se reensambló")
        self.assertFalse(task.done())

    async def test_new_config_in_db_is_pushed_and_confirmed_with_ack(self):
        port = free_port(socket.SOCK_STREAM)
        repo = FakeRepo(db_version=1, tcp_port=port)
        task = asyncio.create_task(self._build_session(repo, port, quick_timeouts()).run())
        self.addCleanup(task.cancel)

        dev = await self._connect_device(port)
        loop = asyncio.get_running_loop()
        await loop.sock_sendall(dev, tcp_framed(data_1_packet(applied_version=1)))
        self.assertTrue(await self._wait_until(lambda: len(repo.data_1) >= 1))

        repo.db_version = 2
        await loop.sock_sendall(dev, tcp_framed(data_1_packet(applied_version=1)))

        raw = await self._recv_frame(dev)
        pushed = DataCodec.deserialize_config(raw)
        self.assertIsNotNone(pushed)
        self.assertEqual(pushed.config_version, 2)

        await loop.sock_sendall(dev, tcp_framed(config_ack_packet(version=2)))

        result = await asyncio.wait_for(task, timeout=5.0)
        self.assertIsNotNone(result)
        self.assertEqual(result.config_version, 2)

    async def test_deep_sleep_reopens_socket_and_accepts_device_back(self):
        """Lo propio de TCP: el aviso de deep sleep corta la conexión, la sesión
        reabre el socket y el device vuelve a conectarse al despertar."""
        port = free_port(socket.SOCK_STREAM)
        repo = FakeRepo(db_version=1, tcp_port=port)
        task = asyncio.create_task(self._build_session(repo, port, quick_timeouts()).run())
        self.addCleanup(task.cancel)

        loop = asyncio.get_running_loop()
        dev = await self._connect_device(port)
        await loop.sock_sendall(dev, tcp_framed(data_1_packet(applied_version=1)))
        self.assertTrue(await self._wait_until(lambda: len(repo.data_1) >= 1))

        # El device avisa que se duerme y cierra
        await loop.sock_sendall(dev, tcp_framed(DEEP_SLEEP_PACKET))
        dev.close()

        # Al despertar se reconecta: solo funciona si la sesión reabrió el socket
        dev2 = await self._reconnect_and_send_until(
            port, tcp_framed(data_1_packet(applied_version=1)), lambda: len(repo.data_1) >= 2,
        )
        self.assertIsNotNone(dev2, "tras el deep sleep la sesión no volvió a aceptar al device")
        self.assertFalse(task.done(), "la sesión debía seguir viva tras reabrir")

    async def test_device_closing_connection_reopens_instead_of_ending(self):
        """Un corte sin aviso (el device se cae) también reabre, no mata la sesión."""
        port = free_port(socket.SOCK_STREAM)
        repo = FakeRepo(db_version=1, tcp_port=port)
        task = asyncio.create_task(self._build_session(repo, port, quick_timeouts()).run())
        self.addCleanup(task.cancel)

        loop = asyncio.get_running_loop()
        dev = await self._connect_device(port)
        await loop.sock_sendall(dev, tcp_framed(data_1_packet(applied_version=1)))
        self.assertTrue(await self._wait_until(lambda: len(repo.data_1) >= 1))

        dev.close()   # corte abrupto, sin aviso de deep sleep

        dev2 = await self._reconnect_and_send_until(
            port, tcp_framed(data_1_packet(applied_version=1)), lambda: len(repo.data_1) >= 2,
        )
        self.assertIsNotNone(dev2, "tras el corte la sesión no volvió a aceptar al device")

    async def test_session_closes_when_no_device_connects(self):
        port = free_port(socket.SOCK_STREAM)
        repo = FakeRepo(db_version=1, tcp_port=port)
        timeouts = quick_timeouts(no_data_grace_sec=1.0)
        result = await asyncio.wait_for(
            self._build_session(repo, port, timeouts).run(), timeout=15.0,
        )
        self.assertIsNone(result, "sin conexión entrante la sesión debía cerrar con None")

        # El puerto quedó libre
        probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            probe.bind(("0.0.0.0", port))
        finally:
            probe.close()


if __name__ == "__main__":
    unittest.main()
