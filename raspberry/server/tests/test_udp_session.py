"""Tests de integración de la sesión UDP (ProtocolSession + UdpTransport).

Corren la sesión real contra un "device" simulado que habla UDP por loopback.
No tocan Postgres (FakeRepo) ni BLE (FakeBLEDevice).

Correr desde raspberry/server/ con las dependencias instaladas:

    python -m unittest discover -s tests
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
    config_ack_packet, environmental_packet, free_port, make_config,
)


def quick_timeouts(**overrides) -> Timeouts:
    """Timeouts chicos para que los tests corran en segundos, no en minutos."""
    base = dict(
        config_poll_sec=0.2,
        no_data_grace_sec=8.0,
        config_ack_sec=1.0,
        config_ack_retries=3,
    )
    base.update(overrides)
    return Timeouts(**base)


class UdpSessionTests(unittest.IsolatedAsyncioTestCase):

    def _build_session(self, repo, port, timeouts):
        return sessions.UDPDeviceSession(
            FakeBLEDevice().address,
            make_config(1, udp_port=port),
            repo,
            timeouts,
        )

    async def _device_socket(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setblocking(False)
        self.addCleanup(sock.close)
        return sock

    async def _send(self, sock, payload, port):
        loop = asyncio.get_running_loop()
        await loop.sock_sendto(sock, payload, ("127.0.0.1", port))

    async def _wait_until(self, predicate, timeout=5.0):
        """Espera activa corta: evita sleeps fijos que vuelven frágiles los tests."""
        deadline = asyncio.get_running_loop().time() + timeout
        while asyncio.get_running_loop().time() < deadline:
            if predicate():
                return True
            await asyncio.sleep(0.05)
        return False

    def test_udp_session_uses_generic_session(self):
        """UDPDeviceSession debe ser solo ProtocolSession + UdpTransport."""
        self.assertTrue(issubclass(sessions.UDPDeviceSession, sessions.ProtocolSession))
        self.assertEqual(sessions.UDPDeviceSession.transport_cls.__name__, "UdpTransport")

    async def test_telemetry_is_inserted_and_session_stays_alive(self):
        port = free_port()
        repo = FakeRepo(db_version=1, udp_port=port)
        task = asyncio.create_task(self._build_session(repo, port, quick_timeouts()).run())
        self.addCleanup(task.cancel)

        dev = await self._device_socket()
        # Se manda repetido: UDP descarta en silencio lo que llegue antes del bind.
        for _ in range(5):
            await self._send(dev, environmental_packet(applied_version=1), port)
            if await self._wait_until(lambda: len(repo.environmental) >= 1, timeout=0.5):
                break

        self.assertGreaterEqual(len(repo.environmental), 1, "no se insertó la telemetría")
        self.assertEqual(repo.environmental[0].id_device, DEVICE_ID)
        self.assertFalse(task.done(), "la sesión no debía cerrar con la config al día")

    async def test_new_config_in_db_is_pushed_and_confirmed_with_ack(self):
        port = free_port()
        repo = FakeRepo(db_version=1, udp_port=port)
        task = asyncio.create_task(self._build_session(repo, port, quick_timeouts()).run())
        self.addCleanup(task.cancel)

        dev = await self._device_socket()
        loop = asyncio.get_running_loop()

        for _ in range(5):
            await self._send(dev, environmental_packet(applied_version=1), port)
            if await self._wait_until(lambda: len(repo.environmental) >= 1, timeout=0.5):
                break
        self.assertGreaterEqual(len(repo.environmental), 1)

        # Alguien cambia la config en la BD
        repo.db_version = 2
        await self._send(dev, environmental_packet(applied_version=1), port)

        raw = await asyncio.wait_for(loop.sock_recv(dev, 2048), timeout=5.0)
        pushed = DataCodec.deserialize_config(raw)
        self.assertIsNotNone(pushed)
        self.assertEqual(pushed.config_version, 2, "se esperaba la config v2 empujada al device")

        # El device confirma
        await self._send(dev, config_ack_packet(version=2), port)

        result = await asyncio.wait_for(task, timeout=5.0)
        self.assertIsNotNone(result, "run() debía devolver la config aplicada")
        self.assertEqual(result.config_version, 2)

    async def test_config_is_resent_until_the_device_acks(self):
        """El device pierde el primer envío de config y el server reintenta.

        Pasa seguido: en UDP el datagrama se puede perder sin más, y al
        despertar de deep sleep el device deja de escuchar config en cuanto
        completa su ventana de envío. Antes la config se mandaba una sola vez
        y los reintentos solo volvían a esperar, así que la sesión se cerraba
        sin que el device se enterara nunca.
        """
        port = free_port()
        repo = FakeRepo(db_version=1, udp_port=port)
        task = asyncio.create_task(self._build_session(repo, port, quick_timeouts()).run())
        self.addCleanup(task.cancel)

        dev = await self._device_socket()
        loop = asyncio.get_running_loop()

        for _ in range(5):
            await self._send(dev, environmental_packet(applied_version=1), port)
            if await self._wait_until(lambda: len(repo.environmental) >= 1, timeout=0.5):
                break

        # Alguien cambia la config en la BD
        repo.db_version = 2
        await self._send(dev, environmental_packet(applied_version=1), port)

        # El device ignora el primer envío y espera el reenvío del server.
        first = await asyncio.wait_for(loop.sock_recv(dev, 2048), timeout=5.0)
        self.assertEqual(DataCodec.deserialize_config(first).config_version, 2)

        resent = await asyncio.wait_for(loop.sock_recv(dev, 2048), timeout=5.0)
        self.assertIsNotNone(
            DataCodec.deserialize_config(resent),
            "el server tenía que reenviar la config al no recibir ACK",
        )
        self.assertEqual(DataCodec.deserialize_config(resent).config_version, 2)

        # Recién ahora el device confirma
        await self._send(dev, config_ack_packet(version=2), port)

        result = await asyncio.wait_for(task, timeout=5.0)
        self.assertIsNotNone(result, "run() debía devolver la config aplicada")
        self.assertEqual(result.config_version, 2)

    async def test_typed_packets_are_never_parsed_as_ack(self):
        """La telemetría que llega esperando el ACK no pasa por el parser de ACK.

        Lleva byte de tipo, así que nunca puede ser un ConfigAck: intentarlo
        igual solo ensuciaba el log con un error por cada paquete recibido.
        """
        self.assertTrue(DataCodec.is_typed_packet(environmental_packet(applied_version=1)))
        self.assertTrue(DataCodec.is_typed_packet(DEEP_SLEEP_PACKET))
        self.assertFalse(DataCodec.is_typed_packet(config_ack_packet(version=2)))

    async def test_proactive_push_without_incoming_telemetry(self):
        """La sesión detecta el cambio de config sondeando la BD, sin depender
        de que llegue telemetría nueva."""
        port = free_port()
        repo = FakeRepo(db_version=1, udp_port=port)
        task = asyncio.create_task(self._build_session(repo, port, quick_timeouts()).run())
        self.addCleanup(task.cancel)

        dev = await self._device_socket()
        loop = asyncio.get_running_loop()

        # Aviso de deep sleep: NO gatilla revisión de config, pero revela la
        # dirección del device (habilita can_send).
        for _ in range(5):
            await self._send(dev, DEEP_SLEEP_PACKET, port)
            await asyncio.sleep(0.15)
        self.assertFalse(task.done(), "un deep sleep no debía cerrar la sesión UDP")

        repo.db_version = 2   # cambia la config sin que el device mande nada más

        raw = await asyncio.wait_for(loop.sock_recv(dev, 2048), timeout=6.0)
        pushed = DataCodec.deserialize_config(raw)
        self.assertIsNotNone(pushed)
        self.assertEqual(pushed.config_version, 2)

        await self._send(dev, config_ack_packet(version=2), port)
        result = await asyncio.wait_for(task, timeout=5.0)
        self.assertEqual(result.config_version, 2)

    async def test_without_device_does_not_push_and_closes_on_timeout(self):
        """can_send=False mientras el device no haya escrito: no se puede
        responder a nadie, así que la sesión solo espera y cierra."""
        port = free_port()
        repo = FakeRepo(db_version=2, udp_port=port)   # BD ya adelantada
        timeouts = quick_timeouts(no_data_grace_sec=1.5, config_ack_sec=0.5,
                                  config_ack_retries=2)
        session = self._build_session(repo, port, timeouts)

        result = await asyncio.wait_for(session.run(), timeout=15.0)
        self.assertIsNone(result, "sin device al otro lado la sesión debía cerrar con None")

    async def test_socket_is_released_when_session_ends(self):
        port = free_port()
        repo = FakeRepo(db_version=1, udp_port=port)
        timeouts = quick_timeouts(no_data_grace_sec=1.0)
        await asyncio.wait_for(self._build_session(repo, port, timeouts).run(), timeout=15.0)

        # Si el transporte no hubiera cerrado el socket, este bind fallaría.
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            probe.bind(("0.0.0.0", port))
        finally:
            probe.close()


if __name__ == "__main__":
    unittest.main()
