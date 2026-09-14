"""UUIDs del servicio GATT que expone el ESP32.

UTILIDAD PRINCIPAL
    Tener en un solo lugar las cuatro características del servicio BLE, para
    que el servidor y el firmware no se desincronicen.

        char A   config       el servidor escribe acá la configuración
        char B   telemetría   el device notifica sus paquetes de datos
        char C   start        semáforo: el servidor avisa que ya puede empezar
        char D   ack          el device confirma qué versión de config aplicó

    Las características A y D también son legibles, y de eso depende la
    reconciliación de BLE: si se pierde la notificación del ACK, el servidor
    puede preguntar leyéndolas (`BleTransport.confirm_config_applied`).

DEBE CALZAR CON EL FIRMWARE
    Los mismos UUIDs están declarados en
    esp32/components/nebulaedge_ble/nebulaedge_ble.c. Si cambian de un lado,
    hay que cambiarlos del otro.

QUIÉN LOS USA
    BleTransport (transport.py) y MasterConnection (discovery.py).
"""

# UUIDs en formato de 128 bits (base BLE)
UUID_SERVICE = "0000ff00-0000-1000-8000-00805f9b34fb"
UUID_CHAR_A  = "0000ff01-0000-1000-8000-00805f9b34fb"  # read/write: config
UUID_CHAR_B  = "0000ff02-0000-1000-8000-00805f9b34fb"  # notify: telemetría
UUID_CHAR_C  = "0000ff03-0000-1000-8000-00805f9b34fb"  # write: semáforo de arranque
UUID_CHAR_D  = "0000ff04-0000-1000-8000-00805f9b34fb"  # read/notify: ConfigAck
