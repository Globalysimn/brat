"""Socket deadlines bound the whole connected-socket lifetime in both runtimes."""
import os
import shutil
import socket
import subprocess
import threading
import time

import _cxx
import _scratch
import _skip

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
ENGINE = os.path.join(ROOT, "lang", "engine")
TMP = _scratch.mkdtemp(prefix="socket-deadline")

PROGRAM = '''fn main() -> i64 {
  let cr = tcp_connect("127.0.0.1:%d")
  match cr {
    Ok(sock) => {
      let first = tcp_recv(sock, 1)
      match first {
        Ok(text) => { print("first:" + text) },
        Err(e) => { print("first-error:" + e) }
      }
      let second = tcp_recv(sock, 1)
      match second {
        Ok(text) => { print("second:" + text) },
        Err(e) => { print("second-error:" + e) }
      }
      tcp_close(sock)
    },
    Err(e) => { print("connect-error:" + e) }
  }
  return 0
}
'''
EXPECTED = "first:a\nsecond-error:socket deadline exceeded\n"


def peer():
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
                time.sleep(0.65)
                conn.sendall(b"a")
                time.sleep(0.65)
                try:
                    conn.sendall(b"b")
                except OSError:
                    pass
        except OSError as exc:
            errors.append(exc)
        finally:
            listener.close()

    thread = threading.Thread(target=serve, daemon=True)
    thread.start()
    return port, thread, errors


def run_native(native, port):
    c_source = os.path.join(TMP, "socket_deadline_%d.c" % port)
    exe = os.path.join(TMP, "socket_deadline_%d" % port)
    brat_source = os.path.join(TMP, "socket_deadline_%d.brat" % port)
    source = PROGRAM % port
    with open(brat_source, "w", encoding="utf-8", newline="\n") as stream:
        stream.write(source)
    emitted = subprocess.run([native, "cc", brat_source], cwd=ROOT,
                             capture_output=True, text=True, timeout=60)
    assert emitted.returncode == 0, emitted.stderr
    with open(c_source, "w", encoding="utf-8", newline="\n") as stream:
        stream.write("#define _VSK_TIMEOUT_S 1\n")
        stream.write(emitted.stdout)
    compiler = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not compiler:
        _skip.skip("no C compiler for native socket deadline test")
    compiled = subprocess.run(
        [compiler, "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined",
         "-fno-omit-frame-pointer", "-I", ENGINE, c_source, "-o", exe,
         "-lm", "-pthread"], cwd=ROOT, capture_output=True, text=True, timeout=90)
    assert compiled.returncode == 0, compiled.stderr
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=1")
    result = subprocess.run([exe], cwd=ROOT, env=env, capture_output=True, text=True,
                            timeout=8)
    assert (result.returncode, result.stdout, result.stderr) == (0, EXPECTED, ""), \
        (result.stdout, result.stderr)



def run_connect_timeout(native):
    if os.name == "nt":
        return
    source = '''fn main() -> i64 {
  let cr = tcp_connect("127.0.0.1:1")
  match cr {
    Ok(sock) => { print("unexpected connect"); tcp_close(sock) },
    Err(e) => { print("connect error: " + e) }
  }
  return 0
}
'''
    brat_source = os.path.join(TMP, "mock_connect_timeout.brat")
    c_source = os.path.join(TMP, "mock_connect_timeout.c")
    exe = os.path.join(TMP, "mock_connect_timeout")
    with open(brat_source, "w", encoding="utf-8", newline="\n") as stream:
        stream.write(source)
    emitted = subprocess.run([native, "cc", brat_source], cwd=ROOT,
                             capture_output=True, text=True, timeout=60)
    assert emitted.returncode == 0, emitted.stderr
    mocks = '''
int brat_test_connect(int fd, const struct sockaddr *addr, socklen_t len) {
  (void)addr; (void)len;
  if (fcntl(fd, F_GETFL, 0) & O_NONBLOCK) { errno = EINPROGRESS; return -1; }
  return 0;
}
int brat_test_poll(struct pollfd *fds, nfds_t count, int timeout) {
  (void)fds; (void)count;
  return timeout > 0 ? 0 : -1;
}
'''
    with open(c_source, "w", encoding="utf-8", newline="\n") as stream:
        stream.write("#define _VSK_TIMEOUT_S 1\n")
        stream.write("#define connect brat_test_connect\n#define poll brat_test_poll\n")
        stream.write(emitted.stdout)
        stream.write(mocks)
    compiler = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not compiler:
        _skip.skip("no C compiler for native socket deadline test")
    compiled = subprocess.run(
        [compiler, "-std=gnu11", "-O1", "-I", ENGINE, c_source, "-o", exe,
         "-lm", "-pthread"], cwd=ROOT, capture_output=True, text=True, timeout=90)
    assert compiled.returncode == 0, compiled.stderr
    result = subprocess.run([exe], cwd=ROOT, capture_output=True, text=True, timeout=5)
    assert (result.returncode, result.stdout, result.stderr) == \
        (0, "connect error: connect failed\n", ""), (result.stdout, result.stderr)


def main():
    native = _cxx.build()
    runs = [lambda port: run_native(native, port)]
    for run in runs:
        port, thread, errors = peer()
        try:
            run(port)
        finally:
            thread.join(timeout=4)
        assert not thread.is_alive(), "slow peer did not finish"
        assert not errors, errors
    run_connect_timeout(native)
    print("CONNECTED SOCKET DEADLINES ALL GREEN")


if __name__ == "__main__":
    main()
