"""Arma el INSERT que carga un modelo convertido en la tabla `model` del servidor.

    .venv/bin/python export_model.py --model model_cwru --version 1
    # en la Raspberry, desde raspberry/:
    docker compose exec -T db psql -U nebulaedge -d nebulaedge < model_cwru_v1.sql

Por qué un .sql y no conectarse a la base desde acá: la base vive en la
Raspberry y la contraseña en su .env. Un archivo se copia y se revisa antes de
correrlo, y no hace falta abrirle el puerto de Postgres al Mac.

Lee lo que dejaron train.py (<model>_test.npz) y convert.py (<model>.tflite).
De paso arma el AUTOTEST del device: una fila del set de test, sin
estandarizar, y lo que el modelo int8 responde para ella. El device corre la
misma fila al terminar de bajar el modelo y lo rechaza si no da lo mismo.

Es un INSERT simple a propósito, sin ON CONFLICT: una versión no se pisa
nunca. Si ya existe, psql falla, y eso es lo correcto (ver 04_infer.sql).
"""

import argparse
import hashlib

import numpy as np
import tensorflow as tf

import features


def run_tflite(tflite_model, x_std):
    """Corre una fila ya estandarizada. Devuelve (salidas, i_sev, i_fault)."""
    interpreter = tf.lite.Interpreter(model_content=tflite_model)
    interpreter.allocate_tensors()
    inp = interpreter.get_input_details()[0]
    outs = interpreter.get_output_details()
    # Igual que en convert.py: el orden de salidas se identifica por forma.
    i_sev = next(i for i, o in enumerate(outs) if int(o["shape"][-1]) == 1)
    i_fault = next(i for i, o in enumerate(outs) if int(o["shape"][-1]) > 1)

    interpreter.set_tensor(inp["index"], x_std.reshape(1, -1).astype(np.float32))
    interpreter.invoke()
    sev = float(interpreter.get_tensor(outs[i_sev]["index"]).ravel()[0])
    probs = [float(p) for p in interpreter.get_tensor(outs[i_fault]["index"]).ravel()]
    return [sev] + probs, i_sev, i_fault


def sql_floats(values):
    return "ARRAY[" + ", ".join(f"{float(v):.9g}" for v in values) + "]::float[]"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", default="model_cwru")
    ap.add_argument("--tflite", default=None,
                    help="el .tflite a cargar; por defecto <model>.tflite (p.ej. model_cwru_f32.tflite)")
    ap.add_argument("--version", type=int, required=True,
                    help="model_version en la tabla; una versión no se reusa")
    ap.add_argument("--trained-on", default="CWRU acc_x; train 0-1hp, val 2hp, test 3hp")
    args = ap.parse_args()

    tflite_path = args.tflite or f"{args.model}.tflite"
    tflite_model = open(tflite_path, "rb").read()
    data = np.load(f"{args.model}_test.npz", allow_pickle=True)
    mean, std = data["feat_mean"], data["feat_std"]
    classes = [str(c) for c in data["classes"]]

    test_input = data["X_raw"][0]
    test_output, i_sev, i_fault = run_tflite(tflite_model, (test_input - mean) / std)

    sha256 = hashlib.sha256(tflite_model).hexdigest()
    class_names = "ARRAY[" + ", ".join(f"'{c}'" for c in classes) + "]::text[]"

    sql = f"""-- Generado por ml/export_model.py. No editar a mano.
-- {tflite_path}, {len(tflite_model)} B, sha256 {sha256}
INSERT INTO nebulaedge_schema.model (
    model_version, tflite, size_bytes, sha256, n_features, n_classes, class_names,
    window_size, fs_hz, feature_set_version, output_severity_index, output_fault_index,
    feat_mean, feat_std, test_input, test_output, trained_on
) VALUES (
    {args.version},
    '\\x{tflite_model.hex()}'::bytea,
    {len(tflite_model)},
    '{sha256}',
    {len(mean)},
    {len(classes)},
    {class_names},
    {features.WINDOW_SIZE},
    {float(data["fs_hz"])},
    {features.FEATURE_SET_VERSION},
    {i_sev},
    {i_fault},
    {sql_floats(mean)},
    {sql_floats(std)},
    {sql_floats(test_input)},
    {sql_floats(test_output)},
    '{args.trained_on}'
);
"""
    out = f"{tflite_path.removesuffix('.tflite')}_v{args.version}.sql"
    open(out, "w").write(sql)

    print(f"{out}")
    print(f"  modelo      : {len(tflite_model):,} B, sha256 {sha256[:16]}...")
    print(f"  salidas     : severidad = output({i_sev}), clases = output({i_fault})")
    print(f"  autotest    : severidad {test_output[0]:.4f}, "
          f"clase {classes[int(np.argmax(test_output[1:]))]} "
          f"(verdad: {classes[int(data['y_fault'][0])]})")


if __name__ == "__main__":
    main()
