import time
from dataclasses import dataclass
from typing import Callable
import pytest

class Conn:
    def __init__(self, sock):
        self.sock = sock

    def send(self, data: bytes):
        self.sock.sendall(data)

    def send_chunks(self, data: bytes, chunk_size: int, delay: float = 0):
        for start in range(0, len(data), chunk_size):
            self.sock.sendall(data[start:start + chunk_size])
            if delay:
                time.sleep(delay)
    
    def send_with_cuts(self, data, cuts, delay=0.1):
        curr_ptr = 0
        for cut in cuts:
            self.sock.sendall(data[curr_ptr:cut])
            if delay:
                time.sleep(delay)
            curr_ptr = cut
        self.sock.sendall(data[curr_ptr:])
    
    def read_response(self, timeout: float = 2.0) -> bytes:
        self.sock.settimeout(timeout)
        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                break
            buf += chunk

        head, sep, body = buf.partition(b"\r\n\r\n")
        assert sep, "Connection closed before complete response headers"
        blen = 0
        for line in head.split(b"\r\n"):
            if line.lower().startswith(b"content-length:"):
                blen = int(line.split(b":")[1])
                while len(body) < blen:
                    chunk = self.sock.recv(65536)
                    if not chunk:
                        break
                    body += chunk
        assert len(body) == blen, f"Response body length must match declared body length: expected {blen}, received {len(body)}"

        return head + b"\r\n\r\n" + body
    
    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.sock.close()

@dataclass(frozen=True)
class SendMode:
    name: str
    send: Callable

def regular(conn, data):
    conn.send(data)

def dripped(conn, data):
    conn.send_chunks(data, chunk_size=1, delay=0.005)

def interrupted(conn, data):
    cut = data.find(b"Host:") + 3

    if cut < 3:
        cut = len(data) // 2

    conn.send_with_cuts(data, cuts=(cut,), delay=0.2)

def chunked_128k(conn, data):
    conn.send_chunks(data, chunk_size=128 * 1024)


SEND_MODES = {
    mode.name: mode
    for mode in [
        SendMode("regular", regular),
        SendMode("dripped", dripped),
        SendMode("interrupted", interrupted),
        SendMode("chunked-128k", chunked_128k)
    ]
}


