"""Registro de los datos con que se entrena. Un dataset nuevo es un cargador
nuevo y una entrada en DATA_SOURCES; train.py, export_testset.py y
export_model.py no se tocan.

CONTRATO DEL CARGADOR
    `load()` devuelve un dict con, para N ventanas:

        windows      (N, features.WINDOW_SIZE) float   muestras crudas de UN eje
        label        (N,) int        índice en `classes`
        severity     (N,) float      0..1 (la cabeza de severidad termina en sigmoid)
        source_file  (N,) str        grabación de origen: el split no la parte
        rpm          (N,) float      velocidad del eje; las bandas de orden la usan
        fs_hz        float           frecuencia de muestreo, la misma para todo el dataset

    y las columnas propias que use su split (load_hp en CWRU).

    Las ventanas tienen que servirle a features.extract() tal como está: mismo
    largo, una fs a la que las bandas tengan sentido y una rpm conocida. Datos
    que no cumplan eso (otra fs, sin rpm) piden un conjunto de features nuevo,
    con FEATURE_SET_VERSION nueva en features.py y en el firmware.

LA ENTRADA
    classes       nombres de las clases, en el orden de `label`
    split         ds -> (train, val, test), tres máscaras booleanas
    description   de dónde salen los datos y cómo se dividen; termina en
                  trained_on de la tabla model
    test_dataset  nombre del set en infer_sample (lo arma export_testset.py
                  con el test del split, y el servidor lo recorre en el
                  device). None si este dataset no tiene uno.
"""

from dataclasses import dataclass
from typing import Callable

import numpy as np

import cwru
import synthetic


@dataclass(frozen=True)
class DataSource:
    load: Callable[..., dict]
    classes: list
    split: Callable[[dict], tuple]
    description: str
    test_dataset: str | None


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


DATA_SOURCES = {
    "cwru": DataSource(
        load=cwru.load_windows,
        classes=cwru.CLASSES,
        split=lambda ds: split_by_load(ds["load_hp"]),
        description="CWRU acc_x; train 0-1hp, val 2hp, test 3hp",
        test_dataset="cwru_3hp",
    ),
    "synthetic": DataSource(
        load=lambda n_recordings=48, windows_per_recording=50: synthetic.generate_dataset(
            n_recordings=n_recordings, windows_per_recording=windows_per_recording, seed=1),
        classes=synthetic.CLASSES,
        split=lambda ds: split_by_recording(ds["source_file"]),
        description="sintético; split por grabación",
        test_dataset=None,
    ),
}
