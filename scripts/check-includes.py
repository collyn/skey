#!/usr/bin/env python3
"""Audit unused includes using a configured Clang compilation database.

Report-only by default while existing include warnings are being reviewed.
--strict fails on suggestions as well as tool errors. Never edits sources.
"""

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default="build-clang")
    parser.add_argument("--tool", default="clang-include-cleaner")
    parser.add_argument("--strict", action="store_true")
    parser.add_argument("files", nargs="*", help="optional subset of compiled src/*.cpp files")
    args = parser.parse_args()
    tool = shutil.which(args.tool)
    if not tool:
        parser.error(f"{args.tool} not found; install clang-tools or pass --tool")
    root = Path(__file__).resolve().parent.parent
    build = Path(args.build_dir).resolve()
    try:
        entries = json.loads((build / "compile_commands.json").read_text())
    except (OSError, ValueError) as exc:
        parser.error(f"configure a Clang build first: {exc}")
    sources = set()
    for entry in entries:
        source = (Path(entry["directory"]) / entry["file"]).resolve()
        if source.is_relative_to(root / "src") and source.suffix == ".cpp":
            sources.add(source)
    if args.files:
        selected = {Path(name).resolve() for name in args.files}
        if selected - sources:
            parser.error("files not in compilation database: " + ", ".join(map(str, selected - sources)))
        sources = selected
    if not sources:
        parser.error("no project C++ sources found in compilation database")
    suggestions = errors = 0
    for source in sorted(sources):
        print(f"Checking {source.relative_to(root)}", flush=True)
        result = subprocess.run(
            [tool, "-p", str(build), "--disable-insert", "--print=changes", str(source)],
            capture_output=True, text=True,
        )
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
        errors += result.returncode != 0
        suggestions += bool(result.stdout.strip())
    print(f"Include audit: {len(sources)} files, {suggestions} with suggestions, {errors} tool errors.")
    return 1 if errors or (args.strict and suggestions) else 0


if __name__ == "__main__":
    sys.exit(main())
