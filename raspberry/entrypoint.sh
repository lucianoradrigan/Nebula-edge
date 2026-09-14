#!/bin/bash

# service dbus start
# service bluetooth start

# exec reemplaza este shell por el proceso de Python (queda como PID 1 del
# contenedor) en vez de correr como hijo de un "python3 ...; while true; do
# sleep 60; done" que seguía vivo para siempre incluso si server.py se
# caía. Con eso, un crash real hacía que el contenedor quedara "Up" para
# siempre sin servir nada, y `restart: unless-stopped` (docker-compose.yml)
# nunca se disparaba porque el contenedor nunca terminaba. Con exec, si
# server.py se cae, el contenedor termina y Docker lo reinicia.
exec python3 -u server.py
