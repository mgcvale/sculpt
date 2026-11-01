import socket, subprocess, time, pytest
from pathlib import Path
from helpers import Conn
from subprocess import TimeoutExpired

BUILD_PATH = Path(__file__).resolve().parents[1] / "build"
VALGRIND_EXIT_TIMEOUT = 15

def pytest_addoption(parser):
    parser.addoption("--valgrind", action="store_true", help="run servers under valgrind")

def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]
    
def wait_until_accepting(port, timeout=5.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError(f"Server never came up on port {port}")

class Server:
    def __init__(self, port):
        self.port = port

    def connect(self, timeout=0.2):
        return Conn(socket.create_connection(("127.0.0.1", self.port), timeout=timeout))

@pytest.fixture
def sculpt(tmp_path, request):
    # initializes a sculpt server with max_conns=4, conn_timeout=1 and returns it as a Server object
    procs = []

    def start(**config):
        port = free_port()
        config.setdefault("log_level", 3)
        
        lines = [f"port = {port}"]
        for k, v in config.items():
            v = str(v).lower() if isinstance(v, bool) else v # to convert from True to true
            lines.append(f"{k} = {v}")

        cfg_path = tmp_path / f"sculpt_test_{port}.conf"
        cfg_path.write_text("\n".join(lines) + "\n")

        cmd = [BUILD_PATH / "test_server", str(cfg_path)]
        if request.config.getoption("--valgrind"):
            cmd = ["valgrind", "--error-exitcode=99", "--leak-check=full"] + cmd

        proc = subprocess.Popen(cmd)
        procs.append(proc)
        wait_until_accepting(port)
        return Server(port)
    
    yield start

    for proc in procs:
        try:
            proc.terminate()
            assert proc.wait(timeout=VALGRIND_EXIT_TIMEOUT) == 0, "valgrind reported errors"
        except TimeoutExpired:
            proc.kill()
            proc.wait()
            pytest.fail(f"sculpt + valgrind process exceeded the {VALGRIND_EXIT_TIMEOUT}s timeout to be killed")


