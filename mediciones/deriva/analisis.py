#!/usr/bin/env python3
"""Análisis de deriva del reloj del device en deep sleep.

Uso: python3 analisis.py derivaN.csv derivaN_fases.txt

El CSV sale de nebulaedge_schema.data_1 de un device (time_server, time_sent,
seq, temperature). Las fases son las líneas que escribe el runner: "A <epoch>",
"B<T> <epoch>" o "fin <epoch>". A es continuo (sin deep sleep); B<T> duerme T s
por ciclo.

Método:
- offset = time_server - time_sent, en cada paquete;
- un ciclo es una racha de paquetes sin huecos de más de 8 s;
- el offset de un ciclo es su mínimo (filtra la latencia de red);
- el salto por ciclo es la diferencia de offset entre ciclos consecutivos de la
  misma fase. Positivo = el device se atrasa;
- se descarta el ciclo de transición (periodo lejos de T + 18 s) y los saltos
  a más de 1 s de la mediana de su punto: son el ajuste de hora al aplicar una
  config nueva, no deriva.
Ajuste por mínimos cuadrados: salto = c + k * T_sleep.
"""
import csv
import math
import statistics as st
import sys

AWAKE_S = 18          # ventana despierta aproximada (window=20 a 1 s)
CYCLE_GAP_S = 8       # hueco que separa dos ciclos
SKIP_AFTER_PHASE_S = 20


def load(csv_path, phases_path):
    with open(csv_path, encoding="utf-8") as f:
        r = csv.reader(f)
        next(r)
        rows = [(float(a), float(b), int(c), float(d) if d else math.nan) for a, b, c, d in r]
    phases = []
    with open(phases_path, encoding="utf-8") as f:
        for line in f:
            p = line.split()
            if len(p) < 2 or not (p[0] == "A" or p[0] == "fin" or p[0].startswith("B")):
                continue
            T = 0 if p[0] in ("A", "fin") else int(p[0][1:])
            phases.append((p[0], T, float(p[1])))
    return rows, phases


def cycles(rows):
    out, cur = [], []
    for r in rows:
        if cur and r[0] - cur[-1][0] > CYCLE_GAP_S:
            out.append(cur)
            cur = []
        cur.append(r)
    if cur:
        out.append(cur)
    return out


def phase_of(t, phases):
    idx = None
    for i, (_, _, start) in enumerate(phases):
        if t >= start + SKIP_AFTER_PHASE_S:
            idx = i
    return idx


def fit(points):
    X = [p[0] for p in points]
    Y = [p[1] for p in points]
    n = len(X)
    mx, my = sum(X) / n, sum(Y) / n
    sxx = sum((x - mx) ** 2 for x in X)
    k = sum((x - mx) * (y - my) for x, y in zip(X, Y)) / sxx
    c = my - k * mx
    s = math.sqrt(sum((y - c - k * x) ** 2 for x, y in zip(X, Y)) / (n - 2)) if n > 2 else 0.0
    return c, k, s, s / math.sqrt(sxx), s * math.sqrt(1 / n + mx * mx / sxx)


def main():
    rows, phases = load(sys.argv[1], sys.argv[2])
    C = []
    for c in cycles(rows):
        C.append(dict(t=c[0][0], off=min(x[0] - x[1] for x in c), ph=phase_of(c[0][0], phases),
                      s0=c[0][2], s1=c[-1][2], temp=st.mean(x[3] for x in c)))

    # Fase continua: deriva despierta en ppm (pendiente del offset contra el tiempo).
    for i, (name, T, start) in enumerate(phases):
        if T == 0 and name == "A":
            pts = [(x[0], x[0] - x[1]) for x in rows
                   if phase_of(x[0], phases) == i]
            if len(pts) > 2:
                _, k, _, _, _ = fit(pts)
                print(f"{name}: deriva despierta {k * 1e6:+.2f} ppm ({len(pts)} paquetes)")

    # Saltos por ciclo; la pasada es el número de vez que aparece ese T.
    seen = {}
    pass_of = {}
    for i, (_, T, _) in enumerate(phases):
        if T > 0:
            seen[T] = seen.get(T, 0) + 1
            pass_of[i] = seen[T]
    J = []      # (T, pasada, salto_ms, seq_perdidos, temp)
    for a, b in zip(C, C[1:]):
        if a["ph"] is None or a["ph"] != b["ph"] or phases[a["ph"]][1] == 0:
            continue
        T = phases[a["ph"]][1]
        if abs((b["t"] - a["t"]) - (T + AWAKE_S)) > 15:
            continue
        J.append((T, pass_of[a["ph"]], (b["off"] - a["off"]) * 1000, b["s0"] - a["s1"] - 1, a["temp"]))
    if not J:
        print("sin ciclos de deep sleep")
        return

    meds = {}
    for T, p, j, *_ in J:
        meds.setdefault((T, p), []).append(j)
    meds = {k: st.median(v) for k, v in meds.items()}
    dropped = [x for x in J if abs(x[2] - meds[(x[0], x[1])]) > 1000]
    J = [x for x in J if abs(x[2] - meds[(x[0], x[1])]) <= 1000]
    print(f"saltos usados {len(J)}, descartados por ajuste de hora {len(dropped)}")

    Ts = sorted(set(x[0] for x in J))
    if len(Ts) >= 2:
        c, k, s, sk, sc = fit([(x[0], x[2]) for x in J])
        print(f"ajuste: c = {c:+.0f} ± {sc:.0f} ms, k = {k:+.3f} ± {sk:.3f} ms/s, "
              f"desviación de residuos {s:.0f} ms")
        passes = sorted(set(x[1] for x in J))
        if len(passes) > 1:
            for p in passes:
                P = [x for x in J if x[1] == p]
                if len(set(x[0] for x in P)) >= 2:
                    cp, kp, _, skp, scp = fit([(x[0], x[2]) for x in P])
                    print(f"  pasada {p}: c = {cp:+.0f} ± {scp:.0f} ms, k = {kp:+.3f} ± {skp:.3f} ms/s, "
                          f"{min(x[4] for x in P):.1f}-{max(x[4] for x in P):.1f} °C")
    else:
        c = k = None

    print("   T    n  mediana  residuo  desv  perdidos  temp")
    for T in Ts:
        P = [x for x in J if x[0] == T]
        j = [x[2] for x in P]
        res = f"{st.median(j) - c - k * T:+7.0f}" if c is not None else "      -"
        print(f"{T:4d} {len(j):4d} {st.median(j):+8.0f} {res} {st.pstdev(j):5.0f} "
              f"{sum(1 for x in P if x[3]):8d}  {st.mean(x[4] for x in P):.1f}")


if __name__ == "__main__":
    main()
