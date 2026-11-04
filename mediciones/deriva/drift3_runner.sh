#!/bin/bash
# Deriva con CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP=y y CLOCK_RESYNC_SEC=0.
# B60 30 min, B150 35 min, B300 45 min. Al final vuelve a sleep 0 y repone CLOCK_RESYNC_SEC=300.
MAC=34:85:18:A2:DE:1A
PORT=/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_34:85:18:A2:DE:18-if00
D=~/bench-logs/deriva3
PY=~/bench/venv/bin/python
export PYTHONIOENCODING=utf-8
python3 ~/bench/serial_log_quiet.py $PORT $D/imv2-1.log 9000 &
sleep 90   # arranque frío tras el flasheo: config por BLE
echo "B60 $($PY ~/bench/clock_cfg.py $MAC 60 20)" >> $D/runner.log
sleep 1800
echo "B150 $($PY ~/bench/clock_cfg.py $MAC 150 20)" >> $D/runner.log
sleep 2100
echo "B300 $($PY ~/bench/clock_cfg.py $MAC 300 20)" >> $D/runner.log
sleep 2700
echo "fin $($PY ~/bench/clock_cfg.py $MAC 0 20)" >> $D/runner.log
cd ~/Nebula-edge/raspberry && sed -i "s/^CLOCK_RESYNC_SEC=0$/CLOCK_RESYNC_SEC=300/" .env && docker compose up -d server >> $D/runner.log 2>&1
echo "resync $(docker exec nebulaedge_server printenv CLOCK_RESYNC_SEC)" >> $D/runner.log
touch $D/done
