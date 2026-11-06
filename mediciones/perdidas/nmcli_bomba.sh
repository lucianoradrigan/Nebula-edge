#!/bin/bash
# Repite, desde el contenedor del servidor, las consultas de LocalWifiConfig
# (system.py) dos veces por segundo, y registra cada respuesta vacía o con error.
# Uso: nmcli_bomba.sh SALIDA SEGUNDOS
OUT=$1; END=$(( $(date +%s) + $2 ))
while [ $(date +%s) -lt $END ]; do
  docker exec nebulaedge_server sh -c '
    t=$(date +%s.%N | cut -c1-14)
    c=$(sudo nmcli -t -g GENERAL.CONNECTION dev show wlan0 2>/tmp/e1); r1=$?
    s=$(sudo nmcli -t -g 802-11-wireless.ssid connection show "$c" 2>/tmp/e2); r2=$?
    p=$(sudo nmcli -s -g 802-11-wireless-security.psk connection show "$c" 2>/tmp/e3); r3=$?
    i=$(sudo nmcli -t -g IP4.ADDRESS dev show wlan0 2>/tmp/e4); r4=$?
    st=$(sudo nmcli -t -g GENERAL.STATE dev show wlan0 2>/dev/null)
    ok=1; [ -z "$s" ] && ok=0; [ -z "$p" ] && ok=0
    echo "$t ok=$ok conn=[$c]/$r1 ssid=[$s]/$r2 psk_len=${#p}/$r3 ip=[$i]/$r4 estado=[$st] err=[$(cat /tmp/e1 /tmp/e2 /tmp/e3 /tmp/e4 | tr "\n" " " | cut -c1-150)]"
  ' >> $OUT 2>&1
  sleep 0.5
done
