import asyncio
import argparse
from typing import Optional
from bleak import BleakScanner, BleakClient

# UUIDs en formato de 128 bits (base BLE)
UUID_SERVICE = "0000ff00-0000-1000-8000-00805f9b34fb"
UUID_CHAR_A  = "0000ff01-0000-1000-8000-00805f9b34fb"  # read/write/notify
UUID_CHAR_B  = "0000ff02-0000-1000-8000-00805f9b34fb"  # read
UUID_CHAR_C  = "0000ff03-0000-1000-8000-00805f9b34fb"  # write

LABEL_TO_UUID = {"A": UUID_CHAR_A, "B": UUID_CHAR_B, "C": UUID_CHAR_C}

# Busca la dirección MAC por nombre de dispositivo
# name: nombre de dispositivo
async def find_device(name: str) -> str:
    print(f"Buscando dispositivo por nombre: {name}...")
    devices = await BleakScanner.discover(timeout=5)
    for d in devices:
        if d.name == name:
            print(f"Encontrado: {d.name} [{d.address}]")
            return d.address
    print("No encontrado. Verifica que está anunciando y el nombre coincide.")
    return ""

# Convierte un string a bytes
def parse_data_arg(data: str) -> bytes:
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

# Lee bytes en una característica
async def read_char(client: BleakClient, uuid: str):
    val = await client.read_gatt_char(uuid)
    # Decodifica y printea los bytes en ASCII
    print(f"READ {uuid}: {val.hex()}  | ascii='{val.decode(errors='ignore')}'")
    return val

# Escribe bytes en una característica
async def write_char(client: BleakClient, uuid: str, data: bytes, with_response: bool = True):
    await client.write_gatt_char(uuid, data, response=with_response)
    print(f"Wrote {len(data)} bytes to {uuid}")

# Habilita la recepción automática de una característica BLE
async def enable_notify(client: BleakClient, uuid: str):
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