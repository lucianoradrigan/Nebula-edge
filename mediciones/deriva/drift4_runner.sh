#!/bin/bash
# Deriva 4 (16 puntos): verificar desfase por ciclo = c + k·T_sleep.
# CLOCK_RESYNC_SEC=0 durante la prueba; SKIP_VALIDATE_IN_DEEP_SLEEP=y en el firmware.
# Dos pasadas, la segunda en orden inverso, para separar el efecto del largo de la
# deriva lenta de la noche (temperatura). Al final vuelve a sleep 0 y resync 300.
MAC=34:85:18:A2:DE:1A
PORT=/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_34:85:18:A2:DE:18-if00
D=~/bench-logs/deriva4
PY=~/bench/venv/bin/python
AWAKE=18
ORDER="300 90 420 240 540 45 10 180 480 30 600 120 360 20 60 150"
REV=$(echo $ORDER | tr ' ' '\n' | tac | tr '\n' ' ')
export PYTHONIOENCODING=utf-8
cd ~/Nebula-edge/raspberry
sed -i "s/^CLOCK_RESYNC_SEC=300$/CLOCK_RESYNC_SEC=0/" .env && docker compose up -d server >> $D/runner.log 2>&1
echo "resync $(docker exec nebulaedge_server printenv CLOCK_RESYNC_SEC)" >> $D/runner.log
# No abrir dos loggers en el mismo puerto: esperar a que termine el de deriva 3.
( while pgrep -f "^python3 .*serial_log_quiet.py .*DE:18" >/dev/null; do sleep 10; done
  python3 ~/bench/serial_log_quiet.py $PORT $D/imv2-1.log 42000 ) &
sleep 30
for T in $ORDER $REV; do
  N=5
  echo "B$T $($PY ~/bench/clock_cfg.py $MAC $T 20) n=$N" >> $D/runner.log
  sleep $(( N * (T + AWAKE) ))
done
echo "fin $($PY ~/bench/clock_cfg.py $MAC 0 20)" >> $D/runner.log
sed -i "s/^CLOCK_RESYNC_SEC=0$/CLOCK_RESYNC_SEC=300/" .env && docker compose up -d server >> $D/runner.log 2>&1
echo "resync $(docker exec nebulaedge_server printenv CLOCK_RESYNC_SEC)" >> $D/runner.log
touch $D/done
