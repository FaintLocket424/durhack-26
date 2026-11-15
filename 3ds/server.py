
import socket
import time

SERVER_IP = "0.0.0.0"
SERVER_PORT = 8081

server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

server.bind((SERVER_IP, SERVER_PORT))
server.listen(5)

print("Waiting for 3DS connection...")

while True:
    conn, addr = server.accept()

    with conn:
        print("Connected:", addr)

        try:
            while True:
                request = b""

                while not request.endswith(b"\n"):
                    chunk = conn.recv(64)

                    if not chunk:
                        raise ConnectionError("3DS disconnected")

                    request += chunk

                print("Received:", request.decode().strip())

                if request.strip() == b"PING":
                    conn.sendall(b"PONG\n")
                    print("Sent: PONG")

        except (OSError, ConnectionError) as error:
            print("Connection closed:", error)

        print("Waiting for next connection...")
