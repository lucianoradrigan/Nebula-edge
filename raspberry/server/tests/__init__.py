"""Suite de tests del servidor NebulaEdge.

UTILIDAD PRINCIPAL
    Verificar el flujo completo de una sesión -telemetría, cambio de config con
    ACK, deep sleep y cierre por timeout- para los cuatro protocolos, sin
    necesitar Postgres, broker MQTT ni hardware.

CÓMO CORRERLA

    cd raspberry/server
    python -m unittest discover -s tests

QUÉ ES REAL Y QUÉ ES DOBLE
    Real:    la sesión (`ProtocolSession`), el transporte, el router, el codec
             y los sockets/colas por los que hablan.
    Doble:   Postgres (`FakeRepo`, en memoria), el BLEDevice de bleak, y para
             BLE también el `BleakClient` completo (`FakeBleakClient`).

    Por eso los tests de BLE verifican el cableado -notificación, cola, ruteo,
    inserción, reconciliación del ACK- pero no que bleak y BlueZ se comporten
    como el doble. Eso solo se valida con un ESP32 enfrente.

ARCHIVOS
    fakes.py               dobles y constructores de paquetes compartidos
    test_udp_session.py    sesión UDP contra un device simulado en loopback
    test_tcp_session.py    ídem TCP, incluida la reapertura del socket
    test_mqtt_session.py   sesión MQTT contra colas simuladas
    test_ble_session.py    sesión BLE contra un BleakClient falso
"""
