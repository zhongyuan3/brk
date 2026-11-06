#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
format-src.py - Recursively format C sources and headers under src/.

Applies the project's .clang-format (Linux kernel style) to every *.c and *.h
file found under src/, skipping vendor/ directories anywhere in the tree.
Requires clang-format >= 11.

Usage: scripts/format-src.py [--dry-run|--check] [path]
  --dry-run   Only list the files that would be formatted.
  --check     Check formatting without modifying files.
  path        Directory to format (default: <repo>/src).
"""

import os
import sys
import shutil
import subprocess
from concurrent.futures import ThreadPoolExecutor, as_completed

# Resolve the repository root as the parent directory of the scripts/ folder.
ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))


def parse_args(argv):
    dry_run = False
    check = False
    srcdir = None

    for arg in argv:
        if arg in ("--dry-run", "-n"):
            dry_run = True
        elif arg in ("--check", "-c"):
            check = True
        elif arg.startswith("-"):
            print(f"error: unknown option: {arg}", file=sys.stderr)
            sys.exit(1)
        else:
            srcdir = arg

    return dry_run, check, srcdir


def collect_files(srcdir):
    """Collect *.c and *.h files under srcdir, pruning any vendor/ directory."""
    files = []
    for dirpath, dirnames, filenames in os.walk(srcdir):
        # Prune any directory named 'vendor' in the tree.
        dirnames[:] = [d for d in dirnames if d != "vendor"]
        for fn in filenames:
            if fn.endswith((".c", ".h")):
                files.append(os.path.join(dirpath, fn))
    return files


def run_clang_format(clang_format, files, extra_args, parallel):
    """Run clang-format on files in parallel. Return True on success."""
    if not files:
        return True

    workers = min(parallel, len(files))

    def run_one(f):
        return subprocess.run(
            [clang_format, *extra_args, "--style=file", f],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )

    ok = True
    with ThreadPoolExecutor(max_workers=workers) as pool:
        futures = {pool.submit(run_one, f): f for f in files}
        for fut in as_completed(futures):
            f = futures[fut]
            try:
                result = fut.result()
            except Exception as e:
                print(f"error: failed to run clang-format on {f}: {e}", file=sys.stderr)
                ok = False
                continue
            if result.returncode != 0:
                if result.stdout:
                    sys.stdout.buffer.write(result.stdout)
                ok = False
    return ok


def main():
    dry_run, check, srcdir = parse_args(sys.argv[1:])

    srcdir = srcdir or os.path.join(ROOT, "src")
    srcdir = os.path.abspath(srcdir)

    clang_format = shutil.which("clang-format")
    if clang_format is None:
        print("error: clang-format not found in PATH", file=sys.stderr)
        return 1

    if not os.path.isdir(srcdir):
        print(f"error: directory not found: {srcdir}", file=sys.stderr)
        return 1

    files = collect_files(srcdir)
    count = len(files)
    if count == 0:
        print(f"No source files found in {srcdir}.")
        return 0

    print(f"Found {count} file(s) under {srcdir} (vendor/ excluded).")

    if dry_run:
        for f in files:
            print(f)
        return 0

    parallel = os.cpu_count() or 1

    if check:
        if run_clang_format(clang_format, files, ["--dry-run", "--Werror"], parallel):
            print("Formatting check passed.")
            return 0
        print("Formatting check failed.", file=sys.stderr)
        return 1

    # Format in place, in parallel.
    run_clang_format(clang_format, files, ["-i"], parallel)
    print("Formatting complete.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
