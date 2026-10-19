"""Entrena el modelo de dos cabezas y guarda lo que necesita convert.py.

    .venv/bin/python train.py --data cwru --trunk 256,128 --out model_cwru_wide

El modelo es UNO SOLO con dos salidas: un tronco compartido que entiende la
vibración, y dos cabezas que sacan conclusiones distintas del mismo
entendimiento. Las capas ocultas sirven a las dos tareas —"¿hay impactos?,
¿a qué orden?, ¿cuánta energía?" son las mismas preguntas para clasificar y
para medir severidad— así que no hay razón para aprenderlas dos veces.

La estandarización de features va FUERA del modelo (el porqué está más abajo,
junto a feat_mean): sus constantes se guardan en <out>_test.npz y viajan en el
manifiesto, y el ESP32 las aplica antes de invocar. De dónde salen los datos y
cómo se dividen lo dice data_sources.py; convert.py pasa el modelo a TFLite.
"""

import argparse
import numpy as np
import tensorflow as tf
from tensorflow import keras
from tensorflow.keras import layers

import features
from data_sources import DATA_SOURCES, split_by_window

# Reproducibilidad: sin esto cada corrida da una accuracy distinta.
keras.utils.set_random_seed(42)


def build_model(n_features, n_classes, trunk=(64, 32)):
    entrada = keras.Input(shape=(n_features,), name="features")

    x = entrada

    # Tronco compartido: acá vive el "entendimiento" de la vibración.
    x = layers.Dense(trunk[0], activation="relu", name="trunk_1")(x)
    x = layers.Dropout(0.2)(x)
    x = layers.Dense(trunk[1], activation="relu", name="trunk_2")(x)

    # Dos cabezas sobre el MISMO tensor. Cuestan ~150 parámetros entre las
    # dos: el costo está todo en el tronco, que ya se pagó.
    severity = layers.Dense(1, activation="sigmoid", name="severity")(x)
    fault = layers.Dense(n_classes, activation="softmax", name="fault")(x)

    return keras.Model(inputs=entrada, outputs=[severity, fault])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--leaky", action="store_true",
                    help="split por ventana en vez de por grabación (demuestra la fuga)")
    ap.add_argument("--epochs", type=int, default=120)
    ap.add_argument("--recordings", type=int, default=None, help="solo con --data synthetic")
    ap.add_argument("--windows", type=int, default=None, help="solo con --data synthetic")
    ap.add_argument("--data", choices=sorted(DATA_SOURCES), default="synthetic",
                    help="de dónde salen los datos (ver data_sources.py)")
    ap.add_argument("--out", default=None)
    ap.add_argument("--trunk", default="64,32",
                    help="neuronas de las dos capas del tronco, p.ej. 256,128")
    args = ap.parse_args()
    trunk = tuple(int(n) for n in args.trunk.split(","))
    out = args.out or ("model" if args.data == "synthetic" else f"model_{args.data}")
    source = DATA_SOURCES[args.data]

    opts = {}
    if args.recordings is not None:
        opts["n_recordings"] = args.recordings
    if args.windows is not None:
        opts["windows_per_recording"] = args.windows
    print(f"Cargando {args.data}...")
    ds = source.load(**opts)
    classes = source.classes

    print(f"Extrayendo {features.N_FEATURES} features de {len(ds['windows'])} ventanas...")
    X = np.array([features.extract(w, ds["fs_hz"], r)
                  for w, r in zip(ds["windows"], ds["rpm"])], dtype=np.float32)
    y_sev = ds["severity"].astype(np.float32)
    y_fault = ds["label"].astype(np.int32)

    if args.leaky:
        tr, va, te = split_by_window(len(X))
        print("\n*** SPLIT POR VENTANA (con fuga de datos) ***")
    else:
        tr, va, te = source.split(ds)
        print(f"\n{source.description}")
    print(f"  train {tr.sum()}  val {va.sum()}  test {te.sum()}")

    # Estandarización FUERA del modelo, y con estadísticas de entrenamiento
    # solamente (mirar las de test ya sería hacer trampa).
    #
    # Al principio esto era una capa Normalization dentro del modelo, que es
    # más elegante porque las constantes viajan en el .tflite. Hubo que
    # sacarlo: con las features crudas el tensor de entrada abarca 1406x de
    # rango dinámico (clearance_factor llega a 7.3, order_4.5_8.0 a 0.005) y
    # un tensor int8 tiene UNA sola escala para todo. El paso quedaba en
    # 0.057 y once de las veinte features caían en menos de 4 niveles
    # distintos: la cuantización las borraba. Costaba 7.7 puntos de accuracy.
    #
    # Estandarizando antes, la entrada queda en ~N(0,1) y una escala común
    # sirve para todas. El precio es que mean/std hay que llevarlos al ESP32,
    # pero convert.py los emite en el mismo header que el modelo, así que no
    # pueden desincronizarse.
    feat_mean = X[tr].mean(axis=0)
    feat_std = X[tr].std(axis=0) + 1e-8
    # Sin estandarizar, para guardarlas: es lo que recibe el ESP32, que
    # estandariza él mismo con feat_mean/feat_std del manifiesto.
    X_raw = X
    X = ((X - feat_mean) / feat_std).astype(np.float32)

    model = build_model(features.N_FEATURES, len(classes), trunk)
    print(f"\nParámetros: {model.count_params():,}")

    # loss_weights: el MSE de severidad vive en ~0.02 y la crossentropy en
    # ~1.2. Sin balancear, el gradiente de clasificación aplasta al de
    # regresión y queda un regresor que predice siempre el promedio.
    model.compile(
        optimizer=keras.optimizers.Adam(2e-3),
        loss={"severity": "mse", "fault": "sparse_categorical_crossentropy"},
        loss_weights={"severity": 8.0, "fault": 1.0},
        metrics={"severity": ["mae"], "fault": ["accuracy"]},
    )

    model.fit(
        X[tr], {"severity": y_sev[tr], "fault": y_fault[tr]},
        validation_data=(X[va], {"severity": y_sev[va], "fault": y_fault[va]}),
        epochs=args.epochs, batch_size=32, verbose=0,
        callbacks=[keras.callbacks.EarlyStopping(
            monitor="val_loss", patience=25, restore_best_weights=True)],
    )

    sev_pred, fault_pred = model.predict(X[te], verbose=0)
    acc = float(np.mean(np.argmax(fault_pred, axis=1) == y_fault[te]))
    mae = float(np.mean(np.abs(sev_pred.ravel() - y_sev[te])))

    print(f"\n{'='*46}\nTEST (float32)")
    print(f"  accuracy clasificador : {acc*100:.2f}%")
    print(f"  MAE severidad         : {mae:.4f}")
    print("="*46)

    # Matriz de confusión: en qué se equivoca, no solo cuánto.
    pred = np.argmax(fault_pred, axis=1)
    print("\nfilas = verdad, columnas = predicción")
    print(" " * 18 + "".join(f"{c[:8]:>10}" for c in classes))
    for i, c in enumerate(classes):
        row = [int(np.sum((y_fault[te] == i) & (pred == j))) for j in range(len(classes))]
        print(f"{c:>18}" + "".join(f"{v:>10}" for v in row))

    if not args.leaky:
        model.save(f"{out}.keras")
        np.savez(f"{out}_test.npz",
                 X=X[te], y_sev=y_sev[te], y_fault=y_fault[te],
                 X_raw=X_raw[te],      # las mismas, sin estandarizar
                 X_repr=X[tr][:400],   # calibración del cuantizador
                 feat_mean=feat_mean, feat_std=feat_std,
                 classes=np.array(classes), fs_hz=ds["fs_hz"],
                 # Para export_model.py: terminan en la tabla model.
                 trained_on=f"{source.description}; tronco {args.trunk}",
                 test_dataset=source.test_dataset or "")
        print(f"\nGuardados {out}.keras y {out}_test.npz")

    return acc, mae


if __name__ == "__main__":
    main()
