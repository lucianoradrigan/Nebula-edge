"""Convierte el modelo a TFLite (int8, o float32 con --float) y reporta la memoria.

    .venv/bin/python convert.py --model model_cwru_wide --float

Los dos números que deciden si el modelo cabe en el ESP32:

  - TAMAÑO DEL MODELO: va en flash. Cada ranura del modelo (model_a y model_b
    en partitions.csv) tiene 512 KB, así que esto rara vez es el límite. El
    array C (<model>_data.cc) es de antes de INFER, cuando el modelo iba
    compilado en el firmware; hoy viaja por MQTT (export_model.py).

  - ARENA: el buffer contiguo de RAM donde TFLM mantiene las activaciones
    intermedias durante la inferencia. Este SÍ es el límite. En la IM-V2 el
    bloque contiguo libre medido va de 40 a 68 KB según qué protocolo esté
    arriba fragmentando el heap, así que el objetivo es quedar bajo 32 KB.

La estimación de arena que calcula este script es una COTA, no el número
real: TFLM reutiliza buffers de tensores cuyas vidas no se solapan, así que
el valor real suele ser menor. El número definitivo lo reporta el
dispositivo con arena_used_bytes() y es el que hay que creer.
"""

import argparse

import numpy as np
import tensorflow as tf


def representative_dataset(X_repr):
    def gen():
        for row in X_repr:
            yield [row.reshape(1, -1).astype(np.float32)]
    return gen


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", default="model",
                    help="prefijo de lo que dejó train.py: lee <model>.keras y <model>_test.npz")
    ap.add_argument("--float", action="store_true",
                    help="sin cuantizar: escribe <model>_f32.* en vez de <model>.*")
    args = ap.parse_args()
    src = args.model
    name = f"{src}_f32" if args.float else src

    model = tf.keras.models.load_model(f"{src}.keras")
    data = np.load(f"{src}_test.npz", allow_pickle=True)
    X_test, y_sev, y_fault = data["X"], data["y_sev"], data["y_fault"]

    converter = tf.lite.TFLiteConverter.from_keras_model(model)
    if not args.float:
        # Medido con el modelo de CWRU: int8 pierde 0.13 puntos de accuracy
        # pero el MAE de severidad sube de 0.132 a 0.222. En float32 da
        # exactamente lo mismo que Keras, a cambio de ~7 KB más de flash y
        # ~11 KB más de cota de arena.
        converter.optimizations = [tf.lite.Optimize.DEFAULT]
        converter.representative_dataset = representative_dataset(data["X_repr"])
        converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    # La entrada se deja en float32: el ESP32 calcula las features en float
    # y así no tiene que cuantizarlas a mano antes de invocar. La salida
    # también, para no desescalar en C. Los PESOS y las operaciones sí van
    # en int8, que es de donde viene el ahorro.
    tflite_model = converter.convert()

    open(f"{name}.tflite", "wb").write(tflite_model)

    # ---- Evaluación int8: el segundo de los tres puntos de medición ----
    interpreter = tf.lite.Interpreter(model_content=tflite_model)
    interpreter.allocate_tensors()
    inp = interpreter.get_input_details()[0]
    outs = interpreter.get_output_details()

    # El conversor NO respeta el orden en que se declararon las salidas en
    # Keras: acá se declaró [severity, fault] y emitió [fault, severity]. Se
    # identifican por FORMA, que es lo único estable: la cabeza de severidad
    # es la de 1 elemento, la de clasificación la de n_classes.
    i_sev = next(i for i, o in enumerate(outs) if int(o["shape"][-1]) == 1)
    i_fault = next(i for i, o in enumerate(outs) if int(o["shape"][-1]) > 1)

    preds_sev, preds_fault = [], []
    for row in X_test:
        interpreter.set_tensor(inp["index"], row.reshape(1, -1).astype(np.float32))
        interpreter.invoke()
        preds_sev.append(interpreter.get_tensor(outs[i_sev]["index"]).ravel()[0])
        preds_fault.append(interpreter.get_tensor(outs[i_fault]["index"]).ravel())

    acc = float(np.mean(np.argmax(preds_fault, axis=1) == y_fault))
    mae = float(np.mean(np.abs(np.array(preds_sev) - y_sev)))

    sev_f32, fault_f32 = model.predict(X_test, verbose=0)
    acc_f32 = float(np.mean(np.argmax(fault_f32, axis=1) == y_fault))
    mae_f32 = float(np.mean(np.abs(sev_f32.ravel() - y_sev)))

    # ---- Memoria ----
    arena = sum(int(np.prod(t["shape"])) * np.dtype(t["dtype"]).itemsize
                for t in interpreter.get_tensor_details() if t["shape"].size)

    print("=" * 58)
    print(f"{'ORDEN DE LAS SALIDAS (verificar en C)':<58}")
    print("=" * 58)
    for i, o in enumerate(outs):
        papel = "severidad" if i == i_sev else "clasificador"
        print(f"  output({i}) -> {papel:<14} shape {[int(s) for s in o['shape']]}")
    print(f"\n  EN C: output({i_sev}) = severidad, output({i_fault}) = clases")

    print("\n" + "=" * 58)
    print(f"{'':<22}{'keras':>12}{'tflite':>12}{'delta':>12}")
    print("-" * 58)
    print(f"{'accuracy':<22}{acc_f32*100:>11.2f}%{acc*100:>11.2f}%{(acc-acc_f32)*100:>+11.2f}")
    print(f"{'MAE severidad':<22}{mae_f32:>12.4f}{mae:>12.4f}{mae-mae_f32:>+12.4f}")
    print("=" * 58)
    print(f"\nMEMORIA")
    print(f"  modelo .tflite        : {len(tflite_model):>7,} B   -> flash (ranura de 512 KB)")
    print(f"  cota de arena         : {arena:>7,} B   -> RAM contigua")
    print(f"  presupuesto del nodo  : {32*1024:>7,} B   (objetivo bajo 68 KB contiguos)")
    print(f"  margen                : {32*1024-arena:>7,} B   {'OK' if arena < 32*1024 else 'EXCEDIDO'}")

    # ---- Array C ----
    # Para compilar el modelo DENTRO del firmware. Con INFER el modelo llega
    # por MQTT y esto no hace falta, pero sirve para probar TFLM en la placa
    # sin depender de la transferencia.
    with open(f"{name}_data.cc", "w") as f:
        f.write("// Generado por convert.py. No editar a mano.\n")
        f.write("// alignas(16) lo exige TFLM para leer el flatbuffer.\n\n")
        f.write(f'#include "{name}_data.h"\n\n')
        f.write(f"alignas(16) const unsigned char g_model_data[] = {{\n")
        for i in range(0, len(tflite_model), 12):
            f.write("    " + " ".join(f"0x{b:02x}," for b in tflite_model[i:i+12]) + "\n")
        f.write("};\n")
        f.write(f"const unsigned int g_model_data_len = {len(tflite_model)};\n")

    mean, std = data["feat_mean"], data["feat_std"]
    with open(f"{name}_data.h", "w") as f:
        f.write("#pragma once\n\n")
        f.write("// Generado por convert.py. No editar a mano.\n\n")
        f.write("extern const unsigned char g_model_data[];\n")
        f.write("extern const unsigned int g_model_data_len;\n\n")
        f.write(f"#define MODEL_N_FEATURES {len(mean)}\n")
        f.write(f"#define MODEL_N_CLASSES  {int(outs[i_fault]['shape'][-1])}\n\n")
        f.write(f"// Orden de salidas del conversor, VERIFICADO:\n")
        f.write(f"#define MODEL_OUTPUT_SEVERITY {i_sev}\n")
        f.write(f"#define MODEL_OUTPUT_FAULT    {i_fault}\n\n")
        f.write("// Estandarización: (feature - mean) / std antes de invocar.\n")
        f.write("extern const float g_feat_mean[MODEL_N_FEATURES];\n")
        f.write("extern const float g_feat_std[MODEL_N_FEATURES];\n")

    with open(f"{name}_data.cc", "a") as f:
        f.write("\nconst float g_feat_mean[MODEL_N_FEATURES] = {\n")
        f.write("    " + ", ".join(f"{v:.8e}f" for v in mean) + "\n};\n")
        f.write("\nconst float g_feat_std[MODEL_N_FEATURES] = {\n")
        f.write("    " + ", ".join(f"{v:.8e}f" for v in std) + "\n};\n")

    print(f"\nGenerados: {name}.tflite, {name}_data.cc, {name}_data.h")


if __name__ == "__main__":
    main()
