"""Arma el SQL que carga el set de test en la tabla infer_sample del servidor.

    .venv/bin/python export_testset.py --data cwru
    # desde raspberry/:
    docker compose exec -T db psql -U nebulaedge -d nebulaedge < ../ml/testset_cwru_3hp.sql

Es lo que el servidor le manda al device en las fases C1 (features) y C2
(ventanas crudas): el test del split de data_sources.py, que el modelo no vio
al entrenar. En CWRU, las ventanas con carga de 3 hp (train 0-1 hp, val 2 hp,
test 3 hp). El nombre del set es el test_dataset de la entrada, y es el mismo
que export_model.py deja en la tabla model: así el servidor sabe qué set le
toca a cada modelo.

Las features salen de features.extract(), el MISMO código que usó train.py, y
van sin estandarizar: estandarizar es trabajo del device, con el mean/std del
manifiesto. Es un archivo y no una conexión directa por lo mismo que
export_model.py: la base vive en la Raspberry.

Reemplaza el dataset entero (DELETE + INSERT en una transacción), así que se
puede correr de nuevo si cambian las features.
"""

import argparse

import numpy as np

import features
from data_sources import DATA_SOURCES


def floats(values):
    return "ARRAY[" + ",".join(f"{float(v):.9g}" for v in values) + "]::float[]"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", choices=sorted(DATA_SOURCES), default="cwru")
    args = ap.parse_args()

    source = DATA_SOURCES[args.data]
    if source.test_dataset is None:
        raise SystemExit(f"{args.data} no tiene test_dataset en data_sources.py")
    dataset = source.test_dataset
    ds = source.load()
    _, _, te = source.split(ds)
    idx = np.flatnonzero(te)

    # Posición de cada ventana dentro de su grabación: dice de qué parte del
    # archivo salió, para poder volver a ella.
    window_index, seen = [], {}
    for i in idx:
        f = ds["source_file"][i]
        window_index.append(seen.get(f, 0))
        seen[f] = seen.get(f, 0) + 1

    out = f"testset_{dataset}.sql"
    with open(out, "w") as fh:
        fh.write("-- Generado por ml/export_testset.py. No editar a mano.\n")
        fh.write("BEGIN;\n")
        fh.write(f"DELETE FROM nebulaedge_schema.infer_sample WHERE dataset = '{dataset}';\n")
        for n, (i, wi) in enumerate(zip(idx, window_index)):
            w = ds["windows"][i]
            feats = features.extract(w, ds["fs_hz"], ds["rpm"][i])
            fh.write(
                "INSERT INTO nebulaedge_schema.infer_sample (dataset, sample_index, source_file, window_index, "
                "true_fault, true_severity, rpm, fs_hz, features, \"window\") VALUES ("
                f"'{dataset}', {n}, '{ds['source_file'][i]}', {wi}, {int(ds['label'][i])}, "
                f"{float(ds['severity'][i]):.9g}, {float(ds['rpm'][i]):.9g}, {float(ds['fs_hz']):.9g}, "
                f"{floats(feats)}, {floats(w)});\n")
        fh.write("COMMIT;\n")

    counts = np.bincount(ds["label"][idx], minlength=len(source.classes))
    print(out)
    print(f"  {len(idx)} ventanas de test, de {len(seen)} grabaciones")
    print("  por clase: " + ", ".join(f"{c} {k}" for c, k in zip(source.classes, counts)))


if __name__ == "__main__":
    main()
