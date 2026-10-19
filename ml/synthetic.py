"""Generador de vibración sintética de rodamientos.

Existe para tener el pipeline completo —entrenar, cuantizar, transferir,
inferir en el ESP32— andando ANTES de que llegue el dataset real, y para
poder depurarlo con señales de las que se conoce la respuesta exacta.

No pretende sustituir a CWRU. Sigue el modelo clásico de vibración de
rodamiento con defecto localizado:

    señal = ruido de fondo
          + armónicos de rotación del eje
          + tren de impactos periódicos

Cada impacto excita una resonancia estructural de alta frecuencia que decae
exponencialmente: es lo que se oye como un "tic" y lo que levanta la
kurtosis. La PERIODICIDAD de esos impactos es lo que identifica el defecto,
y la MODULACIÓN de su amplitud es lo que distingue pista interior de
exterior:

  - Pista EXTERIOR: el defecto está fijo respecto de la zona de carga, así
    que todos los impactos pegan con la misma fuerza.
  - Pista INTERIOR: el defecto gira con el eje y entra y sale de la zona de
    carga, así que la amplitud se modula a la frecuencia de giro.

Esa diferencia es física y real, y es lo que el clasificador tiene que
aprender.
"""

import numpy as np

CLASSES = ["sano", "pista_interior", "pista_exterior", "elemento_rodante"]

# Orden del tren de impactos de cada defecto (múltiplos de la frecuencia de
# giro). Valores típicos de un rodamiento de bolas de ~9 elementos.
FAULT_ORDERS = {
    "sano": None,
    "pista_interior": 5.42,
    "pista_exterior": 3.58,
    "elemento_rodante": 2.36,
}

# Resonancia estructural que excitan los impactos, y su amortiguamiento.
RESONANCE_HZ = 3000.0
DECAY_RATE = 800.0

# Frecuencia de la jaula, que modula al defecto en elemento rodante.
CAGE_ORDER = 0.40


def _impact_train(n, fs_hz, f_shaft, fault_order, severity, modulation, rng):
    """Tren de impactos resonantes a la frecuencia característica del defecto."""
    t = np.arange(n) / fs_hz
    signal = np.zeros(n)

    f_impact = f_shaft * fault_order
    period = fs_hz / f_impact

    # Jitter de ~1%: en un rodamiento real los elementos deslizan un poco y
    # el tren nunca es perfectamente periódico. Sin esto el espectro queda
    # irrealmente limpio y el modelo aprende algo que no va a encontrar.
    k = 0
    while True:
        idx = int(k * period + rng.normal(0.0, period * 0.01))
        if idx >= n:
            break
        if idx >= 0:
            local_t = t[idx:] - t[idx]
            burst = np.exp(-DECAY_RATE * local_t) * np.sin(
                2.0 * np.pi * RESONANCE_HZ * local_t
            )
            amp = severity
            if modulation == "shaft":
                # Pista interior: entra y sale de la zona de carga.
                amp *= 0.5 + 0.5 * np.sin(2.0 * np.pi * f_shaft * t[idx])
            elif modulation == "cage":
                amp *= 0.5 + 0.5 * np.sin(2.0 * np.pi * f_shaft * CAGE_ORDER * t[idx])
            signal[idx:] += amp * burst
        k += 1

    return signal


def generate_window(fault_class, severity, n=2048, fs_hz=12000.0, rpm=1750.0, rng=None):
    """Una ventana de vibración. severity en [0, 1]; se ignora si la clase es sana."""
    if rng is None:
        rng = np.random.default_rng()

    t = np.arange(n) / fs_hz
    f_shaft = rpm / 60.0

    # Ruido de fondo: electrónica del sensor y vibración de la estructura.
    signal = rng.normal(0.0, 0.08, n)

    # Armónicos de rotación: siempre presentes, incluso sano. El 1x es
    # desbalance residual, el 2x desalineamiento del acoplamiento.
    signal += 0.30 * np.sin(2.0 * np.pi * f_shaft * t + rng.uniform(0, 2 * np.pi))
    signal += 0.12 * np.sin(2.0 * np.pi * 2 * f_shaft * t + rng.uniform(0, 2 * np.pi))
    signal += 0.05 * np.sin(2.0 * np.pi * 3 * f_shaft * t + rng.uniform(0, 2 * np.pi))

    order = FAULT_ORDERS[fault_class]
    if order is not None and severity > 0.0:
        modulation = {
            "pista_interior": "shaft",
            "pista_exterior": None,
            "elemento_rodante": "cage",
        }[fault_class]
        signal += _impact_train(
            n, fs_hz, f_shaft, order, severity, modulation, rng
        )

    return signal.astype(np.float64)


def generate_dataset(n_recordings=40, windows_per_recording=60, seed=0,
                     n=2048, fs_hz=12000.0):
    """Un dataset sintético con la MISMA estructura que el real.

    Genera "grabaciones" y no ventanas sueltas a propósito: el split honesto
    es por archivo de origen, y para poder ensayarlo acá hace falta que
    exista esa agrupación. Cada grabación tiene su propio nivel de ruido y
    su propia rpm, que es justamente la huella que un split mal hecho
    permitiría al modelo memorizar.
    """
    rng = np.random.default_rng(seed)
    windows, labels, severities, recordings, rpms = [], [], [], [], []

    for rec in range(n_recordings):
        fault_class = CLASSES[rec % len(CLASSES)]
        severity = 0.0 if fault_class == "sano" else rng.uniform(0.15, 1.0)
        rpm = rng.uniform(1600.0, 1800.0)
        gain = rng.uniform(0.85, 1.15)      # huella de calibración por grabación

        for _ in range(windows_per_recording):
            w = generate_window(fault_class, severity, n, fs_hz, rpm, rng) * gain
            windows.append(w)
            labels.append(CLASSES.index(fault_class))
            severities.append(severity)
            recordings.append(f"rec_{rec:03d}")
            rpms.append(rpm)

    return {
        "windows": np.array(windows),
        "label": np.array(labels, dtype=np.int32),
        "severity": np.array(severities, dtype=np.float32),
        "source_file": np.array(recordings),
        "rpm": np.array(rpms, dtype=np.float64),
        "fs_hz": fs_hz,
    }
