#!/usr/bin/env python3
"""
Check and optionally fix C/C++ header guard naming conventions.

Expected naming:
- include/foo/bar.h  -> _FOO_BAR_H
- include/foo/baz.hpp -> _FOO_BAZ_HPP
- fuz.h              -> _FUZ_H
- internal/fuz.h     -> _FUZ_H

Usage:
    python check_header_guards.py path [path ...]
    python check_header_guards.py --fix path [path ...]
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Iterable

HEADER_SUFFIXES = {".h", ".hh", ".hpp", ".hxx", ".inl"}

GUARD_PATTERN = re.compile(
    r"^\s*#\s*ifndef\s+([A-Za-z_][A-Za-z0-9_]*)\s*$"
    r"(?:(?:[ \t]*\r?\n)[ \t]*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)\s*)?",
    re.MULTILINE,
)

ENDIF_PATTERN = re.compile(
    r"^\s*#\s*endif(?://+|/\*+|\s)\s*([A-Za-z_][A-Za-z0-9_]*)\s*(?:\*/)?\s*$"
)


def collect_headers(paths: Iterable[Path]) -> list[Path]:
    """Collect C/C++ header files from paths or directories."""
    headers: list[Path] = []

    for path in paths:
        if path.is_file():
            if path.suffix.lower() in HEADER_SUFFIXES:
                headers.append(path)
        elif path.is_dir():
            headers.extend(
                p
                for p in path.rglob("*")
                if p.is_file() and p.suffix.lower() in HEADER_SUFFIXES
            )
        else:
            print(f"warning: path does not exist: {path}", file=sys.stderr)

    return sorted(set(headers))


def normalize_component(component: str) -> str:
    """Convert one path component into a valid C/C++ macro component."""
    component = re.sub(r"[^A-Za-z0-9]+", "_", component)
    component = re.sub(r"_+", "_", component).strip("_")
    if not component:
        component = "HEADER"
    if component[0].isdigit():
        component = "_" + component
    return component.upper()


def expected_guard(path: Path) -> str:
    """
    Calculate the expected header guard.

    For a file below an `include` directory, path components after `include`
    are used. Otherwise, only the filename is used because the header is
    treated as module-private.
    """
    normalized_parts = [part for part in path.parts if part not in (".", "..")]

    include_indexes = [
        index
        for index, part in enumerate(normalized_parts)
        if part.lower() == "include"
    ]

    if include_indexes:
        relevant_parts = normalized_parts[include_indexes[-1] + 1 :]
    else:
        relevant_parts = normalized_parts[-1:]

    stem = Path(*relevant_parts).stem
    macro_parts = [normalize_component(part) for part in relevant_parts[:-1]]
    macro_parts.append(normalize_component(stem))
    macro_parts.append(normalize_component(path.suffix.lstrip(".")))

    return "_" + "_".join(macro_parts)


def find_existing_guard(content: str) -> tuple[int, str] | None:
    """Return the index and name of the first `#ifndef` guard, if present."""
    match = re.search(
        r"^\s*#\s*ifndef\s+([A-Za-z_][A-Za-z0-9_]*)\s*$",
        content,
        re.MULTILINE,
    )
    if not match:
        return None
    return match.start(1), match.group(1)


def build_guard_block(macro: str) -> str:
    return f"#ifndef {macro}\n#define {macro}\n"


def replace_guard(content: str, old_macro: str, new_macro: str) -> str:
    """Replace an existing guard while preserving surrounding formatting."""
    if old_macro == new_macro:
        return content

    content = re.sub(
        rf"(#\s*ifndef\s+){re.escape(old_macro)}\b",
        rf"\g<1>{new_macro}",
        content,
        count=1,
    )

    content = re.sub(
        rf"(#\s*define\s+){re.escape(old_macro)}\b",
        rf"\g<1>{new_macro}",
        content,
        count=1,
    )

    content = re.sub(
        rf"(#\s*endif\b[^A-Za-z0-9_]*?){re.escape(old_macro)}\b",
        rf"\g<1>{new_macro}",
        content,
    )

    return content


def add_missing_guard(content: str, macro: str) -> str:
    """Add a header guard if the file does not contain one."""
    if find_existing_guard(content):
        return content

    guard_block = build_guard_block(macro)

    if not content:
        return guard_block + "\n#endif // " + macro + "\n"

    lines = content.splitlines(keepends=True)
    insertion_index = 0

    # Keep a leading UTF-8 BOM in place.
    if lines and lines[0].startswith("\ufeff"):
        insertion_index = 1

    lines.insert(insertion_index, guard_block)

    if not content.endswith("\n"):
        lines.append("\n")

    lines.append("\n#endif // " + macro + "\n")
    return "".join(lines)


def process_file(path: Path, fix: bool) -> bool:
    """
    Check one header file.

    Returns True when the file is valid after processing.
    """
    try:
        content = path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        print(f"error: not a UTF-8 text file: {path}")
        return False
    except OSError as exc:
        print(f"error: cannot read {path}: {exc}")
        return False

    macro = expected_guard(path)
    existing = find_existing_guard(content)
    valid = existing is not None and existing[1] == macro

    if valid:
        print(f"OK:       {path}")
        return True

    if not fix:
        if existing is None:
            print(f"MISSING:  {path} expected {macro}")
        else:
            print(f"MISMATCH: {path} " f"found {existing[1]}, expected {macro}")
        return False

    try:
        if existing is None:
            new_content = add_missing_guard(content, macro)
        else:
            new_content = replace_guard(content, existing[1], macro)

        if new_content != content:
            path.write_text(new_content, encoding="utf-8")

        print(f"FIXED:    {path} -> {macro}")
        return True
    except OSError as exc:
        print(f"error: cannot write {path}: {exc}")
        return False


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Check or fix C/C++ header guards. "
            "Public headers use the path below include/; private headers use the filename."
        )
    )
    parser.add_argument(
        "paths",
        nargs="+",
        type=Path,
        help="Header files or directories to process recursively.",
    )
    parser.add_argument(
        "--fix",
        action="store_true",
        help="Fix invalid or missing header guards.",
    )
    args = parser.parse_args()

    headers = collect_headers(args.paths)

    if not headers:
        print("No header files found.")
        return 0

    failures = 0

    for header in headers:
        if not process_file(header, args.fix):
            failures += 1

    total = len(headers)
    print(f"\nChecked {total} file(s), {failures} problem(s).")

    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
