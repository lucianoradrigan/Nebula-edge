# syntax=docker/dockerfile:1

# Debian 12 "bookworm". La base anterior (bullseye) dejó de tener soporte LTS el
# 31-08-2026: sus paquetes de seguridad se movieron a archive.debian.org y el
# apt-get install empezó a dar 404. El tag flotante 3.11 sigue recibiendo
# parches del intérprete sin cambiar de versión menor de Python.
FROM python:3.11-slim-bookworm
WORKDIR /nebulaedge

# network-manager -> nmcli, que usa LocalWifiConfig para leer SSID/IP del host.
# bluez -> pila BLE que bleak maneja por D-Bus.
RUN apt-get update && apt-get install -y \
    network-manager \
    sudo \
    bluez \
    && rm -rf /var/lib/apt/lists/*

COPY requirements.txt requirements.txt
RUN pip install -r requirements.txt

COPY server/ .
COPY entrypoint.sh .
RUN chmod +x entrypoint.sh

ENTRYPOINT ["./entrypoint.sh"]