import socket
import time
import threading

class PRNG:
    def __init__(self):
        self.prng_state = 1234

    def random(self):
        self.prng_state = ((1103515245 * self.prng_state) + 12345) % (1 << 31)
        return self.prng_state & 0xFF

class TestPacket:
    def __init__(self, size: int):
        prng = PRNG()
        self.data = bytes([prng.random() for _ in range(1024)])

    def validate(self, data: bytes, addr) -> bool:
        if len(data) != len(self.data):
            print(f"[{time.time()}]Invalid test packet length from address {addr[0]}:{addr[1]}")
            return False

        if data != self.data:
            print(f"[{time.time()}]Invalid test packet content from address {addr[0]}:{addr[1]}")
            return False

        print(f"[{time.time()}]Valid test packet from address {addr[0]}:{addr[1]}")
        return True

    def get_data(self) -> bytes:
        return self.data

sock_upload = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock_echo = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock_download = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

sock_upload.setblocking(False)
sock_echo.setblocking(False)
sock_download.setblocking(False)

test_packet = TestPacket(1024)
test_packet_valid_response = "OK".encode("utf-8")
test_packet_invalid_response = "INVALID".encode("utf-8")

running = True

def upload_handler():
    global sock_upload
    global test_packet
    global running

    valid = 0
    invalid = 0

    sock_upload.bind(('0.0.0.0', 7777))

    while running:
        try:
            data, addr = sock_upload.recvfrom(1024)
        except:
            time.sleep(0.05)
            continue

        if test_packet.validate(data, addr):
            valid += 1
        else:
            invalid += 1

def echo_handler():
    global sock_echo
    global test_packet
    global running

    sock_echo.bind(('0.0.0.0', 7778))

    while running:
        try:
            data, addr = sock_echo.recvfrom(1024)
        except:
            time.sleep(0.05)
            continue

        if test_packet.validate(data, addr):
                print(f"[{time.time()}]Echo: Sending packet")
                sock_echo.sendto(data, addr)

def download_handler():
    global sock_download
    global test_packet
    global running

    sock_download.bind(('0.0.0.0', 7779))

    while running:
        try:
            data, addr = sock_download.recvfrom(1024)
        except:
            time.sleep(0.05)
            continue

        if test_packet.validate(data, addr):
            for _ in range(20):
                for _ in range(5):
                    print(f"[{time.time()}]Download: Sending packet")
                    sock_download.sendto(test_packet.data, addr)

                time.sleep(0.1)

threading.Thread(target=upload_handler).start()
threading.Thread(target=echo_handler).start()
threading.Thread(target=download_handler).start()

input("Enter anything to close the server")

running = False

# Close sockets
sock_download.close()
sock_upload.close()
sock_echo.close()
