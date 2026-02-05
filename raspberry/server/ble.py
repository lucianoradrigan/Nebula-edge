import asyncio
import argparse
from typing import Optional
from bleak import BleakScanner, BleakClient

# UUIDs en formato de 128 bits (base BLE)
UUID_SERVICE = "0000ff00-0000-1000-8000-00805f9b34fb"
UUID_CHAR_A  = "0000ff01-0000-1000-8000-00805f9b34fb"  # read/write/notify
UUID_CHAR_B  = "0000ff02-0000-1000-8000-00805f9b34fb"  # read
UUID_CHAR_C  = "0000ff03-0000-1000-8000-00805f9b34fb"  # write (semaforo)
UUID_CHAR_D  = "0000ff04-0000-1000-8000-00805f9b34fb"  # notify (ack)

LABEL_TO_UUID = {"A": UUID_CHAR_A, "B": UUID_CHAR_B, "C": UUID_CHAR_C, "D": UUID_CHAR_D}

def parse_data_arg(data: str) -> bytes:
    """Convierte un string a bytes (hex o texto)."""
    # Si comienza con 0x o contiene espacios, tratar como hex; si no, como texto
    s = data.strip()
    try:
        if s.startswith("0x") or all(c in "0123456789abcdefABCDEF " for c in s):
            s = s.replace("0x", "").replace(" ", "")
            return bytes.fromhex(s)
        return s.encode("utf-8")
    except Exception:
        # fallback a texto
        return data.encode("utf-8")

async def enable_notify(client: BleakClient, uuid: str):
    """Habilita notificaciones y imprime datos entrantes."""
    def cb(_, data: bytearray):
        print(f"NOTIFY {uuid}: {data.hex()}  | ascii='{data.decode(errors='ignore')}'")
    await client.start_notify(uuid, cb)
    print(f"Notify habilitado en {uuid}. Pulsa Ctrl+C para salir.")
    try:
        while True:
            await asyncio.sleep(1.0)
    except KeyboardInterrupt:
        pass
    finally:
        await client.stop_notify(uuid)
        print("Notify deshabilitado.")

async def main():
    """Ejemplo básico de conexión BLE y escritura en característica A."""

    addr = await find_device("ESP_GATTS_DEMO")
    if addr == "":
        return

    # Durante este bloque estará emparejado con la ESP32
    async with BleakClient(addr) as client:
        if not client.is_connected:
            print("No se pudo conectar.")
            return
        print(f"Conectado a {addr}")

        mensaje = b"Hola mundo"
        await write_char(client, UUID_CHAR_A, mensaje)

        try:
            svcs = await client.get_services()
            svc = svcs.get_service(UUID_SERVICE)
            if svc:
                print(f"Servicio {UUID_SERVICE} encontrado con {len(svc.characteristics)} características.")
        except Exception:
            pass

if __name__ == "__main__":
    asyncio.run(main())