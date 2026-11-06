#!/usr/bin/env python3
"""Análisis de la prueba masiva de pérdida (varias placas, varias corridas).

Uso: python3 analisis.py runner.log data_1.csv[.gz] data_2.csv[.gz] [MARGEN_MIN]

- runner.log: lo escribe ~/bench/masiva_runner.sh. Cada corrida es un par
  "INICIO <nombre> proto=.. sleep=.. window=.. regimen_min=.." / "FIN <nombre>",
  con una línea "  <MAC> <epoch> v<versión> ..." por placa entre medio.
- data_N.csv: id_device,time_server,time_sent,seq,config_version_applied
  (epoch en segundos), de nebulaedge_schema.data_N durante toda la prueba.

Por corrida, placa y flujo:
- régimen: desde INICIO + MARGEN_MIN (3 por defecto) hasta FIN, y solo filas
  con la versión de config de esa corrida o más nueva;
- perdidos: saltos de seq dentro del régimen; duplicados; reinicios (seq que baja);
- posición en la ventana de cada pérdida (solo deep sleep, con ciclos de 10);
- latencia: time_server - time_sent, relativa al mínimo de la placa en la
  corrida (cancela el desfase del reloj); mediana y p95 en ms;
- transición: segundos desde INICIO hasta el primer paquete con la versión nueva.
"""
import collections
import csv
import gzip
import re
import statistics as st
import sys

PROTO = {0: "MQTT", 1: "UDP", 2: "TCP", 3: "BLE"}
CYCLE_GAP_S = 8


def load_runs(path):
    runs, cur = [], None
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.match(r"(\d+) INICIO (\S+) proto=(\d) sleep=(\d+) window=(\d+) regimen_min=(\d+)", line)
            if m:
                cur = dict(start=int(m[1]), name=m[2], proto=int(m[3]), sleep=int(m[4]),
                           window=int(m[5]), versions={}, events=[])
                runs.append(cur)
                continue
            m = re.match(r"\s+(\S+) [\d.]+ v(\d+) ", line)
            if m and cur is not None and "end" not in cur:   # las de después del FIN son de otra cosa
                cur["versions"][m[1]] = int(m[2])
                continue
            m = re.match(r"(\d+) (EVENTO\S*) (\S+)", line)
            if m and cur is not None:
                cur["events"].append((int(m[1]), m[2]))
                continue
            m = re.match(r"(\d+) FIN (\S+)", line)
            if m and cur is not None and m[2] == cur["name"]:
                cur["end"] = int(m[1])
    return [r for r in runs if "end" in r]


def load_data(path):
    by_dev = collections.defaultdict(list)
    opener = gzip.open if path.endswith(".gz") else open
    with opener(path, "rt", encoding="utf-8", errors="replace") as f:
        for row in csv.reader(f):
            if len(row) < 5 or not row[3]:
                continue
            dev, ts, tsent, seq, ver = row[:5]
            by_dev[dev].append((float(ts), float(tsent) if tsent else None, int(seq), int(ver or 0)))
    for v in by_dev.values():
        v.sort()
    return by_dev


def pct(a, b):
    return 100.0 * a / b if b else 0.0


def analyze(run, data, margin_min):
    out = {}
    t0 = run["start"] + margin_min * 60
    for dev, ver in run["versions"].items():
        rows = data.get(dev, [])
        allrun = [r for r in rows if run["start"] <= r[0] < run["end"]]
        first_new = next((r[0] for r in allrun if r[3] >= ver), None)
        reg = [r for r in allrun if r[0] >= t0 and r[3] >= ver]
        seqs = [r[2] for r in reg]
        resets = sum(1 for a, b in zip(seqs, seqs[1:]) if b < a)
        dups = len(seqs) - len(set(seqs))
        lost, positions = 0, collections.Counter()
        # Pérdidas por tramos sin reinicio (un reinicio corta la secuencia).
        seg = []
        for s in seqs + [None]:
            if s is None or (seg and s < seg[-1]):
                if seg:
                    u = sorted(set(seg))
                    miss = [x for x in range(u[0], u[-1] + 1) if x not in set(u)]
                    lost += len(miss)
                    if run["sleep"] > 0 and run["window"] == 10:
                        positions.update((x - u[0]) % 10 for x in miss)
                seg = []
            if s is not None:
                seg.append(s)
        lat = [r[0] - r[1] for r in reg if r[1]]
        latrel = sorted((x - min(lat)) * 1000 for x in lat) if lat else []
        out[dev] = dict(
            recv=len(set(seqs)), lost=lost, dups=dups, resets=resets, pos=positions,
            lat_med=st.median(latrel) if latrel else None,
            lat_p95=latrel[int(0.95 * (len(latrel) - 1))] if latrel else None,
            trans=(first_new - run["start"]) if first_new else None,
        )
    return out


def main():
    runs = load_runs(sys.argv[1])
    d1, d2 = load_data(sys.argv[2]), load_data(sys.argv[3])
    margin = int(sys.argv[4]) if len(sys.argv) > 4 else 3
    print("corrida              proto modo  flujo  recibidos perdidos   %     dup reinic  lat_med lat_p95  transición(med)")
    for run in runs:
        modo = f"B{run['sleep']}" if run["sleep"] else "cont"
        for flow, data in (("D1", d1), ("D2", d2)):
            res = analyze(run, data, margin)
            recv = sum(r["recv"] for r in res.values())
            lost = sum(r["lost"] for r in res.values())
            dups = sum(r["dups"] for r in res.values())
            rst = sum(r["resets"] for r in res.values())
            lm = [r["lat_med"] for r in res.values() if r["lat_med"] is not None]
            lp = [r["lat_p95"] for r in res.values() if r["lat_p95"] is not None]
            tr = [r["trans"] for r in res.values() if r["trans"] is not None]
            print(f"{run['name']:<20} {PROTO[run['proto']]:<5} {modo:<5} {flow:<5} {recv:>9} {lost:>8} "
                  f"{pct(lost, recv + lost):>6.2f} {dups:>4} {rst:>6} "
                  f"{(st.median(lm) if lm else float('nan')):>8.0f} {(st.median(lp) if lp else float('nan')):>7.0f} "
                  f"{(st.median(tr) if tr else float('nan')):>8.0f} s ({len(tr)}/{len(res)} placas)")
        for dev, r in sorted(analyze(run, d1, margin).items()):
            r2 = analyze(run, d2, margin)[dev]
            pos = sorted((r["pos"] + r2["pos"]).items())
            print(f"    {dev[-5:]}: D1 {r['recv']}/{r['lost']}  D2 {r2['recv']}/{r2['lost']}  "
                  f"reinicios {r['resets'] + r2['resets']}  posiciones {pos}")
        for t, e in run["events"]:
            print(f"    {e} a los {t - run['start']} s")


if __name__ == "__main__":
    main()
