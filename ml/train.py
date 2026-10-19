"""Entrena el modelo de dos cabezas y lo exporta cuantizado a int8.

El modelo es UNO SOLO con dos salidas: un tronco compartido que entiende la
vibración, y dos cabezas que sacan conclusiones distintas del mismo
entendimiento. Las capas ocultas sirven a las dos tareas —"¿hay impactos?,
¿a qué orden?, ¿cuánta energía?" son las mismas preguntas para clasificar y
para medir severidad— así que no hay razón para aprenderlas dos veces.

La normalización de features va DENTRO del modelo, como primera capa. Es una
decisión deliberada: así las constantes viajan dentro del .tflite y el ESP32
no tiene que recibirlas ni aplicarlas por separado. Elimina de raíz el bug
clásico de desplegar con una normalización distinta a la del entrenamiento.
"""

import argparse
import numpy as np
import tensorflow as tf
from tensorflow import keras
from tensorflow.keras import layers

import cwru
import features
import synthetic

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


def split_by_recording(source_files, val_frac=0.2, test_frac=0.2, seed=0):
    """Divide por grabación, nunca por ventana.

    Si ventanas de la misma grabación caen en entrenamiento y en test, el
    modelo aprende a reconocer LA GRABACIÓN —su nivel de ruido, su ganancia,
    sus resonancias de montaje— en vez de la física de la falla. La accuracy
    sale cerca de 100% y no significa nada. Con --leaky se puede ver cuánto
    infla exactamente.
    """
    rng = np.random.default_rng(seed)
    recordings = np.unique(source_files)
    rng.shuffle(recordings)

    n_test = max(1, int(len(recordings) * test_frac))
    n_val = max(1, int(len(recordings) * val_frac))

    test_recs = set(recordings[:n_test])
    val_recs = set(recordings[n_test:n_test + n_val])

    is_test = np.array([f in test_recs for f in source_files])
    is_val = np.array([f in val_recs for f in source_files])
    return ~(is_test | is_val), is_val, is_test


def split_by_load(load_hp, val_load=2, test_load=3):
    """Divide por carga del motor: estudia con unas, se examina con otra.

    Más exigente que dividir por grabación: el test es una condición de
    operación que el modelo nunca vio. Si acierta igual, aprendió la falla y
    no la carga.
    """
    is_test = load_hp == test_load
    is_val = load_hp == val_load
    return ~(is_test | is_val), is_val, is_test


def split_by_window(n, val_frac=0.2, test_frac=0.2, seed=0):
    """Split ingenuo, solo para demostrar la fuga de datos."""
    rng = np.random.default_rng(seed)
    idx = rng.permutation(n)
    n_test, n_val = int(n * test_frac), int(n * val_frac)
    test_i, val_i = idx[:n_test], idx[n_test:n_test + n_val]
    is_test = np.zeros(n, bool); is_test[test_i] = True
    is_val = np.zeros(n, bool); is_val[val_i] = True
    return ~(is_test | is_val), is_val, is_test


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--leaky", action="store_true",
                    help="split por ventana en vez de por grabación (demuestra la fuga)")
    ap.add_argument("--epochs", type=int, default=120)
    ap.add_argument("--recordings", type=int, default=48)
    ap.add_argument("--windows", type=int, default=50)
    ap.add_argument("--data", choices=["synthetic", "cwru"], default="synthetic")
    ap.add_argument("--out", default=None)
    ap.add_argument("--trunk", default="64,32",
                    help="neuronas de las dos capas del tronco, p.ej. 256,128")
    args = ap.parse_args()
    trunk = tuple(int(n) for n in args.trunk.split(","))
    out = args.out or ("model_cwru" if args.data == "cwru" else "model")

    if args.data == "cwru":
        print("Leyendo CWRU...")
        ds = cwru.load_windows()
        classes = cwru.CLASSES
    else:
        print("Generando señal sintética...")
        ds = synthetic.generate_dataset(
            n_recordings=args.recordings, windows_per_recording=args.windows, seed=1)
        classes = synthetic.CLASSES

    print(f"Extrayendo {features.N_FEATURES} features de {len(ds['windows'])} ventanas...")
    X = np.array([features.extract(w, ds["fs_hz"], r)
                  for w, r in zip(ds["windows"], ds["rpm"])], dtype=np.float32)
    y_sev = ds["severity"].astype(np.float32)
    y_fault = ds["label"].astype(np.int32)

    if args.leaky:
        tr, va, te = split_by_window(len(X))
        print("\n*** SPLIT POR VENTANA (con fuga de datos) ***")
    elif args.data == "cwru":
        tr, va, te = split_by_load(ds["load_hp"])
        print("\nSplit por carga: train 0-1 hp, val 2 hp, test 3 hp")
    else:
        tr, va, te = split_by_recording(ds["source_file"])
        print("\nSplit por grabación (honesto)")
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
                 classes=np.array(classes), fs_hz=ds["fs_hz"])
        print(f"\nGuardados {out}.keras y {out}_test.npz")

    return acc, mae


if __name__ == "__main__":
    main()
