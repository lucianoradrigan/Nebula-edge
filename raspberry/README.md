# Raspberry (Servidor Central)

Este directorio contiene el stack de la Raspberry:

- Servidor Python (`server/classes.py`).
- PostgreSQL con inicialización automática (`db_init/`).
- Docker Compose para levantar ambos servicios.

Para el flujo completo del proyecto (ESP32 + Raspberry + cambio de protocolo en caliente), revisa también el README de la raíz del repositorio.

Si necesitas la guía del firmware de los nodos, revisa `esp32/README.md`.

## Requisitos

1. Raspberry Pi OS.
2. Raspberry Pi con Docker y Docker Compose.
3. Dongle BLE USB (obligatorio para ejecutar `server/classes.py`).
4. Acceso a Bluetooth del host (`rfkill`, DBus y bluez).
5. Cliente PostgreSQL (`psql`) para aplicar cambios en caliente.

## Levantar servicios

```bash
sudo docker compose build
sudo docker compose up -d
```

Servicios:

- `nebulaedge_server`: proceso principal (BLE + sesiones MQTT/UDP/TCP/BLE).
- `nebulaedge_db`: PostgreSQL con tablas `config`, `log`, `data_1`.

Ver logs:

```bash
sudo docker logs -f nebulaedge_server
sudo docker logs -f nebulaedge_db
```

## Reinicializar base de datos

Si necesitas reiniciar esquema y seed inicial:

```bash
sudo docker compose down -v
sudo docker compose up --build -d
```

## Bluetooth: desbloqueo de dongle

Algunos dongles llegan con `Soft blocked: yes`:

```bash
rfkill list
sudo rfkill unblock bluetooth
```

El servidor detecta adaptador USB automáticamente (`BLE_ADAPTER=auto`).

Si deseas forzar uno específico:

```bash
BLE_ADAPTER=hci1 sudo docker compose up -d --build
```

## Configuración en caliente (DB)

Instalar `psql` (si falta):

```bash
sudo apt update
sudo apt install -y postgresql-client
```

Entrar a PostgreSQL:

```bash
sudo docker exec -it nebulaedge_db psql -U nebulaedge -d nebulaedge
```

Alternativa desde el host con `psql` local:

```bash
psql "postgresql://nebulaedge:1234@localhost:5432/nebulaedge"
```

Ejemplo de cambio de protocolo para un dispositivo:

```sql
UPDATE config
SET protocol_conf = 2,
    config_version = config_version + 1,
    tcp_port = 1826,
    send_interval_ms = 1000
WHERE id_device = 'C0:49:EF:08:D0:C2';
```

Importante: siempre incrementar `config_version` para que el cambio se aplique.

## Desarrollo local sin Docker (opcional)

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
cd server
python3 classes.py
```

Requiere una base PostgreSQL local con:

- DB: `nebulaedge`
- usuario: `nebulaedge`
- password: `1234`