"""HTTP clients bound accumulated response bytes in both runtimes."""
import os
import socket
import subprocess
import threading

import _cxx
import _scratch

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
ENGINE = os.path.join(ROOT, "lang", "engine")
TMP = _scratch.mkdtemp(prefix="http-response")
LIMIT = 16 * 1024 * 1024
EXPECTED = "HTTP response exceeds 16 MiB\n"


def slow_large_response():
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    port = listener.getsockname()[1]
    errors = []

    def serve():
        try:
            conn, _ = listener.accept()
            with conn:
                conn.settimeout(5)
                request = bytearray()
                while b"\r\n\r\n" not in request and len(request) < 65536:
                    chunk = conn.recv(4096)
                    if not chunk:
                        return
                    request.extend(chunk)
                body = b"x" * (LIMIT + 4096)
                header = ("HTTP/1.1 200 OK\r\nContent-Length: %d\r\n"
                          "Connection: close\r\n\r\n" % len(body)).encode("ascii")
                conn.sendall(header)
                for at in range(0, len(body), 65536):
                    try:
                        conn.sendall(body[at:at + 65536])
                    except OSError:
                        break
        except OSError as exc:
            errors.append(exc)
        finally:
            listener.close()

    thread = threading.Thread(target=serve, daemon=True)
    thread.start()
    return port, thread, errors


def run(argv, timeout=40):
    return subprocess.run(argv, cwd=ROOT, capture_output=True, text=True, timeout=timeout)


def main():
    native = _cxx.build()
    path = os.path.join(TMP, "large_response.brat")
    for backend in ("python", "native"):
        port, thread, errors = slow_large_response()
        source = '''import std.http
fn main() -> i64 {
  let result = std.http.get("http://127.0.0.1:%d/")
  match result {
    Ok(_) => { print("unexpected success") },
    Err(e) => { print(e) }
  }
  return 0
}
''' % port
        with open(path, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(source)
        if backend == "python":
            result = run(_cxx.cli_args("run", "-O0", path))
        else:
            result = run([native, "run", path])
        thread.join(timeout=15)
        assert not thread.is_alive(), "large-response peer did not finish"
        assert not errors, errors
        assert (result.returncode, result.stdout, result.stderr) == \
               (0, EXPECTED, ""), (backend, result.stdout, result.stderr)
    print("BOUNDED HTTP RESPONSE ALL GREEN")


if __name__ == "__main__":
    main()
