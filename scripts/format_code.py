#!/usr/bin/env python3
"""Batch C/C++ source formatter using clang-format."""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

DEFAULT_STYLE = "file"
BACKUP_SUFFIX = ".bak"
DEFAULT_TARGET_DIRS = ("arch", "drivers", "fs", "include", "kernel", "lib", "mm")
DEFAULT_EXTENSIONS = ("c", "h")

RED = "\033[0;31m"
GREEN = "\033[0;32m"
YELLOW = "\033[1;33m"
BLUE = "\033[0;34m"
NC = "\033[0m"

EXT_PATTERN = re.compile(r"^[A-Za-z0-9]+$")


def color(text: str, code: str) -> str:
    if not sys.stdout.isatty():
        return text
    return f"{code}{text}{NC}"


def parse_extensions(raw: str) -> list[str]:
    exts: list[str] = []
    for part in raw.split(","):
        ext = part.strip()
        if not ext:
            continue
        if not EXT_PATTERN.fullmatch(ext):
            print(color(f"Error: Invalid extension '{ext}'", RED), file=sys.stderr)
            sys.exit(1)
        exts.append(ext)
    if not exts:
        print(color("Error: No valid file extensions specified", RED), file=sys.stderr)
        sys.exit(1)
    return exts


def resolve_directories(dirs: list[str]) -> list[Path]:
    resolved: list[Path] = []
    seen: set[Path] = set()

    for raw in dirs:
        path = Path(raw)
        if not path.is_dir():
            print(color(f"Error: Directory '{raw}' does not exist", RED))
            sys.exit(1)
        abs_path = path.resolve()
        if abs_path not in seen:
            seen.add(abs_path)
            resolved.append(abs_path)

    return resolved


def collect_files(directory: Path, extensions: list[str]) -> list[Path]:
    files: list[Path] = []
    for ext in extensions:
        files.extend(directory.rglob(f"*.{ext}"))
    return sorted({f.resolve() for f in files if f.is_file()})


def check_clang_format() -> str:
    clang_format = shutil.which("clang-format")
    if clang_format is None:
        print(color("Error: clang-format command not found", RED))
        print("Please install clang-format:")
        print("  Ubuntu/Debian: sudo apt-get install clang-format")
        print("  CentOS/RHEL:   sudo yum install clang-format")
        print("  macOS:         brew install clang-format")
        sys.exit(1)
    return clang_format


def format_file(
    file_path: Path,
    dir_context: Path,
    *,
    clang_format: str,
    style: str,
    backup_enabled: bool,
    dry_run: bool,
    verbose: bool,
) -> bool:
    rel_path = file_path.relative_to(dir_context)
    display = f"{dir_context}/{rel_path}"

    if dry_run:
        print(color(f"[DRY RUN] Would format: {display}", YELLOW))
        return True

    backup_path = file_path.with_name(file_path.name + BACKUP_SUFFIX)
    if backup_enabled:
        shutil.copy2(file_path, backup_path)
        if verbose:
            print(color(f"[BACKUP] {rel_path} -> {rel_path}{BACKUP_SUFFIX}", BLUE))

    cmd = [clang_format, "-i", f"-style={style}", str(file_path)]
    if verbose:
        print(color(f'[EXEC] {" ".join(cmd)}', BLUE))

    try:
        subprocess.run(cmd, check=True, capture_output=not verbose, text=True)
    except subprocess.CalledProcessError as exc:
        print(color(f"[FAILED] Failed to format: {display}", RED))
        if verbose and exc.stderr:
            print(exc.stderr, file=sys.stderr)
        if backup_enabled and backup_path.is_file():
            backup_path.replace(file_path)
            print(color(f"[RESTORED] Restored original file: {display}", YELLOW))
        return False

    print(color(f"[SUCCESS] Formatted: {display}", GREEN))
    return True


def cleanup_backups(directories: list[Path], *, processed: int, dry_run: bool) -> None:
    if processed == 0 or dry_run:
        return

    print("----------------------------------------")
    if not sys.stdin.isatty():
        print(color("Non-interactive mode: backup files preserved", BLUE))
        return

    prompt = color("Delete all backup files from all directories? (y/N): ", YELLOW)
    response = input(prompt).strip()
    if response.lower() != "y":
        print(color("Backup files preserved in directories", BLUE))
        return

    deleted = 0
    for directory in directories:
        for backup in directory.rglob(f"*{BACKUP_SUFFIX}"):
            if backup.is_file():
                backup.unlink()
                deleted += 1

    print("Deleting backup files...")
    print(color(f"Backup files deleted ({deleted})", GREEN))


def build_parser() -> argparse.ArgumentParser:
    defaults = " ".join(DEFAULT_TARGET_DIRS)
    parser = argparse.ArgumentParser(
        description="Format C/C++ source files using clang-format.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Note:\n"
            "  clang-format is required\n"
            "  When using 'file' style, clang-format looks for .clang-format\n"
            "  in each target directory and its parent directories"
        ),
    )
    parser.add_argument(
        "directories",
        nargs="*",
        help=f"Target directories (default: {defaults})",
    )
    parser.add_argument(
        "-s",
        "--style",
        default=DEFAULT_STYLE,
        help=f"Code style (default: {DEFAULT_STYLE})",
    )
    backup = parser.add_mutually_exclusive_group()
    backup.add_argument(
        "-b",
        "--backup",
        action="store_true",
        help="Create backup files before formatting",
    )
    backup.add_argument(
        "-n",
        "--no-backup",
        action="store_true",
        help="Do not create backup files (default)",
    )
    parser.add_argument(
        "-e",
        "--extensions",
        default=",".join(DEFAULT_EXTENSIONS),
        help='File extensions, comma-separated (default: "c,h")',
    )
    parser.add_argument("-v", "--verbose", action="store_true", help="Verbose output")
    parser.add_argument(
        "-d",
        "--dry-run",
        action="store_true",
        help="Preview files without modifying them",
    )
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    target_dirs = args.directories or list(DEFAULT_TARGET_DIRS)
    extensions = parse_extensions(args.extensions)
    backup_enabled = args.backup
    directories = resolve_directories(target_dirs)

    print(color("Configuration:", BLUE))
    print(f"  Code style: {args.style}")
    print(f"  File extensions: {args.extensions}")
    print(f"  Backup mode: {'enabled' if backup_enabled else 'disabled'}")
    if args.dry_run:
        print(color("  [Dry Run Mode] No files will be actually modified", YELLOW))
    print(color(f"Target directories ({len(directories)}):", BLUE))
    for directory in directories:
        print(f"  - {directory}")
    print("----------------------------------------")

    clang_format = check_clang_format()

    total_files = 0
    processed_count = 0
    error_count = 0

    for directory in directories:
        print(color(f"Processing directory: {directory}", BLUE))
        if args.verbose:
            print(color(f"Search extensions: {args.extensions}", BLUE))

        files = collect_files(directory, extensions)
        total_files += len(files)

        if not files:
            print(color("  No matching source files found", YELLOW))
            print()
            continue

        print(color(f"  Found {len(files)} file(s)", BLUE))

        dir_processed = 0
        dir_errors = 0
        for file_path in files:
            ok = format_file(
                file_path,
                directory,
                clang_format=clang_format,
                style=args.style,
                backup_enabled=backup_enabled,
                dry_run=args.dry_run,
                verbose=args.verbose,
            )
            if ok:
                dir_processed += 1
                processed_count += 1
            else:
                dir_errors += 1
                error_count += 1

        if args.dry_run:
            summary = (
                f"  Directory summary: {color(f'{dir_processed} would be formatted', YELLOW)}, "
                f"{color(f'{dir_errors} failed', RED)}"
            )
        else:
            summary = (
                f"  Directory summary: {color(f'{dir_processed} formatted', GREEN)}, "
                f"{color(f'{dir_errors} failed', RED)}"
            )
        print(summary)
        print()

    print("----------------------------------------")
    if total_files == 0:
        print(color("No files found in any directory", YELLOW))
    else:
        print(color("Overall processing complete!", GREEN))
        print(color(f"Total files found: {total_files}", BLUE))
        if args.dry_run:
            print(color(f"Would format: {processed_count}", YELLOW))
        else:
            print(color(f"Successfully formatted: {processed_count}", GREEN))
        if error_count > 0:
            print(color(f"Failed: {error_count}", RED))

    if backup_enabled and not args.dry_run:
        cleanup_backups(directories, processed=processed_count, dry_run=args.dry_run)

    return 1 if error_count else 0


if __name__ == "__main__":
    sys.exit(main())
