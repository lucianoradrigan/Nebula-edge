"""Tests de la sesión INFER, fase A: transferencia del modelo.

Igual que test_mqtt_session.py: sin broker. Se reemplazan las funciones de
mqtt_client.py que usa el transporte y el "device" se simula poniendo bytes en
las colas.

    cd raspberry/server && python -m unittest discover -s tests
"""
from __future__ import annotations
import asyncio
import hashlib
import queue
import unittest
from unittest import mock

import sessions
import transport
from codec import DataCodec
from models import InferInput, ModelState, Timeouts
from protocol_dispatch import PROTOCOL_INFER
from tests.fakes import (
    DEVICE_ID, FakeRepo, config_ack_packet, data_1_packet,
    make_config, make_model, model_chunk_request_packet, model_status_packet,
    make_sample, infer_result_packet,
)

MANIFEST_TOPIC = f"/topic/nebulaedge/{DEVICE_ID}/model/manifest"
CHUNK_TOPIC = f"/topic/nebulaedge/{DEVICE_ID}/model/chunk"
CONFIG_TOPIC = f"/topic/nebulaedge/{DEVICE_ID}/config"


def quick_timeouts(**overrides) -> Timeouts:
    base = dict(
        config_poll_sec=0.2,
        no_data_grace_sec=3.0,
        config_ack_sec=1.0,
        config_ack_retries=3,
        clock_resync_sec=3600.0,
    )
    base.update(overrides)
    return Timeouts(**base)


class _InferHarness(unittest.IsolatedAsyncioTestCase):
    """Armado común: colas del device y dobles de MQTT. Sin tests propios."""

    def setUp(self):
        self.data_queue: queue.Queue = queue.Queue()
        self.ack_queue: queue.Queue = queue.Queue()
        # (tópico, payload, retain, qos)
        self.published: list[tuple[str, bytes, bool, int]] = []

        patches = [
            mock.patch.object(transport, "mqtt_start", lambda: None),
            mock.patch.object(transport, "get_data_queue", lambda _id: self.data_queue),
            mock.patch.object(transport, "get_ack_queue", lambda _id: self.ack_queue),
            mock.patch.object(transport, "mqtt_publish",
                              lambda topic, data, retain=False, qos=0:
                                  self.published.append((topic, data, retain, qos))),
        ]
        for p in patches:
            p.start()
            self.addCleanup(p.stop)

    def _start(self, repo, model_version=1, timeouts=None):
        session = sessions.InferDeviceSession(
            DEVICE_ID,
            make_config(1, protocol_conf=PROTOCOL_INFER, model_version=model_version),
            repo, timeouts or quick_timeouts(),
        )
        task = asyncio.create_task(session.run())
        self.addCleanup(task.cancel)
        return task

    def _on(self, topic):
        return [p for p in self.published if p[0] == topic]

    async def _wait_until(self, predicate, timeout=5.0):
        loop = asyncio.get_running_loop()
        deadline = loop.time() + timeout
        while loop.time() < deadline:
            if predicate():
                return True
            await asyncio.sleep(0.02)
        return False

class InferSessionTests(_InferHarness):
    """Fase A: transferencia del modelo."""

    def test_infer_is_protocol_4(self):
        self.assertEqual(PROTOCOL_INFER, 4)
        self.assertTrue(issubclass(sessions.InferDeviceSession, sessions.ProtocolSession))

    async def test_manifest_is_published_retained_on_start(self):
        model = make_model(version=1, size=5000)
        self._start(FakeRepo(db_version=1, models={1: model}))

        self.assertTrue(await self._wait_until(lambda: self._on(MANIFEST_TOPIC)))
        _, payload, retain, qos = self._on(MANIFEST_TOPIC)[0]
        self.assertTrue(retain, "el manifiesto va retenido, como la config")
        self.assertEqual(qos, 1)

        m = DataCodec.deserialize_model_manifest(payload)
        self.assertEqual(m.model_version, 1)
        self.assertEqual(m.size_bytes, 5000)
        self.assertEqual(m.chunk_size, sessions.InferDeviceSession.CHUNK_SIZE)
        self.assertEqual(m.n_chunks, 3)         # 2048 + 2048 + 904
        self.assertEqual(m.sha256, model.sha256)
        self.assertEqual(m.output_fault_index, 0)
        self.assertEqual(len(m.feat_mean), 20)

    async def test_current_config_replaces_the_retained_one_on_start(self):
        """Lo retenido en /config puede ser de un protocolo anterior: la sesión
        publica la vigente, retenida, para pisarla."""
        repo = FakeRepo(db_version=1, protocol_conf=PROTOCOL_INFER, model_version=1,
                        models={1: make_model()})
        self._start(repo)
        self.assertTrue(await self._wait_until(lambda: self._on(CONFIG_TOPIC)))
        _, payload, retain, _ = self._on(CONFIG_TOPIC)[0]
        self.assertTrue(retain)
        pushed = DataCodec.deserialize_config(payload)
        self.assertEqual((pushed.config_version, pushed.protocol_conf), (1, PROTOCOL_INFER))

    async def test_device_reassembles_the_exact_model_from_chunks(self):
        """Lo que importa de verdad: pidiendo los trozos uno por uno y
        escribiéndolos en su offset, el device arma el mismo archivo."""
        model = make_model(version=1, size=5000)
        self._start(FakeRepo(db_version=1, models={1: model}))
        self.assertTrue(await self._wait_until(lambda: self._on(MANIFEST_TOPIC)))
        n_chunks = DataCodec.deserialize_model_manifest(self._on(MANIFEST_TOPIC)[0][1]).n_chunks

        image = bytearray(5000)
        for i in range(n_chunks):
            self.data_queue.put(model_chunk_request_packet(1, i))
            self.assertTrue(await self._wait_until(lambda: len(self._on(CHUNK_TOPIC)) > i))
            _, payload, retain, qos = self._on(CHUNK_TOPIC)[i]
            self.assertFalse(retain, "un trozo no se retiene: es respuesta a un pedido")
            self.assertEqual(qos, 1)
            c = DataCodec.deserialize_model_chunk(payload)
            self.assertEqual(c.chunk_index, i)
            image[c.offset:c.offset + len(c.data)] = c.data

        self.assertEqual(hashlib.sha256(image).digest(), model.sha256)

    async def test_repeated_request_gets_the_same_chunk(self):
        """QoS 1 puede duplicar, y el device reintenta: pedir dos veces el
        mismo trozo tiene que dar los mismos bytes en el mismo offset."""
        self._start(FakeRepo(db_version=1, models={1: make_model()}))
        self.data_queue.put(model_chunk_request_packet(1, 1))
        self.data_queue.put(model_chunk_request_packet(1, 1))
        self.assertTrue(await self._wait_until(lambda: len(self._on(CHUNK_TOPIC)) == 2))
        self.assertEqual(self._on(CHUNK_TOPIC)[0][1], self._on(CHUNK_TOPIC)[1][1])

    async def test_invalid_requests_get_no_chunk_and_republish_manifest(self):
        self._start(FakeRepo(db_version=1, models={1: make_model(size=5000)}))
        self.assertTrue(await self._wait_until(lambda: len(self._on(MANIFEST_TOPIC)) == 1))

        # Fuera de rango, y de una versión que no es la vigente. Fuera de la
        # ventana de tope, cada uno republica el manifiesto.
        with mock.patch.object(sessions.InferDeviceSession, "_MANIFEST_REPUBLISH_SEC", 0.0):
            self.data_queue.put(model_chunk_request_packet(1, 3))
            self.data_queue.put(model_chunk_request_packet(9, 0))
            self.assertTrue(await self._wait_until(lambda: len(self._on(MANIFEST_TOPIC)) == 3))

        self.assertEqual(self._on(CHUNK_TOPIC), [])

    async def test_manifest_republish_is_rate_limited(self):
        """Un device con manifiesto viejo insiste: no se le republica en cada pedido."""
        self._start(FakeRepo(db_version=1, models={1: make_model()}))
        self.assertTrue(await self._wait_until(lambda: len(self._on(MANIFEST_TOPIC)) == 1))
        for _ in range(10):
            self.data_queue.put(model_chunk_request_packet(9, 0))
        await asyncio.sleep(0.3)
        self.assertEqual(len(self._on(MANIFEST_TOPIC)), 1,
                         "dentro de _MANIFEST_REPUBLISH_SEC del anterior no se republica")

    async def test_missing_model_publishes_empty_manifest(self):
        """La config pide un modelo que no está en la tabla: manifiesto vacío
        (model_version 0), para reemplazar cualquier retenido anterior."""
        task = self._start(FakeRepo(db_version=1, models={}), model_version=7)
        self.assertTrue(await self._wait_until(lambda: self._on(MANIFEST_TOPIC)))
        m = DataCodec.deserialize_model_manifest(self._on(MANIFEST_TOPIC)[0][1])
        self.assertEqual(m.model_version, 0)
        self.assertFalse(task.done(), "sin modelo la sesión sigue: el device puede seguir vivo")

    async def test_status_keeps_session_alive_without_telemetry(self):
        """En INFER no hay telemetría: el ModelStatus periódico es la prueba
        de vida. Sin él, el timeout cierra la sesión."""
        timeouts = quick_timeouts(no_data_grace_sec=1.0)
        task = self._start(FakeRepo(db_version=1, models={1: make_model()}), timeouts=timeouts)

        for _ in range(6):     # ~1,8 s, más que el timeout de 1 s
            self.data_queue.put(model_status_packet(ModelState.READY))
            await asyncio.sleep(0.3)
        self.assertFalse(task.done())

        result = await asyncio.wait_for(task, timeout=5.0)
        self.assertIsNone(result, "sin más status, la sesión cierra por timeout")

    async def test_config_change_is_seen_while_chunks_stream(self):
        """La BD se consulta por reloj: un pedido de trozos tras otro no puede
        tapar un cambio de config."""
        repo = FakeRepo(db_version=1, protocol_conf=PROTOCOL_INFER, model_version=1,
                        models={1: make_model(size=200_000)})
        task = self._start(repo)
        self.assertTrue(await self._wait_until(lambda: self._on(MANIFEST_TOPIC)))

        repo.db_version = 2
        repo.protocol_conf = 0      # vuelta a MQTT

        # La sesión ya republicó la v1 vigente al empezar (ver
        # test_current_config_replaces_the_retained_one_on_start): lo que
        # interesa acá es que aparezca la v2.
        def pushed_v2():
            return [c for c in (DataCodec.deserialize_config(p[1]) for p in self._on(CONFIG_TOPIC))
                    if c.config_version == 2]

        async def stream_requests():
            i = 0
            while not pushed_v2():
                self.data_queue.put(model_chunk_request_packet(1, i % 90))
                i += 1
                await asyncio.sleep(0.01)
        streamer = asyncio.create_task(stream_requests())
        self.addCleanup(streamer.cancel)

        self.assertTrue(await self._wait_until(lambda: pushed_v2()),
                        "el cambio de config no se empujó mientras llegaban pedidos")
        self.assertEqual(pushed_v2()[0].protocol_conf, 0)

        self.ack_queue.put(config_ack_packet(version=2))
        result = await asyncio.wait_for(task, timeout=5.0)
        self.assertEqual(result.protocol_conf, 0)

    async def test_status_with_newer_applied_config_closes_for_reconfig(self):
        """El device aplicó una config nueva por el canal de rescate BLE: su
        status lo dice y la sesión tiene que cerrar para reconfigurar."""
        repo = FakeRepo(db_version=2, models={1: make_model()})
        # Que el empuje proactivo no gane la carrera: poll lento.
        task = self._start(repo, timeouts=quick_timeouts(config_poll_sec=30.0))
        self.data_queue.put(model_status_packet(ModelState.READY, applied_version=2))
        result = await asyncio.wait_for(task, timeout=5.0)
        self.assertEqual(result.config_version, 2)

    async def test_telemetry_is_ignored_in_infer(self):
        repo = FakeRepo(db_version=1, models={1: make_model()})
        task = self._start(repo)
        self.data_queue.put(data_1_packet(applied_version=1))
        await asyncio.sleep(0.3)
        self.assertEqual(repo.data_1, [])
        self.assertFalse(task.done())


INFER_REQ_TOPIC = f"/topic/nebulaedge/{DEVICE_ID}/infer/req"


class InferRunTests(_InferHarness):
    """Fase C: el servidor recorre el set de test, una ventana a la vez."""

    def _run_repo(self, n_samples=3, **kw):
        repo = FakeRepo(db_version=1, models={1: make_model()}, **kw)
        repo.samples = [make_sample(i, true_fault=i % 4) for i in range(n_samples)]
        return repo

    def _ready(self):
        """El device dice READY, con v1 activo y cargado en TFLM."""
        self.data_queue.put(model_status_packet(ModelState.READY, active=1, arena=2528))

    def _requests(self):
        return [DataCodec.deserialize_infer_request(p[1]) for p in self._on(INFER_REQ_TOPIC)]

    async def test_run_waits_for_model_loaded_in_tflm(self):
        repo = self._run_repo()
        self._start(repo)
        # READY pero sin arena: el modelo está en flash, no cargado. No se manda nada.
        self.data_queue.put(model_status_packet(ModelState.READY, active=1, arena=0))
        await asyncio.sleep(0.4)
        self.assertEqual(self._requests(), [])
        self._ready()
        self.assertTrue(await self._wait_until(lambda: len(self._requests()) == 1))

    async def test_run_sends_features_one_at_a_time_and_records_results(self):
        repo = self._run_repo(n_samples=3)
        self._start(repo)
        self._ready()
        self.assertTrue(await self._wait_until(lambda: len(self._requests()) == 1))
        await asyncio.sleep(0.3)
        self.assertEqual(len(self._requests()), 1, "no se manda el siguiente sin respuesta")

        first = self._requests()[0]
        self.assertEqual(list(first.features), [pytest_approx(v) for v in make_sample(0).features])
        self.assertEqual(len(first.window), 0, "en C1 va solo el vector de features")
        self.assertEqual(first.model_version, 1)

        for i in range(3):
            req = self._requests()[i]
            self.data_queue.put(infer_result_packet(req.request_id, pred_fault=i % 4))
            if i < 2:
                self.assertTrue(await self._wait_until(lambda: len(self._requests()) == i + 2))

        self.assertTrue(await self._wait_until(
            lambda: all(r["result"] for r in repo.inference_rows.values()) and len(repo.inference_rows) == 3))
        await asyncio.sleep(0.2)
        self.assertEqual(len(self._requests()), 3, "terminado el set, no se manda nada más")

    async def test_run_raw_windows_in_c2(self):
        repo = self._run_repo(n_samples=1)
        repo.infer_source = 1       # que el empuje proactivo no vea un cambio
        session = sessions.InferDeviceSession(
            DEVICE_ID, make_config(1, protocol_conf=PROTOCOL_INFER, model_version=1),
            repo, quick_timeouts())
        session.config.infer_source = 1
        task = asyncio.create_task(session.run())
        self.addCleanup(task.cancel)
        self._ready()
        self.assertTrue(await self._wait_until(lambda: len(self._requests()) == 1))
        req = self._requests()[0]
        self.assertEqual((len(req.window), len(req.features)), (2048, 0))
        self.assertAlmostEqual(req.rpm, 1730.0)
        self.assertEqual(repo.inference_rows[req.request_id]["input_kind"], 1)

    async def test_run_marks_lost_after_timeout_and_moves_on(self):
        repo = self._run_repo(n_samples=2)
        with mock.patch.object(sessions.InferDeviceSession, "INFER_RESULT_TIMEOUT_SEC", 0.3):
            self._start(repo)
            self._ready()
            self.assertTrue(await self._wait_until(lambda: len(self._requests()) == 2, timeout=3.0),
                            "tras el timeout del primero tenía que salir el segundo")
        lost = repo.inference_rows[self._requests()[0].request_id]
        self.assertIsNone(lost["result"], "el perdido queda sin resultado: así se cuenta la pérdida")

    async def test_run_not_repeated_when_already_complete(self):
        repo = self._run_repo(n_samples=3)
        repo.completed_before = 3
        self._start(repo)
        self._ready()
        await asyncio.sleep(0.4)
        self.assertEqual(self._requests(), [])

    async def test_run_not_started_for_sensor_source(self):
        repo = self._run_repo(n_samples=3)
        session = sessions.InferDeviceSession(
            DEVICE_ID, make_config(1, protocol_conf=PROTOCOL_INFER, model_version=1),
            repo, quick_timeouts())
        session.config.infer_source = 2
        task = asyncio.create_task(session.run())
        self.addCleanup(task.cancel)
        self._ready()
        await asyncio.sleep(0.4)
        self.assertEqual(self._requests(), [])

    async def test_run_not_started_without_test_dataset(self):
        repo = self._run_repo(n_samples=3)
        repo.models = {1: make_model(test_dataset=None)}
        self._start(repo)
        self._ready()
        await asyncio.sleep(0.4)
        self.assertEqual(self._requests(), [])

    async def test_unsolicited_sensor_result_is_stored(self):
        repo = self._run_repo(n_samples=3)
        session = sessions.InferDeviceSession(
            DEVICE_ID, make_config(1, protocol_conf=PROTOCOL_INFER, model_version=1),
            repo, quick_timeouts())
        session.config.infer_source = 2
        task = asyncio.create_task(session.run())
        self.addCleanup(task.cancel)
        self._ready()
        for _ in range(2):
            self.data_queue.put(infer_result_packet(0, pred_fault=2, input_kind=InferInput.SENSOR))
        self.assertTrue(await self._wait_until(lambda: len(getattr(repo, "unsolicited_rows", [])) == 2))
        r = repo.unsolicited_rows[0]
        self.assertEqual((r.request_id, r.input_kind, r.pred_fault), (0, InferInput.SENSOR, 2))
        self.assertEqual(self._requests(), [], "con sensores el servidor no pide nada")

    async def test_unsolicited_result_does_not_disturb_a_run(self):
        repo = self._run_repo(n_samples=2)
        self._start(repo)
        self._ready()
        self.assertTrue(await self._wait_until(lambda: len(self._requests()) == 1))
        self.data_queue.put(infer_result_packet(0, pred_fault=1, input_kind=InferInput.SENSOR))
        self.data_queue.put(infer_result_packet(self._requests()[0].request_id, pred_fault=0))
        self.assertTrue(await self._wait_until(lambda: len(self._requests()) == 2))
        self.assertEqual(len(repo.unsolicited_rows), 1)

    async def test_run_abandoned_if_device_has_no_model(self):
        repo = self._run_repo(n_samples=3)
        self._start(repo)
        self._ready()
        self.assertTrue(await self._wait_until(lambda: len(self._requests()) == 1))
        self.data_queue.put(infer_result_packet(self._requests()[0].request_id, 0, status=1))
        await asyncio.sleep(0.4)
        self.assertEqual(len(self._requests()), 1, "sin modelo en el device no se sigue mandando")

    def test_request_ids_unique_even_if_clock_stalls(self):
        """Con run_id * 1000 + índice, la ventana 1000 de una corrida chocaba
        con la 0 de una corrida empezada 1 ms después."""
        session = sessions.InferDeviceSession(
            DEVICE_ID, make_config(1, protocol_conf=PROTOCOL_INFER, model_version=1),
            FakeRepo(1), quick_timeouts())
        with mock.patch.object(sessions.time, "time_ns", lambda: 1_735_000_000_000_000_000):
            ids = [session._new_request_id() for _ in range(2000)]
        self.assertEqual(len(set(ids)), 2000)
        self.assertNotIn(0, ids, "0 está reservado para los resultados sin pedido")


def pytest_approx(v):
    """float32 en el cable: se compara redondeado a lo que entra en 32 bits."""
    import struct
    return struct.unpack("f", struct.pack("f", v))[0]


class ConfigCodecInferTests(unittest.TestCase):

    def test_config_carries_model_fields(self):
        cfg = make_config(3, protocol_conf=PROTOCOL_INFER, model_version=5)
        back = DataCodec.deserialize_config(DataCodec.serialize_config(cfg))
        self.assertEqual((back.protocol_conf, back.model_version), (4, 5))


if __name__ == "__main__":
    unittest.main()
