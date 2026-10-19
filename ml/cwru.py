"""Lee CWRU desde archive/ y lo corta en ventanas, con las mismas etiquetas que synthetic.

Solo se lee acc_x (el acelerómetro del lado del motor, el de señal más fuerte)
y las columnas de etiqueta. Parquet permite leer columnas sueltas, así que
el resto del archivo ni se toca.
"""

import glob
import os
import re

import numpy as np
import pyarrow.parquet as pq

import features

DATA_DIR = os.path.join(os.path.dirname(__file__), "archive", "CWRU", "CWRU", "raw_parquet")

# Mismo orden que synthetic.CLASSES, para que el índice 0 sea siempre "sano".
CLASSES = ["sano", "pista_interior", "pista_exterior", "elemento_rodante"]
LABEL_TO_INDEX = {"normal": 0, "inner_race": 1, "outer_race": 2, "ball": 3}

# Severidad en CWRU va de 0 (sano) a 3 (falla más grande); la cabeza de
# severidad termina en sigmoid, que entrega 0..1, así que se divide por 3.
MAX_SEVERITY = 3.0

# normal_1hp y normal_2hp vienen sin rpm. Se usa la velocidad nominal de
# cada carga que publica CWRU.
NOMINAL_RPM = {0: 1797.0, 1: 1772.0, 2: 1750.0, 3: 1730.0}


def load_windows(n=features.WINDOW_SIZE):
    """Devuelve un dict con windows, rpm, label, severity, load_hp, source_file y fs_hz.

    Las ventanas no se solapan: cada muestra cae en una sola ventana.
    """
    out = {k: [] for k in ("windows", "rpm", "label", "severity", "load_hp", "source_file")}
    fs_hz = None

    for path in sorted(glob.glob(os.path.join(DATA_DIR, "*.parquet"))):
        name = os.path.basename(path)
        # La carga se saca del nombre: load_condition trae variantes como "3hp_OR@6".
        load_hp = int(re.search(r"_(\d)hp\.parquet$", name).group(1))

        t = pq.read_table(path, columns=["acc_x", "rpm", "label", "severity", "fs_hz"]).to_pandas()
        fs_hz = float(t["fs_hz"].iloc[0])
        signal = t["acc_x"].to_numpy(np.float32)

        rpm = t["rpm"].median()
        if np.isnan(rpm):
            rpm = NOMINAL_RPM[load_hp]

        label = LABEL_TO_INDEX[t["label"].iloc[0]]
        severity = float(t["severity"].iloc[0]) / MAX_SEVERITY

        for start in range(0, len(signal) - n + 1, n):
            out["windows"].append(signal[start:start + n])
            out["rpm"].append(rpm)
            out["label"].append(label)
            out["severity"].append(severity)
            out["load_hp"].append(load_hp)
            out["source_file"].append(name)

    ds = {k: np.array(v) for k, v in out.items()}
    ds["fs_hz"] = fs_hz
    return ds
