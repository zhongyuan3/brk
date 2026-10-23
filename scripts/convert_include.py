#!/usr/bin/env python3
"""
递归转换 C/C++/汇编 文件中 #include 头文件引用样式
用法:
    python convert_include.py <目录> <模式>
模式:
    to-quote    : 将 <header> 转换为 "header"
    to-angle    : 将 "header" 转换为 <header>
    toggle      : 自动翻转（<> <-> ""）
    to-quote-sys: 将 <header> 转为 "header"，但仅针对非系统头文件（用户自定义目录）
选项:
    --dry-run   只显示修改，不实际写入
    --backup    修改前生成 .bak 备份
"""

import argparse
import os
import re
import shutil
import sys

# 支持的源文件扩展名
SUPPORTED_EXTS = {".c", ".h", ".S", ".s", ".cpp", ".hpp", ".cc", ".hh"}

# 匹配 #include 行的正则（保留前导空白和 #include 关键字）
# 组1: 前导空白 + # include
# 组2: 引用符号 < 或 "
# 组3: 头文件路径（不含引号/尖括号）
# 组4: 结束符号 > 或 "
INCLUDE_RE = re.compile(
    r'^([ \t]*#[ \t]*include[ \t]+)([<"])([^>"]+)([>"])',
    re.MULTILINE,
)


def is_system_like(header_path: str) -> bool:
    """粗略判断是否像是系统头文件（无扩展名、无路径分隔符、小写带.h等）"""
    # 这些不算系统头：用户自定义通常带 ./ ../ 或路径分隔符，或项目内
    if "/" in header_path or "\\" in header_path:
        return False
    # 常见 C 标准库头
    common_sys = {
        "stdio.h",
        "stdlib.h",
        "string.h",
        "stdint.h",
        "stddef.h",
        "stdbool.h",
        "stdarg.h",
        "ctype.h",
        "errno.h",
        "assert.h",
        "math.h",
        "time.h",
        "limits.h",
        "float.h",
        "signal.h",
        "setjmp.h",
        "locale.h",
        "wchar.h",
        "wctype.h",
        "inttypes.h",
        "unistd.h",
        "fcntl.h",
        "sys",
        "pthread.h",
        "semaphore.h",
        "stdatomic.h",
        "threads.h",
        "uchar.h",
        "complex.h",
        "tgmath.h",
    }
    if header_path in common_sys:
        return True
    # 无扩展名的也当作系统头
    if "." not in os.path.basename(header_path):
        return True
    return False


def convert_line(match, mode: str):
    """根据模式返回转换后的 include 行（或 None 表示不修改）"""
    prefix, open_sym, header, close_sym = match.groups()
    cur_angle = open_sym == "<"
    cur_quote = open_sym == '"'

    # 一致性检查：开始和结束必须匹配
    if (open_sym == "<" and close_sym != ">") or (open_sym == '"' and close_sym != '"'):
        return None  # 异常格式，跳过

    target = None
    if mode == "to-quote":
        if cur_angle:
            target = "quote"
    elif mode == "to-angle":
        if cur_quote:
            target = "angle"
    elif mode == "toggle":
        target = "quote" if cur_angle else "angle"
    elif mode == "to-quote-sys":
        # 只转换系统头：<> -> ""，且只对系统风格头
        if cur_angle and is_system_like(header):
            target = "quote"
    else:
        raise ValueError(f"未知模式: {mode}")

    if target is None:
        return None

    if target == "quote":
        new_line = f'{prefix}"{header}"'
    else:
        new_line = f"{prefix}<{header}>"

    return new_line


def process_file(path: str, mode: str, dry_run: bool, backup: bool) -> int:
    """处理单个文件，返回修改行数"""
    try:
        with open(path, "r", encoding="utf-8", errors="surrogateescape") as f:
            content = f.read()
    except (OSError, UnicodeDecodeError) as e:
        print(f"[跳过] 无法读取 {path}: {e}", file=sys.stderr)
        return 0

    changes = []

    def repl(match):
        new_line = convert_line(match, mode)
        if new_line is None or new_line == match.group(0):
            return match.group(0)
        changes.append((match.group(0), new_line))
        return new_line

    new_content = INCLUDE_RE.sub(repl, content)

    if not changes:
        return 0

    print(f"\n{path}  ({len(changes)} 处修改)")
    for old, new in changes:
        print(f"  - {old.strip()}")
        print(f"  + {new.strip()}")

    if dry_run:
        return len(changes)

    if backup:
        bak = path + ".bak"
        shutil.copy2(path, bak)
        print(f"  已备份到 {bak}")

    try:
        with open(path, "w", encoding="utf-8", errors="surrogateescape") as f:
            f.write(new_content)
    except OSError as e:
        print(f"[错误] 无法写入 {path}: {e}", file=sys.stderr)
        return 0

    return len(changes)


def iter_files(root: str):
    """递归遍历目录下的所有支持文件（跳过 .git / build 等目录）"""
    skip_dirs = {
        ".git",
        ".svn",
        ".hg",
        "node_modules",
        "__pycache__",
        "build",
        "out",
        ".cache",
        "CMakeFiles",
    }
    for dirpath, dirnames, filenames in os.walk(root):
        # 就地过滤跳过目录
        dirnames[:] = [d for d in dirnames if d not in skip_dirs]
        for name in filenames:
            ext = os.path.splitext(name)[1]
            if ext in SUPPORTED_EXTS:
                yield os.path.join(dirpath, name)


def main():
    parser = argparse.ArgumentParser(
        description="递归转换目录下 C/C++/汇编 文件的 #include 头文件引用样式"
    )
    parser.add_argument("directory", help="要处理的根目录")
    parser.add_argument(
        "mode",
        choices=["to-quote", "to-angle", "toggle", "to-quote-sys"],
        help='转换模式：to-quote(<->""), to-angle(""-><>), toggle(自动翻转), '
        'to-quote-sys(仅系统头 <->"")',
    )
    parser.add_argument("--dry-run", action="store_true", help="只显示，不实际修改")
    parser.add_argument("--backup", action="store_true", help="修改前生成 .bak 备份")
    parser.add_argument(
        "--ext",
        nargs="+",
        default=None,
        help="自定义要处理的扩展名，如 --ext .c .h（默认: c,h,S,s,cpp,hpp,cc,hh）",
    )

    args = parser.parse_args()

    if not os.path.isdir(args.directory):
        print(f"错误: {args.directory} 不是目录", file=sys.stderr)
        sys.exit(1)

    global SUPPORTED_EXTS
    if args.ext:
        SUPPORTED_EXTS = {e if e.startswith(".") else "." + e for e in args.ext}

    total_files = 0
    total_changes = 0
    for path in iter_files(args.directory):
        n = process_file(path, args.mode, args.dry_run, args.backup)
        if n:
            total_files += 1
            total_changes += n

    print(
        f"\n{'[DRY-RUN] ' if args.dry_run else ''}"
        f"完成：{total_files} 个文件，{total_changes} 处修改。"
    )


if __name__ == "__main__":
    main()
