#!/usr/bin/env python3
"""Build/run the host tests. No game, emulator, SD card, or devkitA64 needed."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    args = parser.parse_args()
    out = ROOT / "obj" / "tests"
    out.mkdir(parents=True, exist_ok=True)
    exe = out / ("js_height.exe" if os.name == "nt" else "js_height")
    flags = {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}
    env = os.environ.copy()
    if Path(args.cc).is_absolute():
        env["PATH"] = str(Path(args.cc).parent) + os.pathsep + env.get("PATH", "")

    def run(command):
        result = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True, **flags)
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
        result.check_returncode()

    run([
        args.cc, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-parameter", "-Isrc/ootgz", "tests/js_height.c",
        "src/ootgz/watches.c", "src/ootgz/settings.c", "src/ootgz/gfx.c",
        "src/ootgz/font.c", "src/ootgz/menu.c", "-lm", "-o", str(exe),
    ])
    run([str(exe), str(out / "js_height.ppm"), str(out / "menu_filler.ppm")])


if __name__ == "__main__":
    main()
