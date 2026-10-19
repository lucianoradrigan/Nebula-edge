#include <math.h>
#include <string.h>
#include "nebulaedge_features.h"

/* Ver ml/features.py: el porqué de cada feature está allá, que es la
 * referencia. Acá solo lo que es propio del port. */

#define N      FEATURES_WINDOW_SIZE
#define M      (N / 2)                  // la FFT compleja que de verdad se hace
#define EPS    1e-12

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const float ORDER_BANDS[6][2] = {
    {0.2f, 0.8f},   // jaula
    {0.8f, 1.5f},   // giro del eje
    {1.5f, 2.5f},   // 2x
    {2.5f, 4.5f},   // elemento rodante / pista exterior
    {4.5f, 8.0f},   // pista interior
    {8.0f, 16.0f},  // armónicos altos
};

/* UNA FFT REAL DE N COMO UNA COMPLEJA DE N/2
 *
 * La señal es real, así que la mitad de una FFT compleja de N sería trabajo
 * tirado. Se empaquetan las muestras pares como parte real y las impares como
 * imaginaria (z[k] = x[2k] + i x[2k+1]), se hace una FFT compleja de M = N/2 y
 * se recombina. Es la mitad de cálculo y la mitad de memoria: 8 KB de buffer
 * en vez de 16, que es lo que decide si entra junto a la arena de TFLM. */
static float s_z[2 * M];                // re, im intercalados
static float s_tw[M];                   // cos y sin de -2*pi*j/M, j < M/2, intercalados
static int   s_tw_ready = 0;

static void init_twiddles(void) {
    for (int j = 0; j < M / 2; j++) {
        double a = -2.0 * M_PI * j / M;
        s_tw[2 * j] = (float)cos(a);
        s_tw[2 * j + 1] = (float)sin(a);
    }
    s_tw_ready = 1;
}

/* FFT compleja radix-2, en el lugar, sobre s_z (M puntos). */
static void fft_m(void) {
    // Permutación por inversión de bits.
    for (int i = 1, j = 0; i < M; i++) {
        int bit = M >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float tr = s_z[2 * i], ti = s_z[2 * i + 1];
            s_z[2 * i] = s_z[2 * j];
            s_z[2 * i + 1] = s_z[2 * j + 1];
            s_z[2 * j] = tr;
            s_z[2 * j + 1] = ti;
        }
    }
    for (int len = 2; len <= M; len <<= 1) {
        int half = len >> 1, step = M / len;
        for (int i = 0; i < M; i += len) {
            for (int k = 0; k < half; k++) {
                float wr = s_tw[2 * k * step], wi = s_tw[2 * k * step + 1];
                float *a = &s_z[2 * (i + k)], *b = &s_z[2 * (i + k + half)];
                float br = b[0] * wr - b[1] * wi;
                float bi = b[0] * wi + b[1] * wr;
                b[0] = a[0] - br;
                b[1] = a[1] - bi;
                a[0] += br;
                a[1] += bi;
            }
        }
    }
}

/* |X[k]|^2 de la FFT real de N puntos de lo que haya en s_z (ya empaquetado),
 * para k = 0..M, acumulado por banda de orden. Escribe en `out[6]` la fracción
 * de la potencia total (sin el bin DC) que cae en cada banda. */
static void band_fractions(float fs_hz, float rpm, float *out) {
    fft_m();

    double f_shaft = rpm / 60.0;
    double bin_order = (fs_hz / (double)N) / f_shaft;     // orden de cada bin k: k * bin_order
    double band[6] = {0};
    double total = 0;

    for (int k = 1; k <= M; k++) {
        // X[k] = (Z[k] + conj(Z[M-k]))/2 + e^{-2*pi*i*k/N} (Z[k] - conj(Z[M-k]))/(2i)
        int k1 = k % M, k2 = (M - k) % M;
        double zr = s_z[2 * k1], zi = s_z[2 * k1 + 1];
        double cr = s_z[2 * k2], ci = -s_z[2 * k2 + 1];
        double er = 0.5 * (zr + cr), ei = 0.5 * (zi + ci);       // parte par
        double or_ = 0.5 * (zi - ci), oi = -0.5 * (zr - cr);     // parte impar: (Z - conj)/(2i)
        double a = -2.0 * M_PI * k / N;
        double wr = cos(a), wi = sin(a);
        double xr = er + wr * or_ - wi * oi;
        double xi = ei + wr * oi + wi * or_;
        double p = xr * xr + xi * xi;

        total += p;
        double order = k * bin_order;
        for (int b = 0; b < 6; b++) {
            if (order >= ORDER_BANDS[b][0] && order < ORDER_BANDS[b][1]) {
                band[b] += p;
            }
        }
    }

    for (int b = 0; b < 6; b++) {
        out[b] = total > EPS ? (float)(band[b] / total) : 0.0f;
    }
}

static inline float hann(int i) {
    return (float)(0.5 - 0.5 * cos(2.0 * M_PI * i / N));
}

int features_extract(const float *x, size_t n, float fs_hz, float rpm, float out[FEATURES_COUNT]) {
    if (n != N) {
        return -1;
    }
    if (!s_tw_ready) {
        init_twiddles();
    }

    /* ---- temporales: acumuladores en double, como numpy ---- */
    double sum = 0, sum_sq = 0, sum_abs = 0, sum_sqrt = 0;
    float peak = 0, mx = x[0], mn = x[0];
    for (int i = 0; i < N; i++) {
        double v = x[i];
        sum += v;
        sum_sq += v * v;
        double av = fabs(v);
        sum_abs += av;
        sum_sqrt += sqrt(av);
        if (av > peak) peak = (float)av;
        if (x[i] > mx) mx = x[i];
        if (x[i] < mn) mn = x[i];
    }
    double mean = sum / N;
    double m2 = 0, m3 = 0, m4 = 0;
    for (int i = 0; i < N; i++) {
        double c = x[i] - mean, c2 = c * c;
        m2 += c2;
        m3 += c2 * c;
        m4 += c2 * c2;
    }
    m2 /= N; m3 /= N; m4 /= N;

    double rms = sqrt(sum_sq / N);
    double std = sqrt(m2);
    double abs_mean = sum_abs / N;
    double sqrt_mean = sum_sqrt / N;

    out[0] = (float)rms;
    out[1] = std > EPS ? (float)(m4 / (std * std * std * std)) : 3.0f;
    out[2] = std > EPS ? (float)(m3 / (std * std * std)) : 0.0f;
    out[3] = rms > EPS ? (float)(peak / rms) : 0.0f;
    out[4] = abs_mean > EPS ? (float)(rms / abs_mean) : 0.0f;
    out[5] = abs_mean > EPS ? (float)(peak / abs_mean) : 0.0f;
    out[6] = sqrt_mean > EPS ? (float)(peak / (sqrt_mean * sqrt_mean)) : 0.0f;
    out[7] = mx - mn;

    if (rpm / 60.0 <= EPS) {
        memset(&out[8], 0, 12 * sizeof(float));
        return 0;
    }

    /* ---- orden: FFT de la señal con Hann ---- */
    for (int k = 0; k < M; k++) {
        s_z[2 * k] = x[2 * k] * hann(2 * k);
        s_z[2 * k + 1] = x[2 * k + 1] * hann(2 * k + 1);
    }
    band_fractions(fs_hz, rpm, &out[8]);

    /* ---- envolvente: |diferencia de primer orden| sin media, con Hann ----
     * np.diff(x, prepend=x[0]): la primera vale 0. */
    double env_sum = 0;
    for (int i = 0; i < N; i++) {
        env_sum += fabsf(i ? x[i] - x[i - 1] : 0.0f);
    }
    float env_mean = (float)(env_sum / N);
    for (int k = 0; k < M; k++) {
        int i0 = 2 * k, i1 = 2 * k + 1;
        float e0 = fabsf(i0 ? x[i0] - x[i0 - 1] : 0.0f) - env_mean;
        float e1 = fabsf(x[i1] - x[i1 - 1]) - env_mean;
        s_z[2 * k] = e0 * hann(i0);
        s_z[2 * k + 1] = e1 * hann(i1);
    }
    band_fractions(fs_hz, rpm, &out[14]);

    return 0;
}
