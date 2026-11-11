"""The native JSON tape must not expose malformed UTF-8 as a string."""
import _skip
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

SRC = Path(__file__).resolve().parents[1] / "cxx" / "src"
CC = shutil.which("c++") or shutil.which("g++")
if not CC:
    _skip.skip("no C++ compiler for the JSON wire regression")

HARNESS = r'''
#include "lsp_wire.h"
#include <iostream>
#include <string>
#include <vector>

int main() {
    lsp_wire::LspJsonTape empty_tape;
    int failures = 0;
    if (!empty_tape.is_null(0) || empty_tape.is_object(0) || empty_tape.is_object(999) ||
        empty_tape.at(0).kind != lsp_wire::JKind::Null ||
        empty_tape.at(999).kind != lsp_wire::JKind::Null) {
        std::cerr << "unparsed LspJsonTape sentinel failed\n";
        ++failures;
    }
    lsp_wire::LspJsonTape tape;
    const std::vector<std::string> invalid = {
        "\xff", "\x80", "\xc0\xaf", "\xc1\xbf", "\xc2", "\xe2\x82",
        "\xe2\x28\xa1", "\xe0\x80\xaf", "\xed\xa0\x80", "\xed\xbf\xbf",
        "\xf0\x80\x80\xaf", "\xf4\x90\x80\x80", "\xf5\x80\x80\x80",
        "\xf0\x9f\x98", "\xfe"
    };
    for (const auto &bytes : invalid) {
        // Raw string, raw run after an escape, and object key.
        for (const auto &json : {"\"" + bytes + "\"",
                                 "\"ok\\n" + bytes + "\"",
                                 "{\"" + bytes + "\":0}"}) {
            if (tape.parse(json)) {
                std::cerr << "accepted malformed UTF-8\n";
                ++failures;
            }
        }
    }
    const std::vector<std::string> valid = {
        "", "ASCII", "\xc2\x80", "\xdf\xbf", "\xe0\xa0\x80",
        "\xed\x9f\xbf", "\xee\x80\x80", "\xef\xbf\xbd",
        "\xf0\x90\x80\x80", "\xf4\x8f\xbf\xbf", "\xf0\x9f\x98\x80"
    };
    for (const auto &bytes : valid) {
        if (!tape.parse("\"" + bytes + "\"") || tape.str_view(tape.root()) != bytes)
            ++failures;
        if (!tape.parse("\"\\n" + bytes + "\"") || tape.str_view(tape.root()) != "\n" + bytes)
            ++failures;
    }
    if (!tape.parse("\"\\ud83d\\ude00\"") || tape.str_view(tape.root()) != "\xf0\x9f\x98\x80")
        ++failures;
    std::cout << "JSON UTF-8: 68 checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
'''

with tempfile.TemporaryDirectory(prefix="brat-wire-utf8-") as tmp:
    harness = Path(tmp) / "wire.cpp"
    harness.write_text(HARNESS, encoding="utf-8")
    binary = Path(tmp) / ("wire.exe" if os.name == "nt" else "wire")
    flags = ["-std=c++20", "-O1", "-D_GLIBCXX_ASSERTIONS"]
    if os.environ.get("BRAT_WIRE_SANITIZE") == "1":
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    subprocess.run([CC, *flags, "-I", str(SRC), str(harness),
                    str(SRC / "lsp_wire.cpp"), "-o", str(binary)], check=True, timeout=120)
    subprocess.run([str(binary)], check=True, timeout=30)
