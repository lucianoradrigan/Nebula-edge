import psycopg2

# Este script se corre una sola vez!
# Cada vez que se necesite o crear las tablas en la BD
# por primera vez o en caso de alguna modificación.

def init_db(): 
    # Parámetros de conexión
    conexion = psycopg2.connect(
        host="localhost",       # servidor de la BD
        database="postgres",    # base de datos inicial
        user="gabodiazc",      # tu usuario de PostgreSQL
        password="1234"  # tu contraseña
    )

    conexion.autocommit = True  # Necesario para crear base de datos

    cursor = conexion.cursor()

    # Elimina la base de datos anteriro
    cursor.execute("DROP DATABASE nebulaedge;")

    # Crear una base de datos nueva
    cursor.execute("CREATE DATABASE nebulaedge;")
    print("Base de datos creada con éxito")

    # Conectarse a la nueva base de datos
    conexion.close()

    conexion = psycopg2.connect(
        host="localhost",
        database="nebulaedge",
        user="gabodiazc",
        password="1234"
    )
    cursor = conexion.cursor()

    # Creación tabla config
    cursor.execute("""
    CREATE TABLE config (
        id_device INT PRIMARY KEY,
        status_conf INT,
        protocol_conf INT,
        acc_sampling INT,
        gyro_sensibility INT,
        bme688_sampling INT,
        discontinuous_time INT,
        tcp_port INT,
        udp_port INT,
        host_ip_addr VARCHAR(45),
        ssid VARCHAR(45),
        pass VARCHAR(45)
    );
    """)

    # Inserta valor inicial
    cursor.execute("""
        INSERT INTO config (
        id_device,
        status_conf,
        protocol_conf,
        acc_sampling,
        gyro_sensibility,
        bme688_sampling,
        discontinuous_time,
        tcp_port,
        udp_port,
        host_ip_addr,
        ssid,
        pass
        ) VALUES (
        0,                       -- id_device (elige uno distinto si ya existe 1)
        1,                       -- status_conf ()
        1,                       -- protocol_conf: 0 es MQTT, 1 es UDP, 2 es TCP
        100,                     -- acc_sampling
        250,                     -- gyro_sensibility
        60,                      -- bme688_sampling
        10,                      -- discontinuous_time
        1818,                    -- tcp_port
        1234,                    -- udp_port
        '172.20.10.3',           -- host_ip_addr
        'iPhone de Gabriel',     -- ssid
        '12345678'               -- pass
        )
    """)


    # Creación tabla log
    cursor.execute("""
    CREATE TABLE log (
        id_device INT,
        status_report INT,
        protocol_report INT,
        batt_level INT,
        conf_peripheral INT,
        time_client INT,
        time_server INT
    );
    """)

    # Creación tabla data_1
    cursor.execute("""
    CREATE TABLE data_1 (
        id_device INT,
        temperature INT,
        press INT,
        hum INT,
        co FLOAT,
        rms FLOAT,
        amp_x FLOAT,
        freq_x FLOAT,
        amp_y FLOAT,
        freq_y FLOAT,
        amp_z FLOAT,
        freq_z FLOAT
    );
    """)

    conexion.commit()
    print("Tablas creadas exitosamente")

    # Cerrar conexión
    cursor.close()
    conexion.close()


init_db()