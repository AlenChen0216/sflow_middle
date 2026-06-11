import socket


FILE = "build/test_output.json"
SERVER_IP = "192.168.4.1"
SERVER_PORT = 8080

def main():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.connect((SERVER_IP, SERVER_PORT))
        with open(FILE, "r") as f:
            for line in f:
                s.sendall(line.encode())

if __name__ == "__main__":
    main()