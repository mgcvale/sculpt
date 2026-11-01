import pytest

from helpers import SEND_MODES

def with_send_modes(*names):
    if not names:
        names = tuple(SEND_MODES)

    modes = [SEND_MODES[name] for name in names]

    return pytest.mark.parametrize(
        "send_mode",
        modes,
        ids=lambda mode: mode.name
    )

all_send_modes = with_send_modes()

REQUESTS = [
    pytest.param(
        b"GET / HTTP/1.1\r\n"
        b"Host: localhost\r\n\r\n",
        id="basic-get-root"
    ),
    pytest.param(
        b"GET /any/ HTTP/1.1\r\n"
        b"Host: localhost\r\n\r\n",
        id="basic-get-any"
    )
]

@all_send_modes
@pytest.mark.parametrize("request_bytes", REQUESTS)
def test_base_requests(sculpt, send_mode, request_bytes):
    server = sculpt(conn_timeout=1)

    with server.connect() as conn:
        send_mode.send(conn, request_bytes)
        response = conn.read_response()

    assert response.startswith(b"HTTP/1.1 200 OK\r\n")
    assert response.endswith(b"Sculpt Base Response")


