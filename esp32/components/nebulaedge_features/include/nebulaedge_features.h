#ifndef NEBULAEDGE_FEATURES
#define NEBULAEDGE_FEATURES

#include <stddef.h>

/* Port a C de ml/features.py: 20 features de vibración a partir de una ventana
 * cruda. Python es la REFERENCIA; esto tiene que dar lo mismo, porque el
 * modelo se entrenó con lo que da Python.
 *
 * C portable a propósito: no incluye nada de ESP-IDF, así se compila también en
 * el Mac y se compara contra Python ventana por ventana
 * (ml/check_features_c.py) antes de llevarlo a la placa.
 *
 * Tiene que calzar con FEATURE_SET_VERSION de ml/features.py. Si cambia algo
 * que mueva los números -una banda, la ventana de Hann, el orden-, se suben
 * las dos. */

#define FEATURES_SET_VERSION  1
#define FEATURES_COUNT        20
#define FEATURES_WINDOW_SIZE  2048

/* Calcula las features de `x` (FEATURES_WINDOW_SIZE muestras) en `out`.
 * Devuelve 0, o -1 si `n` no es FEATURES_WINDOW_SIZE.
 *
 * No es reentrante: usa un buffer estático de 12 KB para las FFT, en vez de
 * pedirlos al heap en cada llamada (en INFER el bloque contiguo libre ronda
 * los 30 KB). Se llama desde una sola task. */
int features_extract(const float *x, size_t n, float fs_hz, float rpm, float out[FEATURES_COUNT]);

#endif
