"""Compara el port a C de las features (firmware) contra features.py, ventana por ventana.

    .venv/bin/python check_features_c.py

Compila esp32/components/nebulaedge_features en el Mac como biblioteca
compartida, le pasa las ventanas de CWRU de 3 hp y compara cada una de las 20
features con lo que da Python. Además corre el modelo con las dos versiones de
features y cuenta cuántas veces cambia la clase predicha: esa es la diferencia
que importa.

Va antes de flashear: un error del port acá se ve en un segundo; en la placa,
como una accuracy más baja sin explicación.
"""

import ctypes
import os
import subprocess
import tempfile

import numpy as np

import cwru
import features

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "esp32", "components", "nebulaedge_features")


def build_lib():
    out = os.path.join(tempfile.mkdtemp(), "libfeatures.dylib")
    subprocess.run(["cc", "-O2", "-shared", "-fPIC", "-I", os.path.join(SRC, "include"),
                    os.path.join(SRC, "nebulaedge_features.c"), "-o", out], check=True)
    lib = ctypes.CDLL(out)
    lib.features_extract.argtypes = [ctypes.POINTER(ctypes.c_float), ctypes.c_size_t,
                                     ctypes.c_float, ctypes.c_float, ctypes.POINTER(ctypes.c_float)]
    lib.features_extract.restype = ctypes.c_int
    return lib


def main():
    lib = build_lib()
    ds = cwru.load_windows()
    idx = np.flatnonzero(ds["load_hp"] == 3)

    py, c = [], []
    out = (ctypes.c_float * features.N_FEATURES)()
    for i in idx:
        w = np.ascontiguousarray(ds["windows"][i], dtype=np.float32)
        py.append(features.extract(w, ds["fs_hz"], ds["rpm"][i]))
        rc = lib.features_extract(w.ctypes.data_as(ctypes.POINTER(ctypes.c_float)), len(w),
                                  ds["fs_hz"], float(ds["rpm"][i]), out)
        assert rc == 0
        c.append(np.array(out[:], dtype=np.float32))
    py, c = np.array(py), np.array(c)

    rel = np.abs(c - py) / np.maximum(np.abs(py), 1e-9)
    print(f"{len(idx)} ventanas. Error relativo por feature (máximo / mediana):")
    for j, name in enumerate(features.FEATURE_NAMES):
        print(f"  {name:<18} {rel[:, j].max():9.2e}  {np.median(rel[:, j]):9.2e}")

    # Lo que importa: ¿cambia lo que dice el modelo?
    import tensorflow as tf
    for tfl in ["model_cwru.tflite", "model_cwru_f32.tflite", "model_cwru_wide_f32.tflite"]:
        if not os.path.exists(os.path.join(HERE, tfl)):
            continue
        npz = tfl.replace("_f32.tflite", "").replace(".tflite", "") + "_test.npz"
        d = np.load(os.path.join(HERE, npz))
        mean, std = d["feat_mean"], d["feat_std"]
        it = tf.lite.Interpreter(model_path=os.path.join(HERE, tfl))
        it.allocate_tensors()
        inp = it.get_input_details()[0]
        outs = it.get_output_details()
        i_fault = next(k for k, o in enumerate(outs) if int(o["shape"][-1]) > 1)

        def classes(X):
            r = []
            for row in X:
                it.set_tensor(inp["index"], ((row - mean) / std).reshape(1, -1).astype(np.float32))
                it.invoke()
                r.append(int(np.argmax(it.get_tensor(outs[i_fault]["index"]))))
            return np.array(r)

        a, b = classes(py), classes(c)
        y = ds["label"][idx]
        print(f"{tfl:<28} clase distinta en {np.sum(a != b)}/{len(a)} ventanas; "
              f"accuracy Python {np.mean(a == y) * 100:.2f}%  C {np.mean(b == y) * 100:.2f}%")


if __name__ == "__main__":
    main()
