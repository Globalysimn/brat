"""Bitwise abstract interpretation (-O1 IV, -O2 IV x Granger congruence x DBM) and proof certificates."""
import os
import subprocess

import _cxx
import _scratch

BIN = _cxx.build()
TMP = _scratch.mkdtemp(prefix="brat-bitopt-")


def run_cli(*args):
    return subprocess.run([BIN] + list(args), capture_output=True, text=True, timeout=120)


def write_src(name, content):
    p = os.path.join(TMP, name)
    with open(p, "w", encoding="utf-8") as f:
        f.write(content)
    return p


PROOF_SRC = """fn mask_lookup(v: vec[i64], raw: i64) -> i64 {
  if len(v) >= 16 {
    let idx = (raw >> 3) & 15
    let odd_div = (raw << 3) | 1
    return v[idx] / odd_div
  }
  return 0
}

fn aligned_mod(x: i64) -> i64 {
  if x >= 0 && x <= 1000 {
    let aligned = (x << 4) & ~15
    return aligned % 16
  }
  return 0
}

fn bitnot_div(x: i64) -> i64 {
  if x >= 0 && x <= 10 {
    let neg_d = ~x
    return (-1000) / neg_d
  }
  return 0
}

fn xor_parity_div(a: i64, x: i64) -> i64 {
  let even = x & ~3
  let nz = even ^ 2
  return a / nz
}

fn main() -> i64 {
  let mut table: vec[i64] = []
  for i in 0..16 {
    table.push((i + 1) * 100)
  }
  let inputs: vec[i64] = [0, 1, -1, 7, -8, 63, 64, -64, 1023, -1024, 0x1234_5678]
  for x in inputs {
    print(mask_lookup(table, x))
    print(aligned_mod(x & 511))
    print(bitnot_div(x & 7))
    print(xor_parity_div(500, x))
  }
  return 0
}
"""

proof_path = write_src("bitwise_proofs.brat", PROOF_SRC)

# 1. Verify -O2 proof certificates for bounds elision, congruence non-zero divisor, exact mod, and ~x.
cc_o2 = run_cli("cc", "-O2", "--why-opt", proof_path)
assert cc_o2.returncode == 0, cc_o2.stderr
proofs_o2 = cc_o2.stderr
assert "fn mask_lookup" in proofs_o2 and "bounds check dropped on `v[...]`" in proofs_o2, proofs_o2
assert "8Z + 1" in proofs_o2 and "div-by-zero check dropped" in proofs_o2, proofs_o2
assert "fn aligned_mod" in proofs_o2 and "check dropped" in proofs_o2, proofs_o2
assert "fn bitnot_div" in proofs_o2 and "check dropped" in proofs_o2, proofs_o2
assert "fn xor_parity_div" in proofs_o2 and "4Z + 2" in proofs_o2, proofs_o2

# 2. Verify -O1 interval proofs on mask / bitnot as well.
cc_o1 = run_cli("cc", "-O1", "--why-opt", proof_path)
assert cc_o1.returncode == 0, cc_o1.stderr
assert "fn bitnot_div" in cc_o1.stderr, cc_o1.stderr

# 3. Verify 3-way execution parity (-O0, -O1, -O2).
outs = {}
for flag in ("-O0", "-O1", "-O2"):
    r = run_cli("run", flag, proof_path)
    assert r.returncode == 0, f"{flag} failed:\n{r.stderr}"
    outs[flag] = r.stdout
assert outs["-O0"] == outs["-O1"] == outs["-O2"], outs

# 4. Soundness check: a potentially-zero bitwise divisor must not elide the trap.
UNSAFE_DIV_SRC = """fn maybe_zero(a: i64, x: i64) -> i64 {
  let d = (x << 3) & ~7
  return a / d
}

fn main() -> i64 {
  try {
    print(maybe_zero(10, 0))
  } catch e {
    print("caught")
  }
  return 0
}
"""
unsafe_path = write_src("unsafe_div.brat", UNSAFE_DIV_SRC)
cc_unsafe = run_cli("cc", "-O2", "--why-opt", unsafe_path)
assert cc_unsafe.returncode == 0, cc_unsafe.stderr
assert "fn maybe_zero" not in cc_unsafe.stderr, (
    f"unsound check elision on potentially-zero divisor:\n{cc_unsafe.stderr}"
)
for flag in ("-O0", "-O1", "-O2"):
    ru = run_cli("run", flag, unsafe_path)
    assert ru.returncode == 0 and ru.stdout == "caught\n", f"{flag}: {ru.stdout!r} {ru.stderr!r}"

print("BITWISE OPT ALL GREEN")
