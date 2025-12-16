import socket

HOST = '0.0.0.0'  # Escucha en todas las interfaces
PORT = 1234      # Puerto del servidor

with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
    s.bind((HOST, PORT))
    s.listen()
    print(f'Servidor escuchando en {HOST}:{PORT}')
    conn, addr = s.accept()
    with conn:
        print('Conexión establecida desde', addr)
        while True:
            data = conn.recv(1024)
            if not data:
                break
            print('Recibido:', data.decode())
            respuesta = 'Mensaje recibido'
            conn.sendall(respuesta.encode())