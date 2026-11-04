"""Native FFI header proofs must match supported ABI layouts and fail closed."""
import json
import os
import subprocess
import sys
import tempfile

import _cxx

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
CLI = os.path.join(ROOT, "lang", "engine", "cli.py")


def run(argv, env=None):
    child_env = dict(os.environ)
    if env:
        child_env.update(env)
    return subprocess.run(argv, env=child_env, capture_output=True, text=True, timeout=120)


def write(path, text):
    with open(path, "w", encoding="utf-8", newline="\n") as stream:
        stream.write(text)


def main():
    native = _cxx.build()
    with tempfile.TemporaryDirectory(prefix="brat-ffi-") as td:
        source = os.path.join(td, "packet.brat")
        header = os.path.join(td, "packet.h")
        write(source, "extern struct Packet { tag: u8, ts: u64, len: u32 }\n"
                      "fn packet_len(p: Packet) -> usize\n"
                      "fn main() -> i64 { return 0 }\n")
        write(header, "#ifndef PACKET_H\n#define PACKET_H\n"
                      "#include <stdint.h>\n"
                      "typedef struct Packet { uint8_t tag; uint64_t ts; uint32_t len; } Packet;\n"
                      "#endif\n")
        args = ["ffi", source, "--header", header, "--abi-matrix", "--json"]
        native_result = run([native] + args)
        assert native_result.returncode == 0, native_result.stderr
        native_json = json.loads(native_result.stdout)
        assert native_json["ok"] is True and len(native_json["targets"]) == 5
        assert native_json["differential"]["Packet"]["wasm32-unknown-wasi"]["brat"]["offsets"] == {
            "tag": 0, "ts": 8, "len": 16
        }
        reference = run([sys.executable, CLI] + args, env={"BRAT_INTERP": "1"})
        assert reference.returncode == 0, reference.stderr
        reference_json = json.loads(reference.stdout)
        assert native_json["ok"] == reference_json["ok"]
        assert native_json["differential"] == reference_json["differential"]
        routed = run([sys.executable, CLI] + args)
        assert (routed.returncode, routed.stdout, routed.stderr) == \
               (native_result.returncode, native_result.stdout, native_result.stderr)

        packed_source = os.path.join(td, "packed.brat")
        packed_header = os.path.join(td, "packed.h")
        write(packed_source, "extern packed struct Packet { tag: u8, ts: u64, len: u32 }\n"
                             "fn main() -> i64 { return 0 }\n")
        write(packed_header, "#pragma pack(push, 1)\n"
                             "typedef struct Packet { uint8_t tag; uint64_t ts; uint32_t len; } Packet;\n"
                             "#pragma pack(pop)\n")
        packed = run([native, "ffi", packed_source, "--header", packed_header,
                      "--abi-matrix", "--json"])
        assert packed.returncode == 0, packed.stderr
        assert json.loads(packed.stdout)["ok"] is True
        packed_mismatch = run([native, "ffi", source, "--header", packed_header,
                               "--abi-matrix", "--json"])
        assert packed_mismatch.returncode == 1
        assert json.loads(packed_mismatch.stdout)["ok"] is False

        wrong_source = os.path.join(td, "wrong.brat")
        wrong_header = os.path.join(td, "wrong.h")
        write(wrong_source, "extern struct BadLong { tag: u8, count: i64 }\n"
                            "fn main() -> i64 { return 0 }\n")
        write(wrong_header, "typedef struct BadLong { unsigned char tag; long count; } BadLong;\n")
        wrong = run([native, "ffi", wrong_source, "--header", wrong_header,
                     "--abi-matrix", "--json"])
        assert wrong.returncode == 1, wrong.stdout + wrong.stderr
        wrong_json = json.loads(wrong.stdout)
        per_target = {item["target"]: item["errors"] for item in wrong_json["targets"]}
        assert per_target["x86_64-unknown-linux-gnu"] == 0
        assert per_target["x86_64-pc-windows-msvc"] > 0
        assert per_target["i686-unknown-linux-gnu"] > 0

        conditional_header = os.path.join(td, "conditional.h")
        write(conditional_header, "#ifdef USE_PACKED_PACKET\n"
                                  "struct Packet { uint8_t tag; uint64_t ts; uint32_t len; };\n"
                                  "#endif\n")
        refused = run([native, "ffi", source, "--header", conditional_header,
                       "--abi-matrix", "--json"])
        assert refused.returncode != 0
        assert "unsupported C preprocessor directive" in refused.stderr

        aligned_header = os.path.join(td, "aligned.h")
        write(aligned_header, "typedef struct __attribute__((aligned(16))) Packet {\n"
                              "uint8_t tag; uint64_t ts; uint32_t len;\n} Packet;\n")
        refused = run([native, "ffi", source, "--header", aligned_header,
                       "--abi-matrix", "--json"])
        assert refused.returncode != 0
        assert "unsupported C attribute" in refused.stderr

        no_path = run([native, "ffi", source, "--header", header, "--abi-matrix", "--json"],
                      env={"PATH": ""})
        assert no_path.returncode == 0, no_path.stderr

    print("NATIVE FFI ALL GREEN")


if __name__ == "__main__":
    main()
