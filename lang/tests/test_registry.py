"""A registry index resolves package versions and their URL, sha256 and vendor files.

These cases cover exact versions, numeric constraints, transitive unification,
unsatisfiable dependencies, and verification before packages land in `vendor/`.
"""
import _cxx
import _scratch

import hashlib
import json
import os
import shutil
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "engine"))
TMP = _scratch.mkdtemp(prefix="brat-reg-")
PKG = os.path.join(TMP, "mathx.brat")

with open(PKG, "w", encoding="utf-8") as f:
    f.write("pub fn square(x: i64) -> i64 { return x * x }\n")
DIGEST = hashlib.sha256(open(PKG, "rb").read()).hexdigest()


def write_index(sha=DIGEST, versions=("1.0", "1.2")):
    index = {"packages": {"acme/mathx": {
        v: {"url": "file://" + PKG, "sha256": sha} for v in versions}}}
    with open(os.path.join(TMP, "brat-index.json"), "w", encoding="utf-8") as f:
        json.dump(index, f, indent=2)


def pkg(*args):
    return subprocess.run(_cxx.cli_args("pkg", *(list(args))), cwd=TMP,
                          capture_output=True, text=True, timeout=120)


write_index()

r = pkg("add", "acme/mathx@1.2")
assert r.returncode == 0, r.stdout + r.stderr
assert r.stdout == "add acme/mathx 1.2 %s\n" % DIGEST[:12], r.stdout
assert os.path.exists(os.path.join(TMP, "vendor", "mathx.brat"))
lock = json.load(open(os.path.join(TMP, "brat.lock"), encoding="utf-8"))
assert lock[0]["name"] == "acme/mathx" and lock[0]["version"] == "1.2", lock

r = pkg("list")
assert r.stdout == "acme/mathx@1.2  %s  file://%s\n" % (DIGEST[:12], PKG), r.stdout

# No version asks for the newest one the index knows.
shutil.rmtree(os.path.join(TMP, "vendor"))
os.remove(os.path.join(TMP, "brat.lock"))
r = pkg("add", "acme/mathx")
assert r.returncode == 0 and r.stdout.startswith("add acme/mathx 1.2 "), r.stdout

# A version the index does not have names the ones it does.
r = pkg("add", "acme/mathx@9.9")
assert r.returncode == 1, r.stdout
assert r.stderr == "brat pkg: acme/mathx has no version 9.9 (index has 1.0, 1.2)\n", r.stderr

r = pkg("add", "acme/nope")
assert r.returncode == 1 and "is not in" in r.stderr, r.stderr

r = pkg("add", "acme/mathx@^1.0")
assert r.returncode == 1 and "invalid package version constraint" in r.stderr, r.stderr

shutil.rmtree(os.path.join(TMP, "vendor"))
os.remove(os.path.join(TMP, "brat.lock"))
r = pkg("add", "acme/mathx@>=1.0,<1.2")
assert r.returncode == 0 and r.stdout.startswith("add acme/mathx 1.0 "), r.stdout + r.stderr

# A hash that does not match the index stops before anything is vendored.
write_index(sha="0" * 64)
shutil.rmtree(os.path.join(TMP, "vendor"))
os.remove(os.path.join(TMP, "brat.lock"))
r = pkg("add", "acme/mathx@1.2")
assert r.returncode == 1, r.stdout
assert "HASH MISMATCH acme/mathx@1.2" in r.stderr, r.stderr
assert not os.path.exists(os.path.join(TMP, "vendor", "mathx.brat")), "bad hash vendored"
assert not os.path.exists(os.path.join(TMP, "brat.lock")), "bad hash locked"

unhashed = {"packages": {"acme/mathx": {
    "1.2": {"url": "file://" + PKG}}}}
with open(os.path.join(TMP, "brat-index.json"), "w", encoding="utf-8") as f:
    json.dump(unhashed, f)
r = pkg("add", "acme/mathx@1.2")
assert r.returncode == 1 and "no valid SHA-256" in r.stderr, r.stderr
assert not os.path.exists(os.path.join(TMP, "vendor")), "unhashed package created vendor"
assert not os.path.exists(os.path.join(TMP, "brat.lock")), "unhashed package wrote lock"

malformed = {"packages": {"acme/mathx": {
    "1.2": {"url": "file:///missing-package", "sha256": "not-a-hash"}}}}
with open(os.path.join(TMP, "brat-index.json"), "w", encoding="utf-8") as f:
    json.dump(malformed, f)
r = pkg("add", "acme/mathx@1.2")
assert r.returncode == 1 and "no valid SHA-256" in r.stderr, r.stderr

r = pkg("add", "acme/../escape@1.2")
assert r.returncode == 1 and "invalid package name" in r.stderr, r.stderr

colliding = {"packages": {
    "root/app": {"1.0": {"url": "file://" + PKG, "sha256": DIGEST,
                         "dependencies": {"a/Mathx": "1.0", "b/mathx": "1.0"}}},
    "a/Mathx": {"1.0": {"url": "file://" + PKG, "sha256": DIGEST}},
    "b/mathx": {"1.0": {"url": "file://" + PKG, "sha256": DIGEST}},
}}
with open(os.path.join(TMP, "brat-index.json"), "w", encoding="utf-8") as f:
    json.dump(colliding, f)
r = pkg("add", "root/app@1.0")
assert r.returncode == 1 and "both install as mathx.brat" in r.stderr, r.stderr
assert not os.path.exists(os.path.join(TMP, "vendor")), "colliding packages created vendor"
assert not os.path.exists(os.path.join(TMP, "brat.lock")), "colliding packages wrote lock"

range_index = {"packages": {
    "root/app": {"1.0": {"url": "file://" + PKG, "sha256": DIGEST,
                         "dependencies": {"x/left": "1.0", "x/right": "1.0"}}},
    "x/left": {"1.0": {"url": "file://" + PKG, "sha256": DIGEST,
                       "dependencies": {"z/core": ">=1.0,<4.0"}}},
    "x/right": {"1.0": {"url": "file://" + PKG, "sha256": DIGEST,
                        "dependencies": {"z/core": "<3.0"}}},
    "z/core": {
        "1.0": {"url": "file://" + PKG, "sha256": DIGEST},
        "2.0": {"url": "file://" + PKG, "sha256": DIGEST},
        "3.0": {"url": "file://" + PKG, "sha256": DIGEST},
    },
}}
with open(os.path.join(TMP, "brat-index.json"), "w", encoding="utf-8") as f:
    json.dump(range_index, f)
r = pkg("add", "root/app@1.0")
assert r.returncode == 0, r.stdout + r.stderr
lock = json.load(open(os.path.join(TMP, "brat.lock"), encoding="utf-8"))
locked = {item["name"]: item["version"] for item in lock}
assert locked["z/core"] == "2.0", locked
assert r.stdout.splitlines()[0].startswith("add z/core 2.0 "), r.stdout

range_index["packages"]["other/app"] = {
    "1.0": {"url": "file://" + PKG, "sha256": DIGEST,
            "dependencies": {"z/core": ">=3.0"}}
}
with open(os.path.join(TMP, "brat-index.json"), "w", encoding="utf-8") as f:
    json.dump(range_index, f)
r = pkg("add", "other/app@1.0")
assert r.returncode == 1 and "no version of z/core satisfies" in r.stderr, r.stderr

range_index["packages"]["x/right"]["1.0"]["dependencies"]["z/core"] = ">=3.0"
range_index["packages"]["x/left"]["1.0"]["dependencies"]["z/core"] = ">=1.0,<3.0"
with open(os.path.join(TMP, "brat-index.json"), "w", encoding="utf-8") as f:
    json.dump(range_index, f)
shutil.rmtree(os.path.join(TMP, "vendor"))
os.remove(os.path.join(TMP, "brat.lock"))
r = pkg("add", "root/app@1.0")
assert r.returncode == 1 and "no version of z/core satisfies" in r.stderr, r.stderr
assert not os.path.exists(os.path.join(TMP, "vendor")), "unsatisfiable ranges created vendor"
assert not os.path.exists(os.path.join(TMP, "brat.lock")), "unsatisfiable ranges wrote lock"

def write_source(name, text):
    path = os.path.join(TMP, name)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    return path, hashlib.sha256(text.encode("utf-8")).hexdigest()


core_v1, core_hash1 = write_source(
    "core-1.brat", "pub fn value() -> i64 { return 1 }\n")
core_v2, core_hash2 = write_source(
    "core-2.brat", "pub fn value() -> i64 { return 2 }\n")
util_v1, util_hash1 = write_source(
    "util-1.brat", "pub fn utility() -> i64 { return 1 }\n")
util_v2, util_hash2 = write_source(
    "util-2.brat", "pub fn utility() -> i64 { return 2 }\n")
app_v1, app_hash1 = write_source(
    "app-1.brat", "pub fn app() -> i64 { return 1 }\n")
app_v2, app_hash2 = write_source(
    "app-2.brat", "pub fn app() -> i64 { return 2 }\n")
keep_v1, keep_hash1 = write_source(
    "keep-1.brat", "pub fn keep() -> i64 { return 1 }\n")
keep_v2, keep_hash2 = write_source(
    "keep-2.brat", "pub fn keep() -> i64 { return 2 }\n")
update_index = {"packages": {
    "acme/core": {
        "1.0": {"url": "file://" + core_v1, "sha256": core_hash1,
                "dependencies": {"acme/util": ">=1.0,<2.0"}},
        "1.2": {"url": "file://" + core_v2, "sha256": core_hash2,
                "dependencies": {"acme/util": ">=1.0,<2.0"}},
    },
    "acme/util": {
        "1.0": {"url": "file://" + util_v1, "sha256": util_hash1},
        "1.2": {"url": "file://" + util_v2, "sha256": util_hash2},
    },
    "acme/app": {
        "1.0": {"url": "file://" + app_v1, "sha256": app_hash1,
                "dependencies": {"acme/core": ">=1.0,<2.0"}},
        "1.2": {"url": "file://" + app_v2, "sha256": app_hash2,
                "dependencies": {"acme/core": ">=1.0,<2.0"}},
    },
    "other/keep": {
        "1.0": {"url": "file://" + keep_v1, "sha256": keep_hash1},
        "1.1": {"url": "file://" + keep_v2, "sha256": keep_hash2},
    },
}}
with open(os.path.join(TMP, "brat-index.json"), "w", encoding="utf-8") as f:
    json.dump(update_index, f)
if os.path.isdir(os.path.join(TMP, "vendor")):
    shutil.rmtree(os.path.join(TMP, "vendor"))
try:
    os.remove(os.path.join(TMP, "brat.lock"))
except FileNotFoundError:
    pass
for package in ("acme/util@1.0", "acme/core@1.0",
                "acme/app@1.0", "other/keep@1.0"):
    r = pkg("add", package)
    assert r.returncode == 0, r.stdout + r.stderr

before_lock = open(os.path.join(TMP, "brat.lock"), "rb").read()
core_path = os.path.join(TMP, "vendor", "core.brat")
before_core = open(core_path, "rb").read()
r = pkg("update", "acme/core", "--dry-run")
assert r.returncode == 0 and "would update acme/core 1.0 " in r.stdout, r.stdout + r.stderr
assert "would update acme/util 1.0 " in r.stdout, r.stdout
assert open(os.path.join(TMP, "brat.lock"), "rb").read() == before_lock, \
       "dry-run changed the lockfile"
assert open(core_path, "rb").read() == before_core, "dry-run changed vendor files"

r = pkg("update", "acme/core")
assert r.returncode == 0 and "updated acme/core 1.0 " in r.stdout, r.stdout + r.stderr
assert "updated acme/util 1.0 " in r.stdout, r.stdout
lock = json.load(open(os.path.join(TMP, "brat.lock"), encoding="utf-8"))
locked = {item["name"]: item["version"] for item in lock}
assert locked == {"acme/core": "1.2", "acme/util": "1.2",
                  "acme/app": "1.0", "other/keep": "1.0"}, locked
assert open(core_path, "rb").read() == open(core_v2, "rb").read()

r = pkg("update", "--dry-run")
assert r.returncode == 0, r.stdout + r.stderr
assert "would update acme/app 1.0 " in r.stdout, r.stdout
assert "would update other/keep 1.0 " in r.stdout, r.stdout
before_lock = open(os.path.join(TMP, "brat.lock"), "rb").read()
r = pkg("update")
assert r.returncode == 0, r.stdout + r.stderr
lock = json.load(open(os.path.join(TMP, "brat.lock"), encoding="utf-8"))
locked = {item["name"]: item["version"] for item in lock}
assert locked == {"acme/util": "1.2", "acme/core": "1.2",
                  "acme/app": "1.2", "other/keep": "1.1"}, locked

update_index["packages"]["acme/app"]["1.2"]["dependencies"]["acme/core"] = "<1.2"
update_index["packages"]["acme/core"].pop("1.0")
with open(os.path.join(TMP, "brat-index.json"), "w", encoding="utf-8") as f:
    json.dump(update_index, f)
before_lock = open(os.path.join(TMP, "brat.lock"), "rb").read()
before_core = open(core_path, "rb").read()
r = pkg("update", "acme/core")
assert r.returncode == 1 and "no version of acme/core satisfies" in r.stderr, r.stderr
assert open(os.path.join(TMP, "brat.lock"), "rb").read() == before_lock, \
       "failed update changed the lockfile"
assert open(core_path, "rb").read() == before_core, "failed update changed vendor files"

# A verified package is importable and compiles to a native binary.
write_index()
shutil.rmtree(os.path.join(TMP, "vendor"))
os.remove(os.path.join(TMP, "brat.lock"))
r = pkg("add", "acme/mathx@1.2")
assert r.returncode == 0, r.stderr
main = os.path.join(TMP, "main.brat")
with open(main, "w", encoding="utf-8") as f:
    f.write("import vendor.mathx\n\nfn main() -> i64 {\n  print(mathx.square(7))\n"
            "  return 0\n}\n")
run = subprocess.run(_cxx.cli_args("run", "-O0", main), cwd=TMP, capture_output=True,
                     text=True, timeout=120)
assert run.returncode == 0 and run.stdout == "49\n", run.stdout + run.stderr
exe = os.path.join(TMP, "main.exe")
b = subprocess.run(_cxx.cli_args("build", main, "-o", exe), cwd=TMP,
                   capture_output=True, text=True, timeout=300)
assert b.returncode == 0, (b.stdout + b.stderr)[-600:]
n = subprocess.run([exe], capture_output=True, text=True, timeout=600)
assert n.stdout == "49\n", n.stdout + n.stderr

r = pkg("install")
assert r.returncode == 0 and r.stdout == "ok mathx.brat\n", r.stdout

helper = os.path.join(TMP, "helper-source.brat")
with open(helper, "w", encoding="utf-8") as f:
    f.write("pub fn twice(x: i64) -> i64 { return x * 2 }\n")
helper_digest = hashlib.sha256(open(helper, "rb").read()).hexdigest()
main_pkg = os.path.join(TMP, "main-source.brat")
with open(main_pkg, "w", encoding="utf-8") as f:
    f.write("import helper\npub fn quad(x: i64) -> i64 { return helper.twice(x) * 2 }\n")
r = pkg("publish", "acme/helper@1.0", helper)
assert r.returncode == 0 and r.stdout.startswith("publish acme/helper 1.0 "), r.stdout + r.stderr
r = pkg("publish", "acme/main@1.0", main_pkg, "--depends", "acme/helper@1.0")
assert r.returncode == 0 and r.stdout.startswith("publish acme/main 1.0 "), r.stdout + r.stderr

r = pkg("add", "acme/main@1.0")
assert r.returncode == 0, r.stdout + r.stderr
assert r.stdout.splitlines()[0].startswith("add acme/helper 1.0 "), r.stdout
assert r.stdout.splitlines()[1].startswith("add acme/main 1.0 "), r.stdout
assert os.path.exists(os.path.join(TMP, "vendor", "helper.brat"))
assert os.path.exists(os.path.join(TMP, "vendor", "main.brat"))
main = os.path.join(TMP, "transitive-main.brat")
with open(main, "w", encoding="utf-8") as f:
    f.write("import vendor.main\nfn main() -> i64 {\n  print(main.quad(7))\n  return 0\n}\n")
run = subprocess.run(_cxx.cli_args("run", "-O0", main), cwd=TMP, capture_output=True,
                     text=True, timeout=120)
assert run.returncode == 0 and run.stdout == "28\n", run.stdout + run.stderr

cycle_index = {"packages": {
    "cycle/a": {"1.0": {"url": "file://" + helper, "sha256": helper_digest,
                        "dependencies": {"cycle/b": "1.0"}}},
    "cycle/b": {"1.0": {"url": "file://" + helper, "sha256": helper_digest,
                        "dependencies": {"cycle/a": "1.0"}}},
}}
with open(os.path.join(TMP, "brat-index.json"), "w", encoding="utf-8") as f:
    json.dump(cycle_index, f)
shutil.rmtree(os.path.join(TMP, "vendor"))
os.remove(os.path.join(TMP, "brat.lock"))
r = pkg("add", "cycle/a@1.0")
assert r.returncode == 1 and "dependency cycle: cycle/a -> cycle/b -> cycle/a" in r.stderr, r.stderr
assert not os.path.exists(os.path.join(TMP, "vendor")), "cycle created vendor directory"
assert not os.path.exists(os.path.join(TMP, "brat.lock")), "cycle wrote lock"

vendor = os.path.join(TMP, "vendor")
os.makedirs(vendor, exist_ok=True)
old_file = os.path.join(vendor, "mathx.brat")
with open(old_file, "w", encoding="utf-8") as f:
    f.write("old package\n")
lock_dir = os.path.join(TMP, "brat.lock")
os.makedirs(os.path.join(lock_dir, "blocking_subdir"), exist_ok=True)
try:
    r_fail = pkg("add", "acme/mathx@1.2")
    assert r_fail.returncode != 0, "installation succeeded despite unwritable brat.lock"
finally:
    shutil.rmtree(lock_dir, ignore_errors=True)
with open(old_file, encoding="utf-8") as f:
    assert f.read() == "old package\n", "failed install did not restore prior vendor file"

print("REGISTRY ALL GREEN")
