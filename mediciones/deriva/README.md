# Deriva del reloj del device en deep sleep

Mediciones del 3 y 4 de noviembre de 2026 en una sola placa IM-V2 (DE:1A), por
UDP a 1 paquete/s, `sleep_window_size = 20`, con la corrección de hora del
servidor apagada (`CLOCK_RESYNC_SEC=0`).

## Cómo cruza el reloj el deep sleep

El firmware no usa la hora RTC de ESP-IDF. Antes de dormir guarda
`hora_actual + sleep_us` (la duración programada), y al despertar pone
`hora = lo_guardado + esp_timer_get_time()` (`nebulaedge_device.c`). El error
por ciclo tiene entonces dos partes:

- **c, fija:** el arranque que no se cuenta (ROM, bootloader, validación de la
  imagen) y la cola desde que se guarda la hora hasta que el chip duerme;
- **k·T_sleep, proporcional:** el oscilador RC interno (`RTC_CLK_SRC_INT_RC`)
  despierta al chip después de un sueño real distinto del programado.

## Resultados

Salto por ciclo = c + k·T_sleep (positivo = el device se atrasa). Ajuste por
mínimos cuadrados sobre todos los saltos; incertidumbre de 1σ.

| Corrida | Firmware | Puntos (T_sleep) | Saltos | c | k |
|---|---|---|---|---|---|
| Deriva 2 | valida la imagen al despertar | 60, 300 s | 30 | +672 ± 28 ms | −5,89 ± 0,17 ms/s |
| Deriva 3 | `SKIP_VALIDATE_IN_DEEP_SLEEP=y` | 60, 150, 300 s | 41 | +194 ± 25 ms | −4,96 ± 0,16 ms/s |
| Deriva 4 | `SKIP_VALIDATE_IN_DEEP_SLEEP=y` | 16 puntos, 10–600 s, 2 pasadas | 128 | +210 ± 28 ms | −5,46 ± 0,09 ms/s |

Despierta (deriva 2, fase A) la deriva es de −2,8 ppm: el error viene solo
del deep sleep.

Lo que se concluye:

1. **El modelo lineal se sostiene** entre 10 y 600 s: ningún punto de
   deriva 4 se aleja más de ~60 ms de la recta hasta 240 s, y los largos
   quedan dentro de su propia dispersión.
2. **Validar la imagen al despertar cuesta ~0,5 s por ciclo** (c baja de
   ~670 a ~200 ms).
3. **El RC cuenta de más ~0,5–0,6 % del sueño.** k fue estable dentro de una
   noche (pasada 1: −5,42; pasada 2: −5,50 ms/s, con ~1 °C de diferencia),
   pero varió entre corridas (−4,96 a −5,89) sin que la temperatura del
   BME688 lo explique. El BME688 mide la placa, no el RC.
4. **La dispersión crece con T:** ~30 ms con sueños cortos y 200–400 ms con
   5–10 min, ~0,05 % del sueño. El RC tiembla de un sueño a otro además del
   sesgo fijo.

Con sueños de 5 min el device se desvía ~1,5 s por ciclo. Por eso el servidor
reenvía la hora cada 300 s (`CLOCK_RESYNC_SEC`) y `time_server` es la hora de
referencia de los datos. Corregir k de fondo exige un cristal de 32 kHz.

## Archivos

- `derivaN.csv`: filas de `data_1` del device durante la corrida
  (`time_server`, `time_sent`, `seq`, `temperature`), epoch en segundos.
- `derivaN_fases.txt`: log del runner, con el inicio de cada fase.
- `analisis.py`: el análisis de arriba. `python3 analisis.py deriva4.csv deriva4_fases.txt`.
- `drift3_runner.sh`, `drift4_runner.sh`: los scripts que corrieron en la Pi
  (`~/bench/`). `clock_cfg.py` cambia la config del device subiendo la versión.
  Deriva 2 usó un runner equivalente que no se guardó.
