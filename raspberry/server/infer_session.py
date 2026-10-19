"""La sesión INFER: el device baja un modelo por MQTT e infiere.

Vivía en sessions.py, donde era más de un tercio del archivo. Es una
ProtocolSession más -misma apertura, mismo handshake de config con
reintentos, mismo empuje proactivo- pero con su propio bucle: en INFER no hay
telemetría, sino transferencia del modelo y pedidos de inferencia. Ver la
documentación de la clase.
"""
from __future__ import annotations
import time

from models import (
    ConfigData, ModelInfo, ModelChunkRequestData, ModelStatusData, ModelState,
    InferSample, InferResultData, InferStatus, InferInput,
)
from codec import DataCodec
from system import log
from sessions import ProtocolSession
from transport import InferTransport


class InferDeviceSession(ProtocolSession):
    """Sesión INFER: MQTT sin telemetría; el device baja un modelo e infiere.

    Comparte con ProtocolSession la apertura, el handshake de config con
    reintentos y el empuje proactivo. Lo que reemplaza es el bucle: en vez de
    telemetría llegan dos cosas, ambas por la cola de datos y con byte de tipo:

        ModelChunkRequest  -> se responde con el trozo pedido
        ModelStatus        -> prueba de vida; trae config_version_applied

    FASE A (esto): transferencia. Los pedidos y resultados de inferencia
    (fase C) se suman acá mismo.

    POR QUÉ LA BD SE CONSULTA POR RELOJ Y NO "CUANDO NO LLEGA NADA"
        En ProtocolSession el empuje proactivo corre solo si pasa
        `config_poll_sec` sin paquetes. Durante una transferencia los pedidos
        de trozos llegan sin pausa, así que con esa regla un cambio de config
        no se vería hasta terminar de bajar el modelo -o nunca, si el device
        se queda pidiendo-. Acá se consulta cada `config_poll_sec` llegue lo
        que llegue.
    """
    transport_cls = InferTransport

    # Bytes de modelo por trozo. Lo decide el servidor y viaja en el
    # manifiesto, así que se puede cambiar sin tocar el firmware. 2 KB pasa
    # el buffer por defecto de esp-mqtt (1 KB): el firmware tiene que rearmar
    # los mensajes que llegan partidos en varios MQTT_EVENT_DATA.
    CHUNK_SIZE = 2048

    # Un device con un manifiesto viejo va a pedir trozos de otra versión
    # hasta enterarse. Se le republica el manifiesto, pero no por cada pedido.
    _MANIFEST_REPUBLISH_SEC = 2.0

    # FASE C: cuánto se espera cada respuesta. De dónde salen las ventanas lo
    # dice el modelo (ModelInfo.test_dataset): cada modelo trae su set de test.
    INFER_RESULT_TIMEOUT_SEC = 5.0
    # infer_source de la config -> input_kind de inference_result. Con
    # sensores (2) no hay corrida: el device clasifica sus propias ventanas y
    # publica los resultados sin pedido (ver _on_unsolicited_result).
    _INPUT_KIND = {0: InferInput.FEATURES, 1: InferInput.WINDOW}
    # Resultados sin pedido: uno por ventana, así que el log va resumido.
    _UNSOLICITED_LOG_EVERY = 100

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._model: ModelInfo | None = None
        self._n_chunks = 0
        self._manifest_sent_at: float | None = None

        # Corrida del set de test (fase C). Se recorre una ventana a la vez.
        self._device_ready = False      # READY con el modelo pedido ya cargado en TFLM
        self._run_samples: list[InferSample] | None = None
        self._run_id: int | None = None
        self._run_idx = 0
        self._run_done = False
        self._pending: tuple[int, float] | None = None     # (request_id, monotonic al enviar)
        self._last_request_id = 0
        self._unsolicited = 0

    async def _load_model(self) -> ModelInfo | None:
        version = self.config.model_version
        if not version:
            log(f"INFER: {self.device_id} no tiene model_version en su config; no hay nada que transferir")
            return None
        try:
            model = await self.database_repo.get_model_async(version)
        except Exception as e:
            log(f"INFER: error leyendo el modelo v{version} de la base: {e}")
            return None
        if model is None:
            log(f"INFER: la config de {self.device_id} pide el modelo v{version}, que no está en la tabla model")
        return model

    async def _send_manifest(self, tx: InferTransport) -> None:
        await tx.send_manifest(
            DataCodec.serialize_model_manifest(self.device_id, self._model, self.CHUNK_SIZE)
        )
        self._manifest_sent_at = time.monotonic()
        if self._model is None:
            log(f"INFER: manifiesto vacío publicado para {self.device_id}")
        else:
            log(f"INFER: manifiesto v{self._model.model_version} publicado para {self.device_id} "
                f"({len(self._model.tflite)} B en {self._n_chunks} trozos)")

    async def _serve_chunk(self, tx: InferTransport, req: ModelChunkRequestData) -> None:
        model = self._model
        valid = (
            model is not None
            and req.model_version == model.model_version
            and 0 <= req.chunk_index < self._n_chunks
        )
        if not valid:
            log(f"INFER: pedido inválido de {self.device_id}: trozo {req.chunk_index} de v{req.model_version} "
                f"(vigente: v{model.model_version if model else 0}, {self._n_chunks} trozos)")
            # Lo más probable es que el device tenga un manifiesto viejo.
            sent_at = self._manifest_sent_at
            if sent_at is None or time.monotonic() - sent_at >= self._MANIFEST_REPUBLISH_SEC:
                await self._send_manifest(tx)
            return

        await tx.send_chunk(DataCodec.serialize_model_chunk(model, req.chunk_index, self.CHUNK_SIZE))

    async def _on_status(self, st: ModelStatusData) -> "ConfigData | None":
        """Registra el estado. Devuelve una config si el device aplicó una más
        nueva por otro camino (canal de rescate BLE): hay que reconfigurar."""
        self._update_last_client_time(st)
        try:
            state = ModelState(st.state).name
        except ValueError:
            state = str(st.state)
        tflm = ""
        if st.arena_used_bytes:
            tflm = (f", TFLM: arena {st.arena_used_bytes} B, inferencia {st.invoke_us} us, "
                    f"autotest err {st.selftest_max_error:.2e}")
        log(f"INFER: {self.device_id} estado {state} v{st.model_version} "
            f"(trozos {st.chunks_received}/{self._n_chunks}, error {st.error}, "
            f"activo v{st.model_version_active}{tflm})")

        if st.config_version_applied > self.config.config_version:
            db_config = await self.database_repo.get_config_async(self.device_id)
            if db_config is not None and db_config.config_version == st.config_version_applied:
                log(f"INFER: {self.device_id} ya aplicó v{st.config_version_applied}. Cerrando sesión para reconfigurar.")
                return db_config

        # Listo para inferir = READY, con el modelo que le toca, y cargado en
        # TFLM (arena > 0). Sin lo último el device tendría el modelo en flash
        # pero no podría correrlo.
        self._device_ready = (
            st.state == ModelState.READY
            and self._model is not None
            and st.model_version_active == self._model.model_version
            and st.arena_used_bytes > 0
        )
        return None

    # ------------------------------------------------------- fase C: corrida

    async def _maybe_start_run(self) -> None:
        kind = self._INPUT_KIND.get(self.config.infer_source)
        if (kind is None or self._run_done or self._run_id is not None
                or not self._device_ready or self._model is None):
            return

        dataset = self._model.test_dataset
        if not dataset:
            log(f"INFER: el modelo v{self._model.model_version} no tiene test_dataset: no se corre nada")
            self._run_done = True
            return
        if self._run_samples is None:
            self._run_samples = await self.database_repo.get_infer_samples_async(dataset)
        if not self._run_samples:
            log(f"INFER: no hay ventanas del dataset {dataset} en infer_sample: no se corre nada")
            self._run_done = True
            return

        done = await self.database_repo.count_completed_inferences_async(
            self.device_id, self._model.model_version, kind, self.config.config_version, dataset)
        if done >= len(self._run_samples):
            log(f"INFER: {self.device_id} ya tiene una corrida completa de {dataset} con "
                f"v{self._model.model_version} y config v{self.config.config_version}: no se repite")
            self._run_done = True
            return

        self._run_id = int(time.time() * 1000)
        self._run_idx = 0
        log(f"INFER: corrida {self._run_id} empieza: {len(self._run_samples)} ventanas de {dataset} "
            f"({'features' if kind == 0 else 'ventanas crudas'}) a {self.device_id}, modelo v{self._model.model_version}")

    def _new_request_id(self) -> int:
        """Id único por device, también entre sesiones y reinicios del servidor.

        Es la hora en microsegundos, o el anterior + 1 si la hora no avanzó.
        Antes era run_id * 1000 + índice, que choca con la corrida siguiente
        en cuanto un set pasa de mil ventanas, y el UPDATE del resultado caía
        sobre la fila de la otra corrida.
        """
        self._last_request_id = max(self._last_request_id + 1, time.time_ns() // 1000)
        return self._last_request_id

    async def _send_next(self, tx: InferTransport) -> None:
        if self._run_idx >= len(self._run_samples):
            await self._finish_run()
            return
        sample = self._run_samples[self._run_idx]
        kind = self._INPUT_KIND[self.config.infer_source]
        request_id = self._new_request_id()
        await self.database_repo.insert_inference_request_async(
            self.device_id, request_id, self._run_id, self._model.model_version, kind,
            self.config.config_version, sample, time.time())
        await tx.send_infer_request(
            DataCodec.serialize_infer_request(request_id, self._model.model_version, sample, raw_window=kind == 1))
        self._pending = (request_id, time.monotonic())

    async def _finish_run(self) -> None:
        s = await self.database_repo.inference_run_summary_async(self._run_id)
        lost = s["sent"] - s["ok"]
        if s["ok"]:
            log(f"INFER: corrida {self._run_id} terminada en {self.device_id}: "
                f"accuracy {s['accuracy'] * 100:.2f}%, MAE severidad {s['mae']:.4f}, "
                f"{s['ok']}/{s['sent']} respondidas ({lost} perdidas), "
                f"inferencia {s['inference_us']:.0f} us, features {s['feature_us'] or 0:.0f} us, "
                f"ida y vuelta {s['rtt_s'] * 1000:.0f} ms")
        else:
            log(f"INFER: corrida {self._run_id} terminada en {self.device_id} sin respuestas válidas")
        self._run_done = True
        self._run_id = None

    async def _on_unsolicited_result(self, r: InferResultData) -> None:
        """El device clasificó una ventana suya (infer_source = 2). No hay
        pedido ni verdad: se guarda tal cual, fuera de toda corrida."""
        await self.database_repo.insert_unsolicited_inference_async(self.device_id, r, time.time())
        self._unsolicited += 1
        if self._unsolicited == 1 or self._unsolicited % self._UNSOLICITED_LOG_EVERY == 0:
            try:
                kind = InferInput(r.input_kind).name
            except ValueError:
                kind = str(r.input_kind)
            log(f"INFER: {self.device_id} lleva {self._unsolicited} resultados sin pedido ({kind}); "
                f"último: clase {r.pred_fault}, severidad {r.severity:.3f}, estado {r.status}")

    async def _on_infer_result(self, tx: InferTransport, r: InferResultData) -> None:
        if r.request_id == 0:
            await self._on_unsolicited_result(r)
            return
        if self._pending is None or r.request_id != self._pending[0]:
            return      # de un pedido que ya se dio por perdido
        await self.database_repo.complete_inference_async(self.device_id, r, time.time())
        self._pending = None

        if r.status != InferStatus.OK:
            # Sin modelo o con otro: no tiene sentido seguir mandando. La
            # corrida se abandona y empieza de nuevo cuando vuelva a estar listo.
            log(f"INFER: {self.device_id} no pudo inferir (estado {InferStatus(r.status).name}); se abandona la corrida")
            self._device_ready = False
            self._run_id = None
            return

        self._run_idx += 1
        if self._run_idx % 100 == 0:
            log(f"INFER: corrida {self._run_id}: {self._run_idx}/{len(self._run_samples)} ventanas")
        await self._send_next(tx)

    async def _run_tick(self, tx: InferTransport) -> None:
        """Avanza la corrida: la empieza si corresponde, da por perdido un
        pedido vencido y manda el siguiente si no hay ninguno en vuelo."""
        await self._maybe_start_run()
        if self._run_id is None:
            return
        if self._pending is not None:
            if time.monotonic() - self._pending[1] < self.INFER_RESULT_TIMEOUT_SEC:
                return
            log(f"INFER: sin respuesta al pedido {self._pending[0]} en {self.INFER_RESULT_TIMEOUT_SEC:.0f} s: "
                f"queda como perdido")
            self._pending = None
            self._run_idx += 1
        await self._send_next(tx)

    async def _replace_retained_config(self, tx: InferTransport) -> None:
        """Publica la config vigente en /config, retenida, para pisar la que
        haya quedado en el broker.

        POR QUÉ HACE FALTA
            La config de INFER suele entrar por BLE (descubrimiento) o por el
            transporte del protocolo anterior, no por el tópico /config de
            este device. Lo retenido ahí es entonces la última config que pasó
            por MQTT, que puede ser de hace semanas: medido en banco, en cada
            conexión el device recibía la v1310 estando en la v1311, y la
            rechazaba con un ACK negativo. Inofensivo, pero es un mensaje viejo
            que el broker le entrega para siempre.

            Reenviar la misma versión que el device ya tiene es idempotente:
            responde ACK positivo y la descarta (ver vTaskGetResponse() en
            main.c). Ese ACK llega a la cola y el bucle lo ignora.
        """
        db_config = await self.database_repo.get_config_async(self.device_id)
        if db_config is None or db_config.config_version != self.config.config_version:
            return      # la base ya cambió: el empuje proactivo se encarga
        await tx.send(DataCodec.serialize_config(db_config))

    async def _session_loop(self, tx: InferTransport) -> "ConfigData | None":
        await self._replace_retained_config(tx)
        self._model = await self._load_model()
        self._n_chunks = DataCodec.model_n_chunks(self._model, self.CHUNK_SIZE) if self._model else 0
        await self._send_manifest(tx)

        timeout_sec = self._sleep_timeout_sec()
        deadline = time.monotonic() + timeout_sec
        next_poll = time.monotonic() + self.timeouts.config_poll_sec

        while True:
            now = time.monotonic()
            if now >= deadline:
                log(f"Timeout {tx.name} ({timeout_sec}s) sin noticias de {self.device_id}. Cerrando sesión.")
                return None

            if now >= next_poll:
                next_poll = now + self.timeouts.config_poll_sec
                new_cfg = await self._proactive_config_push(lambda cfg: self._push_and_wait(tx, cfg))
                if new_cfg is not None:
                    return new_cfg

            await self._run_tick(tx)

            # Con un pedido de inferencia en vuelo se despierta a tiempo para su timeout.
            wake = min(deadline, next_poll)
            if self._pending is not None:
                wake = min(wake, self._pending[1] + self.INFER_RESULT_TIMEOUT_SEC)
            packet = await tx.recv(max(0.0, wake - time.monotonic()))
            if packet is None:
                continue

            data, data_type = DataCodec.deserialize_typed_packet(packet)
            if data is None:
                continue    # ACK suelto, o algo que no se pudo decodificar

            if data_type == DataCodec.TYPE_INFER_RESULT:
                deadline = time.monotonic() + timeout_sec
                await self._on_infer_result(tx, data)
            elif data_type == DataCodec.TYPE_MODEL_CHUNK_REQUEST:
                deadline = time.monotonic() + timeout_sec
                await self._serve_chunk(tx, data)
            elif data_type == DataCodec.TYPE_MODEL_STATUS:
                deadline = time.monotonic() + timeout_sec
                new_cfg = await self._on_status(data)
                if new_cfg is not None:
                    return new_cfg
            else:
                log(f"INFER: paquete tipo 0x{data_type:02x} de {self.device_id} ignorado (en INFER no hay telemetría)")
