"""Extracción de features de vibración.

Este módulo es la REFERENCIA del extractor que después corre en el ESP32.
Todo lo que está acá tiene que poder escribirse en C sobre float32 sin
sorpresas: aritmética básica y una FFT, nada más. Si acá se usara algo que
numpy hace fácil y C no, la paridad entre los dos lados se rompe y el modelo
"funciona distinto en el dispositivo" sin que el modelo tenga nada malo.

Las features son de dos familias:

  - TEMPORALES ADIMENSIONALES (kurtosis, factor de cresta, ...). No dependen
    de la frecuencia de muestreo ni de la calibración del acelerómetro, así
    que valen igual en CWRU a 12 kHz que en Paderborn a 64 kHz.

  - DE ORDEN. El eje de frecuencia se normaliza por la velocidad de giro: en
    vez de "energía en 157 Hz" se trabaja con "energía en el orden 3.58x".
    Un modelo entrenado a 1750 rpm sirve así a 1200 rpm, y las bandas
    significan lo mismo en datasets con fs distinto. Es la razón por la que
    el dataset trae la columna rpm.

  - DE ENVOLVENTE, que es la familia que de verdad diagnostica rodamientos.

El motivo de la tercera familia merece explicación, porque es contraintuitivo
y porque se midió acá: la frecuencia del defecto NO aparece en el espectro
crudo. Un defecto localizado golpea a su frecuencia característica (p.ej.
3.58x, unos 104 Hz a 1750 rpm), pero cada golpe excita una resonancia
ESTRUCTURAL de varios kHz. Toda la energía se va a esa resonancia: midiendo
esta señal sintética, el 26% de la energía está en 2500-3500 Hz y apenas el
0.07% en 100-200 Hz, que es donde está el defecto.

Lo que el defecto hace en el espectro crudo es modular esa resonancia, o sea
aparecer como bandas laterales alrededor de ella. Para recuperarlo se
rectifica la señal pasada por alto —eso es la envolvente— y se le hace la FFT
a ESO. Ahí el defecto aparece limpio: en la misma medición, el pico de la
envolvente cayó en orden 3.62x contra 3.58x teórico.

Para el port a C: son DOS FFT, no una. La envolvente es una operación no
lineal (valor absoluto) y necesita su propia transformada.
"""

import numpy as np

WINDOW_SIZE = 2048

# Evita divisiones por cero en señales constantes o silenciosas.
_EPS = 1e-12

# Bandas de orden donde viven los defectos típicos de un rodamiento. Las
# frecuencias exactas dependen de la geometría (número de elementos, ángulo
# de contacto), que el dataset no entrega, así que se usan rangos que las
# cubren: FTF (jaula) ~0.4x, BSF (elemento rodante) ~2-3x, BPFO (pista
# exterior) ~3-5x, BPFI (pista interior) ~5-8x, más armónicos.
ORDER_BANDS = (
    (0.2, 0.8),    # jaula
    (0.8, 1.5),    # giro del eje: desbalance
    (1.5, 2.5),    # 2x: desalineamiento
    (2.5, 4.5),    # elemento rodante / pista exterior
    (4.5, 8.0),    # pista interior
    (8.0, 16.0),   # armónicos altos
)

FEATURE_NAMES = [
    "rms",
    "kurtosis",
    "skewness",
    "crest_factor",
    "shape_factor",
    "impulse_factor",
    "clearance_factor",
    "peak_to_peak",
] + [f"order_{lo}_{hi}" for lo, hi in ORDER_BANDS] \
  + [f"env_{lo}_{hi}" for lo, hi in ORDER_BANDS]

N_FEATURES = len(FEATURE_NAMES)

# Versión de ESTE cálculo de features. Viaja en el manifiesto del modelo: el
# firmware sabe calcular una versión concreta y rechaza modelos entrenados con
# otra. Subirla cada vez que cambie algo que mueva los números -una banda, la
# ventana de Hann, el orden de FEATURE_NAMES-, aunque N_FEATURES siga igual.
FEATURE_SET_VERSION = 1


def time_domain_features(window):
    """Ocho descriptores estadísticos de la forma de onda.

    La kurtosis es la importante para rodamientos: una falla incipiente
    genera impactos cortos y periódicos, y esos impactos levantan la
    kurtosis mucho antes de que el RMS se mueva. Vale 3.0 para ruido
    gaussiano (es kurtosis cruda, no en exceso).
    """
    x = np.asarray(window, dtype=np.float64)

    mean = float(np.mean(x))
    centered = x - mean

    rms = float(np.sqrt(np.mean(x * x)))
    std = float(np.sqrt(np.mean(centered * centered)))
    peak = float(np.max(np.abs(x)))
    abs_mean = float(np.mean(np.abs(x)))
    sqrt_mean = float(np.mean(np.sqrt(np.abs(x))))

    if std > _EPS:
        kurtosis = float(np.mean(centered ** 4) / (std ** 4))
        skewness = float(np.mean(centered ** 3) / (std ** 3))
    else:
        kurtosis, skewness = 3.0, 0.0

    crest = peak / rms if rms > _EPS else 0.0
    shape = rms / abs_mean if abs_mean > _EPS else 0.0
    impulse = peak / abs_mean if abs_mean > _EPS else 0.0
    clearance = peak / (sqrt_mean ** 2) if sqrt_mean > _EPS else 0.0
    p2p = float(np.max(x) - np.min(x))

    return [rms, kurtosis, skewness, crest, shape, impulse, clearance, p2p]


def order_domain_features(window, fs_hz, rpm):
    """Energía relativa en cada banda de orden.

    Se devuelve la FRACCIÓN de la energía total, no la energía absoluta: así
    la feature tampoco depende de la calibración del acelerómetro ni de la
    ganancia del amplificador, que varían entre datasets.
    """
    x = np.asarray(window, dtype=np.float64)
    n = len(x)

    f_shaft = float(rpm) / 60.0
    if f_shaft <= _EPS:
        return [0.0] * len(ORDER_BANDS)

    # Hann para contener la fuga espectral. Una ventana rectangular esparce
    # la energía de cada pico sobre bandas vecinas y ensucia justo lo que se
    # quiere medir.
    hann = 0.5 - 0.5 * np.cos(2.0 * np.pi * np.arange(n) / n)
    spectrum = np.abs(np.fft.rfft(x * hann))
    power = spectrum * spectrum

    orders = np.fft.rfftfreq(n, d=1.0 / float(fs_hz)) / f_shaft

    # Se excluye el bin DC: el offset del sensor no es información de falla.
    total = float(np.sum(power[1:]))
    if total <= _EPS:
        return [0.0] * len(ORDER_BANDS)

    out = []
    for lo, hi in ORDER_BANDS:
        mask = (orders >= lo) & (orders < hi)
        mask[0] = False
        out.append(float(np.sum(power[mask]) / total))
    return out


def envelope_domain_features(window, fs_hz, rpm):
    """Energía relativa por banda de orden en el espectro de la ENVOLVENTE.

    Es la demodulación clásica de diagnóstico de rodamientos, en su versión
    barata: pasaalto, rectificar, FFT. El pasaalto es una diferencia de
    primer orden —y = x[n] - x[n-1]— que quita los armónicos de rotación,
    que son dos órdenes de magnitud más grandes y si no taparían todo. El
    valor absoluto es el detector de envolvente.

    Se evita la transformada de Hilbert a propósito: daría una envolvente
    más limpia pero cuesta una FFT compleja más una inversa, y hay que
    escribir esto en C sobre un ESP32.
    """
    x = np.asarray(window, dtype=np.float64)
    n = len(x)

    f_shaft = float(rpm) / 60.0
    if f_shaft <= _EPS:
        return [0.0] * len(ORDER_BANDS)

    highpassed = np.diff(x, prepend=x[0])
    envelope = np.abs(highpassed)
    envelope = envelope - np.mean(envelope)

    hann = 0.5 - 0.5 * np.cos(2.0 * np.pi * np.arange(n) / n)
    spectrum = np.abs(np.fft.rfft(envelope * hann))
    power = spectrum * spectrum

    orders = np.fft.rfftfreq(n, d=1.0 / float(fs_hz)) / f_shaft

    total = float(np.sum(power[1:]))
    if total <= _EPS:
        return [0.0] * len(ORDER_BANDS)

    out = []
    for lo, hi in ORDER_BANDS:
        mask = (orders >= lo) & (orders < hi)
        mask[0] = False
        out.append(float(np.sum(power[mask]) / total))
    return out


def extract(window, fs_hz, rpm):
    """Vector completo de features para una ventana. Devuelve N_FEATURES floats."""
    return np.array(
        time_domain_features(window)
        + order_domain_features(window, fs_hz, rpm)
        + envelope_domain_features(window, fs_hz, rpm),
        dtype=np.float32,
    )
