"""`brat mcp`: the same five verbs as the CLI, spoken over the Model Context
Protocol, for agents that drive BRAT.

Rules taken from the protocol, not invented here:

- stdio is the message stream. Every byte on stdout is JSON-RPC; logs and any
  stray text would corrupt the session, so diagnostics go to stderr.
- A tool that fails returns `isError: true` inside the result, with the reason in
  `content`. The agent has to see the failure to correct itself; a JSON-RPC error
  is reserved for protocol faults (unknown method, bad params).
- Nothing is invented: each tool runs the real CLI and returns what it printed.
"""

import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CLI = os.path.join(HERE, "cli.py")
PROTOCOL = "2025-06-18"

TOOLS = [
    {
        "name": "brat_check",
        "description": ("Type-check and ownership-check a BRAT file. Returns the "
                        "diagnostic protocol JSON: every diagnostic has a stable "
                        "code (E0382 use after move, T0001 type error, P0001 syntax "
                        "error), a line, a message and a `helps` remedy."),
        "inputSchema": {"type": "object",
                        "properties": {"path": {"type": "string",
                                                "description": "path to a .brat file"}},
                        "required": ["path"]},
    },
    {
        "name": "brat_run",
        "description": "Run a BRAT file through the CLI. Returns stdout, stderr and the exit code.",
        "inputSchema": {"type": "object",
                        "properties": {"path": {"type": "string"}},
                        "required": ["path"]},
    },
    {
        "name": "brat_build",
        "description": ("Compile a BRAT file to a native binary through C. "
                        "Returns the compiler output and the path written."),
        "inputSchema": {"type": "object",
                        "properties": {"path": {"type": "string"},
                                       "out": {"type": "string",
                                               "description": "output path, optional"}},
                        "required": ["path"]},
    },
    {
        "name": "brat_api",
        "description": ("Exported surface of a module and everything it imports, as "
                        "JSON: module name, file, and each export's name, params and "
                        "return type."),
        "inputSchema": {"type": "object",
                        "properties": {"path": {"type": "string"}},
                        "required": ["path"]},
    },
    {
        "name": "brat_deps",
        "description": "Import graph of a program as JSON: root, module count, edges.",
        "inputSchema": {"type": "object",
                        "properties": {"path": {"type": "string"}},
                        "required": ["path"]},
    },
    {
        "name": "brat_abi_matrix",
        "description": ("Verify an FFI module against a C header across LP64, LLP64 and "
                        "ILP32 data models (`brat ffi --abi-matrix --json`), returning "
                        "per-model contract proofs or drift mismatches."),
        "inputSchema": {"type": "object",
                        "properties": {"path": {"type": "string",
                                                "description": "path to a .brat file"},
                                       "header": {"type": "string",
                                                  "description": "path to a C .h file"}},
                        "required": ["path", "header"]},
    },
    {
        "name": "brat_pkg_verify",
        "description": ("Verify `brat.lock` and `vendor/` integrity, SHA-256 digests, "
                        "`.bratbind` proofs and `c_sources` compilation inside a project directory."),
        "inputSchema": {"type": "object",
                        "properties": {"dir": {"type": "string",
                                               "description": "project root containing brat.lock"},
                                       "target": {"type": "string",
                                                  "description": "optional target triple"}},
                        "required": ["dir"]},
    },
    {
        "name": "brat_doc",
        "description": ("Extract structured API documentation (`brat doc --json`) from doc comments, "
                        "`.bri` interfaces, `pub` items and FFI contracts for a file or directory."),
        "inputSchema": {"type": "object",
                        "properties": {"path": {"type": "string",
                                                "description": "path to a .brat file or directory"}},
                        "required": ["path"]},
    },
    {
        "name": "brat_opt_proofs",
        "description": ("Run the proof-carrying abstract interpreter (`brat-cxx cc -O1|-O2 --why-opt`) "
                        "and return all proven division/modulo and bounds-check elision certificates."),
        "inputSchema": {"type": "object",
                        "properties": {"path": {"type": "string",
                                                "description": "path to a .brat file"},
                                       "level": {"type": "string",
                                                 "description": "optimization level: O1 or O2 (default O2)"}},
                        "required": ["path"]},
    },
]


def log(msg):
    if os.environ.get("BRAT_MCP_DEBUG"):
        sys.stderr.write("brat mcp: %s\n" % msg)
        sys.stderr.flush()


MAX_TOOL_OUTPUT = 2 * 1024 * 1024


def _run_bounded(cmd, timeout=300, cwd=None):
    import signal
    import threading
    import time

    popen_kw = {
        "stdin": subprocess.DEVNULL,
        "stdout": subprocess.PIPE,
        "stderr": subprocess.PIPE,
        "cwd": cwd,
    }
    if os.name == "nt":
        popen_kw["creationflags"] = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
    else:
        popen_kw["start_new_session"] = True

    proc = subprocess.Popen(cmd, **popen_kw)
    out_chunks = []
    err_chunks = []
    state = {"out_len": 0, "err_len": 0, "over": False}
    lock = threading.Lock()

    def _kill_tree():
        if os.name != "nt":
            try:
                os.killpg(proc.pid, signal.SIGKILL)
                return
            except OSError:
                pass
        try:
            proc.kill()
        except OSError:
            pass

    def _drain(pipe, chunks, key):
        try:
            while True:
                data = pipe.read(65536)
                if not data:
                    break
                with lock:
                    state[key] += len(data)
                    if state[key] > MAX_TOOL_OUTPUT:
                        state["over"] = True
                        _kill_tree()
                        break
                    chunks.append(data)
        finally:
            try:
                pipe.close()
            except OSError:
                pass

    t_out = threading.Thread(target=_drain, args=(proc.stdout, out_chunks, "out_len"), daemon=True)
    t_err = threading.Thread(target=_drain, args=(proc.stderr, err_chunks, "err_len"), daemon=True)
    t_out.start()
    t_err.start()

    deadline = time.monotonic() + float(timeout)
    timed_out = False
    while proc.poll() is None:
        rem = deadline - time.monotonic()
        if rem <= 0:
            timed_out = True
            _kill_tree()
            break
        try:
            proc.wait(timeout=min(0.1, rem))
        except subprocess.TimeoutExpired:
            pass

    proc.wait()
    t_out.join(timeout=2.0)
    t_err.join(timeout=2.0)
    if timed_out:
        raise subprocess.TimeoutExpired(cmd, timeout)
    if state["over"]:
        raise ValueError("command output exceeds 2 MiB limit")
    out_s = b"".join(out_chunks).decode("utf-8", errors="replace")
    err_s = b"".join(err_chunks).decode("utf-8", errors="replace")
    return proc.returncode, out_s, err_s


def run_cli(argv, timeout=300, cwd=None):
    return _run_bounded([sys.executable, CLI] + argv, timeout=timeout, cwd=cwd)


def need_path(args):
    path = args.get("path")
    if not isinstance(path, str) or not path:
        raise ValueError("`path` is required and must be a string")
    path = os.path.abspath(path)
    if not os.path.isfile(path):
        raise ValueError("no such file: %s" % path)
    return path


def call_tool(name, args):
    """(text, is_error) for one tool call."""
    if name == "brat_check":
        rc, out, err = run_cli(["check", need_path(args), "--json"])
        return (out or err or "{}"), rc != 0
    if name == "brat_run":
        rc, out, err = run_cli(["run", need_path(args)])
        body = {"exit": rc, "stdout": out, "stderr": err}
        return json.dumps(body, ensure_ascii=False), rc != 0
    if name == "brat_build":
        path = need_path(args)
        raw_out = args.get("out")
        if raw_out is not None:
            if not isinstance(raw_out, str) or not raw_out:
                raise ValueError("`out` must be a non-empty string")
            if raw_out.startswith("-"):
                raise ValueError("`out` must not start with '-'")
        out_path = os.path.abspath(raw_out) if raw_out else os.path.splitext(path)[0] + ".exe"
        rc, out, err = run_cli(["build", path, "-o", out_path], timeout=600)
        body = {"exit": rc, "stdout": out, "stderr": err,
                "binary": out_path if rc == 0 else None}
        return json.dumps(body, ensure_ascii=False), rc != 0
    if name == "brat_api":
        rc, out, err = run_cli(["api", need_path(args), "--json"])
        return (out or err), rc != 0
    if name == "brat_deps":
        rc, out, err = run_cli(["deps", need_path(args), "--json"])
        return (out or err), rc != 0
    if name == "brat_abi_matrix":
        path = need_path(args)
        hdr = args.get("header")
        if not isinstance(hdr, str) or not hdr:
            raise ValueError("`header` is required and must be a string")
        hdr = os.path.abspath(hdr)
        if not os.path.isfile(hdr):
            raise ValueError("no such header: %s" % hdr)
        rc, out, err = run_cli(["ffi", path, "--header", hdr, "--abi-matrix", "--json"])
        return (out or err or "{}"), rc != 0
    if name == "brat_pkg_verify":
        raw_dir = args.get("dir")
        if not isinstance(raw_dir, str) or not raw_dir:
            raise ValueError("`dir` is required and must be a string")
        proj_dir = os.path.abspath(raw_dir)
        if not os.path.isdir(proj_dir):
            raise ValueError("no such directory: %s" % proj_dir)
        cmd = ["pkg", "verify"]
        tgt = args.get("target")
        if tgt is not None:
            if not isinstance(tgt, str) or not tgt or tgt.startswith("-"):
                raise ValueError("`target` must be a safe target triple string")
            cmd.extend(["--target", tgt])
        rc, out, err = run_cli(cmd, cwd=proj_dir)
        body = {"exit": rc, "stdout": out, "stderr": err}
        return json.dumps(body, ensure_ascii=False), rc != 0
    if name == "brat_doc":
        raw_p = args.get("path")
        if not isinstance(raw_p, str) or not raw_p:
            raise ValueError("`path` is required and must be a string")
        p_abs = os.path.abspath(raw_p)
        if not os.path.exists(p_abs):
            raise ValueError("no such file or directory: %s" % p_abs)
        rc, out, err = run_cli(["doc", p_abs, "--json"])
        return (out or err or "{}"), rc != 0
    if name == "brat_opt_proofs":
        path = need_path(args)
        lvl = args.get("level", "O2")
        if lvl not in ("O1", "O2", "-O1", "-O2"):
            raise ValueError("`level` must be 'O1' or 'O2'")
        opt_flag = lvl if lvl.startswith("-") else ("-" + lvl)
        cxx_bin = os.path.join(os.path.dirname(HERE), "cxx", "bin", "brat-cxx")
        if not os.path.isfile(cxx_bin):
            raise ValueError("brat-cxx binary is not built")
        rc_opt, _, err_opt = _run_bounded(
            [cxx_bin, "cc", opt_flag, "--why-opt", "--no-check", path], timeout=60)
        grouped = []
        cur = []
        for ln in (err_opt.splitlines() if rc_opt == 0 else []):
            if ln.startswith("fn ") and cur:
                grouped.append("\n".join(cur))
                cur = [ln]
            elif ln.strip():
                cur.append(ln)
        if cur:
            grouped.append("\n".join(cur))
        body = {
            "exit": rc_opt,
            "level": opt_flag,
            "count": len(grouped),
            "proofs": grouped,
            "stderr": err_opt if rc_opt != 0 else "",
        }
        return json.dumps(body, ensure_ascii=False), rc_opt != 0
    raise KeyError(name)


MAX_HEADER_LINE = 8 * 1024
MAX_HEADERS = 64
MAX_BODY = 16 * 1024 * 1024
_EOF = object()
_INVALID = object()


class Server:
    def __init__(self, rfile, wfile):
        self.rfile = rfile
        self.wfile = wfile
        self.next_id = 1
        self.ready = False
        self.jsonl_mode = False

    def send(self, payload):
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        if self.jsonl_mode:
            self.wfile.write(body + b"\n")
        else:
            self.wfile.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        self.wfile.flush()

    def reply(self, msg_id, result):
        self.send({"jsonrpc": "2.0", "id": msg_id, "result": result})

    def error(self, msg_id, code, message):
        self.send({"jsonrpc": "2.0", "id": msg_id,
                   "error": {"code": code, "message": message}})

    def read_message(self):
        # Support both Content-Length framing and standard MCP 2025-06-18
        # newline-delimited JSON-RPC over stdio, capped at MAX_BODY.
        headers = {}
        first = True
        while True:
            limit = (MAX_BODY if self.jsonl_mode or first else MAX_HEADER_LINE) + 1
            line = self.rfile.readline(limit)
            if not line:
                return _EOF
            if first:
                stripped = line.lstrip()
                if not stripped:
                    continue
                if stripped.startswith((b"{", b"[")):
                    self.jsonl_mode = True
                    if len(line) > MAX_BODY or not line.endswith(b"\n"):
                        return _INVALID
                    try:
                        return json.loads(stripped.decode("utf-8"))
                    except Exception:
                        return _INVALID
                first = False
            if len(line) > MAX_HEADER_LINE:
                return _INVALID
            if line in (b"\r\n", b"\n"):
                break
            try:
                k, v = line.decode("ascii").split(":", 1)
            except (ValueError, UnicodeDecodeError):
                return _INVALID
            headers[k.strip().lower()] = v.strip()
            if len(headers) > MAX_HEADERS:
                return _INVALID
        try:
            length = int(headers.get("content-length", "0"))
        except ValueError:
            return _INVALID
        if length <= 0 or length > MAX_BODY:
            return _INVALID
        raw = self.rfile.read(length)
        if len(raw) != length:
            return _INVALID
        try:
            return json.loads(raw.decode("utf-8"))
        except Exception:
            return _INVALID

    def handle(self, msg):
        if not isinstance(msg, dict):
            self.error(None, -32600, "invalid request")
            return
        method = msg.get("method")
        msg_id = msg.get("id")
        if "id" in msg:
            if isinstance(msg_id, bool) or (msg_id is not None and not isinstance(msg_id, (str, int, float))):
                self.error(None, -32600, "invalid request")
                return
        if msg.get("jsonrpc") != "2.0" or not isinstance(method, str):
            self.error(msg_id, -32600, "invalid request")
            return
        params = msg.get("params")
        if params is None:
            params = {}
        if not isinstance(params, dict):
            self.error(msg_id, -32602, "params must be an object")
            return
        if method == "initialize":
            self.ready = True
            self.reply(msg_id, {
                "protocolVersion": PROTOCOL,
                "capabilities": {"tools": {"listChanged": False}},
                "serverInfo": {"name": "brat-mcp", "version": "0.1"},
            })
            return
        if method in ("notifications/initialized", "initialized"):
            return
        if method == "ping":
            self.reply(msg_id, {})
            return
        if method == "tools/list":
            self.reply(msg_id, {"tools": TOOLS})
            return
        if method == "tools/call":
            if not self.ready:
                self.error(msg_id, -32002, "server not initialized")
                return
            name = params.get("name")
            args = params.get("arguments")
            if args is None:
                args = {}
            if not isinstance(name, str):
                self.error(msg_id, -32602, "tools/call needs a tool name")
                return
            if name not in {t["name"] for t in TOOLS}:
                self.error(msg_id, -32602, "unknown tool `%s`" % name)
                return
            if not isinstance(args, dict):
                self.error(msg_id, -32602, "arguments must be an object")
                return
            try:
                text, failed = call_tool(name, args)
            except ValueError as e:
                self.reply(msg_id, {"content": [{"type": "text", "text": str(e)}],
                                    "isError": True})
                return
            except subprocess.TimeoutExpired:
                self.reply(msg_id, {"content": [{"type": "text",
                                                 "text": "timed out"}],
                                    "isError": True})
                return
            except Exception as e:
                self.reply(msg_id, {"content": [{"type": "text",
                                                 "text": "%s: %s" % (type(e).__name__, e)}],
                                    "isError": True})
                return
            self.reply(msg_id, {"content": [{"type": "text", "text": text}],
                                "isError": bool(failed)})
            return
        if msg_id is not None and method:
            self.error(msg_id, -32601, "unknown method `%s`" % method)

    def run(self):
        while True:
            msg = self.read_message()
            if msg is _EOF:
                return 0
            if msg is _INVALID:
                self.error(None, -32700, "parse error")
                continue
            try:
                self.handle(msg)
            except Exception as e:
                log("crash: %s" % e)
                if isinstance(msg, dict) and msg.get("id") is not None:
                    self.error(msg["id"], -32603, "%s: %s" % (type(e).__name__, e))


def main():
    return Server(sys.stdin.buffer, sys.stdout.buffer).run()


if __name__ == "__main__":
    sys.exit(main())
