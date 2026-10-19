# ml: modelos para el modo INFER

Entrena el modelo que corre en el ESP32 en modo INFER (`protocol_conf = 4`) y lo
deja listo para cargar en la base del servidor. El modelo es uno solo con dos
salidas: el **tipo de falla** de rodamiento (sano, pista interior, pista
exterior, elemento rodante) y su **severidad** (0 a 1), a partir de 20 features
calculadas sobre una ventana de vibración de 2048 muestras.

## Entorno

Python 3.11 en un entorno virtual propio (no se versiona: TensorFlow pesa
~1,8 GB). Probado con TensorFlow 2.21, numpy 2.4 y pyarrow 26.

```bash
cd ml
python3.11 -m venv .venv
.venv/bin/pip install tensorflow numpy pyarrow
```

Los datos van en `ml/archive/` (no se versiona, ~47 GB). `cwru.py` lee los
parquet de CWRU en `archive/CWRU/CWRU/raw_parquet/`.

## De los datos a la placa

```bash
# 1. Entrenar. Deja <out>.keras y <out>_test.npz
.venv/bin/python train.py --data cwru --trunk 256,128 --out model_cwru_wide

# 2. Convertir a TFLite (--float = float32; sin él, int8). Verifica contra Keras
.venv/bin/python convert.py --model model_cwru_wide --float

# 3. INSERT del modelo para la tabla `model` (una versión no se reusa nunca)
.venv/bin/python export_model.py --model model_cwru_wide --tflite model_cwru_wide_f32.tflite --version 3

# 4. El set de prueba para `infer_sample` (el test del split, sin estandarizar)
.venv/bin/python export_testset.py --data cwru

# 5. Cargar ambos en la base, desde raspberry/
docker compose exec -T db psql -U nebulaedge -d nebulaedge < ../ml/model_cwru_wide_f32_v3.sql
docker compose exec -T db psql -U nebulaedge -d nebulaedge < ../ml/testset_cwru_3hp.sql
```

Después se apunta un device al modelo desde la tabla `config` (ver la sección 9
del README de la raíz). El device lo baja por MQTT, lo verifica y lo activa sin
reflashear.

Antes de flashear un cambio en las features, `check_features_c.py` compila el
port a C del firmware (`esp32/components/nebulaedge_features`) en el computador
y lo compara contra Python ventana por ventana.

## Archivos

| Archivo | Para qué sirve |
|---|---|
| `data_sources.py` | Registro de datasets y el contrato de lo que entrega un cargador. Un dataset nuevo = un cargador + una entrada acá. |
| `cwru.py`, `synthetic.py` | Cargadores: CWRU (solo `acc_x`) y una señal sintética para ensayar el pipeline. |
| `features.py` | Las 20 features. Es la REFERENCIA del firmware: el port a C tiene que dar lo mismo. |
| `train.py` | Entrena el modelo de dos cabezas y guarda el split de test. |
| `convert.py` | Pasa a TFLite y compara contra Keras. |
| `export_model.py` | Arma el `INSERT` del modelo, con su autotest (una fila y lo que el modelo responde). |
| `export_testset.py` | Arma el set de prueba que el servidor manda al device. |
| `check_features_c.py` | Compara las features del firmware contra las de Python. |

Lo que producen (`*.keras`, `*.npz`, `*.tflite`, `*_data.cc/.h`, `*_v*.sql`,
`testset_*.sql`) no se versiona.

## Reglas que no hay que romper

- **Split por condición, no por ventana.** CWRU se divide por carga del motor:
  train 0-1 hp, val 2 hp, test 3 hp. Partir por ventana mete ventanas de la misma
  grabación en train y test, y la accuracy sale inflada (`train.py --leaky` lo
  muestra).
- **Las features tienen versión.** `FEATURE_SET_VERSION` en `features.py` y
  `FEATURES_SET_VERSION` en el firmware tienen que coincidir. El device rechaza un
  modelo entrenado con otra versión en vez de inferir basura. Cambiar una banda,
  la ventana o el orden = subir las dos y reflashear.
- **La estandarización viaja con el modelo.** `feat_mean` y `feat_std` salen del
  train y van en el manifiesto; el device las aplica antes de invocar.

## Resultado de referencia

Modelo v3 (tronco 256-128, float32, 158 KB), set de 3 hp de CWRU (769 ventanas):
99,09% de accuracy y MAE de severidad 0,0721. Da lo mismo en el computador y en
la IM-V2, con las features calculadas en Python o en el device.

Un modelo entrenado con CWRU (12 kHz, acelerómetro industrial) no sirve tal cual
para los sensores de la placa: el BMI270 muestrea a 1,6 kHz como máximo. Para
clasificar datos propios hace falta capturarlos y, probablemente, un conjunto de
features nuevo pensado para esa frecuencia.
