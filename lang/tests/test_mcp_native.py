"""Native MCP framing and tool dispatch run without a Python executable."""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import _cxx


def pythonless_env(directory):
    os.makedirs(directory, exist_ok=True)
    for tool in ("cc", "gcc", "clang", "as", "ld", "ar", "collect2", "sh", "rm", "cp", "mkdir"):
        real = shutil.which(tool)
        if real:
            link = os.path.join(directory, tool)
            if not os.path.lexists(link):
                os.symlink(real, link)
    env = dict(os.environ, PATH=directory)
    assert shutil.which("python", path=env["PATH"]) is None
    assert shutil.which("python3", path=env["PATH"]) is None
    return env


class Client:
    def __init__(self, binary, env, jsonl=False):
        self.jsonl = jsonl
        self.proc = subprocess.Popen([binary, "mcp"], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                     env=env)
        self.next_id = 1

    def send(self, payload):
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        if self.jsonl:
            self.proc.stdin.write(body + b"\n")
        else:
            self.proc.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        self.proc.stdin.flush()

    def read(self):
        if self.jsonl:
            line = self.proc.stdout.readline()
            return None if not line else json.loads(line.decode("utf-8"))
        headers = {}
        while True:
            line = self.proc.stdout.readline()
            if not line:
                raise AssertionError("native MCP closed unexpectedly: %r" %
                                     self.proc.stderr.read()[-1000:])
            if line in (b"\r\n", b"\n"):
                break
            key, value = line.decode("ascii").split(":", 1)
            headers[key.strip().lower()] = value.strip()
        length = int(headers["content-length"])
        return json.loads(self.proc.stdout.read(length).decode("utf-8"))

    def request(self, method, params=None):
        msg_id = self.next_id
        self.next_id += 1
        self.send({"jsonrpc": "2.0", "id": msg_id, "method": method,
                   "params": params or {}})
        while True:
            response = self.read()
            if response.get("id") == msg_id:
                return response

    def close(self):
        self.proc.stdin.close()
        code = self.proc.wait(timeout=15)
        assert code == 0, self.proc.stderr.read()[-2000:]
        assert not self.proc.stdout.read()
        assert not self.proc.stderr.read()


def call(client, name, args):
    response = client.request("tools/call", {"name": name, "arguments": args})
    assert "result" in response, response
    result = response["result"]
    assert len(result["content"]) == 1 and result["content"][0]["type"] == "text", result
    return result


def main():
    binary = _cxx.build()
    with tempfile.TemporaryDirectory(prefix="brat-mcp-") as td:
        env = pythonless_env(os.path.join(td, "tools"))
        source = os.path.join(td, "main.brat")
        with open(source, "w", encoding="utf-8") as f:
            f.write("fn main() -> i64 { print(42) return 0 }\n")
        client = Client(binary, env)
        init = client.request("initialize", {"protocolVersion": "2025-06-18",
                                               "capabilities": {},
                                               "clientInfo": {"name": "test", "version": "1"}})
        assert init["result"]["serverInfo"]["name"] == "brat-mcp"
        assert init["result"]["protocolVersion"] == "2025-06-18"
        listed = client.request("tools/list")["result"]["tools"]
        expected = ["brat_check", "brat_run", "brat_build", "brat_api", "brat_deps",
                    "brat_abi_matrix", "brat_pkg_verify", "brat_doc", "brat_opt_proofs"]
        assert [tool["name"] for tool in listed] == expected
        assert all(tool["inputSchema"]["type"] == "object" for tool in listed)
        client.send({"jsonrpc": "2.0", "method": "notifications/initialized"})

        checked = call(client, "brat_check", {"path": source})
        check_payload = json.loads(checked["content"][0]["text"])
        assert not checked["isError"] and check_payload["diagnostics"] == []

        ran = call(client, "brat_run", {"path": source})
        run_payload = json.loads(ran["content"][0]["text"])
        assert not ran["isError"] and run_payload["exit"] == 0 and run_payload["stdout"] == "42\n"

        built_path = source + ".exe"
        built = call(client, "brat_build", {"path": source, "out": built_path})
        build_payload = json.loads(built["content"][0]["text"])
        assert not built["isError"] and build_payload["binary"] == built_path
        assert os.path.isfile(built_path)

        module = os.path.join(td, "shapes.brat")
        with open(module, "w", encoding="utf-8") as f:
            f.write("pub fn area(r: i64) -> i64 { return r * r }\n")
        app = os.path.join(td, "app.brat")
        with open(app, "w", encoding="utf-8") as f:
            f.write("import shapes\nfn main() -> i64 { return shapes.area(2) }\n")
        api = call(client, "brat_api", {"path": app})
        api_payload = json.loads(api["content"][0]["text"])
        assert not api["isError"] and api_payload["modules"][0]["exports"][0]["name"] == "area"
        deps = call(client, "brat_deps", {"path": app})
        deps_payload = json.loads(deps["content"][0]["text"])
        assert not deps["isError"] and deps_payload["modules"] == 2
        docs = call(client, "brat_doc", {"path": td})
        assert not docs["isError"] and json.loads(docs["content"][0]["text"])["modules"]
        proofs = call(client, "brat_opt_proofs", {"path": source, "level": "O1"})
        proof_payload = json.loads(proofs["content"][0]["text"])
        assert not proofs["isError"] and proof_payload["level"] == "-O1"
        assert isinstance(proof_payload["proofs"], list)

        pkg_dir = os.path.join(td, "pkg")
        vendor = os.path.join(pkg_dir, "vendor")
        os.makedirs(vendor)
        pkg_data = b"pub fn answer() -> i64 { return 42 }\n"
        with open(os.path.join(vendor, "answer.brat"), "wb") as f:
            f.write(pkg_data)
        lock_item = {"file": "answer.brat", "url": "file:///tmp/answer.brat",
                     "sha256": hashlib.sha256(pkg_data).hexdigest()}
        with open(os.path.join(pkg_dir, "brat.lock"), "w", encoding="utf-8") as f:
            json.dump([lock_item], f)
        verified = call(client, "brat_pkg_verify", {"dir": pkg_dir})
        verify_payload = json.loads(verified["content"][0]["text"])
        assert not verified["isError"] and verify_payload["exit"] == 0
        assert "verified 1 package(s)" in verify_payload["stdout"]

        missing = call(client, "brat_check", {"path": os.path.join(td, "missing.brat")})
        assert missing["isError"] and "no such file" in missing["content"][0]["text"]
        unknown = client.request("tools/call", {"name": "brat_unknown", "arguments": {}})
        assert unknown.get("error", {}).get("code") == -32602
        assert client.request("ping")["result"] == {}
        client.close()

        # JSONL and Content-Length framing can coexist with the same JSON-RPC protocol surface.
        jsonl = Client(binary, env, jsonl=True)
        assert jsonl.request("initialize")["result"]["serverInfo"]["name"] == "brat-mcp"
        assert jsonl.request("ping")["result"] == {}
        jsonl.close()

        # A malformed JSON frame is reported without corrupting the following frame.
        malformed = Client(binary, env)
        malformed.proc.stdin.write(b"Content-Length: 1\r\n\r\n{")
        malformed.proc.stdin.flush()
        assert malformed.read().get("error", {}).get("code") == -32700
        assert malformed.request("ping")["result"] == {}

        # Framed sessions reject unframed JSONL lines and duplicate Content-Length headers.
        malformed.proc.stdin.write(b'{"jsonrpc":"2.0","id":77,"method":"ping"}\n')
        malformed.proc.stdin.flush()
        assert malformed.read().get("error", {}).get("code") == -32700
        malformed.close()

        dup = Client(binary, env)
        dup.proc.stdin.write(b"Content-Length: 10\r\nContent-Length: 2\r\n\r\n")
        dup.proc.stdin.flush()
        assert dup.read().get("error", {}).get("code") == -32700
        dup.close()

    print("NATIVE MCP ALL GREEN")


if __name__ == "__main__":
    main()
