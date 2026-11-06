#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Check/fix header guard format for C/C++ header files.

Rules:
1. If a header file is located under some 'include' directory (including subdirectories),
   the guard macro is based on the path relative to that 'include' directory:
       include/a.h       -> A_H
       include/a/b/c.h   -> A_B_C_H
2. If a header file is NOT under any 'include' directory, the guard is based on the file name:
       xx.h              -> XX_H

Usage:
    python fix_header_guard.py <directory>            # modify files directly, no output
    python fix_header_guard.py <directory> --check    # only print non-conforming header files
"""

import os
import re
import sys
import argparse

# Regex to match the guard macro name in #ifndef XXX / #define XXX lines
IFNDEF_RE = re.compile(r"^(\s*#\s*ifndef\s+)(\S+)(.*)$")
DEFINE_RE = re.compile(r"^(\s*#\s*define\s+)(\S+)(.*)$")


def compute_expected_guard(file_path):
    """Compute the expected header guard macro name for the given file path."""
    parts = os.path.normpath(file_path).split(os.sep)

    # Find the closest 'include' directory (search from the end)
    include_idx = None
    for i in range(len(parts) - 1, -1, -1):
        if parts[i] == "include":
            include_idx = i
            break

    if include_idx is not None:
        rel_parts = parts[include_idx + 1 :]
    else:
        rel_parts = [parts[-1]]

    # Strip the extension from the file name (last part)
    rel_parts = list(rel_parts)
    rel_parts[-1] = os.path.splitext(rel_parts[-1])[0]

    # Split directory names and the file name on '.'
    name_parts = []
    for p in rel_parts:
        name_parts.extend(p.split("."))

    # Filter out empty strings
    name_parts = [p for p in name_parts if p]

    guard = "_".join(name_parts).upper() + "_H"
    # Replace any non-alphanumeric characters with underscores
    guard = re.sub(r"[^A-Z0-9_]", "_", guard)
    return guard


def get_current_guard(lines):
    """Extract the currently used guard macro (the first macro found in #ifndef)."""
    for line in lines:
        m = IFNDEF_RE.match(line)
        if m:
            return m.group(2)
    return None


def fix_file(file_path, expected_guard):
    """Replace the old guard macro with the expected one. Return True if modified."""
    with open(file_path, "r", encoding="utf-8", errors="ignore") as f:
        content = f.read()
    lines = content.splitlines(keepends=True)

    old_guard = get_current_guard(lines)
    if old_guard is None:
        # No #ifndef found, skip this file
        return False
    if old_guard == expected_guard:
        return False

    new_lines = []
    for line in lines:
        m = IFNDEF_RE.match(line)
        if m and m.group(2) == old_guard:
            line = m.group(1) + expected_guard + m.group(3) + "\n"
        else:
            m2 = DEFINE_RE.match(line)
            if m2 and m2.group(2) == old_guard:
                line = m2.group(1) + expected_guard + m2.group(3) + "\n"
        new_lines.append(line)

    with open(file_path, "w", encoding="utf-8") as f:
        f.writelines(new_lines)
    return True


def iter_header_files(root):
    """Recursively yield all header files under the root directory."""
    for dirpath, _dirnames, filenames in os.walk(root):
        for fn in filenames:
            if fn.endswith((".h", ".hpp", ".hh", ".hxx", ".H", ".h++")):
                yield os.path.join(dirpath, fn)


def main():
    parser = argparse.ArgumentParser(
        description="Check/fix header guard format for C/C++ header files."
    )
    parser.add_argument("directory", help="Directory to check recursively")
    parser.add_argument(
        "--check",
        action="store_true",
        help="Only print non-conforming header files; do not modify them",
    )
    args = parser.parse_args()

    root = args.directory
    if not os.path.isdir(root):
        print(f"Error: {root} is not a directory", file=sys.stderr)
        return 1

    for file_path in iter_header_files(root):
        expected = compute_expected_guard(file_path)
        with open(file_path, "r", encoding="utf-8", errors="ignore") as f:
            lines = f.readlines()
        current = get_current_guard(lines)

        if current == expected:
            continue

        if args.check:
            # Print file path, current guard macro, and expected guard macro
            print(f'{file_path}: current="{current}", expected="{expected}"')
        else:
            fix_file(file_path, expected)

    return 0


if __name__ == "__main__":
    sys.exit(main())
