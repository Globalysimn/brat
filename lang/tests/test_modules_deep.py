"""Module chains, diamonds, cycles, interfaces and callbacks on native paths.

"""

import _scratch

import argparse
import importlib.util
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "engine"))
CXX = os.path.abspath(os.path.join(HERE, "..", "cxx", "bin", "brat-cxx"))
TMP = _scratch.mkdtemp(prefix="brat-deepmod-")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cxx  # noqa: E402

fails = []
accepted_count = 0
refused_count = 0


def fail(msg):
    fails.append(msg)
    print("FAIL " + msg)


def write(rel, src):
    path = os.path.join(TMP, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(src)
    return path



def flat_cxx(path):
    r = subprocess.run([_cxx.build(), "flat", os.path.abspath(path)],
                       capture_output=True, text=True, timeout=120)
    return r.stdout if r.returncode == 0 else None


def expect_output(path, want, build_flags=()):
    global accepted_count
    accepted_count += 1
    runs = [("native run", [CXX, "run"] + list(build_flags) + [path])]
    for label, command in runs:
        result = subprocess.run(command, capture_output=True, text=True, timeout=300)
        if result.returncode != 0 or result.stdout != want:
            fail("%s/%s: rc=%d out=%r want=%r"
                 % (os.path.relpath(path, TMP), label, result.returncode,
                    (result.stdout + result.stderr)[-300:], want))
            return
    exe = os.path.join(TMP, "bin", path.replace(TMP, "").strip(os.sep).replace(os.sep, "-") + ".exe")
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    built = subprocess.run([CXX, "build"] + list(build_flags) + [path, "-o", exe],
                           capture_output=True, text=True, timeout=300)
    if built.returncode != 0:
        fail("%s: build %r" % (os.path.relpath(path, TMP), (built.stdout + built.stderr)[-300:]))
        return
    native = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    if native.stdout != want or native.returncode != 0:
        fail("%s: native rc=%d out=%r want=%r"
             % (os.path.relpath(path, TMP), native.returncode, native.stdout, want))
    got = flat_cxx(path)
    if got is None:
        fail("%s: brat-cxx flat refused" % os.path.relpath(path, TMP))


def refused(path, text):
    global refused_count
    refused_count += 1
    exe = os.path.join(TMP, "no-" + os.path.basename(os.path.dirname(path)) + ".exe")
    runs = [("native run", [CXX, "run", path]),
            ("native build", [CXX, "build", path, "-o", exe])]
    for tag, argv in runs:
        result = subprocess.run(argv, capture_output=True, text=True, timeout=300)
        out = (result.stdout + result.stderr).replace("\n", " ")
        if result.returncode == 0 or text not in out:
            fail("%s/%s: rc=%d want %r in %r"
                 % (os.path.relpath(path, TMP), tag, result.returncode, text, out[-300:]))
    if os.path.exists(exe):
        fail("%s: refused program produced an executable" % path)
    if flat_cxx(path) is not None:
        fail("%s: native loader accepted a refused program" % os.path.relpath(path, TMP))


def check_cxx_agrees(path, text):
    """`brat-cxx check` must apply the same link rules as the loader.

    It reported `0 error(s)` for every program here until it stopped skipping
    `flatten`, while `brat-cxx cc` on the same file refused them.
    """
    r = subprocess.run([_cxx.build(), "check", os.path.abspath(path)],
                       capture_output=True, text=True, timeout=120)
    out = (r.stdout + r.stderr).replace("\n", " ")
    if r.returncode == 0 or text not in out:
        fail("%s: brat-cxx check rc=%d want %r in %r"
             % (os.path.relpath(path, TMP), r.returncode, text, out[-300:]))

def main():
    global accepted_count, refused_count
    accepted_count = refused_count = 0
    fails.clear()
    _cxx.build()
    # a facade one level up re-exporting a function from a subdirectory
    write("chain/lib/base.brat", "pub fn base() -> i64 { return 5 }\n")
    write("chain/lib/mid.brat",
          "import base\npub use base.base\npub fn mid() -> i64 { return base() + 1 }\n")
    write("chain/facade.brat", "import lib.mid\npub use lib.mid.base\npub use lib.mid.mid\n")
    expect_output(write("chain/app.brat",
               "import facade\n"
               "fn main() -> i64 {\n  print(facade.base())\n  print(facade.mid())\n  return 0\n}\n"),
         "5\n6\n")

    # the same module reached by two paths: one definition, two names
    write("diamond/lib/base.brat", "pub fn base() -> i64 { return 7 }\n")
    write("diamond/facade.brat", "import lib.base\npub use lib.base.base\n")
    expect_output(write("diamond/app.brat",
               "import lib.base\nimport facade\n"
               "fn main() -> i64 {\n  print(base())\n  print(facade.base())\n  return 0\n}\n"),
         "7\n7\n")

    # a cycle: two modules importing each other, resolved once
    write("cycle/cyc1.brat", "import cyc2\npub fn a() -> i64 { return cyc2.b() + 1 }\n")
    write("cycle/cyc2.brat", "import cyc1\npub fn b() -> i64 { return 1 }\n")
    expect_output(write("cycle/app.brat",
               "import cyc1\nfn main() -> i64 { print(cyc1.a()); return 0 }\n"), "2\n")

    # an interface decides what is exported, and a re-export of an interface-exported
    # function is public even though the function is not `pub`
    write("iface/lib/thing.brat", "fn hidden() -> i64 { return 42 }\n")
    write("iface/lib/thing.bri", "fn hidden() -> i64\n")
    write("iface/facade.brat", "import lib.thing\npub use lib.thing.hidden\n")
    expect_output(write("iface/app.brat",
               "import facade\nfn main() -> i64 { print(facade.hidden()); return 0 }\n"),
         "42\n")

    # two re-exports of the same name from two modules: ambiguous, and refused
    write("amb/lib/s1.brat", "pub fn shout() -> i64 { return 1 }\n")
    write("amb/lib/s2.brat", "pub fn shout() -> i64 { return 2 }\n")
    write("amb/facade.brat",
          "import lib.s1\nimport lib.s2\npub use lib.s1.shout\npub use lib.s2.shout\n")
    amb = write("amb/app.brat",
                "import facade\nfn main() -> i64 { print(facade.shout()); return 0 }\n")
    refused(amb, "`shout` is re-exported from two modules")

    # two re-exports of the same name from the same module: not ambiguous, one name
    write("same/lib/s1.brat", "pub fn only() -> i64 { return 3 }\n")
    write("same/facade.brat",
          "import lib.s1\npub use lib.s1.only\npub use lib.s1.only\n")
    expect_output(write("same/app.brat",
               "import facade\nfn main() -> i64 { print(facade.only()); return 0 }\n"), "3\n")


    # `pub use geo.circle.area` in a facade, called as `geo.area(...)`: the dotted
    # name is the facade module, and it is how the guide documents re-exporting
    write("dotted/geo/circle.brat",
          "pub fn area(r: i64) -> i64 { return r * r }\n"
          "pub fn perim(r: i64) -> i64 { return 4 * r }\n")
    write("dotted/geo.brat", "import geo.circle\npub use geo.circle.area\n")
    expect_output(write("dotted/app.brat",
               "import geo\n"
               "fn main() -> i64 {\n  print(geo.area(3))\n  print(area(4))\n  return 0\n}\n"),
         "9\n16\n")

    # the same through a chain: app -> geo -> mid -> base, dotted at every step
    write("dotted_chain/base.brat", "pub fn val() -> i64 { return 42 }\n")
    write("dotted_chain/mid.brat", "import base\npub use base.val\n")
    write("dotted_chain/geo.brat", "import mid\npub use mid.val\n")
    expect_output(write("dotted_chain/app.brat",
               "import geo\nfn main() -> i64 { print(geo.val()); return 0 }\n"), "42\n")

    # an interface the implementation does not honour: refused by every front end
    write("ifacebad/m.brat", "fn f(a: i64, b: i64) -> i64 { return a + b }\n")
    write("ifacebad/m.bri", "fn f(a: i64) -> i64\n")
    ifacebad = write("ifacebad/app.brat",
                     "import m\nfn main() -> i64 { print(m.f(1, 2)); return 0 }\n")
    refused(ifacebad, "does not match its interface")
    check_cxx_agrees(ifacebad, "does not match its interface")

    # a function that is neither `pub` nor in the interface: not importable
    write("private/m.brat", "export fn h() -> i64 { return 3 }\n")
    private = write("private/app.brat",
                    "import m\nfn main() -> i64 { print(m.h()); return 0 }\n")
    refused(private, "module `m` has no function `h`")
    check_cxx_agrees(private, "module `m` has no function `h`")

    # Extern callbacks in a module must resolve the callback string to the same
    # mangled BRAT symbol in the interpreter and the native loader/emitter.
    callback_lib = os.path.join(TMP, "callback", "libcallback.so")
    callback_c = write("callback/libcallback.c",
                       "long long apply_cb(long long (*cb)(long long)) { return cb(21); }\n")
    compiled = subprocess.run(["cc", "-shared", "-fPIC", callback_c, "-o", callback_lib],
                              capture_output=True, text=True, timeout=120)
    assert compiled.returncode == 0, compiled.stdout + compiled.stderr
    write("callback/lib/callbacks.brat",
          'extern "%s" fn apply_cb(cb: fn(i64) -> i64) -> i64\n'
          'fn double_it(x: i64) -> i64 { return x * 2 }\n'
          'pub fn run() -> i64 { return apply_cb("double_it") }\n' % callback_lib)
    expect_output(write("callback/app.brat",
               "import lib.callbacks\n"
               "fn main() -> i64 { print(lib.callbacks.run()); return 0 }\n"),
         "42\n", build_flags=("--unsafe-ffi",))

    if fails:
        raise AssertionError("DEEP MODULES FAILED (%d)" % len(fails))
    print("DEEP MODULES ALL GREEN (%d execution cases, %d refusals)" % (accepted_count, refused_count))


if __name__ == "__main__":
    main()
