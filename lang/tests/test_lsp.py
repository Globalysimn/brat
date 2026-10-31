"""`brat lsp`: the server speaks the protocol, and the protocol survives abuse.

The client here is a real client: Content-Length framing over pipes, the same
bytes an editor sends. Assertions are literal — capability numbers, diagnostic
codes and lines, symbol names, jump targets — because a language server that is
"roughly right" is worse than none: it shows the wrong line with confidence.
"""
import _scratch
import _cxx

import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CLI = os.path.abspath(os.path.join(HERE, "..", "engine", "cli.py"))
TMP = _scratch.mkdtemp(prefix="brat-lsp-")
_cxx.build()

n = 0


def check(cond, msg):
    global n
    assert cond, msg
    n += 1


def write(name, text):
    path = os.path.join(TMP, name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    return path


class Client:
    def __init__(self):
        self.p = subprocess.Popen([sys.executable, CLI, "lsp"],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE)
        self.next_id = 1

    def send(self, payload):
        body = json.dumps(payload).encode()
        self.p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        self.p.stdin.flush()

    def request(self, method, params, msg_id=None):
        msg_id = msg_id if msg_id is not None else self.next_id
        self.next_id = max(self.next_id, msg_id) + 1
        self.send({"jsonrpc": "2.0", "id": msg_id, "method": method, "params": params})
        while True:
            msg = self.read()
            if msg is None:
                raise AssertionError("server closed before answering %s" % method)
            if msg.get("id") == msg_id:
                return msg

    def notify(self, method, params):
        self.send({"jsonrpc": "2.0", "method": method, "params": params})

    def read(self):
        headers = {}
        while True:
            line = self.p.stdout.readline()
            if not line:
                return None
            if line in (b"\r\n", b"\n"):
                break
            k, v = line.decode().split(":", 1)
            headers[k.strip().lower()] = v.strip()
        body = self.p.stdout.read(int(headers["content-length"]))
        return json.loads(body.decode())

    def wait_notification(self, method):
        while True:
            msg = self.read()
            if msg is None:
                raise AssertionError("no %s; server died: %r"
                                     % (method, self.p.stderr.read()[-800:]))
            if msg.get("method") == method:
                return msg

    def close(self):
        try:
            self.notify("exit", {})
            self.p.wait(timeout=10)
        except Exception:
            self.p.kill()


GOOD = '''fn twice(n: i64) -> i64 {
  return n * 2
}

fn main() -> i64 {
  let a = twice(21)
  print(a)
  return 0
}
'''

BADTYPE = GOOD.replace("return n * 2", 'return n + "bad"')
MOVED = '''fn main() -> i64 {
  let parts = ["a"]
  let alias = move parts
  print(len(parts))
  return 0
}
'''

BROKEN = '''fn main( -> i64 {
'''

root = write("good.brat", GOOD)
uri = "file://" + root

c = Client()
init = c.request("initialize", {"processId": None, "rootUri": "file://" + TMP,
                                "capabilities": {}})
check(init["result"]["serverInfo"]["name"] == "brat-lsp", "serverInfo: %r" % init)
caps = init["result"]["capabilities"]
check(caps["textDocumentSync"] == {"openClose": True, "change": 2},
      "sync: %r" % (caps["textDocumentSync"],))
check(caps["documentSymbolProvider"] is True, "symbols capability")
check(caps["definitionProvider"] is True, "definition capability")
c.notify("initialized", {})

c.notify("textDocument/didOpen", {"textDocument": {
    "uri": uri, "languageId": "brat", "version": 1, "text": GOOD}})
msg = c.wait_notification("textDocument/publishDiagnostics")
check(msg["params"] == {"uri": uri, "diagnostics": []}, "clean file: %r" % msg["params"])

syms = c.request("textDocument/documentSymbol", {"textDocument": {"uri": uri}})
names = [(s["name"], s["kind"], s["selectionRange"]["start"]["line"]) for s in syms["result"]]
check(names == [("twice", 12, 0), ("main", 12, 4)], "symbols: %r" % (names,))

d = c.request("textDocument/definition", {
    "textDocument": {"uri": uri}, "position": {"line": 5, "character": 10}})
# `fn twice` — the name starts at column 3, the `t` of twice
check(d["result"] == {"uri": uri,
                      "range": {"start": {"line": 0, "character": 3},
                                "end": {"line": 0, "character": 8}}},
      "definition of twice: %r" % (d["result"],))

# Local binding: cursor on `a` at line 6 resolves to the `let a` on line 5.
d = c.request("textDocument/definition", {
    "textDocument": {"uri": uri}, "position": {"line": 6, "character": 8}})
check(d["result"]["range"]["start"]["line"] == 5, "definition of a: %r" % (d["result"],))

# The whole file is re-read on change: the move is introduced and reported.
c.notify("textDocument/didChange", {
    "textDocument": {"uri": uri, "version": 2},
    "contentChanges": [{"text": MOVED}]})
msg = c.wait_notification("textDocument/publishDiagnostics")
got = [(d["code"], d["severity"], d["range"]["start"]["line"]) for d in msg["params"]["diagnostics"]]
check(got == [("W0001", 2, 2), ("E0382", 1, 3)], "after change: %r" % (got,))
check(msg["params"]["diagnostics"][1]["source"] == "brat", "source tag")
check("use of moved `parts`" in msg["params"]["diagnostics"][1]["message"],
      "message: %r" % msg["params"]["diagnostics"][1]["message"])
check("relatedInformation" in msg["params"]["diagnostics"][1],
      "move note becomes relatedInformation")

# A syntax error is one diagnostic, not a dead server.
c.notify("textDocument/didChange", {
    "textDocument": {"uri": uri, "version": 3},
    "contentChanges": [{"text": BROKEN}]})
msg = c.wait_notification("textDocument/publishDiagnostics")
got = [(d["code"], d["range"]["start"]["line"]) for d in msg["params"]["diagnostics"]]
check(got == [("P0001", 0)], "syntax error: %r" % (got,))
syms = c.request("textDocument/documentSymbol", {"textDocument": {"uri": uri}})
check(syms["result"] == [], "symbols while broken: %r" % (syms["result"],))

# ... and it recovers when the text comes back.
c.notify("textDocument/didChange", {
    "textDocument": {"uri": uri, "version": 4},
    "contentChanges": [{"text": GOOD}]})
msg = c.wait_notification("textDocument/publishDiagnostics")
check(msg["params"]["diagnostics"] == [], "recovered: %r" % msg["params"])

# Ranged changes are applied in order using the protocol's UTF-16 positions.
c.notify("textDocument/didChange", {"textDocument": {"uri": uri, "version": 5},
    "contentChanges": [
        {"range": {"start": {"line": 1, "character": 9},
                   "end": {"line": 1, "character": 14}}, "text": 'n + "bad"'},
    ]})
msg = c.wait_notification("textDocument/publishDiagnostics")
check([(d["code"], d["range"]["start"]["line"]) for d in msg["params"]["diagnostics"]]
      == [("T0001", 1)], "ranged edit diagnostics: %r" % msg["params"]["diagnostics"])

c.notify("textDocument/didChange", {"textDocument": {"uri": uri, "version": 6},
    "contentChanges": [
        {"range": {"start": {"line": 1, "character": 9},
                   "end": {"line": 1, "character": 18}}, "text": "n * 3"},
        {"range": {"start": {"line": 1, "character": 9},
                   "end": {"line": 1, "character": 14}}, "text": "n * 2"},
    ]})
msg = c.wait_notification("textDocument/publishDiagnostics")
check(msg["params"]["diagnostics"] == [],
      "sequential ranged changes restore clean diagnostics: %r" % msg["params"])

unicode_text = "// 😀 foo\nfn main() -> i64 {\n  return 0\n}\n"
unicode_uri = "file://" + write("utf16.brat", unicode_text)
c.notify("textDocument/didOpen", {"textDocument": {
    "uri": unicode_uri, "languageId": "brat", "version": 1, "text": unicode_text}})
msg = c.wait_notification("textDocument/publishDiagnostics")
check(msg["params"]["diagnostics"] == [], "unicode document opens cleanly")
c.notify("textDocument/didChange", {"textDocument": {"uri": unicode_uri, "version": 2},
    "contentChanges": [{"range": {"start": {"line": 0, "character": 6},
                                 "end": {"line": 0, "character": 9}},
                        "rangeLength": 3, "text": "bar"}]})
msg = c.wait_notification("textDocument/publishDiagnostics")
check(msg["params"]["diagnostics"] == [], "UTF-16 ranged edit after astral character")
c.notify("textDocument/didChange", {"textDocument": {"uri": unicode_uri, "version": 3},
    "contentChanges": [{"range": {"start": {"line": 0, "character": 4},
                                 "end": {"line": 0, "character": 5}},
                        "text": "x"}]})
logged = c.wait_notification("window/logMessage")
check("invalid UTF-16 range" in logged["params"]["message"],
      "invalid UTF-16 range is reported: %r" % logged)
c.notify("textDocument/didChange", {"textDocument": {"uri": unicode_uri, "version": 4},
    "contentChanges": [{"range": {"start": {"line": 0, "character": 6},
                                 "end": {"line": 0, "character": 9}},
                        "text": "baz"}]})
msg = c.wait_notification("textDocument/publishDiagnostics")
check(msg["params"]["diagnostics"] == [], "document accepts a valid edit after invalid range")
c.notify("textDocument/didChange", {"textDocument": {"uri": unicode_uri, "version": 5},
    "contentChanges": [{"range": {"start": {"line": 0, "character": 1e100},
                                 "end": {"line": 0, "character": 2}},
                        "text": "x"}]})
logged = c.wait_notification("window/logMessage")
check("invalid UTF-16 range" in logged["params"]["message"],
      "out-of-range numeric position is rejected: %r" % logged)

# A body-only edit keeps the declarations fixed and checks only the changed
# function; the diagnostic must be refreshed rather than retained from cache.
c.notify("textDocument/didChange", {
    "textDocument": {"uri": uri, "version": 7},
    "contentChanges": [{"text": BADTYPE}]})
msg = c.wait_notification("textDocument/publishDiagnostics")
check([(d["code"], d["range"]["start"]["line"])
       for d in msg["params"]["diagnostics"]] == [("T0001", 1)],
      "incremental type error: %r" % msg["params"]["diagnostics"])
c.notify("textDocument/didChange", {
    "textDocument": {"uri": uri, "version": 8},
    "contentChanges": [{"text": GOOD}]})
msg = c.wait_notification("textDocument/publishDiagnostics")
check(msg["params"]["diagnostics"] == [], "incremental recovery: %r" % msg["params"])

# Body edits may change names without touching the function signature.
missing_name = GOOD.replace("return n * 2", "return n + missing")
c.notify("textDocument/didChange", {
    "textDocument": {"uri": uri, "version": 9},
    "contentChanges": [{"text": missing_name}]})
msg = c.wait_notification("textDocument/publishDiagnostics")
check([(d["code"], d["range"]["start"]["line"])
       for d in msg["params"]["diagnostics"]] == [("B0101", 1)],
      "incremental identifier addition: %r" % msg["params"]["diagnostics"])
renamed_body = missing_name.replace("n + missing", "n + n")
c.notify("textDocument/didChange", {
    "textDocument": {"uri": uri, "version": 10},
    "contentChanges": [{"text": renamed_body}]})
msg = c.wait_notification("textDocument/publishDiagnostics")
check(msg["params"]["diagnostics"] == [],
      "incremental identifier recovery: %r" % msg["params"])

# Adding or removing body lines keeps the check local and shifts later diagnostics.
later_error = GOOD.replace("print(a)", "print(unknown)")
c.notify("textDocument/didChange", {
    "textDocument": {"uri": uri, "version": 11},
    "contentChanges": [{"text": later_error}]})
msg = c.wait_notification("textDocument/publishDiagnostics")
check([(d["code"], d["range"]["start"]["line"])
       for d in msg["params"]["diagnostics"]] == [("W0001", 5), ("B0101", 6)],
      "later function diagnostic before line insertion: %r" % msg["params"])
with_extra_line = later_error.replace(
    "return n * 2", "let doubled = n * 2\n  return doubled")
c.notify("textDocument/didChange", {
    "textDocument": {"uri": uri, "version": 12},
    "contentChanges": [{"text": with_extra_line}]})
msg = c.wait_notification("textDocument/publishDiagnostics")
check([(d["code"], d["range"]["start"]["line"])
       for d in msg["params"]["diagnostics"]] == [("W0001", 6), ("B0101", 7)],
      "line insertion shifts later diagnostic: %r" % msg["params"])
syms = c.request("textDocument/documentSymbol", {"textDocument": {"uri": uri}})
check([(s["name"], s["selectionRange"]["start"]["line"]) for s in syms["result"]]
      == [("twice", 0), ("main", 5)],
      "symbols after line insertion: %r" % syms["result"])
c.notify("textDocument/didChange", {
    "textDocument": {"uri": uri, "version": 13},
    "contentChanges": [{"text": later_error}]})
msg = c.wait_notification("textDocument/publishDiagnostics")
check([(d["code"], d["range"]["start"]["line"])
       for d in msg["params"]["diagnostics"]] == [("W0001", 5), ("B0101", 6)],
      "line deletion shifts later diagnostic back: %r" % msg["params"])
c.notify("textDocument/didChange", {
    "textDocument": {"uri": uri, "version": 14},
    "contentChanges": [{"text": GOOD}]})
msg = c.wait_notification("textDocument/publishDiagnostics")
check(msg["params"]["diagnostics"] == [], "restore original text: %r" % msg["params"])

# A late notification must not roll the document back to an older version.
c.notify("textDocument/didChange", {
    "textDocument": {"uri": uri, "version": 6},
    "contentChanges": [{"text": MOVED}]})
still_current = c.request("textDocument/definition", {
    "textDocument": {"uri": uri}, "position": {"line": 5, "character": 10}})
check(still_current["result"]["range"]["start"]["line"] == 0,
      "stale change overwrote current text: %r" % still_current["result"])

# An unknown method is an error with the right code, and the server lives.
err = c.request("workspace/nonsense", {})
check(err.get("error", {}).get("code") == -32601, "unknown method: %r" % err)
again = c.request("textDocument/documentSymbol", {"textDocument": {"uri": uri}})
check(len(again["result"]) == 2, "alive after unknown method")

# Malformed JSON gets a parse error without taking down the server.
body = b"{"
c.p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
c.p.stdin.flush()
parse_error = c.read()
check(parse_error.get("error", {}).get("code") == -32700,
      "malformed JSON: %r" % parse_error)
again = c.request("textDocument/documentSymbol", {"textDocument": {"uri": uri}})
check(len(again["result"]) == 2, "alive after parse error")

# A request for a file the server never opened is answered, not crashed.
ghost = c.request("textDocument/documentSymbol",
                  {"textDocument": {"uri": "file://" + TMP + "/ghost.brat"}})
check(ghost["result"] == [], "ghost file: %r" % ghost)

shut = c.request("shutdown", {})
check(shut["result"] is None, "shutdown: %r" % shut)
c.close()

# UTF-16 positions: two units for an emoji, one for a Cyrillic letter. A server
# that counts Python characters resolves the wrong word on the same line.
WIDE = '''fn main() -> i64 {
  let s = "😀привет"
  let n = 7
  print(n + len(s))
  return 0
}
'''
wide_path = write("wide.brat", WIDE)
c = Client()
c.request("initialize", {"processId": None, "rootUri": None, "capabilities": {}})
c.notify("initialized", {})
c.notify("textDocument/didOpen", {"textDocument": {
    "uri": "file://" + wide_path, "languageId": "brat", "version": 1, "text": WIDE}})
c.wait_notification("textDocument/publishDiagnostics")
# line 3 is `print(n + len(s))`; `n` sits at UTF-16 column 8
d = c.request("textDocument/definition", {
    "textDocument": {"uri": "file://" + wide_path},
    "position": {"line": 3, "character": 9}})
check(d["result"] is not None and d["result"]["range"]["start"]["line"] == 2,
      "utf16 position: %r" % (d["result"],))
# and `s` on the same line, after the emoji has shifted nothing on this line
d = c.request("textDocument/definition", {
    "textDocument": {"uri": "file://" + wide_path},
    "position": {"line": 3, "character": 17}})
check(d["result"] is not None and d["result"]["range"]["start"]["line"] == 1,
      "utf16 second word: %r" % (d["result"],))
c.close()

# Cross-module jump: the cursor on shapes.area lands in the module file.
write("lib/shapes.brat", '''pub fn area(r: i64) -> i64 {
  return r * r
}
''')
main_path = write("lib/main.brat", '''import shapes

fn main() -> i64 {
  print(shapes.area(3))
  return 0
}
''')
c = Client()
c.request("initialize", {"processId": None, "rootUri": None, "capabilities": {}})
c.notify("initialized", {})
text = open(main_path, encoding="utf-8").read()
c.notify("textDocument/didOpen", {"textDocument": {
    "uri": "file://" + main_path, "languageId": "brat", "version": 1, "text": text}})
msg = c.wait_notification("textDocument/publishDiagnostics")
check(msg["params"]["diagnostics"] == [], "module clean: %r" % msg["params"])
# `  print(shapes.area(3))` — `area` occupies columns 15..19
d = c.request("textDocument/definition", {
    "textDocument": {"uri": "file://" + main_path},
    "position": {"line": 3, "character": 17}})
check(d["result"] is not None and d["result"]["uri"].endswith("shapes.brat"),
      "cross-module: %r" % (d["result"],))
check(d["result"]["range"]["start"]["line"] == 0, "module target line: %r" % (d["result"],))

# Module resolution follows unsaved import edits from the editor buffer, not
# the root file's older on-disk contents.
mathx_path = write("lib/mathx.brat", '''pub fn square(r: i64) -> i64 {
  return r * r
}
''')
updated = text.replace("import shapes", "import mathx").replace(
    "shapes.area(3)", "mathx.square(3)")
c.notify("textDocument/didChange", {"textDocument": {
    "uri": "file://" + main_path, "version": 2},
    "contentChanges": [{"text": updated}]})
msg = c.wait_notification("textDocument/publishDiagnostics")
check(msg["params"]["diagnostics"] == [], "unsaved import change: %r" % msg["params"])
d = c.request("textDocument/definition", {
    "textDocument": {"uri": "file://" + main_path},
    "position": {"line": 3, "character": 16}})
check(d["result"] is not None and d["result"]["uri"].endswith("mathx.brat"),
      "definition after unsaved import: %r" % (d["result"],))
c.close()

borrowed_path = write("borrowed-move.brat", '''fn consume(v: vec[i64]) -> i64 {
  let owned = move v
  return len(owned)
}
''')
c = Client()
c.request("initialize", {"processId": None, "rootUri": None, "capabilities": {}})
c.notify("initialized", {})
c.notify("textDocument/didOpen", {"textDocument": {
    "uri": "file://" + borrowed_path, "languageId": "brat", "version": 1,
    "text": open(borrowed_path, encoding="utf-8").read()}})
msg = c.wait_notification("textDocument/publishDiagnostics")
check([(d["code"], d["range"]["start"]["line"])
       for d in msg["params"]["diagnostics"]] == [("E0507", 1)],
      "borrowed parameter move diagnostic: %r" % msg["params"])
c.close()

print("LSP ALL GREEN (%d asserts)" % n)
