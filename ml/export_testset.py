"""Arma el SQL que carga el set de test en la tabla infer_sample del servidor.

    .venv/bin/python export_testset.py
    # desde raspberry/:
    docker compose exec -T db psql -U nebulaedge -d nebulaedge < ../ml/testset_cwru_3hp.sql

Es lo que el servidor le manda al device en las fases C1 (features) y C2
(ventanas crudas): las ventanas de CWRU con carga de 3 hp, que el modelo no vio
al entrenar (train.py separa por carga: train 0-1 hp, val 2 hp, test 3 hp).

Las features salen de features.extract(), el MISMO código que usó train.py, y
van sin estandarizar: estandarizar es trabajo del device, con el mean/std del
manifiesto. Es un archivo y no una conexión directa por lo mismo que
export_model.py: la base vive en la Raspberry.

Reemplaza el dataset entero (DELETE + INSERT en una transacción), así que se
puede correr de nuevo si cambian las features.
"""

import argparse

import numpy as np

import cwru
import features


def floats(values):
    return "ARRAY[" + ",".join(f"{float(v):.9g}" for v in values) + "]::float[]"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dataset", default="cwru_3hp")
    ap.add_argument("--load", type=int, default=3, help="carga del motor (hp) que forma el set de test")
    args = ap.parse_args()

    ds = cwru.load_windows()
    idx = np.flatnonzero(ds["load_hp"] == args.load)

    # Posición de cada ventana dentro de su grabación: dice de qué parte del
    # archivo salió, para poder volver a ella.
    window_index, seen = [], {}
    for i in idx:
        f = ds["source_file"][i]
        window_index.append(seen.get(f, 0))
        seen[f] = seen.get(f, 0) + 1

    out = f"testset_{args.dataset}.sql"
    with open(out, "w") as fh:
        fh.write("-- Generado por ml/export_testset.py. No editar a mano.\n")
        fh.write("BEGIN;\n")
        fh.write(f"DELETE FROM nebulaedge_schema.infer_sample WHERE dataset = '{args.dataset}';\n")
        for n, (i, wi) in enumerate(zip(idx, window_index)):
            w = ds["windows"][i]
            feats = features.extract(w, ds["fs_hz"], ds["rpm"][i])
            fh.write(
                "INSERT INTO nebulaedge_schema.infer_sample (dataset, sample_index, source_file, window_index, "
                "true_fault, true_severity, rpm, fs_hz, features, \"window\") VALUES ("
                f"'{args.dataset}', {n}, '{ds['source_file'][i]}', {wi}, {int(ds['label'][i])}, "
                f"{float(ds['severity'][i]):.9g}, {float(ds['rpm'][i]):.9g}, {float(ds['fs_hz']):.9g}, "
                f"{floats(feats)}, {floats(w)});\n")
        fh.write("COMMIT;\n")

    counts = np.bincount(ds["label"][idx], minlength=len(cwru.CLASSES))
    print(out)
    print(f"  {len(idx)} ventanas de {args.load} hp, de {len(seen)} grabaciones")
    print("  por clase: " + ", ".join(f"{c} {k}" for c, k in zip(cwru.CLASSES, counts)))


if __name__ == "__main__":
    main()
