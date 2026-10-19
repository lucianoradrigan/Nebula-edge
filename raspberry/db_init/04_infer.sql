-- Modo INFER (protocol_conf = 4): catálogo de modelos y resultados.
--
-- Idempotente a propósito. En una base nueva lo corre Postgres solo, después
-- de 01-03, como todo lo de db_init. Pero db_init se ejecuta UNA vez, al crear
-- el volumen: en una base que ya existe hay que correrlo a mano, y tiene que
-- poder repetirse sin romper nada:
--
--   docker compose exec -T db psql -U nebulaedge -d nebulaedge < db_init/04_infer.sql
--
-- Sin esto el servidor no arranca sesiones: get_config() lee model_version e
-- infer_source.

ALTER TABLE nebulaedge_schema.config
    ADD COLUMN IF NOT EXISTS model_version INT,     -- modelo que debe tener activo en INFER; NULL/0 = ninguno
    ADD COLUMN IF NOT EXISTS infer_source INT;      -- 0 = features del dataset (C1), 1 = ventanas del dataset (C2), 2 = sensores

-- Modelos entrenados disponibles para transferir. Un registro por versión; una
-- versión no se modifica nunca: un modelo nuevo es una versión nueva. Así un
-- resultado de inference_result siempre se puede atribuir al modelo exacto que
-- lo produjo.
CREATE TABLE IF NOT EXISTS nebulaedge_schema.model (
    model_version INT PRIMARY KEY,
    tflite BYTEA NOT NULL,          -- el .tflite int8 completo
    size_bytes INT NOT NULL,
    sha256 CHAR(64) NOT NULL,       -- hex; el device lo verifica antes de activarlo
    n_features INT NOT NULL,
    n_classes INT NOT NULL,
    class_names TEXT[],             -- en el orden de las salidas del clasificador
    window_size INT NOT NULL,
    fs_hz FLOAT NOT NULL,
    feature_set_version INT NOT NULL,   -- qué versión de ml/features.py espera
    output_severity_index INT NOT NULL, -- orden de salidas VERIFICADO por convert.py
    output_fault_index INT NOT NULL,
    feat_mean FLOAT[] NOT NULL,     -- estandarización: viaja con el modelo
    feat_std FLOAT[] NOT NULL,
    test_input FLOAT[] NOT NULL,    -- autotest: features sin estandarizar
    test_output FLOAT[] NOT NULL,   -- [severidad, prob_0..]: lo que dio el int8 en Python
    trained_on TEXT,                -- de qué datos y con qué split salió
    created_at TIMESTAMP DEFAULT (now() AT TIME ZONE 'utc')
);

-- El set de test que el servidor le manda al device en las fases C1 y C2: una
-- fila por ventana, con la verdad. Se carga con ml/export_testset.py.
-- `features` va SIN estandarizar (C1); `window` son las muestras crudas (C2).
CREATE TABLE IF NOT EXISTS nebulaedge_schema.infer_sample (
    dataset VARCHAR(32),            -- p.ej. 'cwru_3hp': la carga que el modelo no vio
    sample_index INT,
    source_file VARCHAR(64),
    window_index INT,               -- posición de la ventana dentro de su grabación
    true_fault INT,
    true_severity FLOAT,            -- ya en 0..1, como la entrega el modelo
    rpm FLOAT,
    fs_hz FLOAT,
    features FLOAT[],
    "window" FLOAT[],
    PRIMARY KEY (dataset, sample_index)
);

-- Una fila por InferRequest. Se inserta al enviar el pedido, con la verdad
-- conocida, y se completa al llegar el InferResult. Las que quedan con pred_*
-- en NULL son pedidos sin respuesta: la pérdida se mide sola.
CREATE TABLE IF NOT EXISTS nebulaedge_schema.inference_result (
    id_device VARCHAR(45),
    request_id BIGINT,
    model_version INT,
    input_kind INT,                 -- 0 = features (C1), 1 = ventana cruda (C2), 2 = sensores
    source_file VARCHAR(64),        -- grabación de origen, p.ej. 'B007_3.mat'
    window_index INT,
    true_fault INT,                 -- NULL con entrada de sensores reales
    true_severity FLOAT,
    pred_fault INT,
    pred_severity FLOAT,
    probs FLOAT[],
    feature_us INT,                 -- tiempo de features en el device
    inference_us INT,               -- tiempo del modelo en el device
    config_version_applied INT,
    time_sent TIMESTAMP,            -- servidor, al publicar el pedido
    time_client TIMESTAMP,          -- device, al terminar
    time_server TIMESTAMP,          -- servidor, al recibir el resultado
    PRIMARY KEY (id_device, request_id)
);

-- Columnas de la fase C. ALTER y no en el CREATE de arriba para que una base
-- que ya tenía inference_result (creada en la fase A) también las reciba.
ALTER TABLE nebulaedge_schema.inference_result
    ADD COLUMN IF NOT EXISTS run_id BIGINT,             -- una corrida = un recorrido del set de test
    ADD COLUMN IF NOT EXISTS dataset VARCHAR(32),
    ADD COLUMN IF NOT EXISTS sample_index INT,
    ADD COLUMN IF NOT EXISTS status INT,                -- InferStatus; NULL = sin respuesta
    ADD COLUMN IF NOT EXISTS features_device FLOAT[];   -- C2: las features que calculó el device

-- El set de test de cada modelo. Viaja con el modelo y no fijo en el
-- servidor: un modelo entrenado con otros datos trae su propio set. NULL = no
-- hay set; el modelo se transfiere igual, pero el servidor no corre nada.
ALTER TABLE nebulaedge_schema.model
    ADD COLUMN IF NOT EXISTS test_dataset VARCHAR(32);  -- infer_sample.dataset

-- Resultados SIN pedido: con infer_source = 2 el device clasifica las ventanas
-- de sus propios sensores y publica cada resultado con request_id = 0. No hay
-- request_id que sirva de clave, así que la clave pasa a ser un id propio, y
-- (id_device, request_id) queda único solo donde hay pedido. Los resultados
-- sin pedido se guardan con request_id, run_id y true_* en NULL.
ALTER TABLE nebulaedge_schema.inference_result
    ADD COLUMN IF NOT EXISTS id BIGSERIAL;
DO $$
BEGIN
    -- Solo si la clave todavía es la vieja: así el archivo se puede repetir.
    IF EXISTS (SELECT 1 FROM pg_constraint
               WHERE conrelid = 'nebulaedge_schema.inference_result'::regclass
                 AND contype = 'p'
                 AND pg_get_constraintdef(oid) LIKE '%request_id%') THEN
        ALTER TABLE nebulaedge_schema.inference_result DROP CONSTRAINT inference_result_pkey;
        ALTER TABLE nebulaedge_schema.inference_result ALTER COLUMN request_id DROP NOT NULL;
        ALTER TABLE nebulaedge_schema.inference_result ADD PRIMARY KEY (id);
    END IF;
END $$;
CREATE UNIQUE INDEX IF NOT EXISTS inference_result_request
    ON nebulaedge_schema.inference_result (id_device, request_id)
    WHERE request_id IS NOT NULL;
