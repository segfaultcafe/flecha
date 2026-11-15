#!/usr/bin/env python3
"""Run the behavior reader prototype against a committed flecha-libs/json snapshot."""

import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--json-root", type=Path, required=True)
    parser.add_argument("--json-revision", default="HEAD")
    parser.add_argument("--std-root", type=Path, required=True)
    args = parser.parse_args()
    fixtures = Path(__file__).resolve().parents[1] / "src/tests/fixtures/behaviors/json_prototype"
    revision = subprocess.check_output(
        ["git", "-C", str(args.json_root), "rev-parse", "--verify", args.json_revision + "^{commit}"],
        text=True,
    ).strip()
    with tempfile.TemporaryDirectory(prefix="flecha-behavior-json-") as directory:
        root = Path(directory)
        for name in ("json.fl", "reader.fl", "writer.fl"):
            source = subprocess.check_output(
                ["git", "-C", str(args.json_root), "show", revision + ":src/" + name],
                text=True,
            )
            if name == "json.fl":
                legacy = "export decoration json;"
                if source.count(legacy) != 1:
                    raise SystemExit("Expected the compiler-owned json declaration in the selected revision")
                source = source.replace(legacy, """export decoration json {
    application(structure) target structure;
    application(stringv name) target structure::field;
};""")
            (root / name).write_text(source)
        for fixture in fixtures.glob("*.fl"):
            shutil.copyfile(fixture, root / fixture.name)
        program = root / "prototype"
        compilation = subprocess.run(
            [str(args.compiler.resolve()), "build", str(root / "main.fl"),
             "--root", str(root), "--lib", "std=" + str(args.std_root.resolve()),
             "-o", str(program)], check=False,
        )
        if compilation.returncode:
            raise SystemExit(compilation.returncode)
        result = subprocess.run([str(program)], check=False)
        if result.returncode != 42:
            raise SystemExit(f"Reader prototype returned {result.returncode}; expected 42")
        print(f"Behavior JSON reader prototype passed (json {revision})")


if __name__ == "__main__":
    main()
