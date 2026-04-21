# syntax=docker/dockerfile:1
FROM python:3.11.2-slim-bullseye
WORKDIR /nebulaedge

RUN apt-get update && apt-get install -y \
    network-manager \
    sudo \
    # bluetooth \
    bluez \
    # dbus \
    && rm -rf /var/lib/apt/lists/*

COPY requirements.txt requirements.txt
RUN pip install -r requirements.txt

COPY server/ .
COPY entrypoint.sh .
RUN chmod +x entrypoint.sh

ENTRYPOINT ["./entrypoint.sh"]