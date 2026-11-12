"""Native struct declarations, destructuring, exhaustiveness, borrow Copy rules, and .bri/.api."""
import json
import os
import subprocess

import _cxx
import _scratch

BIN = _cxx.build()
TMP = _scratch.mkdtemp(prefix="brat-structs-")


def run_cli(*args, cwd=None):
    return subprocess.run(
        [BIN] + list(args),
        capture_output=True,
        text=True,
        timeout=120,
        cwd=cwd,
    )


def write_src(name, content):
    p = os.path.join(TMP, name)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8") as f:
        f.write(content)
    return p


def assert_three_way(name, src, expected_stdout):
    path = write_src(name + ".brat", src)
    rc_chk = run_cli("check", path)
    assert rc_chk.returncode == 0, f"{name} check failed:\n{rc_chk.stdout}\n{rc_chk.stderr}"
    for mode in (["-O0"], ["-O1"], ["-O2"]):
        r = run_cli("run", *(mode + [path]))
        assert r.returncode == 0, f"{name} {mode} failed:\n{r.stdout}\n{r.stderr}"
        assert r.stdout == expected_stdout, (
            f"{name} {mode} output mismatch: got {r.stdout!r}, want {expected_stdout!r}"
        )


# 1. Nominal structs, shorthand literals, static/instance impl methods, nested destructuring,
#    exhaustive struct match without wildcard, scalar Copy field/element borrows, acyclic vec push.
assert_three_way(
    "structs_e2e",
    """pub struct Point {
  x: i64,
  y: i64,
}

struct Rect {
  origin: Point,
  w: i64,
  h: i64,
}

enum Shape {
  Dot(p),
  Box(r),
}

impl Point {
  fn new(x: i64, y: i64) -> Point {
    return Point { x, y }
  }
  fn translate(mut self, dx: i64, dy: i64) -> i64 {
    let old_x = self.x
    self.x = old_x + dx
    self.y = self.y + dy
    return self.x + self.y
  }
  fn norm2(self) -> i64 {
    return self.x * self.x + self.y * self.y
  }
}

fn classify_point(p: Point) -> str {
  match p {
    Point { x: 0, y: 0 } => { return "origin" }
    Point { x: 0, y } => { return "y:{y}" }
    Point { x, y: 0 } => { return "x:{x}" }
    Point { x, y } => { return "xy:{x},{y}" }
  }
}

fn shape_score(s: Shape) -> i64 {
  match s {
    Dot(Point { x: 0, y: 0 }) => { return 0 }
    Dot(Point { x, y }) => { return x + y }
    Box(Rect { origin: Point { x, y }, w, h }) => { return x * 100 + y * 10 + w * h }
  }
  return -1
}

fn main() -> i64 {
  let mut p = Point.new(3, 4)
  let px = p.x
  p.x = 6
  assert(px == 3 && p.norm2() == 52)
  assert(p.translate(4, 1) == 15)

  let Point { x: qx, y: qy } = p
  assert(qx == 10 && qy == 5)

  let mut pts: vec[Point] = []
  pts.push(Point.new(1, 2))
  pts.push(Point { x: qx, y: qy })
  assert(len(pts) == 2 && pts[1].x == 10)

  let mut nums: vec[i64] = [7, 8]
  let first = nums[0]
  nums.push(first)
  assert(len(nums) == 3 && nums[2] == 7)

  let cfg = {"host": "localhost", "port": 9000}
  let { host, port } = cfg
  assert(host == "localhost" && port == 9000)

  if let Point { x: 10, y } = p {
    assert(y == 5)
  } else {
    assert(false)
  }

  print(classify_point(Point.new(0, 0)))
  print(classify_point(Point.new(0, 9)))
  print(classify_point(Point.new(4, 0)))
  print(classify_point(Point.new(2, 3)))
  print(shape_score(Dot(Point.new(0, 0))))
  print(shape_score(Dot(Point.new(5, 6))))
  print(shape_score(Box(Rect { origin: Point.new(1, 2), w: 3, h: 4 })))
  return 0
}
""",
    "origin\ny:9\nx:4\nxy:2,3\n0\n11\n132\n",
)


# 2. Negative type & borrow checks: nominal distinction, missing/extra/bad-type fields,
#    guarded struct match non-exhaustiveness, and non-Copy str field borrow protection.
NEG_CASES = [
    (
        "nominal_mismatch",
        """struct Point { x: i64, y: i64 }
struct Vel { x: i64, y: i64 }
fn take_point(p: Point) -> i64 { return p.x }
fn main() -> i64 {
  let v = Vel { x: 1, y: 2 }
  return take_point(v)
}
""",
        "want Point, got Vel",
    ),
    (
        "missing_field",
        """struct Point { x: i64, y: i64 }
fn main() -> i64 {
  let p = Point { x: 1 }
  return p.x
}
""",
        "missing field `y`",
    ),
    (
        "unknown_field",
        """struct Point { x: i64, y: i64 }
fn main() -> i64 {
  let p = Point { x: 1, y: 2, z: 3 }
  return p.x
}
""",
        "no field `z`",
    ),
    (
        "wrong_field_type",
        """struct Point { x: i64, y: i64 }
fn main() -> i64 {
  let p = Point { x: "bad", y: 2 }
  return p.y
}
""",
        "wants i64, got str",
    ),
    (
        "non_copy_str_field_borrow",
        """struct Label { text: str, id: i64 }
fn main() -> i64 {
  let mut l = Label { text: "hello", id: 1 }
  let view = l.text
  l.text = "world"
  print(view)
  return 0
}
""",
        "while borrowed by `view`",
    ),
    (
        "guarded_struct_arm_not_exhaustive",
        """struct Point { x: i64, y: i64 }
fn check_p(p: Point) -> i64 {
  match p {
    Point { x, y } if x > 0 => { return y }
  }
}
fn main() -> i64 { return check_p(Point { x: 1, y: 2 }) }
""",
        "non-exhaustive",
    ),
]

for cname, csrc, needle in NEG_CASES:
    p = write_src("neg_" + cname + ".brat", csrc)
    r = run_cli("check", p)
    combined = r.stdout + r.stderr
    assert r.returncode != 0 and needle in combined, (
        f"expected {cname} to fail with {needle!r}, got rc={r.returncode}:\n{combined}"
    )


# 3. Cross-module `pub struct` + `.bri` verification + `brat api --json` + `brat doc --json`.
mod_dir = os.path.join(TMP, "pkgmod")
os.makedirs(mod_dir, exist_ok=True)
write_src(
    "pkgmod/geom.brat",
    """/// 2D integer vector.
pub struct Vec2 {
  x: i64,
  y: i64,
}

pub fn make_vec(x: i64, y: i64) -> Vec2 {
  return Vec2 { x, y }
}

pub fn dot(a: Vec2, b: Vec2) -> i64 {
  return a.x * b.x + a.y * b.y
}
""",
)
write_src(
    "pkgmod/geom.bri",
    """pub struct Vec2 { x: i64, y: i64 }
fn make_vec(x: i64, y: i64) -> Vec2
fn dot(a: Vec2, b: Vec2) -> i64
""",
)
app_p = write_src(
    "pkgmod/app.brat",
    """import geom

fn main() -> i64 {
  let a = geom.make_vec(3, 4)
  let b = Vec2 { x: 5, y: 6 }
  print(geom.dot(a, b))
  return 0
}
""",
)
r_app = run_cli("run", "-O2", app_p)
assert r_app.returncode == 0 and r_app.stdout == "39\n", r_app.stdout + r_app.stderr

r_api = run_cli("api", "--json", app_p)
assert r_api.returncode == 0, r_api.stderr
api_data = json.loads(r_api.stdout)
assert "Vec2" in json.dumps(api_data), f"Vec2 missing from brat api --json: {api_data}"

r_doc = run_cli("doc", "--json", os.path.join(mod_dir, "geom.brat"))
assert r_doc.returncode == 0, r_doc.stderr
doc_data = json.loads(r_doc.stdout)
assert "Vec2" in json.dumps(doc_data), f"Vec2 missing from brat doc --json: {doc_data}"

# Verify `.bri` drift detection when a struct field type disagrees.
write_src(
    "pkgmod/geom.bri",
    """pub struct Vec2 { x: str, y: i64 }
fn make_vec(x: i64, y: i64) -> Vec2
fn dot(a: Vec2, b: Vec2) -> i64
""",
)
r_drift = run_cli("check", app_p)
assert r_drift.returncode != 0, "expected .bri struct drift to fail check"

print("STRUCTS ALL GREEN")
