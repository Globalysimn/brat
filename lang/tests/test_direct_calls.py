"""Direct-call ABI, fallback, ownership and evaluation-order proofs."""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
ENGINE = os.path.join(ROOT, "lang", "engine")
sys.path.insert(0, HERE)

import _cxx  # noqa: E402
import _scratch  # noqa: E402

TMP = _scratch.mkdtemp(prefix="brat-calls-")
SOURCE = '''let mut trace = ""
let mut global = ["old"]
fn zero() -> i64 { return 0 }
fn one(a: i64) -> i64 { return a }
fn two(a: i64, b: i64) -> i64 { return a + b }
fn three(a: i64, b: i64, c: i64) -> i64 { return a + b + c }
fn four(a: i64, b: i64, c: i64, d: i64) -> i64 { return a + b + c + d }
fn five(a: i64, b: i64, c: i64, d: i64, e: i64) -> i64 { return a + b + c + d + e }
fn add2(a: i64, b: i64) -> i64 { return a + b }
fn step(a: i64, b: i64) -> i64 { return a + b }
fn scan_calls(n: i64) -> i64 {
  let marker = str(n)
  let mut total = 0
  for i in 0..n { total = total + step(i, 3) }
  return total + len(marker)
}
fn flag(n: i64) -> bool { return n != 0 }
fn with_default(a: i64, b: i64 = 7) -> i64 { return a + b }
fn mark(s: str) -> str { trace = trace + s; return s }
fn join(a: str, b: str) -> str { return a + b }
fn reset() -> i64 { global = ["new"]; return 0 }
fn size(v: vec[str], x: i64) -> i64 { return len(v) + x }
fn fail() -> i64 { return 1 / 0 }
fn use2(a: vec[str], b: vec[str]) -> i64 { return len(a) + len(b) }
fn pass(v: vec[str]) -> vec[str] { return v }
impl Counter { fn add(self, d) { return self.v + d } }
fn main() -> i64 {
  print(zero())
  print(one(1))
  print(two(1, 2))
  print(with_default(5))
  print(three(1, 2, 3))
  print(four(1, 2, 3, 4))
  print(five(1, 2, 3, 4, 5))
  print(join(mark("A"), mark("B")))
  print(trace)
  print(scan_calls(4))
  print(flag(1))
  print(flag(0))
  print(size(global, reset()))
  print(global)
  try { print(size(["orphan"], fail())) } catch e { print(e) }
  let mut value = ["old"]
  print(use2(value, pass(move value)))
  let f = fn(a, b) { return add2(a, b) }
  print(f(5, 6))
  let counter = Counter { v: 9 }
  print(counter.add(4))
  return 0
}
'''
SMALL_ABI = '''
fn zero() -> i64 { return 0 }
fn one(a: i64) -> i64 { return a }
fn two(a: i64, b: i64) -> i64 { return a + b }
fn three(a: i64, b: i64, c: i64) -> i64 { return a + b + c }
fn four(a: i64, b: i64, c: i64, d: i64) -> i64 { return a + b + c + d }
fn step(a: i64, b: i64) -> i64 { return a + b }
fn run(n: i64) -> i64 {
  let marker = str(n)
  let mut total = 0
  for i in 0..n { total = total + step(i, 3) }
  return total + len(marker)
}
fn dynamic(x) { return x }
fn text0() -> str { return "a" }
fn text1(a: str) -> str { return a }
fn text2(a: str, b: str) -> str { return a + b }
fn text3(a: str, b: str, c: str) -> str { return a + b + c }
fn text4(a: str, b: str, c: str, d: str) -> str { return a + b + c + d }
fn main() -> i64 {
  return zero() + one(1) + two(1, 2) + three(1, 2, 3) + four(1, 2, 3, 4)
    + run(5) + dynamic(7) + len(text0()) + len(text1("b"))
    + len(text2("c", "d")) + len(text3("e", "f", "g"))
    + len(text4("h", "i", "j", "k"))
}
'''
CLOSURE_SOURCE = '''fn make() {
  let prefix = ["capture-value-alpha"]
  let middle = ["capture-value-beta"]
  let suffix = ["capture-value-gamma"]
  return fn() { return prefix[0] + middle[0] + suffix[0] }
}
fn main() -> i64 {
  let mut i = 0
  while i < 8 {
    let joiner = make()
    print(joiner())
    let fact = fn fact(n) {
      if n <= 1 { return 1 }
      return n * fact(n - 1)
    }
    print(fact(6))
    i += 1
  }
  return 0
}
'''
CLOSURE_EXPECTED = ("capture-value-alphacapture-value-betacapture-value-gamma\n720\n" * 8)

EXPECTED = """0
1
3
12
6
10
15
AB
AB
19
true
false
1
['new']
div by zero
2
11
13
"""


def run(argv, timeout=180, env=None):
    return subprocess.run(argv, capture_output=True, text=True, timeout=timeout,
                          cwd=ROOT, env=env)


def put(name, text):
    path = os.path.join(TMP, name)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return path


def main():
    cxx = _cxx.build()
    source = put("calls.brat", SOURCE)
    small = put("small.brat", SMALL_ABI)

    cpp = run([cxx, "cc", source])
    assert cpp.returncode == 0, cpp.stderr
    cpp_again = run([cxx, "cc", source])
    assert cpp_again.returncode == 0 and cpp_again.stdout == cpp.stdout, \
           "native direct-call emitter is not deterministic"
    assert 'vcall_method_ic(b_counter, "add"' in cpp.stdout, \
           "dynamic receiver dispatch lost its vector fallback"

    interp = run(_cxx.cli_args("run", "--interp", source))
    assert interp.returncode == 0, interp.stderr
    assert interp.stdout == EXPECTED, repr(interp.stdout)

    cfile = os.path.join(TMP, "calls.c")
    exe = os.path.join(TMP, "calls")
    with open(cfile, "w", encoding="utf-8") as f:
        f.write(cpp.stdout)
    compiled = run(["cc", "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", "-fwrapv", "-I", ENGINE, cfile,
                    "-o", exe, "-lm", "-pthread"])
    assert compiled.returncode == 0, compiled.stderr
    native = run([exe], env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1"))
    assert (native.returncode, native.stdout, native.stderr) == \
           (0, EXPECTED, ""), native.stderr

    closure_source = put("closure_slots.brat", CLOSURE_SOURCE)
    closure_cpp = run([cxx, "cc", closure_source])
    assert closure_cpp.returncode == 0, closure_cpp.stderr
    closure_cpp_again = run([cxx, "cc", closure_source])
    assert closure_cpp_again.returncode == 0 and \
           closure_cpp_again.stdout == closure_cpp.stdout, \
           "native closure-slot emitter is not deterministic"
    assert "vclosure_env_new(3)" in closure_cpp.stdout
    assert "vclosure_env_at(cap," in closure_cpp.stdout
    assert "map_get(cap" not in closure_cpp.stdout
    closure_interp = run(_cxx.cli_args("run", "--interp", closure_source))
    assert (closure_interp.returncode, closure_interp.stdout, closure_interp.stderr) == \
           (0, CLOSURE_EXPECTED, "")
    closure_cfile = os.path.join(TMP, "closure_slots.c")
    closure_exe = os.path.join(TMP, "closure_slots")
    with open(closure_cfile, "w", encoding="utf-8") as f:
        f.write(closure_cpp.stdout)
    closure_compiled = run(["cc", "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined",
                            "-fno-omit-frame-pointer", "-fwrapv", "-I", ENGINE, closure_cfile,
                            "-o", closure_exe, "-lm", "-pthread"])
    assert closure_compiled.returncode == 0, closure_compiled.stderr
    closure_native = run([closure_exe],
                          env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1"))
    assert (closure_native.returncode, closure_native.stdout, closure_native.stderr) == \
           (0, CLOSURE_EXPECTED, ""), closure_native.stderr

    small_cpp = run([cxx, "cc", small])
    assert small_cpp.returncode == 0, small_cpp.stderr
    direct = small_cpp.stdout
    small_cpp_again = run([cxx, "cc", small])
    assert small_cpp_again.returncode == 0 and small_cpp_again.stdout == direct, \
           "native small-ABI emitter is not deterministic"

    sites = re.findall(r"bf_c\d+_p\(NULL(?:, [^;]*)?\)", direct)
    assert len(sites) >= 5, "non-fast 0-4 arity calls lost their positional entries"
    raw_site = r"bf_f\d+\(ub_b_i, \(3LL\)\)"
    assert re.search(raw_site, direct), "typed call did not use its raw scalar helper"

    def function_body(c, name):
        match = re.search(
            r"static Val %s\(Map \*cap[^)]*\) \{\n(.*?)\n\}" % re.escape(name),
            c, re.S)
        assert match, "could not find emitted function body %s" % name
        return match.group(1)

    entry = re.search(r"int64_t brat_cb_main\(\) \{(.*?)\n\}", direct, re.S)
    assert entry, "could not find brat_cb_main"
    main_call = re.search(r"\b(bf_c\d+)\(NULL, _a\)", entry.group(1))
    assert main_call, "brat_cb_main does not identify the BRAT main wrapper"
    main_wrapper = main_call.group(1)
    assert re.search(r"static Val %s\(Map \*cap, Vec \*args\) \{" % main_wrapper,
                     direct), "generic vector wrapper for main disappeared"
    main_direct_body = function_body(direct, main_wrapper + "_p")
    assert "VSTACK_VEC_INIT" not in main_direct_body, \
           "direct 0-4 call packed a vector"
    assert "VSTACK_VEC_INIT" in direct, \
           "generic/vector call machinery disappeared from the emitted program"
    print("DIRECT CALL ABI ALL GREEN")


if __name__ == "__main__":
    main()
