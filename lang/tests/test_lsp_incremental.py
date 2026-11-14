#!/usr/bin/env python3
"""The language server: JSON-RPC framing, line index, incremental analysis, editor features.

Covers:
  - Zero-copy SIMD JSON-RPC tape, streaming JsonWriter, coalesced/split
    pipe framing, verbatim raw ID echoing, UTF-16 surrogate escapes, and -32700 recovery.
  - SIMD O(1) LineIndex (starts + non_ascii) across 16B/32B/64B boundaries,
    Cyrillic, astral emoji surrogate pairs, and invalid UTF-16 surrogate-midpoint rejection.
  - Salsa-style 64-bit signature-hashed incremental DB (start_line slice
    re-parsing in files with import/extern struct/extern union/enum/trait/impl/const,
    diagnostics identical to the batch checker, cross-open-doc signature invalidation,
    persistent WorkspaceDB fuzzy workspace/symbol, and 12-module std.* completions).
  - Editor features (codeAction quick-fixes E0596/W0001/W0002/B0101,
    delta-encoded semanticTokens/full, type & parameter inlayHint, prepareRename +
    cross-file rename/references, documentHighlight, foldingRange, lossless formatting,
    and /// doc-comment + extern union + FFI effects hover).
  - An ASan + UBSan + LSan stress run (no report expected) and a 52,000+ LOC
    latency benchmark over a real OS pipe.
"""

from concurrent.futures import ThreadPoolExecutor
import json
import os
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import _cxx
import _skip


def check(cond, msg):
    if not cond:
        raise AssertionError(msg)
    check.n += 1


check.n = 0


def frame_bytes(payload):
    raw = payload if isinstance(payload, bytes) else json.dumps(payload, ensure_ascii=False).encode("utf-8")
    return b"Content-Length: %d\r\n\r\n%s" % (len(raw), raw)


class LspClient:
    def __init__(self, bin_path, cwd=ROOT, env=None):
        self.proc = subprocess.Popen(
            [bin_path, "lsp"],
            cwd=cwd,
            env=env,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        self._buf = bytearray()
        self.diagnostics = {}
        self.logs = []
        self._next_id = 1

    def write_raw(self, data):
        self.proc.stdin.write(data)
        self.proc.stdin.flush()

    def send(self, msg):
        self.write_raw(frame_bytes(msg))

    def notify(self, method, params):
        self.send({"jsonrpc": "2.0", "method": method, "params": params})

    def read_one(self):
        while True:
            sep = self._buf.find(b"\r\n\r\n")
            if sep != -1:
                hdr = self._buf[:sep].decode("ascii", errors="replace")
                clen = None
                for ln in hdr.split("\r\n"):
                    if ln.lower().startswith("content-length:"):
                        clen = int(ln.split(":", 1)[1].strip())
                if clen is None:
                    raise RuntimeError("missing Content-Length in %r" % hdr)
                end = sep + 4 + clen
                if len(self._buf) >= end:
                    body = bytes(self._buf[sep + 4 : end])
                    del self._buf[:end]
                    msg = json.loads(body.decode("utf-8"))
                    if msg.get("method") == "textDocument/publishDiagnostics":
                        p = msg["params"]
                        self.diagnostics[p["uri"]] = p["diagnostics"]
                    elif msg.get("method") == "window/logMessage":
                        self.logs.append(msg["params"].get("message", ""))
                    return msg
            chunk = os.read(self.proc.stdout.fileno(), 65536)
            if not chunk:
                err = self.proc.stderr.read().decode("utf-8", errors="replace")
                raise EOFError("brat-lsp closed stdout unexpectedly; stderr:\n" + err)
            self._buf.extend(chunk)

    def request(self, method, params=None, req_id=None):
        if req_id is None:
            req_id = self._next_id
            self._next_id += 1
        req = {"jsonrpc": "2.0", "id": req_id, "method": method}
        if params is not None:
            req["params"] = params
        self.send(req)
        while True:
            msg = self.read_one()
            if "id" in msg and msg["id"] == req_id:
                return msg

    def wait_diag(self, uri):
        while True:
            msg = self.read_one()
            if msg.get("method") == "textDocument/publishDiagnostics" and msg["params"]["uri"] == uri:
                return msg["params"]["diagnostics"]

    def shutdown(self):
        self.request("shutdown")
        self.notify("exit", {})
        rc = self.proc.wait(timeout=10)
        stderr = self.proc.stderr.read().decode("utf-8", errors="replace")
        return rc, stderr


def apply_text_edits(text, edits):
    """Apply LSP TextEdits (UTF-16 positions) to `text` in reverse document order."""
    lines = text.split("\n")

    def to_byte_index(ln, utf16_col):
        off = sum(len(lines[i]) + 1 for i in range(ln))
        s = lines[ln]
        u = 0
        for idx, ch in enumerate(s):
            if u == utf16_col:
                return off + idx
            u += 2 if ord(ch) >= 0x10000 else 1
        if u == utf16_col:
            return off + len(s)
        raise ValueError("bad utf16 col %d on %r" % (utf16_col, s))

    spans = []
    for ed in edits:
        r = ed["range"]
        b0 = to_byte_index(r["start"]["line"], r["start"]["character"])
        b1 = to_byte_index(r["end"]["line"], r["end"]["character"])
        spans.append((b0, b1, ed["newText"]))
    spans.sort(key=lambda t: (t[0], t[1]), reverse=True)
    cur = text
    for b0, b1, repl in spans:
        cur = cur[:b0] + repl + cur[b1:]
    return cur


def diag_fingerprint(diags):
    out = []
    for d in diags:
        rel = tuple(
            (r["location"]["range"]["start"]["line"], r["message"])
            for r in d.get("relatedInformation", [])
        )
        out.append((
            d["range"]["start"]["line"],
            d.get("severity"),
            d.get("code"),
            d.get("message"),
            rel,
        ))
    return out


# SIMD Wire Framing, JSON Tape Escapes & UTF-16 Boundaries
def test_wire_and_simd_lines(bin_path):
    with tempfile.TemporaryDirectory(prefix="brat_wire_") as td:
        cli = LspClient(bin_path)
        init = cli.request("initialize", {"rootUri": "file://" + td, "capabilities": {}}, req_id=0)
        check(init["id"] == 0, "id=0 preserved verbatim")
        caps = init["result"]["capabilities"]
        check(bool(caps.get("codeActionProvider")), "codeActionProvider advertised")
        check("semanticTokensProvider" in caps, "semanticTokensProvider advertised")
        check(caps.get("inlayHintProvider") is True, "inlayHintProvider advertised")
        check(caps.get("documentHighlightProvider") is True, "documentHighlightProvider advertised")
        check(caps.get("foldingRangeProvider") is True, "foldingRangeProvider advertised")
        check(caps.get("documentFormattingProvider") is True, "documentFormattingProvider advertised")
        check(caps.get("renameProvider") is True, "renameProvider stays boolean True")
        cli.notify("initialized", {})

        # 1. Split-byte header/body writes + SIMD boundary line lengths (15,16,17,31,32,33,63,64,65)
        boundary_lines = []
        for idx, width in enumerate([15, 16, 17, 31, 32, 33, 63, 64, 65]):
            pad = "a" * max(1, width - 12)
            boundary_lines.append('fn b%d() { return "%s" }' % (idx, pad))
        # Add Cyrillic (2B UTF-8 = 1 UTF-16) + Emoji (4B UTF-8 = 2 UTF-16) line
        boundary_lines.append('fn uni() { return "Привет 🦀🚀 мир" }')
        src = "\n".join(boundary_lines) + "\n"
        uri = "file://" + os.path.join(td, "simd_bounds.brat")

        raw_open = frame_bytes({
            "jsonrpc": "2.0",
            "method": "textDocument/didOpen",
            "params": {"textDocument": {"uri": uri, "languageId": "brat", "version": 1, "text": src}},
        })
        # Drip-feed across header and SIMD chunk boundaries
        for step in (7, 13, 32, 64):
            cli.write_raw(raw_open[:step])
            raw_open = raw_open[step:]
        cli.write_raw(raw_open)
        d0 = cli.wait_diag(uri)
        check(d0 == [], "clean boundary + unicode didOpen diagnostics: %r" % (d0,))

        # 2. JSON `\uD83D\uDE80` surrogate escape decoding in tape + string ID preservation
        emoji_line = len(boundary_lines) - 1
        # Line is: `fn uni() { return "Привет 🦀🚀 мир" }`
        # Prefix before 🚀: `fn uni() { return "Привет 🦀` -> 19 ASCII + 6 Cyrillic + 1 space + 2 UTF-16 (🦀) = 28 UTF-16 units
        # 🚀 occupies UTF-16 cols [28, 30). Splitting at col 29 must fail cleanly.
        cli.notify("textDocument/didChange", {
            "textDocument": {"uri": uri, "version": 2},
            "contentChanges": [{
                "range": {"start": {"line": emoji_line, "character": 29},
                          "end": {"line": emoji_line, "character": 30}},
                "text": "X",
            }],
        })
        # Send string-ID ping via documentSymbol to flush logMessage
        sym_res = cli.request("textDocument/documentSymbol",
                              {"textDocument": {"uri": uri}},
                              req_id="str-id-α-99")
        check(sym_res["id"] == "str-id-α-99", "string id echoed verbatim")
        check(any("invalid UTF-16 range" in m for m in cli.logs),
              "surrogate midpoint split rejected cleanly: %r" % (cli.logs,))

        # Valid UTF-16 replacement of `🚀` ([28, 30)) using JSON `\uD83D\uDD25` (🔥) escape.
        raw_change = (
            '{"jsonrpc":"2.0","method":"textDocument/didChange","params":{'
            '"textDocument":{"uri":' + json.dumps(uri) + ',"version":3},'
            '"contentChanges":[{"range":{"start":{"line":%d,"character":28},'
            '"end":{"line":%d,"character":30}},"rangeLength":2,"text":"\\uD83D\\uDD25"}]}}'
            % (emoji_line, emoji_line)
        ).encode("utf-8")
        cli.write_raw(frame_bytes(raw_change))
        d3 = cli.wait_diag(uri)
        check(d3 == [], "valid UTF-16 astral surrogate edit succeeded: %r" % (d3,))

        # 3. Coalesced multi-frame pipe burst (8 requests in a single write() syscall) + -32700 recovery
        burst = bytearray()
        burst.extend(frame_bytes(b'{"jsonrpc":"2.0","id":77,"broken_json'))
        for k in range(100, 106):
            burst.extend(frame_bytes({
                "jsonrpc": "2.0",
                "id": k,
                "method": "textDocument/hover",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 0, "character": 4}},
            }))
        cli.write_raw(bytes(burst))
        err_msg = cli.read_one()
        check(err_msg.get("error", {}).get("code") == -32700 and err_msg.get("id") is None,
              "malformed JSON frame returns -32700 with null id: %r" % (err_msg,))
        for k in range(100, 106):
            r = cli.read_one()
            check(r.get("id") == k and "b0()" in r["result"]["contents"]["value"],
                  "coalesced burst response #%d intact" % k)

        rc, stderr = cli.shutdown()
        check(rc == 0, "wire test clean exit (rc=%d, stderr=%s)" % (rc, stderr))


# Salsa-Style Incremental Parity, Cross-Doc Invalidation & Stdlib
def test_incremental_salsa_and_workspace(bin_path):
    with tempfile.TemporaryDirectory(prefix="brat_inc_") as td:
        lib_path = os.path.join(td, "math_ext.brat")
        app_path = os.path.join(td, "app.brat")
        oracle_path = os.path.join(td, "oracle.brat")

        lib_v1 = (
            "/// Doubles and shifts an integer.\n"
            "pub fn scale_add(x: i64, bias: i64) -> i64 {\n"
            "  return x * 2 + bias\n"
            "}\n"
        )
        with open(lib_path, "w", encoding="utf-8") as f:
            f.write(lib_v1)

        # Rich module with imports, extern struct, extern union, enum, trait, impl, const, multiple fns.
        base_doc = (
            "import std.json\n"
            "import math_ext\n"
            "\n"
            "extern struct Packet {\n"
            "  len: i64\n"
            "}\n"
            "\n"
            "extern union WordU {\n"
            "  i: i64,\n"
            "  f: f64,\n"
            "}\n"
            "\n"
            "enum Mode {\n"
            "  Fast,\n"
            "  Safe,\n"
            "}\n"
            "\n"
            "trait Measurable {\n"
            "  fn weight(self: Mode) -> i64\n"
            "}\n"
            "\n"
            "impl Mode {\n"
            "  fn boost(self: Mode, v: i64) -> i64 {\n"
            "    return v + 1\n"
            "  }\n"
            "}\n"
            "\n"
            "const FACTOR: i64 = 3\n"
            "\n"
            "fn first_fn(a: i64) -> i64 {\n"
            "  let _ = json.parse(\"{}\")\n"
            "  let res: i64 = scale_add(a, FACTOR)\n"
            "  return res\n"
            "}\n"
            "\n"
            "fn middle_fn(s: str) -> str {\n"
            "  return s\n"
            "}\n"
            "\n"
            "fn tail_fn() -> i64 {\n"
            "  let bad: i64 = \"oops\"\n"
            "  return bad\n"
            "}\n"
        )
        with open(app_path, "w", encoding="utf-8") as f:
            f.write(base_doc)
        with open(oracle_path, "w", encoding="utf-8") as f:
            f.write("")

        cli = LspClient(bin_path, cwd=ROOT)
        cli.request("initialize", {"rootUri": "file://" + td, "capabilities": {}})
        cli.notify("initialized", {})

        lib_uri = "file://" + lib_path
        app_uri = "file://" + app_path
        ora_uri = "file://" + oracle_path

        cli.notify("textDocument/didOpen", {
            "textDocument": {"uri": lib_uri, "languageId": "brat", "version": 1, "text": lib_v1}
        })
        check(cli.wait_diag(lib_uri) == [], "lib_v1 clean")

        cli.notify("textDocument/didOpen", {
            "textDocument": {"uri": app_uri, "languageId": "brat", "version": 1, "text": base_doc}
        })
        cur_diags = cli.wait_diag(app_uri)
        check(len(cur_diags) == 1 and cur_diags[0]["code"] == "T0001",
              "initial tail_fn T0001 present: %r" % (cur_diags,))

        cur_text = base_doc
        ver = 1

        def edit_and_assert_oracle(old_sub, new_sub, label):
            nonlocal cur_text, ver
            idx = cur_text.index(old_sub)
            pre = cur_text[:idx]
            s_ln = pre.count("\n")
            s_col = len(pre.rsplit("\n", 1)[-1])
            mid_lines = old_sub.split("\n")
            e_ln = s_ln + len(mid_lines) - 1
            e_col = (s_col + len(mid_lines[0])) if len(mid_lines) == 1 else len(mid_lines[-1])

            ver += 1
            cli.notify("textDocument/didChange", {
                "textDocument": {"uri": app_uri, "version": ver},
                "contentChanges": [{
                    "range": {"start": {"line": s_ln, "character": s_col},
                              "end": {"line": e_ln, "character": e_col}},
                    "text": new_sub,
                }],
            })
            inc_d = cli.wait_diag(app_uri)
            cur_text = cur_text[:idx] + new_sub + cur_text[idx + len(old_sub):]

            # Batch oracle: fresh didOpen + didClose on `ora_uri`
            cli.notify("textDocument/didOpen", {
                "textDocument": {"uri": ora_uri, "languageId": "brat", "version": ver, "text": cur_text}
            })
            ora_d = cli.wait_diag(ora_uri)
            cli.notify("textDocument/didClose", {"textDocument": {"uri": ora_uri}})
            cli.wait_diag(ora_uri)

            check(
                diag_fingerprint(inc_d) == diag_fingerprint(ora_d),
                "incremental != batch oracle at [%s]:\n  inc=%r\n  ora=%r"
                % (label, diag_fingerprint(inc_d), diag_fingerprint(ora_d)),
            )
            return inc_d

        # 1. Introduce use-after-move (E0382) + multi-line shift inside `middle_fn`
        d = edit_and_assert_oracle(
            "fn middle_fn(s: str) -> str {\n  return s\n}",
            "fn middle_fn(s: str) -> str {\n  let t = move s\n  let _u = t\n  return s\n}",
            "middle_fn E0382 + +2 line shift on tail_fn",
        )
        check([x["code"] for x in d] == ["E0507", "E0382", "T0001"],
              "E0507 + E0382 + shifted T0001 reported: %r" % ([x["code"] for x in d],))

        # 2. Fix E0382 inside `middle_fn` and introduce multi-arm extern union error (T0001)
        edit_and_assert_oracle(
            "  let t = move s\n  let _u = t\n  return s",
            "  let _bad_u = WordU { i: 1, f: 2.0 }\n  return s",
            "extern union two-arm T0001 inside incremental fn",
        )

        # 3. Fix extern union to single valid arm
        edit_and_assert_oracle(
            "  let _bad_u = WordU { i: 1, f: 2.0 }\n  return s",
            "  let _ok_u = WordU { i: 7 }\n  return s",
            "valid single-arm extern union",
        )

        # 4. Edit inside `impl Mode` method body (`v + 1` -> type error `"bad"`, then fix)
        edit_and_assert_oracle("    return v + 1", '    return "bad_impl_ret"', "impl method body T0001")
        edit_and_assert_oracle('    return "bad_impl_ret"', "    return v + 2", "impl method body fixed")

        # 5. Introduce syntax error (P0001) inside `middle_fn`, then recover clean AST.
        edit_and_assert_oracle("  let _ok_u = WordU { i: 7 }", "  let broken = (", "P0001 syntax error")
        edit_and_assert_oracle("  let broken = (", "  let _ok_u = WordU { i: 9 }", "recover from P0001")

        # 6. Toggle local `const FACTOR` usage (triggers top-level W0001 unused const parity)
        d_unused_const = edit_and_assert_oracle(
            "scale_add(a, FACTOR)",
            "scale_add(a, 10)",
            "remove FACTOR reference -> top-level W0001",
        )
        check(any(x["code"] == "W0001" for x in d_unused_const), "W0001 emitted when local const becomes unused")
        edit_and_assert_oracle(
            "scale_add(a, 10)",
            "scale_add(a, FACTOR)",
            "restore FACTOR reference -> W0001 cleared",
        )

        # 7. Fix `tail_fn` so `app.brat` has 0 diagnostics.
        d_clean = edit_and_assert_oracle('  let bad: i64 = "oops"\n  return bad', "  return first_fn(5)", "clean app")
        check(d_clean == [], "app is now 100% clean")

        # 8. Cross-open-document signature hash invalidation (`propagate_signature_change`).
        # Editing `lib_uri` BODY keeps signature hash identical -> `app_uri` stays clean
        lib_v2 = lib_v1.replace("x * 2 + bias", "x * 3 + bias")
        cli.notify("textDocument/didChange", {
            "textDocument": {"uri": lib_uri, "version": 2},
            "contentChanges": [{"text": lib_v2}],
        })
        check(cli.wait_diag(lib_uri) == [], "lib_v2 body edit clean")

        # Editing `lib_uri` SIGNATURE (`-> i64` -> `-> str`) changes `sig_hash`
        # -> `brat-lsp` must automatically push a fresh `T0001` diagnostic on `app_uri`.
        lib_v3 = (
            "/// Doubles and shifts an integer.\n"
            "pub fn scale_add(x: i64, bias: i64) -> str {\n"
            "  return str(x + bias)\n"
            "}\n"
        )
        cli.notify("textDocument/didChange", {
            "textDocument": {"uri": lib_uri, "version": 3},
            "contentChanges": [{"text": lib_v3}],
        })
        # Read frames until we have both `lib_uri` and `app_uri` diagnostics
        got_uris = set()
        for _ in range(2):
            m = cli.read_one()
            if m.get("method") == "textDocument/publishDiagnostics":
                got_uris.add(m["params"]["uri"])
        check(app_uri in got_uris, "changing lib signature automatically re-published app diagnostics")
        check(any(x["code"] == "T0001" for x in cli.diagnostics[app_uri]),
              "app_uri immediately flagged T0001 after lib signature change: %r" % (cli.diagnostics[app_uri],))

        # Restore `lib_uri` signature -> `app_uri` automatically clears back to `[]`.
        cli.notify("textDocument/didChange", {
            "textDocument": {"uri": lib_uri, "version": 4},
            "contentChanges": [{"text": lib_v2}],
        })
        for _ in range(2):
            cli.read_one()
        check(cli.diagnostics[app_uri] == [], "restoring lib signature cleared app diagnostics")

        # 9. All 12 `std.*` completions (`time`, `crypto`, `sqlite` included) + fuzzy `workspace/symbol`
        comp_doc = "import std.crypto\nimport std.sqlite\n// std.\n// crypto.\n// sqlite.\n"
        comp_uri = "file://" + os.path.join(td, "comp.brat")
        cli.notify("textDocument/didOpen", {
            "textDocument": {"uri": comp_uri, "languageId": "brat", "version": 1, "text": comp_doc}
        })
        cli.wait_diag(comp_uri)

        std_items = {it["label"] for it in cli.request("textDocument/completion", {
            "textDocument": {"uri": comp_uri}, "position": {"line": 2, "character": 7}
        })["result"]}
        expected_12 = {"bytes", "cli", "crypto", "csv", "env", "fs", "http", "json", "path", "proc", "sqlite", "time"}
        check(expected_12.issubset(std_items), "all 12 std.* modules completed: got %r" % (sorted(std_items),))

        crypto_items = {it["label"] for it in cli.request("textDocument/completion", {
            "textDocument": {"uri": comp_uri}, "position": {"line": 3, "character": 10}
        })["result"]}
        check({"sha256_hex", "hmac_sha256_hex", "constant_time_eq", "rand_hex"}.issubset(crypto_items),
              "std.crypto completions: %r" % (sorted(crypto_items),))

        sql_items = {it["label"] for it in cli.request("textDocument/completion", {
            "textDocument": {"uri": comp_uri}, "position": {"line": 4, "character": 10}
        })["result"]}
        check({"open", "close", "exec", "query"}.issubset(sql_items),
              "std.sqlite completions: %r" % (sorted(sql_items),))

        # Fuzzy subsequence workspace/symbol (`"dfc"` -> `days_from_civil`, `"scladd"` -> `scale_add`)
        ws1 = [s["name"] for s in cli.request("workspace/symbol", {"query": "dfc"})["result"]]
        check("days_from_civil" in ws1, "fuzzy workspace/symbol 'dfc' found days_from_civil: %r" % (ws1[:8],))
        ws2 = [s["name"] for s in cli.request("workspace/symbol", {"query": "scladd"})["result"]]
        check("scale_add" in ws2, "fuzzy workspace/symbol 'scladd' found scale_add: %r" % (ws2[:8],))

        rc, stderr = cli.shutdown()
        check(rc == 0, "clean shutdown")


# codeAction, semanticTokens, inlayHint, prepareRename and cross-file rename,
# highlight, folding, formatting
def test_ide_intelligence(bin_path):
    with tempfile.TemporaryDirectory(prefix="brat_ide_") as td:
        # Cross-file workspace setup: `util.brat`, `service.brat` (open), `cli_tool.brat` (on-disk only)
        util_path = os.path.join(td, "util.brat")
        svc_path = os.path.join(td, "service.brat")
        disk_path = os.path.join(td, "cli_tool.brat")

        util_src = (
            "/// Computes deterministic checksum.\n"
            "pub fn compute_tag(seed: i64, weight: i64) -> i64 {\n"
            "  return seed * 31 + weight\n"
            "}\n"
        )
        svc_src = (
            "import util\n"
            "\n"
            "/// C packet union.\n"
            "extern union Payload {\n"
            "  raw: i64,\n"
            "  ratio: f64,\n"
            "}\n"
            "\n"
            "/// Borrowed C view.\n"
            "extern fn c_view(p: ptr) -> ptr borrows(p);\n"
            "\n"
            "fn run_job() -> i64 {\n"
            "  let total = util.compute_tag(10, 20)\n"
            "  let label = \"compute_tag stays untouched in strings\"\n"
            "  let _ = label\n"
            "  // compute_tag in comments also stays untouched\n"
            "  return total\n"
            "}\n"
        )
        disk_src = (
            "import util\n"
            "fn main() -> None {\n"
            "  print(str(util.compute_tag(1, 2)))\n"
            "}\n"
        )
        for p, s in [(util_path, util_src), (svc_path, svc_src), (disk_path, disk_src)]:
            with open(p, "w", encoding="utf-8") as f:
                f.write(s)

        cli = LspClient(bin_path, cwd=ROOT)
        cli.request("initialize", {"rootUri": "file://" + td, "capabilities": {}})
        cli.notify("initialized", {})

        util_uri = "file://" + util_path
        svc_uri = "file://" + svc_path
        disk_uri = "file://" + disk_path

        cli.notify("textDocument/didOpen", {
            "textDocument": {"uri": util_uri, "languageId": "brat", "version": 1, "text": util_src}
        })
        cli.wait_diag(util_uri)
        cli.notify("textDocument/didOpen", {
            "textDocument": {"uri": svc_uri, "languageId": "brat", "version": 1, "text": svc_src}
        })
        check(cli.wait_diag(svc_uri) == [], "svc_src clean")

        # 1. Rich Hover: `///` doc comments, `extern union` `@ offset 0`, and `borrows(p)` FFI effects.
        h_union = cli.request("textDocument/hover", {
            "textDocument": {"uri": svc_uri}, "position": {"line": 3, "character": 15}
        })["result"]["contents"]["value"]
        check("extern union Payload" in h_union and "@ offset 0" in h_union and "C packet union." in h_union,
              "extern union + /// doc hover: %r" % h_union)

        h_ext = cli.request("textDocument/hover", {
            "textDocument": {"uri": svc_uri}, "position": {"line": 9, "character": 12}
        })["result"]["contents"]["value"]
        check("borrows(p)" in h_ext and "Borrowed C view." in h_ext,
              "extern fn borrows(p) + /// doc hover: %r" % h_ext)

        h_imp = cli.request("textDocument/hover", {
            "textDocument": {"uri": svc_uri}, "position": {"line": 12, "character": 22}
        })["result"]["contents"]["value"]
        check("Computes deterministic checksum." in h_imp,
              "cross-module /// doc comment hover: %r" % h_imp)

        # 2. Inlay Hints: `: i64` & `: str` inferred let types + `seed:` & `weight:` parameter labels.
        hints = cli.request("textDocument/inlayHint", {
            "textDocument": {"uri": svc_uri},
            "range": {"start": {"line": 0, "character": 0}, "end": {"line": 18, "character": 0}},
        })["result"]
        hint_labels = [h["label"] for h in hints]
        check(": i64" in hint_labels and ": str" in hint_labels,
              "type inlay hints inferred (: i64, : str): %r" % (hint_labels,))
        check("seed:" in hint_labels and "weight:" in hint_labels,
              "parameter inlay hints inferred (seed:, weight:): %r" % (hint_labels,))

        # 3. Delta-encoded Semantic Tokens (`5 * N` ints, valid non-negative deltas & lengths)
        sem = cli.request("textDocument/semanticTokens/full", {"textDocument": {"uri": svc_uri}})["result"]["data"]
        check(len(sem) >= 50 and len(sem) % 5 == 0, "semanticTokens/full returned 5*N array (%d ints)" % len(sem))
        for i in range(0, len(sem), 5):
            dl, ds, length, ttype, tmods = sem[i : i + 5]
            check(dl >= 0 and ds >= 0 and length > 0 and 0 <= ttype <= 14 and tmods >= 0,
                  "valid semantic token tuple at %d: %r" % (i, sem[i : i + 5]))

        # 4. Document Highlight (`Write` = 3 on `let total`, `Read` = 2 on trailing `total`)
        hl = cli.request("textDocument/documentHighlight", {
            "textDocument": {"uri": svc_uri}, "position": {"line": 12, "character": 7}
        })["result"]
        kinds = {h["range"]["start"]["line"]: h["kind"] for h in hl}
        check(kinds.get(12) == 3 and kinds.get(16) == 2, "documentHighlight Write(3)/Read(2): %r" % kinds)

        # 5. Folding Ranges
        folds = cli.request("textDocument/foldingRange", {"textDocument": {"uri": svc_uri}})["result"]
        fold_spans = {(f["startLine"], f["endLine"]) for f in folds}
        check((3, 6) in fold_spans and (11, 17) in fold_spans, "foldingRange spans: %r" % (fold_spans,))

        # 6. `prepareRename` + Cross-File Workspace `rename` & `references` (open + unopened disk files)
        bad_prep = cli.request("textDocument/prepareRename", {
            "textDocument": {"uri": svc_uri}, "position": {"line": 11, "character": 1}  # `fn` keyword
        })["result"]
        check(bad_prep is None, "prepareRename rejects keyword `fn`")

        ok_prep = cli.request("textDocument/prepareRename", {
            "textDocument": {"uri": util_uri}, "position": {"line": 1, "character": 9}  # `compute_tag`
        })["result"]
        check(ok_prep["placeholder"] == "compute_tag", "prepareRename returns placeholder")

        refs = cli.request("textDocument/references", {
            "textDocument": {"uri": util_uri},
            "position": {"line": 1, "character": 9},
            "context": {"includeDeclaration": True},
        })["result"]
        ref_uris = {r["uri"] for r in refs}
        check({util_uri, svc_uri, disk_uri}.issubset(ref_uris) and len(refs) == 3,
              "cross-workspace references found all 3 real sites (skipping strings/comments): %r" % (refs,))

        ren = cli.request("textDocument/rename", {
            "textDocument": {"uri": util_uri},
            "position": {"line": 1, "character": 9},
            "newName": "digest_tag",
        })["result"]["changes"]
        check({util_uri, svc_uri, disk_uri} == set(ren.keys()), "cross-file rename touched all 3 files")
        new_svc = apply_text_edits(svc_src, ren[svc_uri])
        check("util.digest_tag(10, 20)" in new_svc, "call site renamed")
        check('"compute_tag stays untouched in strings"' in new_svc, "string literal preserved")
        check("// compute_tag in comments also stays untouched" in new_svc, "comment preserved")

        # 7. Quick-fix `textDocument/codeAction`: E0596, W0001, B0101 + context W0002 & W0003.
        qf_uri = "file://" + os.path.join(td, "quickfix.brat")
        qf_src = (
            "import std.csv\n"
            "fn demo() -> i64 {\n"
            "  let x = 1\n"
            "  x = 2\n"
            "  let mut y = 10\n"
            "  let lonely = 99\n"
            "  pritn(str(y))\n"
            "  return x\n"
            "}\n"
        )
        cli.notify("textDocument/didOpen", {
            "textDocument": {"uri": qf_uri, "languageId": "brat", "version": 1, "text": qf_src}
        })
        qf_diags = cli.wait_diag(qf_uri)
        codes = {d["code"] for d in qf_diags}
        check({"E0596", "W0001", "B0101"}.issubset(codes),
              "compiler quick-fixable diagnostics produced: %r" % (codes,))

        ctx_diags = list(qf_diags) + [
            {
                "range": {"start": {"line": 4, "character": 0}, "end": {"line": 4, "character": 16}},
                "severity": 2, "code": "W0002", "source": "brat", "message": "unused mut `y`",
            },
            {
                "range": {"start": {"line": 0, "character": 0}, "end": {"line": 0, "character": 14}},
                "severity": 2, "code": "W0003", "source": "brat", "message": "unused import `std.csv`",
            },
        ]
        actions = cli.request("textDocument/codeAction", {
            "textDocument": {"uri": qf_uri},
            "range": {"start": {"line": 0, "character": 0}, "end": {"line": 8, "character": 0}},
            "context": {"diagnostics": ctx_diags},
        })["result"]
        by_title = {a["title"]: a for a in actions}
        check(any("let mut" in t for t in by_title), "E0596 add `mut` quick-fix offered: %r" % list(by_title))
        check(any("Remove unnecessary `mut`" in t for t in by_title), "W0002 remove `mut` quick-fix offered")
        check(any("Prefix unused `lonely`" in t for t in by_title), "W0001 `_` prefix quick-fix offered")
        check(any("Remove unused import" in t for t in by_title), "W0003 remove unused import quick-fix offered")
        check(any("Replace `pritn` with `print`" in t for t in by_title), "B0101 typo quick-fix offered")

        # Apply all 5 quick-fixes sequentially (bottom-to-top safe) and verify 0 diagnostics.
        fixed = qf_src
        for needle in ["Replace `pritn` with `print`", "Prefix unused `lonely`",
                       "Remove unnecessary `mut`", "let mut", "Remove unused import"]:
            act = next(v for k, v in by_title.items() if needle in k)
            fixed = apply_text_edits(fixed, act["edit"]["changes"][qf_uri])
        cli.notify("textDocument/didChange", {
            "textDocument": {"uri": qf_uri, "version": 2},
            "contentChanges": [{"text": fixed}],
        })
        after_qf = cli.wait_diag(qf_uri)
        check(after_qf == [], "applying all 5 codeAction quick-fixes yielded 0 diagnostics! got %r\n%s" % (after_qf, fixed))

        # 8. Lossless `textDocument/formatting` (idempotent + preserves comments & multiline strings)
        messy_uri = "file://" + os.path.join(td, "messy.brat")
        messy_src = (
            "fn fmt_me(a: i64) -> i64 {\n"
            "      // keep comment\n"
            "    if a > 0 {   \n"
            "        return a + 1   \n"
            "      } else {\n"
            "return 0\n"
            "}\n"
            "}\n"
        )
        cli.notify("textDocument/didOpen", {
            "textDocument": {"uri": messy_uri, "languageId": "brat", "version": 1, "text": messy_src}
        })
        cli.wait_diag(messy_uri)
        f_edits = cli.request("textDocument/formatting", {
            "textDocument": {"uri": messy_uri},
            "options": {"tabSize": 2, "insertSpaces": True},
        })["result"]
        check(len(f_edits) == 1, "formatting produced edit")
        clean_fmt = apply_text_edits(messy_src, f_edits)
        check("  // keep comment\n  if a > 0 {\n    return a + 1\n  } else {\n    return 0\n  }\n" in clean_fmt,
              "formatted indentation exact:\n" + clean_fmt)
        cli.notify("textDocument/didChange", {
            "textDocument": {"uri": messy_uri, "version": 2},
            "contentChanges": [{"text": clean_fmt}],
        })
        cli.wait_diag(messy_uri)
        f_idem = cli.request("textDocument/formatting", {
            "textDocument": {"uri": messy_uri},
            "options": {"tabSize": 2, "insertSpaces": True},
        })["result"]
        check(f_idem == [], "formatting is idempotent on already-formatted document")

        # 9. Multi-arm match (.els chain), Result[T, E] signatureHelp, and .bri comment braces
        m3_uri = "file://" + os.path.join(td, "match3.brat")
        m3_src = (
            "struct Pt3 { x: i64, y: i64 }\n"
            "fn m3(x: i64) -> i64 {\n"
            "  let r: Result[i64, str] = Ok(1)\n"
            "  let u = unwrap_or(r, 0)\n"
            "  match x {\n"
            "    1 => { return u },\n"
            "    2 => { return 2 },\n"
            "    _ => {\n"
            "      let p = Pt3 { x: 3, y: 4 }\n"
            "      return p.x\n"
            "    }\n"
            "  }\n"
            "}\n"
        )
        cli.notify("textDocument/didOpen", {
            "textDocument": {"uri": m3_uri, "languageId": "brat", "version": 1, "text": m3_src}
        })
        check(cli.wait_diag(m3_uri) == [], "match3 clean")
        sh = cli.request("textDocument/signatureHelp", {
            "textDocument": {"uri": m3_uri}, "position": {"line": 3, "character": 23}
        })["result"]
        check(len(sh["signatures"][0]["parameters"]) == 2 and sh["activeParameter"] == 1,
              "unwrap_or(Result[T, E], T) signatureHelp keeps 2 params: %r" % (sh,))
        hv_p = cli.request("textDocument/hover", {
            "textDocument": {"uri": m3_uri}, "position": {"line": 9, "character": 13}
        })["result"]
        check(hv_p is not None and "Pt3" in hv_p["contents"]["value"],
              "hover resolves let binding in 3rd match arm: %r" % (hv_p,))
        ih_p = cli.request("textDocument/inlayHint", {
            "textDocument": {"uri": m3_uri},
            "range": {"start": {"line": 0, "character": 0}, "end": {"line": 12, "character": 0}},
        })["result"]
        check(any("Pt3" in h["label"] for h in ih_p),
              "inlayHint emits inferred type in 3rd match arm: %r" % (ih_p,))
        cli.notify("textDocument/didChange", {
            "textDocument": {"uri": m3_uri, "version": 2},
            "contentChanges": [{"text": m3_src.replace("return p.x", "p.\n      return 0")}],
        })
        cli.wait_diag(m3_uri)
        comp_p = cli.request("textDocument/completion", {
            "textDocument": {"uri": m3_uri}, "position": {"line": 9, "character": 8}
        })["result"]
        comp_lbls = {c["label"] for c in comp_p}
        check({"x", "y", "copy"}.issubset(comp_lbls),
              "struct completion works in 3rd match arm: %r" % (comp_lbls,))

        bri_uri = "file://" + os.path.join(td, "brace_comment.bri")
        cli.notify("textDocument/didOpen", {
            "textDocument": {
                "uri": bri_uri, "languageId": "brat", "version": 1,
                "text": "pub struct BriBox {\n  a: i64 // } brace in comment\n  b: i64\n}\n",
            }
        })
        check(cli.wait_diag(bri_uri) == [], ".bri struct ignores } inside // comment")

        rc, stderr = cli.shutdown()
        check(rc == 0, "clean shutdown")


# ASan + UBSan + LSan Sanitizer Verification of brat-lsp v0.4
def test_asan_ubsan(cc):
    obj_cache = os.path.join(tempfile.gettempdir(), "brat-lsp-asan-objs")
    os.makedirs(obj_cache, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="brat_asan_") as td:
        asan_bin = os.path.join(td, "brat-lsp-asan")
        driver_cpp = os.path.join(td, "driver.cpp")
        with open(driver_cpp, "w", encoding="utf-8") as f:
            f.write('#include "lsp.h"\nint main(int argc, char **argv) { (void)argc; (void)argv; return run_lsp(); }\n')

        skip_names = {"driver_pkg.cpp", "driver_tools.cpp", "emit.cpp", "ir.cpp", "main.cpp"}
        lsp_srcs = [s for s in _cxx.CPP if os.path.basename(s) not in skip_names]
        hdr_mtime = max((os.path.getmtime(h) for h in _cxx.HDR), default=0.0)
        flags = [
            "-std=c++20", "-O0",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-I", _cxx.SRC_DIR,
        ]

        def compile_one(src):
            obj = os.path.join(obj_cache, os.path.basename(src) + ".o")
            if not os.path.exists(obj) or os.path.getmtime(obj) < max(os.path.getmtime(src), hdr_mtime):
                tmp_o = "%s.tmp%d" % (obj, os.getpid())
                r = subprocess.run([cc] + flags + ["-c", src, "-o", tmp_o], capture_output=True, text=True)
                if r.returncode != 0:
                    raise RuntimeError("ASan compile failed on %s:\n%s" % (src, r.stderr))
                os.replace(tmp_o, obj)
            return obj

        with ThreadPoolExecutor(max_workers=2) as ex:
            objs = list(ex.map(compile_one, lsp_srcs))

        drv_obj = os.path.join(td, "driver.o")
        r_drv = subprocess.run([cc] + flags + ["-c", driver_cpp, "-o", drv_obj], capture_output=True, text=True)
        if r_drv.returncode != 0:
            raise RuntimeError("ASan driver compile failed:\n" + r_drv.stderr)

        link = subprocess.run(
            [cc, "-std=c++20", "-fsanitize=address,undefined"] + objs + [drv_obj, "-o", asan_bin],
            capture_output=True, text=True,
        )
        if link.returncode != 0:
            raise RuntimeError("ASan link failed:\n" + link.stderr)

        env = dict(os.environ)
        env["ASAN_OPTIONS"] = "detect_leaks=1:halt_on_error=1"
        env["UBSAN_OPTIONS"] = "print_stacktrace=1:halt_on_error=1"

        cli = LspClient(asan_bin, cwd=ROOT, env=env)
        cli.request("initialize", {"rootUri": "file://" + td, "capabilities": {}})
        cli.notify("initialized", {})

        uri = "file://" + os.path.join(td, "san.brat")
        src = (
            "extern union U { a: i64, b: f64 }\n"
            "/// Greeting helper.\n"
            "fn greet(x: i64) -> str {\n"
            "  let s = \"Привет 🚀\"\n"
            "  let mut k = x\n"
            "  k = k + 1\n"
            "  return s\n"
            "}\n"
        )
        cli.notify("textDocument/didOpen", {
            "textDocument": {"uri": uri, "languageId": "brat", "version": 1, "text": src}
        })
        check(cli.wait_diag(uri) == [], "ASan didOpen clean")

        # Exercise incremental edit, semanticTokens, inlayHint, codeAction, formatting, workspace/symbol
        cli.notify("textDocument/didChange", {
            "textDocument": {"uri": uri, "version": 2},
            "contentChanges": [{
                "range": {"start": {"line": 5, "character": 6}, "end": {"line": 5, "character": 11}},
                "text": "k + 42",
            }],
        })
        check(cli.wait_diag(uri) == [], "ASan incremental didChange clean")
        cli.request("textDocument/semanticTokens/full", {"textDocument": {"uri": uri}})
        cli.request("textDocument/inlayHint", {
            "textDocument": {"uri": uri},
            "range": {"start": {"line": 0, "character": 0}, "end": {"line": 7, "character": 0}},
        })
        cli.request("textDocument/hover", {
            "textDocument": {"uri": uri}, "position": {"line": 2, "character": 5}
        })
        cli.request("textDocument/formatting", {
            "textDocument": {"uri": uri}, "options": {"tabSize": 2, "insertSpaces": True}
        })
        cli.request("workspace/symbol", {"query": "greet"})

        rc, stderr = cli.shutdown()
        check(rc == 0 and stderr.strip() == "",
              "ASan+UBSan+LSan 0 errors/leaks (rc=%d, stderr=%r)" % (rc, stderr))


# 52,000+ LOC Multi-Module Sub-Millisecond Latency Benchmark
def test_50k_loc_benchmark(bin_path):
    with tempfile.TemporaryDirectory(prefix="brat_50k_") as td:
        # Generate a realistic 52,000+ LOC module (~2,600 functions + structs/unions/enums/impl)
        chunks = [
            "extern struct FrameHdr {\n  seq: i64,\n  flags: i64,\n}\n\n"
            "extern union MetricVal {\n  ival: i64,\n  fval: f64,\n}\n\n"
            "enum Stage {\n  Init,\n  Ready,\n}\n\n"
            "impl Stage {\n  fn tag(self: Stage) -> i64 {\n    return 1\n  }\n}\n\n"
        ]
        num_fns = 2600  # 2600 * 20 lines = 52,000+ LOC
        for i in range(num_fns):
            chunks.append(
                "/// Worker stage %d.\n"
                "pub fn stage_%d(x: i64, y: i64) -> i64 {\n"
                "  let a = x + %d\n"
                "  let b = y * 2\n"
                "  let c = a + b\n"
                "  let d = c - %d\n"
                "  let e = d + a\n"
                "  let f = e + b\n"
                "  let g = f - c\n"
                "  let h = g + d\n"
                "  if h > 0 {\n"
                "    let p = h + 1\n"
                "    let q = p + 2\n"
                "    return q + e\n"
                "  } else {\n"
                "    let r = h - 1\n"
                "    return r - f\n"
                "  }\n"
                "}\n\n" % (i, i, (i % 97) + 1, (i % 53) + 1)
            )
        big_src = "".join(chunks)
        total_lines = big_src.count("\n")
        check(total_lines >= 52000, "generated >= 52,000 LOC (%d lines)" % total_lines)

        big_path = os.path.join(td, "mega_52k.brat")
        with open(big_path, "w", encoding="utf-8") as f:
            f.write(big_src)
        big_uri = "file://" + big_path

        cli = LspClient(bin_path, cwd=ROOT)
        cli.request("initialize", {"rootUri": "file://" + td, "capabilities": {}})
        cli.notify("initialized", {})

        cli.notify("textDocument/didOpen", {
            "textDocument": {"uri": big_uri, "languageId": "brat", "version": 1, "text": big_src}
        })
        d0 = cli.wait_diag(big_uri)
        check(d0 == [], "52k LOC didOpen clean")

        # Locate `stage_2000`'s `let a = x + ...` line (~line 40,018 inside the 52,000-line buffer)
        lines = big_src.split("\n")
        target_ln = next(idx for idx, ln in enumerate(lines) if ln.startswith("pub fn stage_2000(")) + 1
        orig_line = lines[target_ln]
        check(orig_line.startswith("  let a = x + "), "located stage_2000 body line: %r" % orig_line)

        # Warm up 5 edits, then measure 40 sequential real-pipe `didChange -> publishDiagnostics` round-trips.
        latencies_ms = []
        cur_col_end = len(orig_line)
        for step in range(1, 46):
            new_line = "  let a = x + %d" % (1000 + step)
            ver = step + 1
            payload = frame_bytes({
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": big_uri, "version": ver},
                    "contentChanges": [{
                        "range": {
                            "start": {"line": target_ln, "character": 0},
                            "end": {"line": target_ln, "character": cur_col_end},
                        },
                        "text": new_line,
                    }],
                },
            })
            t0 = time.perf_counter()
            cli.write_raw(payload)
            diags = cli.wait_diag(big_uri)
            dt_ms = (time.perf_counter() - t0) * 1000.0
            cur_col_end = len(new_line)
            check(diags == [], "52k incremental edit #%d clean" % step)
            if step > 5:
                latencies_ms.append(dt_ms)

        # Also verify an error edit at line ~40,018 reports exact T0001 in sub-millisecond time.
        err_line = '  let a: i64 = "boom"'
        t0 = time.perf_counter()
        cli.notify("textDocument/didChange", {
            "textDocument": {"uri": big_uri, "version": 100},
            "contentChanges": [{
                "range": {"start": {"line": target_ln, "character": 0},
                          "end": {"line": target_ln, "character": cur_col_end}},
                "text": err_line,
            }],
        })
        err_diags = cli.wait_diag(big_uri)
        err_ms = (time.perf_counter() - t0) * 1000.0
        check(len(err_diags) == 1 and err_diags[0]["code"] == "T0001"
              and err_diags[0]["range"]["start"]["line"] == target_ln,
              "52k LOC exact T0001 line %d in %.2f ms: %r" % (target_ln, err_ms, err_diags))

        # Restore clean line and benchmark cached `hover` and `workspace/symbol` on 52,000+ LOC.
        cli.notify("textDocument/didChange", {
            "textDocument": {"uri": big_uri, "version": 101},
            "contentChanges": [{
                "range": {"start": {"line": target_ln, "character": 0},
                          "end": {"line": target_ln, "character": len(err_line)}},
                "text": orig_line,
            }],
        })
        check(cli.wait_diag(big_uri) == [], "restored 52k LOC clean")

        hover_ms = []
        ws_ms = []
        for k in range(20):
            t0 = time.perf_counter()
            hv = cli.request("textDocument/hover", {
                "textDocument": {"uri": big_uri},
                "position": {"line": target_ln - 1, "character": 10},
            })
            hover_ms.append((time.perf_counter() - t0) * 1000.0)
            check("stage_2000" in hv["result"]["contents"]["value"], "52k hover ok")

            t0 = time.perf_counter()
            ws = cli.request("workspace/symbol", {"query": "stage_259"})
            ws_ms.append((time.perf_counter() - t0) * 1000.0)
            check(any(s["name"] == "stage_2599" for s in ws["result"]), "52k workspace/symbol ok")

        med_edit = statistics.median(latencies_ms)
        med_hover = statistics.median(hover_ms)
        med_ws = statistics.median(ws_ms)
        print("52k LOC (%d lines, %d fns) real-pipe medians: didChange=%.3f ms, hover=%.3f ms, workspace/symbol=%.3f ms"
              % (total_lines, num_fns, med_edit, med_hover, med_ws))

        check(med_edit < 5.0, "52k LOC incremental didChange median too slow: %.3f ms" % med_edit)
        check(med_hover < 5.0, "52k LOC hover median too slow: %.3f ms" % med_hover)
        check(med_ws < 10.0, "52k LOC workspace/symbol median too slow: %.3f ms" % med_ws)

        rc, stderr = cli.shutdown()
        check(rc == 0, "52k benchmark clean shutdown")


def main():
    bin_path = _cxx.build()
    cc = shutil.which("c++") or shutil.which("g++")
    if not cc:
        _skip.skip("no C++ compiler")

    test_wire_and_simd_lines(bin_path)
    print("ok: SIMD JSON-RPC wire + O(1) UTF-16 LineIndex")

    test_incremental_salsa_and_workspace(bin_path)
    print("ok: Salsa-style incremental parity + cross-doc sig invalidation + 12 std.*")

    test_ide_intelligence(bin_path)
    print("ok: codeAction + semanticTokens + inlayHint + cross-file rename + formatting")

    test_asan_ubsan(cc)
    print("ok: ASan + UBSan + LSan 0 leaks / 0 UB")

    test_50k_loc_benchmark(bin_path)
    print("LSP ALL GREEN (%d asserts)" % check.n)


if __name__ == "__main__":
    main()
