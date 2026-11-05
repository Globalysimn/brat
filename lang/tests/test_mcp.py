"""`brat mcp`: an agent-facing server that fails the way the protocol expects.

Three rules are tested literally, because each is a real failure mode of MCP
servers in the wild: tool failures come back as `isError: true` results (the
agent can see them and correct), protocol faults come back as JSON-RPC errors,
and stdout carries nothing but protocol — one stray print and the session dies.
"""
import _cxx
import _scratch

import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TMP = _scratch.mkdtemp(prefix="brat-mcp-")

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
        self.p = subprocess.Popen(_cxx.cli_args("mcp"), stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.n = 1

    def send(self, payload):
        body = json.dumps(payload).encode()
        self.p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        self.p.stdin.flush()

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
        return json.loads(self.p.stdout.read(int(headers["content-length"])))

    def request(self, method, params=None):
        msg_id = self.n
        self.n += 1
        self.send({"jsonrpc": "2.0", "id": msg_id, "method": method,
                   "params": params or {}})
        while True:
            msg = self.read()
            if msg is None:
                raise AssertionError("closed on %s: %r" % (method,
                                                           self.p.stderr.read()[-600:]))
            if msg.get("id") == msg_id:
                return msg

    def close(self):
        try:
            self.p.stdin.close()
            self.p.wait(timeout=15)
        except Exception:
            self.p.kill()


c = Client()
init = c.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                "clientInfo": {"name": "test", "version": "1"}})
check(init["result"]["serverInfo"]["name"] == "brat-mcp", "serverInfo: %r" % init)
check(init["result"]["protocolVersion"] == "2025-06-18", "protocol: %r" % init["result"])
check("tools" in init["result"]["capabilities"], "tools capability")
c.send({"jsonrpc": "2.0", "method": "notifications/initialized", "params": {}})

listed = c.request("tools/list")
names = [t["name"] for t in listed["result"]["tools"]]
check(names == ["brat_check", "brat_run", "brat_build", "brat_api", "brat_deps",
                "brat_abi_matrix", "brat_pkg_verify", "brat_doc", "brat_opt_proofs"],
      "tool order: %r" % (names,))
check(all("inputSchema" in t and t["inputSchema"]["type"] == "object"
          for t in listed["result"]["tools"]), "every tool has an object schema")
check(all(t["description"] and len(t["description"]) > 40 for t in listed["result"]["tools"]),
      "every tool has a real description")

good = write("ok.brat", '''fn main() -> i64 {
  print(6 * 7)
  return 0
}
''')
r = c.request("tools/call", {"name": "brat_check", "arguments": {"path": good}})
check(r["result"]["isError"] is False, "clean check: %r" % r["result"])
payload = json.loads(r["result"]["content"][0]["text"])
check(payload["version"] == 1 and payload["diagnostics"] == [],
      "check payload: %r" % payload)

bad = write("bad.brat", '''fn main() -> i64 {
  let parts = ["a"]
  let alias = move parts
  print(len(parts))
  return 0
}
''')
r = c.request("tools/call", {"name": "brat_check", "arguments": {"path": bad}})
check(r["result"]["isError"] is True, "bad check is an error result, not a crash")
payload = json.loads(r["result"]["content"][0]["text"])
codes = [(d["code"], d["line"]) for d in payload["diagnostics"]]
# `check --json` lines are 1-based, unlike LSP positions
check(codes == [("W0001", 3), ("E0382", 4)], "diagnostics: %r" % (codes,))
check(payload["diagnostics"][1]["helps"], "helps survives the round trip")

r = c.request("tools/call", {"name": "brat_run", "arguments": {"path": good}})
body = json.loads(r["result"]["content"][0]["text"])
check(r["result"]["isError"] is False and body["exit"] == 0 and body["stdout"] == "42\n",
      "run: %r" % body)

r = c.request("tools/call", {"name": "brat_run", "arguments": {"path": bad}})
check(r["result"]["isError"] is True, "run of a bad file is an error result")

# A module pair, so api and deps have something to report.
write("shapes.brat", "pub fn area(r: i64) -> i64 {\n  return r * r\n}\n")
main_mod = write("app.brat", '''import shapes

fn main() -> i64 {
  print(shapes.area(5))
  return 0
}
''')
r = c.request("tools/call", {"name": "brat_api", "arguments": {"path": main_mod}})
api = json.loads(r["result"]["content"][0]["text"])
check(r["result"]["isError"] is False, "api ok: %r" % r["result"])
check([m["module"] for m in api["modules"]] == ["shapes"], "api modules: %r" % api["modules"])
check(api["modules"][0]["exports"][0]["name"] == "area" and
      api["modules"][0]["exports"][0]["ret"] == "i64", "api export: %r" % api["modules"])

r = c.request("tools/call", {"name": "brat_deps", "arguments": {"path": main_mod}})
deps = json.loads(r["result"]["content"][0]["text"])
check(deps["edges"] == [{"from": "main", "import": "shapes", "to": "shapes"}],
      "deps edges: %r" % (deps["edges"],))
check(deps["modules"] == 2, "deps modules: %r" % deps["modules"])

# Bad input is a tool error the agent can read, not a protocol fault.
r = c.request("tools/call", {"name": "brat_check", "arguments": {"path": TMP + "/nope.brat"}})
check(r["result"]["isError"] is True and "no such file" in r["result"]["content"][0]["text"],
      "missing file: %r" % r["result"])
r = c.request("tools/call", {"name": "brat_check", "arguments": {}})
check(r["result"]["isError"] is True and "`path` is required" in r["result"]["content"][0]["text"],
      "missing argument: %r" % r["result"])

# Protocol faults stay protocol faults.
r = c.request("tools/call", {"name": "brat_explode", "arguments": {}})
check(r.get("error", {}).get("code") == -32602, "unknown tool: %r" % r)
r = c.request("workspace/nonsense")
check(r.get("error", {}).get("code") == -32601, "unknown method: %r" % r)
r = c.request("ping")
check(r["result"] == {}, "ping: %r" % r)

# A valid JSON document with the wrong root type is an invalid request, not a
# server crash; the same session must still be able to answer the next ping.
invalid = Client()
try:
    invalid.send([])
    r = invalid.read()
    check(r.get("error", {}).get("code") == -32600,
        "non-object JSON-RPC request: %r" % r)
    invalid.send(None)
    r = invalid.read()
    check(r.get("error", {}).get("code") == -32600,
          "JSON null is a valid JSON but invalid request: %r" % r)
    body = b"{"
    invalid.p.stdin.write(b"Content-Length: 1\r\n\r\n" + body)
    invalid.p.stdin.flush()
    r = invalid.read()
    check(r.get("error", {}).get("code") == -32700,
          "malformed JSON must produce a parse error: %r" % r)
    r = invalid.request("ping")
    check(r.get("result") == {}, "invalid JSON-RPC root killed the session: %r" % r)
finally:
    invalid.close()

c.close()

# stdout carried only protocol: every line of it parsed above. A separate
# process, killed mid-flight, must not have written prose either.
noise = subprocess.run(_cxx.cli_args("mcp"), input=b"",
                       capture_output=True, timeout=60)
check(noise.stdout == b"", "no protocol, no stdout: %r" % noise.stdout)

print("MCP ALL GREEN (%d asserts)" % n)
