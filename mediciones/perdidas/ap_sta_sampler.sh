#!/bin/bash
# Muestrea las estadísticas del AP para una estación cada 0,5 s.
# Uso: ap_sta_sampler.sh MAC_WIFI SEGUNDOS SALIDA
# Una línea por muestra: epoch;campo=valor;...  (si la estación no está asociada: epoch;ausente)
MAC=$1; END=$(( $(date +%s) + $2 )); OUT=$3
while [ $(date +%s) -lt $END ]; do
  T=$(date +%s.%N)
  S=$(/usr/sbin/iw dev wlan_ap station get $MAC 2>/dev/null | awk -F':\t*' '/rx packets|rx drop misc|tx retries|tx failed|signal:|rx bitrate|tx bitrate|connected time|inactive time/{gsub(/^[ \t]+|[ \t]+$/,"",$1);gsub(/^[ \t]+/,"",$2);printf "%s=%s;",$1,$2}')
  echo "$T;${S:-ausente}" >> $OUT
  sleep 0.5
done
