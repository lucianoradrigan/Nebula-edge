#!/bin/bash
# Prueba masiva de pérdida con 7 placas (plan del 04-11).
# Uso: masiva_runner.sh DIRECTORIO humo|noche
#   humo:  UDP, TCP, MQTT y BLE en B30, 5 min de régimen cada uno (red AP en wlan0).
#   noche: A (AP) -> F (fallas) -> D (deriva, resync 0) -> cambio a router -> B (router) -> vuelta al AP.
D=$1; MODE=$2
MACS="34:85:18:A2:DB:4E 34:85:18:A2:DE:16 34:85:18:A2:DB:06 1C:DB:D4:66:51:CE 1C:DB:D4:66:52:16"   # DE:1A y DB:A2 apagadas desde el 05-11
NEAR_PORTS="/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_34:85:18:A2:DB:4C-if00 /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_34:85:18:A2:DE:14-if00"
PY=~/bench/venv/bin/python
RB=~/Nebula-edge/raspberry
M=3     # minutos de margen por transición, antes del régimen
export PYTHONIOENCODING=utf-8
mkdir -p $D

log() { echo "$(date +%s) $*" >> $D/runner.log; }
cfg_all() {    # PROTO SLEEP WINDOW
  for m in $MACS; do echo "  $m $($PY ~/bench/cfg_set.py $m $1 $2 $3)" >> $D/runner.log; done
}
run() {        # NOMBRE PROTO SLEEP WINDOW MINUTOS_DE_RÉGIMEN
  log "INICIO $1 proto=$2 sleep=$3 window=$4 regimen_min=$5"
  cfg_all $2 $3 $4
  sleep $(( ($M + $5) * 60 ))
  log "FIN $1"
}
loggers_on() {
  for p in $NEAR_PORTS; do
    n=$(echo $p | grep -oE "[0-9A-F]{2}:[0-9A-F]{2}-if00" | cut -c1-5 | tr -d :)
    setsid python3 ~/bench/serial_log_quiet.py $p $D/serie_$n.log $1 > /dev/null 2>&1 < /dev/null &
  done
}
loggers_off() { pkill -f "^python3 /home/nebulaedge/bench/serial_log_quiet.py"; sleep 1; }
resync() {
  sed -i "s/^CLOCK_RESYNC_SEC=.*/CLOCK_RESYNC_SEC=$1/" $RB/.env
  (cd $RB && docker compose up -d server > /dev/null 2>&1)
  log "RESYNC $(docker exec nebulaedge_server printenv CLOCK_RESYNC_SEC)"
}
evento_en() {  # NOMBRE PROTO MINUTO_DEL_EVENTO MINUTOS_TOTALES COMANDO
  log "INICIO $1 proto=$2 sleep=0 window=20 regimen_min=$4"
  cfg_all $2 0 20
  sleep $(( ($M + $3) * 60 ))
  log "EVENTO $1"; eval "$5"; log "EVENTO_FIN $1"
  sleep $(( ($4 - $3) * 60 ))
  log "FIN $1"
}
reset_cercanas() {
  loggers_off
  source ~/esp/esp-idf/export.sh > /dev/null 2>&1
  for p in $NEAR_PORTS; do python -m esptool --chip esp32s3 -p $p --after hard_reset read_mac > /dev/null 2>&1; done
  loggers_on 30000
}
red() {        # ap|router
  if [ "$1" = router ]; then sudo -n nmcli con down nebulaedge_ap_int; sudo -n nmcli con up "Casa azul int"
  else sudo -n nmcli con down "Casa azul int"; sudo -n nmcli con up nebulaedge_ap_int; fi > /dev/null 2>&1
  sleep 5
  (cd $RB && docker compose restart server > /dev/null 2>&1)
  log "RED $1 $(nmcli -t -f DEVICE,STATE,CONNECTION dev | grep ^wlan0)"
}
cuantas_mandan() {
  n=$(cd $RB && docker compose exec -T db psql -U nebulaedge -d nebulaedge -At -c \
    "SELECT count(DISTINCT id_device) FROM nebulaedge_schema.data_1 WHERE time_server > now() AT TIME ZONE 'UTC' - interval '2 min'")
  log "MANDAN $n de $(echo $MACS | wc -w)"
}
salud() {      # una línea por minuto hasta que exista done
  while [ ! -f $D/done ]; do
    s=$(docker stats --no-stream --format "{{.Name}}={{.CPUPerc}},{{.MemUsage}}" 2>/dev/null | tr '\n' ' ')
    t=$(vcgencmd measure_temp | cut -d= -f2); th=$(vcgencmd get_throttled | cut -d= -f2)
    st=$(/usr/sbin/iw dev wlan0 station dump 2>/dev/null | grep -c ^Station)
    er=$(docker logs --since 60s nebulaedge_server 2>&1 | grep -ciE "error|falló|excepci|traceback")
    echo "$(date +%s) temp=$t throttled=$th load=$(cut -d' ' -f1 /proc/loadavg) estaciones=$st errores_log=$er $s" >> $D/salud.log
    sleep 60
  done
}

log "MODO $MODE"
salud &
if [ "$MODE" = humo ]; then
  loggers_on 2400
  run humo_udp 1 30 10 5
  run humo_tcp 2 30 10 5
  run humo_mqtt 0 30 10 5
  run humo_ble 3 30 10 5
  cfg_all 1 0 20
elif [ "$MODE" = resto ]; then
  # Lo que se perdió el 05-11 (servidor con SSID vacío): B completo y Z, ~5,5 h.
  loggers_on 21000
  cfg_all 1 30 10; sleep 180
  red router
  sleep 900; cuantas_mandan
  run B_udp_ds 1 30 10 30;  run B_udp_cont 1 0 20 30
  run B_tcp_ds 2 30 10 30;  run B_tcp_cont 2 0 20 30
  run B_mqtt_ds 0 30 10 30; run B_mqtt_cont 0 0 20 30
  cfg_all 1 30 10; sleep 180
  red ap
  sleep 720; cuantas_mandan
  run Z_ble_ds 3 30 10 30;  run Z_ble_cont 3 0 20 30
  cfg_all 1 0 20
else
  loggers_on 40000
  # Bloque A: AP en wlan0
  run A_udp_ds 1 30 10 30;  run A_udp_cont 1 0 20 30
  run A_tcp_ds 2 30 10 30;  run A_tcp_cont 2 0 20 30
  run A_mqtt_ds 0 30 10 30; run A_mqtt_cont 0 0 20 30
  # Bloque F: fallas, en continuo
  evento_en F_server_udp 1 5 10 "(cd $RB && docker compose restart server > /dev/null 2>&1)"
  evento_en F_server_tcp 2 5 10 "(cd $RB && docker compose restart server > /dev/null 2>&1)"
  evento_en F_broker_mqtt 0 5 10 "(cd $RB && docker compose restart mqtt > /dev/null 2>&1)"
  evento_en F_db_udp 1 5 10 "(cd $RB && docker compose restart db > /dev/null 2>&1)"
  evento_en F_reset_cercanas_udp 1 5 10 "reset_cercanas"
  # Bloque D: deriva sin resync
  resync 0
  run D_udp_b60 1 60 10 30
  run D_udp_b300 1 300 10 30
  # Cambio a router, con las placas durmiendo
  cfg_all 1 30 10
  red router
  resync 300
  sleep 900; cuantas_mandan
  # Bloque B: router por wlan0
  run B_udp_ds 1 30 10 30;  run B_udp_cont 1 0 20 30
  run B_tcp_ds 2 30 10 30;  run B_tcp_cont 2 0 20 30
  run B_mqtt_ds 0 30 10 30; run B_mqtt_cont 0 0 20 30
  # Vuelta al AP, otra vez con las placas durmiendo
  cfg_all 1 30 10; sleep 180
  red ap
  sleep 720; cuantas_mandan
  # BLE al final: con 7 placas en deep sleep el dongle no alcanza a reconectarlas
  # todas y alguna puede colgarse; así no afecta a lo demás.
  run Z_ble_ds 3 30 10 30;  run Z_ble_cont 3 0 20 30
  cfg_all 1 0 20
fi
log "TERMINADO"
touch $D/done
