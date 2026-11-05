"""Native CLI tooling and local package workflows match the Python reference."""
import _cxx

import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))


def run(argv, cwd=None, env=None):
    child_env = dict(os.environ)
    if env:
        child_env.update(env)
    return subprocess.run(argv, cwd=cwd, env=child_env, capture_output=True,
                          text=True, timeout=120)


def main():
    native = _cxx.build()
    for args in (["--version"], ["-V"], ["version"]):
        ref = run(_cxx.cli_args(*(args)), env={"BRAT_INTERP": "1"})
        got = run([native] + args)
        assert (got.returncode, got.stdout, got.stderr) == \
               (ref.returncode, ref.stdout, ref.stderr), (args, got.stdout, ref.stdout)
        assert got.returncode == 0 and got.stdout == "brat 0.7.0\n", (args, got.stdout)
    with tempfile.TemporaryDirectory(prefix="brat-cli-") as td:
        os.makedirs(os.path.join(td, "shapes"))
        with open(os.path.join(td, "shapes", "cube.brat"), "w", encoding="utf-8") as f:
            f.write("pub fn area(r: i64) -> i64 { return r * r }\n"
                    "pub fn volume(r: i64) -> i64 { return r * r * r }\n"
                    "fn helper() -> i64 { return 0 }\n")
        with open(os.path.join(td, "shapes", "cube.bri"), "w", encoding="utf-8") as f:
            f.write("fn area(r: i64) -> i64\nfn volume(r: i64) -> i64\n")
        with open(os.path.join(td, "shapes", "circle.brat"), "w", encoding="utf-8") as f:
            f.write("pub fn area(r: f64) -> f64 { return r * r }\n"
                    "pub fn ping() -> i64 { return 1 }\n")
        with open(os.path.join(td, "shapes", "circle.bri"), "w", encoding="utf-8") as f:
            f.write("fn area(r: f64) -> f64\nfn ping() -> i64\n")
        with open(os.path.join(td, "shapes", "empty.brat"), "w", encoding="utf-8") as f:
            f.write("fn helper() -> i64 { return 0 }\n")
        main_file = os.path.join(td, "main.brat")
        with open(main_file, "w", encoding="utf-8") as f:
            f.write("import shapes.cube\nimport shapes.circle\nimport shapes.empty\n"
                    "fn main() -> i64 { return 0 }\n")
        empty_file = os.path.join(td, "empty.brat")
        with open(empty_file, "w", encoding="utf-8") as f:
            f.write("fn main() -> i64 { return 0 }\n")

        # Compare the executable and the Python reference, explicitly disabling
        # native forwarding in the reference process.
        for command, options in (("deps", []), ("deps", ["--json"]),
                                 ("api", []), ("api", ["--json"])):
            args = [command, main_file] + options
            ref = run(_cxx.cli_args(*(args)), env={"BRAT_INTERP": "1"})
            got = run([native] + args)
            assert (got.returncode, got.stdout, got.stderr) == \
                   (ref.returncode, ref.stdout, ref.stderr), (
                       command, options, got.returncode, got.stdout, got.stderr,
                       ref.returncode, ref.stdout, ref.stderr)

        old_api = os.path.join(td, "old.bri")
        new_api = os.path.join(td, "new.bri")
        with open(old_api, "w", encoding="utf-8") as f:
            f.write("fn keep(x: i64) -> i64\nfn change(x: i64) -> i64\n"
                    "fn removed() -> none\n")
        with open(new_api, "w", encoding="utf-8") as f:
            f.write("fn keep(x: i64) -> i64\nfn change(x: f64) -> i64\n"
                    "fn added() -> str\n")
        minor_api = os.path.join(td, "minor.bri")
        with open(minor_api, "w", encoding="utf-8") as f:
            f.write("fn keep(x: i64) -> i64\nfn change(x: i64) -> i64\n"
                    "fn removed() -> none\nfn extra() -> bool\n")
        renamed_args = os.path.join(td, "renamed-args.bri")
        with open(renamed_args, "w", encoding="utf-8") as f:
            f.write("fn keep(y: i64) -> i64\nfn change(y: i64) -> i64\n"
                    "fn removed() -> none\n")
        for options in ([], ["--json"]):
            args = ["api", "--diff", old_api, new_api] + options
            ref = run(_cxx.cli_args(*(args)), env={"BRAT_INTERP": "1"})
            got = run([native] + args)
            assert (got.returncode, got.stdout, got.stderr) == \
                   (ref.returncode, ref.stdout, ref.stderr), (args, got.stdout, ref.stdout)
            if options:
                report = json.loads(got.stdout)
                assert report["recommended_bump"] == "major"
                assert [change["kind"] for change in report["changes"]] == \
                       ["added", "changed", "removed"]
                forwarded = run(_cxx.cli_args(*(args)))
                assert (forwarded.returncode, forwarded.stdout, forwarded.stderr) == \
                       (got.returncode, got.stdout, got.stderr)
        for old_path, new_path, expected in ((old_api, old_api, "none"),
                                             (old_api, renamed_args, "none"),
                                             (old_api, minor_api, "minor")):
            args = ["api", "--diff", old_path, new_path, "--json"]
            ref = run(_cxx.cli_args(*(args)), env={"BRAT_INTERP": "1"})
            got = run([native] + args)
            assert (got.returncode, got.stdout, got.stderr) == \
                   (ref.returncode, ref.stdout, ref.stderr), (args, got.stdout, ref.stdout)
            assert json.loads(got.stdout)["recommended_bump"] == expected

        malformed_api = os.path.join(td, "malformed.bri")
        with open(malformed_api, "w", encoding="utf-8") as f:
            f.write("fn broken( -> i64\n")
        invalid_utf8_api = os.path.join(td, "invalid-utf8.bri")
        with open(invalid_utf8_api, "wb") as f:
            f.write(b"fn valid() -> i64\n// invalid \xff\n")
        missing_api = os.path.join(td, "missing.bri")
        for invalid_api in (malformed_api, invalid_utf8_api, missing_api):
            for options in ([], ["--json"]):
                args = ["api", "--diff", old_api, invalid_api] + options
                ref = run(_cxx.cli_args(*(args)), env={"BRAT_INTERP": "1"})
                got = run([native] + args)
                assert (got.returncode, got.stdout, got.stderr) == \
                       (ref.returncode, ref.stdout, ref.stderr), (
                           "invalid .bri", invalid_api, options, got.stdout, got.stderr,
                           ref.stdout, ref.stderr)

        for command in ("deps", "api"):
            options_list = (["--json"], [], ["--dot"]) if command == "deps" else (["--json"], [])
            for options in options_list:
                args = [command, empty_file] + options
                ref = run(_cxx.cli_args(*(args)), env={"BRAT_INTERP": "1"})
                got = run([native] + args)
                assert (got.returncode, got.stdout, got.stderr) == \
                       (ref.returncode, ref.stdout, ref.stderr), (
                           command, "empty", options, got.stdout, ref.stdout)

        dot_args = ["deps", main_file, "--dot"]
        ref = run(_cxx.cli_args(*(dot_args)), env={"BRAT_INTERP": "1"})
        got = run([native] + dot_args)
        assert (got.returncode, got.stdout, got.stderr) == \
               (ref.returncode, ref.stdout, ref.stderr)
        assert got.stdout.startswith("digraph dependencies {\n")
        forwarded = run(_cxx.cli_args(*(dot_args)))
        assert (forwarded.returncode, forwarded.stdout, forwarded.stderr) == \
               (got.returncode, got.stdout, got.stderr)

        graph = os.path.join(td, "graph.json")
        ref = run(_cxx.cli_args("deps", main_file, "-o", graph),
                  env={"BRAT_INTERP": "1"})
        ref_graph = open(graph, encoding="utf-8").read()
        os.unlink(graph)
        got = run([native, "deps", main_file, "-o", graph])
        got_graph = open(graph, encoding="utf-8").read()
        assert (got.returncode, got.stdout, got.stderr, got_graph) == \
               (ref.returncode, ref.stdout, ref.stderr, ref_graph)

        if hasattr(os, "symlink"):
            victim = os.path.join(td, "victim.txt")
            safe_graph = os.path.join(td, "safe-graph.json")
            temp_slot = safe_graph + ".brat-tmp"
            with open(victim, "w", encoding="utf-8") as f:
                f.write("must remain unchanged\n")
            try:
                os.symlink(victim, temp_slot)
            except OSError:
                pass
            else:
                result = run([native, "deps", main_file, "-o", safe_graph])
                assert result.returncode == 0, result.stderr
                with open(victim, encoding="utf-8") as f:
                    assert f.read() == "must remain unchanged\n"
                assert os.path.islink(temp_slot)
                assert os.path.isfile(safe_graph)

        rewritten = run([native, "deps", main_file, "-o", graph])
        assert rewritten.returncode == 0 and rewritten.stdout.startswith("deps ok ")
        with open(graph, encoding="utf-8") as f:
            assert f.read() == ref_graph

        dot_path = os.path.join(td, "graph.dot")
        ref = run(_cxx.cli_args("deps", main_file, "--dot", "-o", dot_path),
                  env={"BRAT_INTERP": "1"})
        ref_dot = open(dot_path, encoding="utf-8").read()
        os.unlink(dot_path)
        got = run([native, "deps", main_file, "--dot", "-o", dot_path])
        got_dot = open(dot_path, encoding="utf-8").read()
        assert (got.returncode, got.stdout, got.stderr, got_dot) == \
               (ref.returncode, ref.stdout, ref.stderr, ref_dot)

        # These two subcommands have no Python dependency on the user-facing path.
        tool_dir = os.path.join(td, "tool-bin")
        os.mkdir(tool_dir)
        env = dict(os.environ, PATH=tool_dir)
        for command in ("deps", "api"):
            result = subprocess.run([native, command, main_file, "--json"],
                                    env=env, capture_output=True, text=True, timeout=120)
            assert result.returncode == 0, result.stderr
            assert result.stdout.startswith("{\n")
        result = subprocess.run([native, "deps", main_file, "--dot"],
                                env=env, capture_output=True, text=True, timeout=120)
        assert result.returncode == 0 and result.stdout.startswith("digraph dependencies {")
        result = subprocess.run([native, "api", "--diff", old_api, new_api, "--json"],
                                env=env, capture_output=True, text=True, timeout=120)
        assert result.returncode == 0 and json.loads(result.stdout)["recommended_bump"] == "major"

        pkg_dir = os.path.join(td, "native-pkg")
        vendor_dir = os.path.join(pkg_dir, "vendor")
        os.makedirs(vendor_dir)
        pkg_file = "native-demo.brat"
        pkg_url = "file:///tmp/native-demo.brat"
        other_file = "other-demo.brat"
        other_url = "file:///tmp/other-demo.brat"
        pkg_items = [{"file": pkg_file, "url": pkg_url, "sha256": "a" * 64},
                     {"file": other_file, "url": other_url, "sha256": "b" * 64}]
        with open(os.path.join(pkg_dir, "brat.lock"), "w", encoding="utf-8") as f:
            json.dump(pkg_items, f)
        for filename in (pkg_file, other_file):
            with open(os.path.join(vendor_dir, filename), "w", encoding="utf-8") as f:
                f.write("fn demo() -> i64 { return 1 }\n")
        pkg_env = dict(os.environ, PATH="")
        listed = subprocess.run([native, "pkg", "list"], cwd=pkg_dir, env=pkg_env,
                                capture_output=True, text=True, timeout=120)
        assert listed.returncode == 0 and listed.stdout == \
               "native-demo.brat  aaaaaaaaaaaa  file:///tmp/native-demo.brat\n" \
               "other-demo.brat  bbbbbbbbbbbb  file:///tmp/other-demo.brat\n", listed.stderr
        ref_pkg_dir = os.path.join(td, "python-pkg")
        ref_vendor_dir = os.path.join(ref_pkg_dir, "vendor")
        os.makedirs(ref_vendor_dir)
        with open(os.path.join(ref_pkg_dir, "brat.lock"), "w", encoding="utf-8") as f:
            json.dump(pkg_items, f)
        for filename in (pkg_file, other_file):
            with open(os.path.join(ref_vendor_dir, filename), "w", encoding="utf-8") as f:
                f.write("fn demo() -> i64 { return 1 }\n")
        ref_listed = run(_cxx.cli_args("pkg", "list"), cwd=ref_pkg_dir,
                         env={"BRAT_INTERP": "1"})
        assert (listed.returncode, listed.stdout, listed.stderr) == \
               (ref_listed.returncode, ref_listed.stdout, ref_listed.stderr)
        removed = subprocess.run([native, "pkg", "remove", pkg_file], cwd=pkg_dir, env=pkg_env,
                                 capture_output=True, text=True, timeout=120)
        ref_removed = run(_cxx.cli_args("pkg", "remove", pkg_file), cwd=ref_pkg_dir,
                          env={"BRAT_INTERP": "1"})
        assert (removed.returncode, removed.stdout, removed.stderr) == \
               (ref_removed.returncode, ref_removed.stdout, ref_removed.stderr)
        assert removed.returncode == 0 and removed.stdout == "removed native-demo.brat\n"
        assert not os.path.exists(os.path.join(vendor_dir, pkg_file))
        assert not os.path.exists(os.path.join(ref_vendor_dir, pkg_file))
        assert os.path.isfile(os.path.join(vendor_dir, other_file))
        assert os.path.isfile(os.path.join(ref_vendor_dir, other_file))
        assert json.load(open(os.path.join(pkg_dir, "brat.lock"), encoding="utf-8")) == [pkg_items[1]]
        assert json.load(open(os.path.join(ref_pkg_dir, "brat.lock"), encoding="utf-8")) == [pkg_items[1]]

        publish_dir = os.path.join(td, "publish")
        os.mkdir(publish_dir)
        publish_source = os.path.join(publish_dir, "math.brat")
        with open(publish_source, "w", encoding="utf-8", newline="\n") as f:
            f.write("fn add(a: i64, b: i64) -> i64 { return a + b }\n")
        index_path = os.path.join(publish_dir, "brat-index.json")
        initial_index = {"packages": {
            "other/lib": {"0.9": {
                "url": "https://example.test/lib.brat", "sha256": "a" * 64,
                "dependencies": {}}},
            "me/math": {
                "1.2.3": {"url": "https://example.test/old-math.brat",
                           "sha256": "b" * 64, "dependencies": {}},
                "2.0": {"url": "https://example.test/math-2.brat",
                        "sha256": "c" * 64, "dependencies": {}},
            },
        }}
        with open(index_path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(initial_index, f, indent=2, ensure_ascii=False)
            f.write("\n")
        initial_index_bytes = open(index_path, "rb").read()
        publish_args = ["pkg", "publish", "me/math@1.2.3", "math.brat",
                        "--depends", "other/lib@>=0.8,<1.0"]
        native_env = {"BRAT_INDEX": index_path, "PATH": ""}
        published = run([native] + publish_args, cwd=publish_dir, env=native_env)
        assert (published.returncode, published.stdout, published.stderr) == \
               (0, "publish me/math 1.2.3 182dc1160625\n", ""), published.stderr
        native_index_bytes = open(index_path, "rb").read()
        artifact_name = ".brat-packages/me/math-1.2.3-182dc11606258d5c.brat"
        native_artifact = open(os.path.join(publish_dir, artifact_name), "rb").read()
        shutil.rmtree(os.path.join(publish_dir, ".brat-packages"))
        with open(index_path, "wb") as f:
            f.write(initial_index_bytes)
        reference_publish = run(_cxx.cli_args(*(publish_args)), cwd=publish_dir,
                                env={"BRAT_INDEX": index_path, "BRAT_NO_NATIVE_CLI": "1"})
        assert (published.returncode, published.stdout, published.stderr) == \
               (reference_publish.returncode, reference_publish.stdout,
                reference_publish.stderr)
        assert native_index_bytes == open(index_path, "rb").read()
        assert native_artifact == open(os.path.join(publish_dir, artifact_name), "rb").read()
        forwarded_publish = run(_cxx.cli_args(*(publish_args)), cwd=publish_dir,
                                env={"BRAT_INDEX": index_path})
        assert (forwarded_publish.returncode, forwarded_publish.stdout,
                forwarded_publish.stderr) == (published.returncode, published.stdout,
                                               published.stderr)
        unchanged_index = open(index_path, "rb").read()
        invalid_publish = (
            ["pkg", "publish", "me/math@bad", "math.brat"],
            ["pkg", "publish", "me/math@1.2.3", "math.brat",
             "--depends", "me/math@*"],
            ["pkg", "publish", "me/math@1.2.3", "math.brat",
             "--depends", "other/lib@>=1", "--depends", "other/lib@<1"],
            ["pkg", "publish", "me/math@1.2.3", "math.brat", "--depends"],
        )
        for invalid_args in invalid_publish:
            native_error = run([native] + invalid_args, cwd=publish_dir, env=native_env)
            ref_error = run(_cxx.cli_args(*(invalid_args)), cwd=publish_dir,
                            env={"BRAT_INDEX": index_path, "BRAT_NO_NATIVE_CLI": "1"})
            assert (native_error.returncode, native_error.stdout, native_error.stderr) == \
                   (ref_error.returncode, ref_error.stdout, ref_error.stderr), (
                       invalid_args, native_error.stdout, native_error.stderr,
                       ref_error.stdout, ref_error.stderr)
            assert open(index_path, "rb").read() == unchanged_index
        remote_index = run([native, "pkg", "publish", "me/math@1.2.3", "math.brat"],
                           cwd=publish_dir,
                           env={"BRAT_INDEX": "https://example.test/index.json"})
        remote_reference = run(_cxx.cli_args("pkg", "publish", "me/math@1.2.3", "math.brat"), cwd=publish_dir,
                               env={"BRAT_INDEX": "https://example.test/index.json",
                                    "BRAT_NO_NATIVE_CLI": "1"})
        assert (remote_index.returncode, remote_index.stdout, remote_index.stderr) == \
               (remote_reference.returncode, remote_reference.stdout, remote_reference.stderr)
        if hasattr(os, "symlink"):
            guarded_dir = os.path.join(td, "publish-symlink")
            outside_dir = os.path.join(td, "publish-outside")
            os.mkdir(guarded_dir)
            os.mkdir(outside_dir)
            guarded_source = os.path.join(guarded_dir, "pkg.brat")
            with open(guarded_source, "w", encoding="utf-8") as f:
                f.write("fn safe() -> i64 { return 1 }\n")
            guarded_index = os.path.join(guarded_dir, "brat-index.json")
            with open(guarded_index, "w", encoding="utf-8") as f:
                f.write('{"packages": {}}\n')
            sentinel = os.path.join(outside_dir, "sentinel")
            with open(sentinel, "w", encoding="utf-8") as f:
                f.write("unchanged\n")
            try:
                os.symlink(outside_dir, os.path.join(guarded_dir, ".brat-packages"))
            except OSError:
                pass
            else:
                rejected = run([native, "pkg", "publish", "safe/pkg@1", "pkg.brat"],
                               cwd=guarded_dir, env={"BRAT_INDEX": guarded_index})
                assert rejected.returncode == 1 and "not a real directory" in rejected.stderr
                assert open(sentinel, encoding="utf-8").read() == "unchanged\n"
                assert json.load(open(guarded_index, encoding="utf-8")) == {"packages": {}}
                assert not os.path.exists(os.path.join(outside_dir,
                                                       "safe", "pkg-1-"
                                                       + "0000000000000000.brat"))

        registry_dir = os.path.join(td, "registry")
        artifact_dir = os.path.join(registry_dir, "artifact files")
        os.makedirs(artifact_dir)
        package_bodies = {
            "dep": "pub fn value() -> i64 { return 7 }\n",
            "root": "import dep\nfn main() -> i64 { return dep.value() }\n",
        }
        package_entries = {}
        for package_file, body in package_bodies.items():
            artifact = pathlib.Path(artifact_dir, package_file + ".brat")
            artifact.write_text(body, encoding="utf-8", newline="")
            package_entries[package_file] = {
                "url": artifact.as_uri(),
                "sha256": hashlib.sha256(artifact.read_bytes()).hexdigest(),
            }
        registry_index = os.path.join(registry_dir, "brat-index.json")
        registry = {"packages": {
            "acme/dep": {
                "1.0": {**package_entries["dep"], "dependencies": {}},
                "1.1": {**package_entries["dep"], "dependencies": {}},
            },
            "acme/root": {
                "2.0": {**package_entries["root"],
                        "dependencies": {"acme/dep": "<1.0"}},
                "1.0": {**package_entries["root"],
                        "dependencies": {"acme/dep": ">=1.0,<2.0"}},
            },
        }}
        with open(registry_index, "w", encoding="utf-8", newline="\n") as f:
            json.dump(registry, f, indent=2, ensure_ascii=False)
            f.write("\n")
        add_args = ["pkg", "add", "acme/root"]
        registry_env = {"BRAT_INDEX": registry_index, "PATH": ""}
        native_add = run([native] + add_args, cwd=registry_dir, env=registry_env)
        expected_add = ("add acme/dep 1.1 %s\nadd acme/root 1.0 %s\n"
                        % (package_entries["dep"]["sha256"][:12],
                           package_entries["root"]["sha256"][:12]))
        assert (native_add.returncode, native_add.stdout, native_add.stderr) == \
               (0, expected_add, ""), (native_add.stdout, native_add.stderr)
        native_lock = json.load(open(os.path.join(registry_dir, "brat.lock"), encoding="utf-8"))
        native_vendor = {name: open(os.path.join(registry_dir, "vendor", name), "rb").read()
                         for name in ("dep.brat", "root.brat")}
        shutil.rmtree(os.path.join(registry_dir, "vendor"))
        os.unlink(os.path.join(registry_dir, "brat.lock"))
        python_add = run(_cxx.cli_args(*(add_args)), cwd=registry_dir,
                         env={"BRAT_INDEX": registry_index, "BRAT_NO_NATIVE_CLI": "1"})
        assert (native_add.returncode, native_add.stdout, native_add.stderr) == \
               (python_add.returncode, python_add.stdout, python_add.stderr)
        assert native_lock == json.load(open(os.path.join(registry_dir, "brat.lock"),
                                             encoding="utf-8"))
        assert native_vendor == {name: open(os.path.join(registry_dir, "vendor", name), "rb").read()
                                 for name in native_vendor}
        forwarded_add = run(_cxx.cli_args(*(add_args)), cwd=registry_dir,
                            env={"BRAT_INDEX": registry_index})
        assert (forwarded_add.returncode, forwarded_add.stdout, forwarded_add.stderr) == \
               (native_add.returncode, native_add.stdout, native_add.stderr)

        shutil.rmtree(os.path.join(registry_dir, "vendor"))
        native_install = run([native, "pkg", "install"], cwd=registry_dir,
                             env={"PATH": ""})
        assert (native_install.returncode, native_install.stdout, native_install.stderr) == \
               (0, "ok dep.brat\nok root.brat\n", "")
        installed_native = {name: open(os.path.join(registry_dir, "vendor", name), "rb").read()
                            for name in ("dep.brat", "root.brat")}
        shutil.rmtree(os.path.join(registry_dir, "vendor"))
        python_install = run(_cxx.cli_args("pkg", "install"), cwd=registry_dir,
                             env={"BRAT_INDEX": registry_index, "BRAT_NO_NATIVE_CLI": "1"})
        assert (native_install.returncode, native_install.stdout, native_install.stderr) == \
               (python_install.returncode, python_install.stdout, python_install.stderr)
        assert installed_native == {name: open(os.path.join(registry_dir, "vendor", name), "rb").read()
                                    for name in installed_native}
        forwarded_install = run(_cxx.cli_args("pkg", "install"), cwd=registry_dir,
                                env={"BRAT_INDEX": registry_index})
        assert (forwarded_install.returncode, forwarded_install.stdout,
                forwarded_install.stderr) == (native_install.returncode,
                                               native_install.stdout, native_install.stderr)

        extra_body = "pub fn extra() -> i64 { return 3 }\n"
        extra_artifact = pathlib.Path(artifact_dir, "extra-1.0.brat")
        extra_artifact.write_text(extra_body, encoding="utf-8", newline="")
        extra_entry = {"url": extra_artifact.as_uri(),
                       "sha256": hashlib.sha256(extra_artifact.read_bytes()).hexdigest(),
                       "dependencies": {}}
        registry["packages"]["acme/extra"] = {"1.0": extra_entry}
        with open(registry_index, "w", encoding="utf-8", newline="\n") as f:
            json.dump(registry, f, indent=2, ensure_ascii=False)
            f.write("\n")
        update_ref_dir = os.path.join(td, "registry-update-reference")
        shutil.copytree(registry_dir, update_ref_dir)
        update_ref_index = os.path.join(update_ref_dir, "brat-index.json")
        extra_args = ["pkg", "add", "acme/extra"]
        native_extra = run([native] + extra_args, cwd=registry_dir, env=registry_env)
        python_extra = run(_cxx.cli_args(*(extra_args)), cwd=update_ref_dir,
                           env={"BRAT_INDEX": update_ref_index, "BRAT_NO_NATIVE_CLI": "1"})
        assert (native_extra.returncode, native_extra.stdout, native_extra.stderr) == \
               (python_extra.returncode, python_extra.stdout, python_extra.stderr)

        dep_v12 = pathlib.Path(artifact_dir, "dep-1.2.brat")
        dep_v12.write_text("pub fn value() -> i64 { return 8 }\n", encoding="utf-8", newline="")
        extra_v2 = pathlib.Path(artifact_dir, "extra-2.0.brat")
        extra_v2.write_text("pub fn extra() -> i64 { return 4 }\n", encoding="utf-8", newline="")
        registry["packages"]["acme/dep"]["1.2"] = {
            "url": dep_v12.as_uri(),
            "sha256": hashlib.sha256(dep_v12.read_bytes()).hexdigest(),
            "dependencies": {},
        }
        registry["packages"]["acme/root"]["1.1"] = {
            **package_entries["root"], "dependencies": {"acme/dep": ">=1.0,<2.0"},
        }
        registry["packages"]["acme/extra"]["2.0"] = {
            "url": extra_v2.as_uri(),
            "sha256": hashlib.sha256(extra_v2.read_bytes()).hexdigest(),
            "dependencies": {},
        }
        with open(registry_index, "w", encoding="utf-8", newline="\n") as f:
            json.dump(registry, f, indent=2, ensure_ascii=False)
            f.write("\n")
        shutil.copyfile(registry_index, update_ref_index)
        targeted_update = ["pkg", "update", "acme/root", "--dry-run"]
        native_update_dry = run([native] + targeted_update, cwd=registry_dir, env=registry_env)
        python_update_dry = run(_cxx.cli_args(*(targeted_update)), cwd=update_ref_dir,
                                env={"BRAT_INDEX": update_ref_index, "BRAT_NO_NATIVE_CLI": "1"})
        assert (native_update_dry.returncode, native_update_dry.stdout, native_update_dry.stderr) == \
               (python_update_dry.returncode, python_update_dry.stdout, python_update_dry.stderr)
        assert native_update_dry.returncode == 0 and "acme/extra" not in native_update_dry.stdout
        forwarded_update_dry = run(_cxx.cli_args(*(targeted_update)), cwd=registry_dir,
                                   env={"BRAT_INDEX": registry_index})
        assert (forwarded_update_dry.returncode, forwarded_update_dry.stdout,
                forwarded_update_dry.stderr) == (native_update_dry.returncode,
                                                  native_update_dry.stdout,
                                                  native_update_dry.stderr)

        targeted_update.pop()
        native_update = run([native] + targeted_update, cwd=registry_dir, env=registry_env)
        python_update = run(_cxx.cli_args(*(targeted_update)), cwd=update_ref_dir,
                            env={"BRAT_INDEX": update_ref_index, "BRAT_NO_NATIVE_CLI": "1"})
        assert (native_update.returncode, native_update.stdout, native_update.stderr) == \
               (python_update.returncode, python_update.stdout, python_update.stderr)
        assert native_update.returncode == 0 and "updated acme/extra" not in native_update.stdout
        assert json.load(open(os.path.join(registry_dir, "brat.lock"), encoding="utf-8")) == \
               json.load(open(os.path.join(update_ref_dir, "brat.lock"), encoding="utf-8"))
        assert {name: open(os.path.join(registry_dir, "vendor", name), "rb").read()
                for name in ("dep.brat", "root.brat", "extra.brat")} == \
               {name: open(os.path.join(update_ref_dir, "vendor", name), "rb").read()
                for name in ("dep.brat", "root.brat", "extra.brat")}

        all_update_dry = ["pkg", "update", "--dry-run"]
        native_all_dry = run([native] + all_update_dry, cwd=registry_dir, env=registry_env)
        python_all_dry = run(_cxx.cli_args(*(all_update_dry)), cwd=update_ref_dir,
                             env={"BRAT_INDEX": update_ref_index, "BRAT_NO_NATIVE_CLI": "1"})
        assert (native_all_dry.returncode, native_all_dry.stdout, native_all_dry.stderr) == \
               (python_all_dry.returncode, python_all_dry.stdout, python_all_dry.stderr)
        assert "would update acme/extra" in native_all_dry.stdout
        all_update = ["pkg", "update"]
        native_all = run([native] + all_update, cwd=registry_dir, env=registry_env)
        python_all = run(_cxx.cli_args(*(all_update)), cwd=update_ref_dir,
                         env={"BRAT_INDEX": update_ref_index, "BRAT_NO_NATIVE_CLI": "1"})
        assert (native_all.returncode, native_all.stdout, native_all.stderr) == \
               (python_all.returncode, python_all.stdout, python_all.stderr)
        unchanged_update = run([native] + all_update_dry, cwd=registry_dir, env=registry_env)
        assert unchanged_update.returncode == 0 and unchanged_update.stdout == \
               "all packages are up to date\n"
        assert open(os.path.join(registry_dir, "vendor", "extra.brat"), "rb").read() == \
               extra_v2.read_bytes()

        direct_source = pathlib.Path(artifact_dir, "native-direct.brat")
        direct_source.write_text("pub fn direct() -> i64 { return 5 }\n",
                                 encoding="utf-8", newline="")
        direct_url = direct_source.as_uri()
        direct_dir = os.path.join(td, "native-direct")
        direct_ref_dir = os.path.join(td, "python-direct")
        os.mkdir(direct_dir)
        os.mkdir(direct_ref_dir)
        direct_args = ["pkg", "add", direct_url]
        native_direct = run([native] + direct_args, cwd=direct_dir, env={"PATH": ""})
        python_direct = run(_cxx.cli_args(*(direct_args)), cwd=direct_ref_dir,
                            env={"BRAT_NO_NATIVE_CLI": "1"})
        assert (native_direct.returncode, native_direct.stdout, native_direct.stderr) == (
            python_direct.returncode, python_direct.stdout, python_direct.stderr)
        direct_sha = hashlib.sha256(direct_source.read_bytes()).hexdigest()[:12]
        assert native_direct.returncode == 0 and native_direct.stdout == \
               "add native-direct.brat %s\n" % direct_sha
        assert json.load(open(os.path.join(direct_dir, "brat.lock"), encoding="utf-8")) == \
               json.load(open(os.path.join(direct_ref_dir, "brat.lock"), encoding="utf-8"))
        forwarded_direct = run(_cxx.cli_args(*(direct_args)), cwd=direct_dir,
                               env={"PATH": ""})
        assert (forwarded_direct.returncode, forwarded_direct.stdout,
                forwarded_direct.stderr) == (native_direct.returncode,
                                              native_direct.stdout, native_direct.stderr)
        native_direct_install = run([native, "pkg", "install"], cwd=direct_dir,
                                    env={"PATH": ""})
        python_direct_install = run(_cxx.cli_args("pkg", "install"), cwd=direct_ref_dir,
                                    env={"BRAT_NO_NATIVE_CLI": "1"})
        assert (native_direct_install.returncode, native_direct_install.stdout,
                native_direct_install.stderr) == (python_direct_install.returncode,
                                                  python_direct_install.stdout,
                                                  python_direct_install.stderr)
        assert open(os.path.join(direct_dir, "vendor", "native-direct.brat"), "rb").read() == \
               open(os.path.join(direct_ref_dir, "vendor", "native-direct.brat"), "rb").read()

        bad_registry = os.path.join(td, "bad-registry")
        os.mkdir(bad_registry)
        bad_artifact = pathlib.Path(bad_registry, "bad.brat")
        bad_artifact.write_text("fn bad() -> i64 { return 0 }\n", encoding="utf-8")
        bad_index = os.path.join(bad_registry, "brat-index.json")
        with open(bad_index, "w", encoding="utf-8") as f:
            json.dump({"packages": {"acme/bad": {"1.0": {
                "url": bad_artifact.as_uri(), "sha256": "0" * 64, "dependencies": {},
            }}}}, f)
        bad_args = ["pkg", "add", "acme/bad"]
        native_bad = run([native] + bad_args, cwd=bad_registry,
                         env={"BRAT_INDEX": bad_index, "PATH": ""})
        python_bad = run(_cxx.cli_args(*(bad_args)), cwd=bad_registry,
                         env={"BRAT_INDEX": bad_index, "BRAT_NO_NATIVE_CLI": "1"})
        assert (native_bad.returncode, native_bad.stdout, native_bad.stderr) == \
               (python_bad.returncode, python_bad.stdout, python_bad.stderr)
        assert native_bad.returncode == 1 and "HASH MISMATCH acme/bad@1.0" in native_bad.stderr
        assert not os.path.exists(os.path.join(bad_registry, "brat.lock"))
        assert not os.path.exists(os.path.join(bad_registry, "vendor"))

        assert "helper" not in run([native, "api", main_file]).stdout

        broken = os.path.join(td, "broken.brat")
        with open(broken, "w", encoding="utf-8") as f:
            f.write("import missing_module\nfn main() -> i64 { return 0 }\n")
        malformed = os.path.join(td, "malformed.brat")
        with open(malformed, "w", encoding="utf-8") as f:
            f.write("fn main( {\n")
        for command in ("deps", "api"):
            for source in (broken, malformed):
                for options in ([], ["--json"]):
                    args = [command, source] + options
                    ref = run(_cxx.cli_args(*(args)), env={"BRAT_INTERP": "1"})
                    got = run([native] + args)
                    assert (got.returncode, got.stdout, got.stderr) == \
                           (ref.returncode, ref.stdout, ref.stderr), (
                               command, source, options, got.stdout, got.stderr,
                               ref.stdout, ref.stderr)

        invalid_cases = (("deps", main_file, "--bogus"), ("api", main_file, "--bogus"),
                         ("api", "--diff", old_api), ("deps", "-o"),
                         ("deps", main_file, "--target", "native"),
                         ("deps", main_file, "--json", "--dot"))
        for args in invalid_cases:
            ref = run(_cxx.cli_args(*(list(args))), env={"BRAT_INTERP": "1"})
            got = run([native] + list(args))
            assert (got.returncode, got.stdout, got.stderr) == \
                   (ref.returncode, ref.stdout, ref.stderr), (
                       "invalid args", args, got.stdout, got.stderr, ref.stdout, ref.stderr)

    print("NATIVE CLI AND LOCAL PKG WORKFLOWS ALL GREEN")


if __name__ == "__main__":
    main()
