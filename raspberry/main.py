import psycopg2
import schema_pb2
import asyncio
import socket
from bleak import BleakScanner, BleakClient
from ble import *
from mqtt import *
import time

# UUIDs en formato de 128 bits (base BLE)
UUID_SERVICE = "0000ff00-0000-1000-8000-00805f9b34fb"
UUID_CHAR_A  = "0000ff01-0000-1000-8000-00805f9b34fb"  # read/write/notify
UUID_CHAR_B  = "0000ff02-0000-1000-8000-00805f9b34fb"  # read
UUID_CHAR_C  = "0000ff03-0000-1000-8000-00805f9b34fb"  # write


# Conecta a la base de datos PostgreSQL y retorna handler de conexión
def connect_db():
    # Conexión a la Base de Datos
    db_connection = psycopg2.connect(
        host = "localhost",
        database = "nebulaedge",
        user = "gabodiazc",
        password = "1234"
    )
    print("Conexión exitosa a la base de datos.")
    return db_connection


# Extrae valores de configuración de la base de datos y
# devuelve un protobuf unpacked de tipo Config.
# get_config: db_connection -> schema_pb2.Config | None
def get_config(db_connection):

    db_cursor = db_connection.cursor()

    # Ejecuta prompt SQL para obtener datos de tabla config
    db_cursor.execute("""
        SELECT id_device, status_conf, protocol_conf, acc_sampling, gyro_sensibility,
                bme688_sampling, discontinuous_time, tcp_port, udp_port,
                host_ip_addr, ssid, pass
        FROM config
        LIMIT 1;
    """)

    # Devuelve una lista de tuplas: cada tupla es 1 fila
    rows = db_cursor.fetchall()
    
    # Extraemos valores de los atributos
    if rows:
        row = rows[0]
        config = schema_pb2.Config()
        config.id_device           = row[0]
        config.status_conf         = row[1]
        config.protocol_conf       = row[2]
        config.acc_sampling        = row[3]
        config.gyro_sensibility    = row[4]
        config.bme688_sampling     = row[5]
        config.discontinuous_time  = row[6]
        config.tcp_port            = row[7]
        config.udp_port            = row[8]
        config.host_ip_addr        = row[9]
        config.ssid                = row[10]
        config.passwd              = row[11]

        # Debug
        # print(config)
        return config
    else:
        print("No hay valores de configuración en la base de datos.")
        return None

# Desempaqueta bytes de protobuf del tipo Data1.
# unpack_Data1: bytes -> schema_pb2.Data_1 | None
def unpack_data_1(packet):
    # Desempaqueta protobuf
    try:
        unpacked = schema_pb2.Data_1()
        unpacked.ParseFromString(packet)
        return unpacked
    except Exception as e:
        print(f"Error al desempaquetar el paquete: {e}")
        return None

# Empaqueta bytes de protobuf del tipo Config.
# pack_config: schema_pb2.Config -> bytes
def pack_config(unpacked):
    return unpacked.SerializeToString()

# Recibe un cursor de una BD y un protobuf Data1 desempaquetado 
# e inserta cada campo en la tabla data_1 de la BD.
# insert_data_1: schema_pb2.Data_1 -> None
def insert_data_1(db_connection, unpacked):

    db_cursor = db_connection.cursor()

    # Inserta en la base de datos
    db_cursor.execute("""
        INSERT INTO data_1 (
            id_device, temperature, press, hum, co, rms,
            amp_x, freq_x, amp_y, freq_y, amp_z, freq_z
        ) VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s)
    """, (
        unpacked.id_device,
        unpacked.temperature,
        unpacked.press,
        unpacked.hum,
        unpacked.co,
        unpacked.rms,
        unpacked.amp_x,
        unpacked.freq_x,
        unpacked.amp_y,
        unpacked.freq_y,
        unpacked.amp_z,
        unpacked.freq_z
    ))
    db_connection.commit()
    print("Datos insertados en la tabla data_1\n")

# Flujo principal
async def main():

    #############################################################
    ################# INTERACCIÓN BASE DE DATOS #################
    #############################################################
    
    # Conecta a la base de datos
    db_connection = connect_db()

    # Extrae configuración de la base de datos: tipo protobuf Config
    config = get_config(db_connection)

    # Empaqueta protobuf de estructura Config
    config_packet = pack_config(config)

    #############################################################
    ######################## BLE INICIO #########################
    #############################################################

    # Char A: se guarda info de configuración
    # Char B: se guarda dato de sensores
    # Char C: flag

    name_device = "ESP_NEBULAEDGE"

    # Busca dispositivo por nombre
    ble_addr = await find_device(name_device)
    if ble_addr == "":
        return

    while True:
        try:
            # Durante este bloque estará emparejado con la ESP32
            async with BleakClient(ble_addr) as client:

                if not client.is_connected:
                    print("No se pudo conectar.")
                    return
                print(f"Conectado a {ble_addr}")

                # Escribe en la característica A
                await write_char(client, UUID_CHAR_A, config_packet)

                # Escribe un ok en la característica C
                await write_char(client, UUID_CHAR_C, b"ok")
                break

        except TimeoutError as e:
            print(f"Timeout conectando al dispositivo {name_device}")
            print(f"Reintentando...")

    while True:

        time.sleep(1)

        #############################################################
        ############################ MQTT ###########################
        #############################################################
        if config.protocol_conf == 0:
            config_packet_prev = config_packet
            mqtt_start()

            while True:

                # Extrae configuración de la base de datos: tipo protobuf Config
                # SE ACTUALIZA EL VALOR DE CONFIG "GLOBAL"
                config = get_config(db_connection)

                # Empaqueta el protobuf Config
                config_packet = pack_config(config)

                if config_packet != config_packet_prev:
                    print("Se actualizó configuración")
                    mqtt_publish("/topic/nebulaedge/config", config_packet)
                    mqtt_shutdown()
                    break

                config_packet_prev = config_packet

                try:
                    # Espera por un paquete
                    data_1_packet = packet_queue.get(timeout=1)  
                    data_1 = unpack_data_1(data_1_packet)

                    # Debug
                    # print(data_1)

                    # Inserta en la base de datos
                    insert_data_1(db_connection, data_1)
                    
                except queue.Empty:
                    pass  # No llegó ningún paquete en este ciclo

                except Exception as e:
                    print(f"Error al desempaquetar el paquete: {e}")
                    continue
            
                
            
        #############################################################
        ############################ UDP ############################
        #############################################################

        if config.protocol_conf == 1:
            host = '0.0.0.0'                    # Escucha en todas las interfaces
            port = config.udp_port              # Puerto del servidor
            config_packet_prev = config_packet  # Configuración previa para no enviar repetida

            # Se crea un socket IPv4, UDP
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:

                # Reutiliza puerto si es que está abierto
                s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

                # Conexión al host
                s.bind((host, port))
                print(f'Servidor UDP escuchando en {host}:{port}')

                while True:

                    # Escucha para recibir paquete
                    data_1_packet, udp_addr = s.recvfrom(1024)
                    print(f'Paquete recibido de {udp_addr}')

                    # Extrae configuración de la base de datos: tipo protobuf Config
                    # SE ACTUALIZA EL VALOR DE CONFIG "GLOBAL"
                    config = get_config(db_connection)

                    # Empaqueta el protobuf Config
                    config_packet = pack_config(config)

                    if config_packet != config_packet_prev:
                        # Este paquete se puede perder, hay que hacer un bucle try y
                        # esperar confirmación: seguir lógica de ....
                        s.sendto(config_packet, udp_addr)
                        break

                    config_packet_prev = config_packet

                    # Desempaqueta y obtiene protobuf tipo Data1
                    data_1 = unpack_data_1(data_1_packet)

                    # Debug
                    print(data_1)

                    # Inserta en la base de datos
                    insert_data_1(db_connection, data_1)

                    
        #############################################################
        ############################ TCP ############################
        #############################################################

        if config.protocol_conf == 2:
            host = '0.0.0.0'                    # Escucha en todas las interfaces
            port = config.tcp_port              # Puerto del servidor
            config_packet_prev = config_packet  # Configuración previa para no enviar repetida

            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                # Permite reutilizar el puerto inmediatamente tras cerrar
                s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                s.bind((host, port))
                s.listen()
                print(f'Servidor TCP escuchando en {host}:{port}')
                conn, tcp_addr = s.accept()
                with conn:
                    print('Conexión establecida desde', tcp_addr)
                    
                    while True:

                        # Extrae configuración actual de la base de datos
                        config = get_config(db_connection)

                        # Empaqueta el protobuf Config
                        config_packet = pack_config(config)

                        # Si la configuración es distinta de la anterior, escribe
                        if (config_packet != config_packet_prev):
                            # Envía la configuración actual al cliente
                            try:
                                conn.sendall(config_packet)
                                print("Paquete con nueva configuración enviado")
                            except Exception as e:
                                # Manejar este caso
                                print(f"Error enviando configuración: {e}")
                                break  # Sale del bucle interno y espera nueva conexión
                            break
                        
                        config_packet_prev = config_packet

                        # Recibe datos de sensores
                        try:
                            data_1_packet = conn.recv(1024)
                            if not data_1_packet:
                                break
                            print(f'Paquete recibido de {tcp_addr}')
                        except ConnectionResetError:
                            print("Conexión cerrada por el otro extremo. Esperando nueva conexión...")
                            break  # Sale del bucle interno y espera nueva conexión
                        except Exception as e:
                            print(f"Error inesperado: {e}")
                            break
                        
                        # Desempaqueta y obtiene protobuf tipo Data1
                        data_1 = unpack_data_1(data_1_packet)

                        if data_1 == None:
                            continue

                        # Inserta en la base de datos
                        insert_data_1(db_connection, data_1)
                            

        #############################################################
        ############################ BLE ############################
        #############################################################
        if config.protocol_conf == 3:
            data_1_packet_prev = 1              # Paquete previo para no repetir en la BD
            config_packet_prev = config_packet  # Configuración previa para no enviar repetida

            while True:
                try:
                    # ETAPA 1: conexión BLE 
                    # Durante este bloque estará emparejado con la ESP32
                    async with BleakClient(ble_addr) as client:
                        if not client.is_connected:
                            print("No se pudo conectar.")
                            return
                        print(f"Conectado a {ble_addr}")

                        # Escribe la configuración actual en el char de configuración (A)
                        await write_char(client, UUID_CHAR_A, config_packet)

                        # Escribe un ok en la característica C
                        await write_char(client, UUID_CHAR_C, b"ok")
        
                        while True:
                            try:
                                # Espera por un paquete
                                data_1_packet = await read_char(client, UUID_CHAR_B)
                                if data_1_packet == data_1_packet_prev:
                                    # Se salta la escritura en la BD
                                    print("Se leyó duplicado: se descarta")
                                    continue

                                data_1 = unpack_data_1(data_1_packet)
                                data_1_packet_prev = data_1

                                # Debug
                                print(data_1)

                                # Inserta en la base de datos
                                insert_data_1(db_connection, data_1)
                                
                            except Exception as e:
                                print(f"Error al desempaquetar el paquete: {e}")
                                continue
                        
                            # Extrae configuración de la base de datos: tipo protobuf Config
                            config = get_config(db_connection)

                            # Empaqueta protobuf de estructura Config
                            config_packet = pack_config(config)

                            # Si la configuración es distinta de la anterior, escribe
                            if (config_packet != config_packet_prev):
                                # Escribe configuración en la característica A
                                await write_char(client, UUID_CHAR_A, config_packet)
                                
                                print("Se escribió configuración nueva en la característica A")
                                break
                                
                            config_packet_prev = config_packet
                        break

                except TimeoutError as e:
                    print(f"Timeout conectando al dispositivo {name_device}")
                    print(f"Reintentando...")
            
    
# Idea: que otro thread en este script monitoree si cambia la configuración,
# y así se envía solo una vez la configuración por cada cambio (o varias veces
# hasta que se confirme recepción)

if __name__ == "__main__":
    asyncio.run(main())