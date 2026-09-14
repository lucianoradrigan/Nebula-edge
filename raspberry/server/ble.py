"""UUIDs del servicio GATT que expone el ESP32.

Tienen que coincidir con los del firmware
(esp32/components/nebulaedge_ble/nebulaedge_ble.c). Los consumen
BleTransport (transport.py) y MasterConnection (discovery.py).
"""

# UUIDs en formato de 128 bits (base BLE)
UUID_SERVICE = "0000ff00-0000-1000-8000-00805f9b34fb"
UUID_CHAR_A  = "0000ff01-0000-1000-8000-00805f9b34fb"  # read/write: config
UUID_CHAR_B  = "0000ff02-0000-1000-8000-00805f9b34fb"  # notify: telemetría
UUID_CHAR_C  = "0000ff03-0000-1000-8000-00805f9b34fb"  # write: semáforo de arranque
UUID_CHAR_D  = "0000ff04-0000-1000-8000-00805f9b34fb"  # read/notify: ConfigAck
