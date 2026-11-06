# Pérdida de paquetes: pruebas masivas piloto (4 y 5 de noviembre de 2026)

Pruebas **piloto**: el firmware y el servidor cambiaron después (arreglos de
WiFi y servidor sin `nmcli`), así que estos números justifican el diseño de la
batería oficial pero no la reemplazan.

## Banco

- 7 placas el 04-11 (DB:4E y DE:16 a 50 cm de la Pi por USB; DE:1A, DB:A2,
  DB:06, 51:CE y 52:16 a 1-2 m). 5 el 05-11 (sin DE:1A ni DB:A2).
- Firmware `5159b67` (ventana contada por flujo al producir).
- 1 paquete/s por flujo (Data_1 y Data_2). Deep sleep = B30 con ventana 10;
  continuo = sin deep sleep.
- Red: AP en el WiFi integrado de la Pi (`wlan0`, `brcmfmac`) o la Pi como
  cliente del router por `wlan0`. El dongle WiFi `rtl8xxxu` quedó fuera: como AP
  perdía 3-5 % (la radio del ESP32 recibía ACK MAC de cada trama y aun así no
  llegaban a Linux).

## Corridas

| Directorio | Qué | Duración |
|---|---|---|
| `humo/` | UDP, TCP, MQTT y BLE en B30, AP integrado, 7 placas | 5 min de régimen cada una |
| `noche/` | A (AP: UDP/TCP/MQTT × deep sleep/continuo), F (fallas), D (deriva sin resync, B60 y B300), B (router) | 30 min cada una |
| `resto/` | B completo (router) y Z (BLE), 5 placas | 30 min cada una |

En `noche/`, desde las ~05:15 del 05-11 no hay datos: tras pasar al router, el
servidor leyó con `nmcli` una red vacía y la repartió; las placas quedaron
colgadas al no poder conectarse. Las dos cosas se arreglaron después (red desde
el `.env`; tope de conexión en el firmware). En `resto/`, B_tcp_ds tiene un hueco
de ~9 min: el router desapareció a las 18:00 y la Pi no se reconectó sola.

## Resultados (pérdida por saltos de `seq`)

| | AP integrado, deep sleep | AP integrado, continuo | Router, deep sleep | Router, continuo |
|---|---|---|---|---|
| UDP | 0,25 % | 0,26 % | 0,57 % | 0,04 % |
| TCP | 0 % | 0 % | 1,05 % (caída del router) | 0 % |
| MQTT | 0 % | 0 % | 1,57 % | 0,40 % |
| BLE | 3,7 % (ventanas enteras) | 2,0 % | - | - |

Recuperación ante fallas (bloque F, continuo): reinicio del servidor ~21-25 s
de corte, del broker ~12 s, de Postgres ~5 s (los paquetes llegan pero no se
guardan); un reset de placa vuelve solo por BLE.

## Archivos

- `<corrida>/runner.log`: inicio y fin de cada tramo, versiones de config por placa, eventos.
- `<corrida>/data_1.csv.gz`, `data_2.csv.gz`: `id_device,time_server,time_sent,seq,config_version_applied`.
- `<corrida>/resumen.txt`: salida de `analisis.py`.
- `<corrida>/salud.log`: temperatura, throttling, carga y CPU de los contenedores, cada 60 s.
- `resto/nmcli_bomba.log.gz`: las consultas de `nmcli` del servidor repetidas 2 veces por segundo (solo el largo de la clave).
- `analisis.py`: `python3 analisis.py noche/runner.log noche/data_1.csv.gz noche/data_2.csv.gz`.
- Scripts del banco (corren en la Pi, `~/bench/`): `masiva_runner.sh`, `cfg_set.py`,
  `nmcli_bomba.sh`, `udp_sniff.py` (captura UDP sin tcpdump), `ap_sta_sampler.sh`.
