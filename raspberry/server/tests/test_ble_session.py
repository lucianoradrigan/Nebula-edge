"""Tests de integración de la sesión BLE (ProtocolSession + BleTransport).

No hay hardware ni bleak real: se reemplaza BleakClient por un doble que deja
disparar notificaciones a mano y responde las reads de características.

OJO con el alcance: esto verifica el cableado (notificación -> cola -> ruteo ->
insert; escritura de config -> ACK; reconciliación leyendo char D/A; deep sleep
-> reconexión), NO que bleak/BlueZ se comporten como el doble. BLE es el único
de los cuatro protocolos que no se puede validar de verdad sin un ESP32.

    cd raspberry/server && python -m unittest discover -s tests
"""
from __future__ import annotations
import asyncio
import unittest
from unittest import mock

import sessions
import transport
from gatt_uuids import UUID_CHAR_A, UUID_CHAR_B, UUID_CHAR_C, UUID_CHAR_D
from codec import DataCodec
from models import Timeouts
from tests.fakes import (
    DEEP_SLEEP_PACKET, DEVICE_ID, FakeBLEDevice, FakeRepo,
    config_ack_packet, environmental_packet, make_config,
)


def quick_timeouts(**overrides) -> Timeouts:
    base = dict(
        config_poll_sec=0.2,
        no_data_grace_sec=3.0,
        config_ack_sec=0.5,
        config_ack_retries=2,
        ble_ack_short_sec=0.5,
    )
    base.update(overrides)
    return Timeouts(**base)


class FakeBleakClient:
    """Doble de BleakClient. La instancia viva queda en `last_instance` para que el
    test pueda disparar notificaciones como si fuera el ESP32."""

    last_instance: "FakeBleakClient | None" = None
    fail_to_connect = False

    def __init__(self, device, adapter=None):
        self.device = device
        self.adapter = adapter
        self.is_connected = False
        self.writes: list[tuple[str, bytes]] = []
        self.reads: dict[str, bytes] = {}
        self._callbacks: dict[str, callable] = {}
        FakeBleakClient.last_instance = self

    async def connect(self):
        if FakeBleakClient.fail_to_connect:
            raise OSError("no se pudo conectar (simulado)")
        self.is_connected = True

    async def disconnect(self):
        self.is_connected = False

    async def start_notify(self, uuid, callback):
        self._callbacks[uuid] = callback

    async def stop_notify(self, uuid):
        self._callbacks.pop(uuid, None)

    async def write_gatt_char(self, uuid, data, response=False):
        self.writes.append((uuid, bytes(data)))

    async def read_gatt_char(self, uuid):
        if uuid not in self.reads:
            raise OSError(f"característica {uuid} no legible (simulado)")
        return self.reads[uuid]

    def emit_notification(self, uuid, payload: bytes):
        """Simula que el ESP32 manda una notificación por esa característica."""
        cb = self._callbacks.get(uuid)
        if cb is not None:
            cb(None, bytearray(payload))

    def written_config(self):
        for uuid, data in self.writes:
            if uuid == UUID_CHAR_A:
                return DataCodec.deserialize_config(data)
        return None


class BleSessionTests(unittest.IsolatedAsyncioTestCase):

    def setUp(self):
        FakeBleakClient.last_instance = None
        FakeBleakClient.fail_to_connect = False
        p = mock.patch.object(transport, "BleakClient", FakeBleakClient)
        p.start()
        self.addCleanup(p.stop)

    def _build_session(self, repo, timeouts, ble_client=None):
        return sessions.BLEDeviceSession(
            FakeBLEDevice(), make_config(1), repo, None, None, None, None, timeouts,
            ble_client=ble_client,
        )

    async def _wait_until(self, predicate, timeout=5.0):
        loop = asyncio.get_running_loop()
        deadline = loop.time() + timeout
        while loop.time() < deadline:
            if predicate():
                return True
            await asyncio.sleep(0.05)
        return False

    async def _connected_session(self, repo, timeouts=None):
        task = asyncio.create_task(self._build_session(repo, timeouts or quick_timeouts()).run())
        self.addCleanup(task.cancel)
        self.assertTrue(await self._wait_until(lambda: FakeBleakClient.last_instance is not None
                                               and FakeBleakClient.last_instance.is_connected),
                        "la sesión nunca se conectó")
        return task, FakeBleakClient.last_instance

    def test_ble_session_uses_generic_session(self):
        self.assertTrue(issubclass(sessions.BLEDeviceSession, sessions.ProtocolSession))
        self.assertEqual(sessions.BLEDeviceSession.transport_cls.__name__, "BleTransport")

    async def test_writes_start_on_char_c_when_connecting(self):
        """El 'start' en char C libera el semáforo del firmware."""
        repo = FakeRepo(db_version=1)
        _, client = await self._connected_session(repo)
        self.assertTrue(await self._wait_until(
            lambda: any(u == UUID_CHAR_C and d == b"start" for u, d in client.writes)))

    async def test_telemetry_from_notification_is_inserted(self):
        repo = FakeRepo(db_version=1)
        task, client = await self._connected_session(repo)

        client.emit_notification(UUID_CHAR_B, environmental_packet(applied_version=1))

        self.assertTrue(await self._wait_until(lambda: len(repo.environmental) >= 1))
        self.assertEqual(repo.environmental[0].id_device, DEVICE_ID)
        self.assertFalse(task.done())

    async def test_new_config_written_to_char_a_and_ack_arrives_on_char_d(self):
        repo = FakeRepo(db_version=1)
        task, client = await self._connected_session(repo)

        client.emit_notification(UUID_CHAR_B, environmental_packet(applied_version=1))
        self.assertTrue(await self._wait_until(lambda: len(repo.environmental) >= 1))

        repo.db_version = 2
        client.emit_notification(UUID_CHAR_B, environmental_packet(applied_version=1))

        self.assertTrue(await self._wait_until(lambda: client.written_config() is not None),
                        "no se escribió la config nueva en la característica A")
        self.assertEqual(client.written_config().config_version, 2)

        # El device responde el ACK por notificación en D
        client.emit_notification(UUID_CHAR_D, config_ack_packet(version=2))

        result = await asyncio.wait_for(task, timeout=5.0)
        self.assertIsNotNone(result)
        self.assertEqual(result.config_version, 2)

    async def test_reconciles_by_reading_char_d_when_ack_notify_is_lost(self):
        """Lo propio de BLE: el device deja el ACK legible en char D, así que
        aunque se pierda la notificación se puede confirmar preguntando."""
        repo = FakeRepo(db_version=1)
        task, client = await self._connected_session(repo)

        # El device ya aplicó la v2 y dejó el ACK legible, pero NO notifica
        client.reads[UUID_CHAR_D] = config_ack_packet(version=2)

        repo.db_version = 2
        client.emit_notification(UUID_CHAR_B, environmental_packet(applied_version=1))

        result = await asyncio.wait_for(task, timeout=10.0)
        self.assertIsNotNone(result, "la reconciliación por char D debía confirmar la config")
        self.assertEqual(result.config_version, 2)

    async def test_reconciles_by_reading_char_a_when_char_d_unavailable(self):
        """Segunda vía: la config aplicada que el device deja legible en char A."""
        repo = FakeRepo(db_version=1)
        task, client = await self._connected_session(repo)

        # D no responde; A sí, con la config v2 ya aplicada
        client.reads[UUID_CHAR_A] = DataCodec.serialize_config(make_config(2))

        repo.db_version = 2
        client.emit_notification(UUID_CHAR_B, environmental_packet(applied_version=1))

        result = await asyncio.wait_for(task, timeout=10.0)
        self.assertIsNotNone(result, "la reconciliación por char A debía confirmar la config")
        self.assertEqual(result.config_version, 2)

    async def test_deep_sleep_reconnects(self):
        """El aviso de deep sleep corta el enlace; la sesión reabre y se reconecta."""
        repo = FakeRepo(db_version=1)
        task, client = await self._connected_session(repo)
        first = client

        client.emit_notification(UUID_CHAR_B, DEEP_SLEEP_PACKET)

        # Se crea un cliente nuevo al reconectar
        self.assertTrue(await self._wait_until(
            lambda: FakeBleakClient.last_instance is not None and FakeBleakClient.last_instance is not first),
            "tras el deep sleep no se reconectó")
        self.assertFalse(task.done(), "la sesión debía seguir viva tras reconectar")

    async def test_session_ends_when_connection_fails(self):
        FakeBleakClient.fail_to_connect = True
        repo = FakeRepo(db_version=1)
        result = await asyncio.wait_for(
            self._build_session(repo, quick_timeouts()).run(), timeout=10.0,
        )
        self.assertIsNone(result)

    async def test_reuses_discovery_connection(self):
        """Si el descubrimiento entrega una conexión viva, la sesión la adopta
        en vez de abrir otra: reconectar cuesta otro descubrimiento de
        servicios en BlueZ, que es la parte cara."""
        repo = FakeRepo(db_version=1)

        # Conexión que dejó abierta el handshake inicial
        adopted = FakeBleakClient(FakeBLEDevice())
        await adopted.connect()

        task = asyncio.create_task(
            self._build_session(repo, quick_timeouts(), ble_client=adopted).run()
        )
        self.addCleanup(task.cancel)

        # El "start" tiene que salir por la conexión adoptada
        self.assertTrue(await self._wait_until(
            lambda: any(u == UUID_CHAR_C and d == b"start" for u, d in adopted.writes)),
            "no se usó la conexión entregada por el descubrimiento")

        # Y no debe haberse construido ningún cliente nuevo
        self.assertIs(FakeBleakClient.last_instance, adopted,
                      "se abrió una conexión nueva teniendo una viva")

        # La telemetría fluye por esa misma conexión
        adopted.emit_notification(UUID_CHAR_B, environmental_packet(applied_version=1))
        self.assertTrue(await self._wait_until(lambda: len(repo.environmental) >= 1))

    async def test_does_not_reuse_stale_connection_after_deep_sleep(self):
        """La conexión adoptada sirve una sola vez: si el device se duerme y el
        enlace se corta, reabrir tiene que conectar de nuevo."""
        repo = FakeRepo(db_version=1)

        adopted = FakeBleakClient(FakeBLEDevice())
        await adopted.connect()

        task = asyncio.create_task(
            self._build_session(repo, quick_timeouts(), ble_client=adopted).run()
        )
        self.addCleanup(task.cancel)
        self.assertTrue(await self._wait_until(
            lambda: any(u == UUID_CHAR_C for u, _ in adopted.writes)))

        adopted.emit_notification(UUID_CHAR_B, DEEP_SLEEP_PACKET)

        self.assertTrue(await self._wait_until(
            lambda: FakeBleakClient.last_instance is not None and FakeBleakClient.last_instance is not adopted),
            "tras el deep sleep se reusó la conexión vieja en vez de reconectar")
        self.assertFalse(task.done(), "la sesión debía seguir viva tras reconectar")


if __name__ == "__main__":
    unittest.main()
