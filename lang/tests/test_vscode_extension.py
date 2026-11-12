"""VS Code extension manifest, binary resolution, TextMate grammar, and LSP VS Code protocol tests."""
import json
import os
import shutil
import subprocess

import _cxx
import _scratch

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
VS_DIR = os.path.join(ROOT, "editors", "vscode")
BIN = _cxx.build()
TMP = _scratch.mkdtemp(prefix="brat-vscode-")


def test_manifests_and_node_helpers():
    for rel in (
        "package.json",
        "extension.js",
        "language-configuration.json",
        "README.md",
        "LICENSE",
        ".vscodeignore",
        os.path.join("snippets", "brat.code-snippets"),
        os.path.join("syntaxes", "brat.tmLanguage.json"),
    ):
        full = os.path.join(VS_DIR, rel)
        assert os.path.isfile(full), f"missing VS Code extension asset: {full}"

    for root_rel in ("launch.json", "tasks.json", "extensions.json"):
        full = os.path.join(ROOT, ".vscode", root_rel)
        assert os.path.isfile(full), f"missing workspace .vscode/{root_rel}"

    with open(os.path.join(VS_DIR, "package.json"), encoding="utf-8") as f:
        pkg = json.load(f)
    props = pkg["contributes"]["configuration"]["properties"]
    assert props["brat.lsp.path"]["scope"] == "machine-overridable"
    cmd_ids = {c["command"] for c in pkg["contributes"]["commands"]}
    assert {
        "brat.restartServer",
        "brat.runFile",
        "brat.checkFile",
        "brat.showOptProofs",
    }.issubset(cmd_ids)

    with open(os.path.join(VS_DIR, "syntaxes", "brat.tmLanguage.json"), encoding="utf-8") as f:
        tm = json.load(f)
    assert tm["scopeName"] == "source.brat"
    assert "repository" in tm and "strings" in tm["repository"]

    with open(os.path.join(VS_DIR, "snippets", "brat.code-snippets"), encoding="utf-8") as f:
        snippets = json.load(f)
    assert "Struct" in snippets and "Function" in snippets

    node = shutil.which("node")
    if not node:
        return

    # Unit-test extension.js binary resolution & Restricted Mode behavior with a mocked `vscode` module.
    harness = os.path.join(TMP, "verify_ext.js")
    with open(harness, "w", encoding="utf-8") as f:
        f.write(
            r"""
const assert = require("assert");
const Module = require("module");
const path = require("path");
const fs = require("fs");

const repoRoot = process.argv[2];
const extPath = path.join(repoRoot, "editors", "vscode", "extension.js");
const realBin = process.argv[3];
const tmpDir = process.argv[4];

let state = {
  isTrusted: false,
  workspaceFolders: [{ uri: { fsPath: path.join(repoRoot, "examples") } }],
  globalVal: "",
  wsVal: "",
};

const origLoad = Module._load;
Module._load = function (request, parent, isMain) {
  if (request === "vscode") {
    return {
      workspace: {
        get isTrusted() { return state.isTrusted; },
        get workspaceFolders() { return state.workspaceFolders; },
        getConfiguration() {
          return {
            inspect() { return { globalValue: state.globalVal }; },
            get() { return state.wsVal; },
          };
        },
        createFileSystemWatcher() { return { dispose() {} }; },
        onDidChangeConfiguration() { return { dispose() {} }; },
      },
      window: {
        createOutputChannel() { return { appendLine() {}, show() {}, dispose() {} }; },
        showWarningMessage: async () => {},
        showErrorMessage: async () => {},
      },
      commands: {
        registerCommand() { return { dispose() {} }; },
      },
      ViewColumn: { Beside: 2 },
    };
  }
  if (request === "vscode-languageclient/node") {
    return {
      TransportKind: { stdio: 0 },
      LanguageClient: class {
        needsStop() { return false; }
        async start() {}
        async stop() {}
      },
    };
  }
  return origLoad.apply(this, arguments);
};

const ext = require(extPath);

// 1. Restricted mode refuses workspace binary unless globalValue is set.
assert.strictEqual(ext.resolveServerBinary(), null);
state.globalVal = realBin;
assert.strictEqual(ext.resolveServerBinary(), realBin);
state.globalVal = "";

// 2. Trusted workspace finds brat-cxx by walking up from `examples/` to repo root.
state.isTrusted = true;
const resolved = ext.resolveServerBinary();
assert.ok(resolved && fs.existsSync(resolved), "expected checkout binary, got: " + resolved);

// 3. ${workspaceFolder} expansion works.
state.workspaceFolders = [{ uri: { fsPath: repoRoot } }];
state.wsVal = "${workspaceFolder}/lang/cxx/bin/" + path.basename(realBin);
assert.strictEqual(path.resolve(ext.resolveServerBinary()), path.resolve(realBin));
state.wsVal = "";

// 4. Directory named `brat` on PATH is never mistaken for an executable file.
const fakeDir = path.join(tmpDir, "dircheck");
fs.mkdirSync(path.join(fakeDir, "brat"), { recursive: true });
assert.strictEqual(ext.isExecutableFile(path.join(fakeDir, "brat")), false);
assert.strictEqual(ext.isExecutableFile(realBin), true);

// 5. Missing binary returns null instead of `brat.cmd` / `brat` ENOENT.
state.workspaceFolders = [{ uri: { fsPath: fakeDir } }];
const oldPath = process.env.PATH;
process.env.PATH = fakeDir;
assert.strictEqual(ext.resolveServerBinary(), null);
process.env.PATH = oldPath;
"""
        )
    r = subprocess.run([node, harness, ROOT, BIN, TMP], capture_output=True, text=True, timeout=30)
    assert r.returncode == 0, f"node extension harness failed:\n{r.stdout}\n{r.stderr}"


class LspProc:
    def __init__(self):
        self.p = subprocess.Popen(
            [BIN, "lsp"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        self.next_id = 1

    def send(self, payload):
        raw = json.dumps(payload).encode("utf-8")
        hdr = f"Content-Length: {len(raw)}\r\n\r\n".encode("ascii")
        self.p.stdin.write(hdr + raw)
        self.p.stdin.flush()

    def recv(self):
        headers = {}
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise EOFError("LSP stdout closed unexpectedly")
            s = line.decode("ascii", errors="replace").strip()
            if not s:
                break
            if ":" in s:
                k, v = s.split(":", 1)
                headers[k.strip().lower()] = v.strip()
        n = int(headers["content-length"])
        body = b""
        while len(body) < n:
            chunk = self.p.stdout.read(n - len(body))
            if not chunk:
                raise EOFError("truncated LSP frame")
            body += chunk
        return json.loads(body.decode("utf-8"))

    def request(self, method, params):
        rid = self.next_id
        self.next_id += 1
        self.send({"jsonrpc": "2.0", "id": rid, "method": method, "params": params})
        notes = []
        while True:
            msg = self.recv()
            if msg.get("id") == rid:
                return msg, notes
            notes.append(msg)

    def notify_and_wait_diag(self, method, params, target_uri):
        self.send({"jsonrpc": "2.0", "method": method, "params": params})
        while True:
            msg = self.recv()
            if (
                msg.get("method") == "textDocument/publishDiagnostics"
                and msg.get("params", {}).get("uri") == target_uri
            ):
                return msg["params"]["diagnostics"]

    def close(self):
        try:
            self.request("shutdown", {})
            self.send({"jsonrpc": "2.0", "method": "exit", "params": {}})
            self.p.wait(timeout=10)
        finally:
            if self.p.poll() is None:
                self.p.kill()


def path_to_uri(p):
    ap = os.path.abspath(p).replace("\\", "/")
    if not ap.startswith("/"):
        ap = "/" + ap
    return "file://" + ap


def test_vscode_lsp_workflows():
    ws_dir = os.path.join(TMP, "ws")
    os.makedirs(ws_dir, exist_ok=True)
    lsp = LspProc()
    try:
        # 1. Initialize with VS Code's `workspaceFolders` + `prepareSupport: True`.
        init_res, _ = lsp.request(
            "initialize",
            {
                "rootUri": None,
                "workspaceFolders": [{"uri": path_to_uri(ws_dir), "name": "ws"}],
                "capabilities": {
                    "textDocument": {
                        "rename": {"prepareSupport": True},
                    }
                },
            },
        )
        caps = init_res["result"]["capabilities"]
        assert caps["renameProvider"] == {"prepareProvider": True}, caps["renameProvider"]
        lsp.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})

        # 2. Opening a real `.bri` interface file produces 0 bogus parse errors + full Outline symbols!
        bri_path = os.path.join(ws_dir, "shapes.bri")
        bri_src = "pub struct Point { x: i64, y: i64 }\nfn distance(a: Point, b: Point) -> i64\n"
        with open(bri_path, "w", encoding="utf-8") as f:
            f.write(bri_src)
        bri_uri = path_to_uri(bri_path)
        bri_diags = lsp.notify_and_wait_diag(
            "textDocument/didOpen",
            {"textDocument": {"uri": bri_uri, "languageId": "brat", "version": 1, "text": bri_src}},
            bri_uri,
        )
        assert bri_diags == [], f"unexpected diagnostics on valid .bri: {bri_diags}"
        syms_res, _ = lsp.request("textDocument/documentSymbol", {"textDocument": {"uri": bri_uri}})
        sym_names = {s["name"] for s in syms_res["result"]}
        assert {"Point", "distance"}.issubset(sym_names), sym_names

        # Malformed `.bri` reports P0001 on the exact broken line.
        bad_bri_uri = path_to_uri(os.path.join(ws_dir, "broken.bri"))
        bad_diags = lsp.notify_and_wait_diag(
            "textDocument/didOpen",
            {
                "textDocument": {
                    "uri": bad_bri_uri,
                    "languageId": "brat",
                    "version": 1,
                    "text": "fn ok_fn(x: i64) -> i64\nlet not_allowed = 1\n",
                }
            },
            bad_bri_uri,
        )
        assert len(bad_diags) == 1 and bad_diags[0]["code"] == "P0001", bad_diags
        assert bad_diags[0]["range"]["start"]["line"] == 1, bad_diags

        # 3. Untitled VS Code buffer (`untitled:Untitled-1`) resolves `import std.json` + struct completions!
        untitled_uri = "untitled:Untitled-1"
        untitled_src = (
            "import std.json\n"
            "struct Point { x: i64, y: i64 }\n"
            "impl Point {\n"
            "  fn new(x: i64, y: i64) -> Point { return Point { x, y } }\n"
            "  fn norm2(self) -> i64 { return self.x * self.x + self.y * self.y }\n"
            "}\n"
            "fn demo() -> i64 {\n"
            "  let s = json.stringify(123)\n"
            "  let p = Point.new(3, 4)\n"
            "  return p.norm2() + len(s)\n"
            "}\n"
        )
        u_diags = lsp.notify_and_wait_diag(
            "textDocument/didOpen",
            {
                "textDocument": {
                    "uri": untitled_uri,
                    "languageId": "brat",
                    "version": 1,
                    "text": untitled_src,
                }
            },
            untitled_uri,
        )
        assert u_diags == [], f"unexpected diagnostics in untitled buffer: {u_diags}"

        # Dot completion on `p.` inside `return p.norm2()` (line 9, character 11 -> right after `p.`)
        comp_res, _ = lsp.request(
            "textDocument/completion",
            {"textDocument": {"uri": untitled_uri}, "position": {"line": 9, "character": 11}},
        )
        items = comp_res["result"]["items"] if isinstance(comp_res["result"], dict) else comp_res["result"]
        labels = {it["label"] for it in items}
        assert {"x", "y", "norm2"}.issubset(labels), f"missing struct/impl completions: {labels}"

        # Hover on `Point` (line 1, character 8)
        hov_res, _ = lsp.request(
            "textDocument/hover",
            {"textDocument": {"uri": untitled_uri}, "position": {"line": 1, "character": 8}},
        )
        assert hov_res["result"] and "struct Point" in hov_res["result"]["contents"]["value"], hov_res

        # 4. `workspace/didChangeWatchedFiles` refreshes open importers when an on-disk module changes.
        util_path = os.path.join(ws_dir, "mathutil.brat")
        with open(util_path, "w", encoding="utf-8") as f:
            f.write("pub fn calc(x: i64) -> i64 { return x + 1 }\n")
        main_path = os.path.join(ws_dir, "main.brat")
        main_src = "import mathutil\nfn main() -> i64 { return mathutil.calc(5) }\n"
        with open(main_path, "w", encoding="utf-8") as f:
            f.write(main_src)
        main_uri = path_to_uri(main_path)
        m_diags = lsp.notify_and_wait_diag(
            "textDocument/didOpen",
            {"textDocument": {"uri": main_uri, "languageId": "brat", "version": 1, "text": main_src}},
            main_uri,
        )
        assert m_diags == [], m_diags

        # Modify `mathutil.brat` on disk to require 2 arguments and send `workspace/didChangeWatchedFiles`.
        with open(util_path, "w", encoding="utf-8") as f:
            f.write("pub fn calc(x: i64, y: i64) -> i64 { return x + y }\n")
        updated_diags = lsp.notify_and_wait_diag(
            "workspace/didChangeWatchedFiles",
            {"changes": [{"uri": path_to_uri(util_path), "type": 2}]},
            main_uri,
        )
        assert len(updated_diags) >= 1, "expected open importer diagnostics to refresh after disk change"
    finally:
        lsp.close()


test_manifests_and_node_helpers()
test_vscode_lsp_workflows()
print("VSCODE EXTENSION ALL GREEN")
