"""Tests de integración de la sesión MQTT (ProtocolSession + MqttTransport).

No se conecta a ningún broker: se reemplazan las funciones de mqtt_client.py que usa
el transporte (mqtt_start / get_data_queue / get_ack_queue / mqtt_publish) por
dobles, y el "device" se simula poniendo bytes en las colas.

    cd raspberry/server && python -m unittest discover -s tests
"""
from __future__ import annotations
import asyncio
import queue
import unittest
from unittest import mock

import sessions
import transport
from codec import DataCodec
from models import Timeouts
from tests.fakes import (
    DEEP_SLEEP_PACKET, DEVICE_ID, FakeBLEDevice, FakeRepo,
    config_ack_packet, data_1_packet, make_config,
)


def quick_timeouts(**overrides) -> Timeouts:
    base = dict(
        config_poll_sec=0.2,
        no_data_grace_sec=3.0,
        config_ack_sec=1.0,
        config_ack_retries=3,
        # Alto a propósito: estos tests no son sobre la resincronización de la
        # hora, y con un valor chico el reenvío de la config vigente se mezcla
        # con las configs que sí están midiendo. El resync tiene su propio test.
        clock_resync_sec=3600.0,
    )
    base.update(overrides)
    return Timeouts(**base)


class MqttSessionTests(unittest.IsolatedAsyncioTestCase):

    def setUp(self):
        # Colas del device (las llenaría el hilo de paho al llegar mensajes)
        self.data_queue: queue.Queue = queue.Queue()
        self.ack_queue: queue.Queue = queue.Queue()
        # (tópico, payload, retain). El retain se guarda porque es parte del
        # contrato: la config va retenida para que el device la reciba aunque
        # todavía no estuviera suscrito cuando se publicó. Ver mqtt_publish().
        self.published: list[tuple[str, bytes, bool]] = []

        patches = [
            mock.patch.object(transport, "mqtt_start", lambda: None),
            mock.patch.object(transport, "get_data_queue", lambda _id: self.data_queue),
            mock.patch.object(transport, "get_ack_queue", lambda _id: self.ack_queue),
            mock.patch.object(transport, "mqtt_publish",
                              lambda topic, data, retain=False:
                                  self.published.append((topic, data, retain))),
        ]
        for p in patches:
            p.start()
            self.addCleanup(p.stop)

    def _build_session(self, repo, timeouts):
        return sessions.MQTTDeviceSession(
            FakeBLEDevice().address, make_config(1), repo, timeouts,
        )

    async def _wait_until(self, predicate, timeout=5.0):
        loop = asyncio.get_running_loop()
        deadline = loop.time() + timeout
        while loop.time() < deadline:
            if predicate():
                return True
            await asyncio.sleep(0.05)
        return False

    def test_mqtt_session_uses_generic_session(self):
        self.assertTrue(issubclass(sessions.MQTTDeviceSession, sessions.ProtocolSession))
        self.assertEqual(sessions.MQTTDeviceSession.transport_cls.__name__, "MqttTransport")

    async def test_telemetry_from_data_queue_is_inserted(self):
        repo = FakeRepo(db_version=1)
        task = asyncio.create_task(self._build_session(repo, quick_timeouts()).run())
        self.addCleanup(task.cancel)

        self.data_queue.put(data_1_packet(applied_version=1))

        self.assertTrue(await self._wait_until(lambda: len(repo.data_1) >= 1))
        self.assertEqual(repo.data_1[0].id_device, DEVICE_ID)
        self.assertFalse(task.done())

    async def test_new_config_is_published_and_ack_arrives_on_its_own_queue(self):
        """Lo propio de MQTT: el ACK NO viene por el mismo canal que la
        telemetría, sino por el tópico .../config/ack (otra cola). El
        transporte las fusiona para que la sesión no tenga que saberlo."""
        repo = FakeRepo(db_version=1)
        task = asyncio.create_task(self._build_session(repo, quick_timeouts()).run())
        self.addCleanup(task.cancel)

        self.data_queue.put(data_1_packet(applied_version=1))
        self.assertTrue(await self._wait_until(lambda: len(repo.data_1) >= 1))

        # Cambia la config en la BD y llega telemetría con la versión vieja
        repo.db_version = 2
        self.data_queue.put(data_1_packet(applied_version=1))

        self.assertTrue(await self._wait_until(lambda: len(self.published) >= 1),
                        "no se publicó la config nueva")
        topic, payload, retain = self.published[0]
        self.assertTrue(retain, "la config tiene que ir retenida: ver mqtt_publish()")
        self.assertEqual(topic, f"/topic/nebulaedge/{DEVICE_ID}/config")
        pushed = DataCodec.deserialize_config(payload)
        self.assertIsNotNone(pushed)
        self.assertEqual(pushed.config_version, 2)

        # El device confirma por la cola de ACK
        self.ack_queue.put(config_ack_packet(version=2))

        result = await asyncio.wait_for(task, timeout=5.0)
        self.assertIsNotNone(result)
        self.assertEqual(result.config_version, 2)

    async def test_proactive_push_without_telemetry(self):
        """MQTT puede publicar aunque el device no haya mandado nada
        (can_send siempre True: el tópico existe igual)."""
        repo = FakeRepo(db_version=2)   # BD adelantada desde el arranque
        task = asyncio.create_task(self._build_session(repo, quick_timeouts()).run())
        self.addCleanup(task.cancel)

        self.assertTrue(await self._wait_until(lambda: len(self.published) >= 1),
                        "no se empujó la config sin telemetría entrante")
        pushed = DataCodec.deserialize_config(self.published[0][1])
        self.assertEqual(pushed.config_version, 2)

        self.ack_queue.put(config_ack_packet(version=2))
        result = await asyncio.wait_for(task, timeout=5.0)
        self.assertEqual(result.config_version, 2)

    async def test_queues_left_by_a_previous_session(self):
        """Las colas viven más que la sesión: lo que quedó de antes se trata distinto.

        Un ACK viejo no puede confirmar la config que empuja ESTA sesión, aunque
        coincida la versión: no es la respuesta a este envío. La telemetría que
        esperaba, en cambio, son mediciones reales y se guardan.
        """
        self.ack_queue.put(config_ack_packet(version=2))        # de otra sesión
        self.data_queue.put(data_1_packet(applied_version=1))   # llegó entre sesiones

        repo = FakeRepo(db_version=2)
        task = asyncio.create_task(self._build_session(repo, quick_timeouts()).run())
        self.addCleanup(task.cancel)

        self.assertTrue(await self._wait_until(lambda: len(repo.data_1) >= 1),
                        "se perdió la telemetría que esperaba en cola")
        self.assertTrue(await self._wait_until(lambda: len(self.published) >= 1))
        await asyncio.sleep(0.3)
        self.assertFalse(task.done(), "un ACK de una sesión anterior confirmó la config de ésta")

        self.ack_queue.put(config_ack_packet(version=2))        # el de este envío
        result = await asyncio.wait_for(task, timeout=5.0)
        self.assertEqual(result.config_version, 2)

    async def test_database_behind_the_device_does_not_close_the_session(self):
        """La BD retrocedió (v1) y el device sigue con la v5 que ya aplicó.

        Antes cada paquete cerraba la sesión "para reconfigurar"; al reabrir la
        BD seguía igual, así que se cerraba de nuevo: 157 veces en 2,5 min en
        banco, perdiendo un tercio de los paquetes. Tiene que seguir
        recibiendo, sin empujar nada (el device rechaza una config más vieja).
        """
        repo = FakeRepo(db_version=1)
        task = asyncio.create_task(self._build_session(repo, quick_timeouts()).run())
        self.addCleanup(task.cancel)

        for _ in range(3):
            self.data_queue.put(data_1_packet(applied_version=5))

        self.assertTrue(await self._wait_until(lambda: len(repo.data_1) >= 3),
                        "dejó de guardar telemetría")
        await asyncio.sleep(0.3)
        self.assertFalse(task.done(), "la sesión se cerró por una BD más vieja que el device")
        self.assertEqual(self.published, [], "no hay que empujarle una config más vieja")

    async def test_deep_sleep_does_not_close_mqtt_session(self):
        """Sin conexión que cortar: se sigue escuchando el tópico."""
        repo = FakeRepo(db_version=1)
        task = asyncio.create_task(self._build_session(repo, quick_timeouts()).run())
        self.addCleanup(task.cancel)

        self.data_queue.put(DEEP_SLEEP_PACKET)
        await asyncio.sleep(0.5)
        self.assertFalse(task.done(), "un deep sleep no debía cerrar la sesión MQTT")

        # Sigue procesando telemetría después
        self.data_queue.put(data_1_packet(applied_version=1))
        self.assertTrue(await self._wait_until(lambda: len(repo.data_1) >= 1))

    async def test_closes_on_timeout_without_messages(self):
        repo = FakeRepo(db_version=1)
        timeouts = quick_timeouts(no_data_grace_sec=1.0)
        result = await asyncio.wait_for(
            self._build_session(repo, timeouts).run(), timeout=15.0,
        )
        self.assertIsNone(result)


if __name__ == "__main__":
    unittest.main()
