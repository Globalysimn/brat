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
        "description": "Run a BRAT file on the interpreter. Returns stdout, stderr and the exit code.",
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
]


def log(msg):
    if os.environ.get("BRAT_MCP_DEBUG"):
        sys.stderr.write("brat mcp: %s\n" % msg)
        sys.stderr.flush()


def run_cli(argv, timeout=300):
    p = subprocess.run([sys.executable, CLI] + argv, capture_output=True, text=True,
                       timeout=timeout)
    return p.returncode, p.stdout, p.stderr


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
        out_path = args.get("out") or os.path.splitext(path)[0] + ".exe"
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

    def send(self, payload):
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.wfile.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        self.wfile.flush()

    def reply(self, msg_id, result):
        self.send({"jsonrpc": "2.0", "id": msg_id, "result": result})

    def error(self, msg_id, code, message):
        self.send({"jsonrpc": "2.0", "id": msg_id,
                   "error": {"code": code, "message": message}})

    def read_message(self):
        # A client is not trusted to say how much it will send: without a cap a
        # single header line or body size decides the server's memory. The LSP
        # server applies the same limits in `lsp.cpp`.
        headers = {}
        while True:
            line = self.rfile.readline(MAX_HEADER_LINE + 1)
            if not line:
                return _EOF
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
                return None
        try:
            length = int(headers.get("content-length", "0"))
        except ValueError:
            return _INVALID
        if length <= 0 or length > MAX_BODY:
            return _INVALID
        try:
            return json.loads(self.rfile.read(length).decode("utf-8"))
        except Exception:
            return _INVALID

    def handle(self, msg):
        if not isinstance(msg, dict):
            self.error(None, -32600, "invalid request")
            return
        method = msg.get("method")
        msg_id = msg.get("id")
        if msg.get("jsonrpc") != "2.0" or not isinstance(method, str):
            self.error(msg_id, -32600, "invalid request")
            return
        params = msg.get("params") or {}
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
            args = params.get("arguments") or {}
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
