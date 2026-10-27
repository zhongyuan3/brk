#!/usr/bin/env bash
# 检查 build/ 下所有 .o 中"已定义但项目内无引用"的全局符号
# 用法: ./check_unused.sh [build_dir] [nm_tool]

set -euo pipefail

BUILD_DIR="${1:-build}"
NM="${2:-$(command -v riscv64-unknown-elf-nm || command -v riscv64-linux-gnu-nm || command -v nm)}"

if [[ ! -d "$BUILD_DIR" ]]; then
    echo "错误: 目录不存在: $BUILD_DIR" >&2
    exit 1
fi

if ! command -v "$NM" >/dev/null 2>&1; then
    echo "错误: 找不到 nm 工具: $NM" >&2
    exit 1
fi

echo "使用 nm: $NM"
echo "扫描目录: $BUILD_DIR"
echo

# 所有目标文件（排除 .d 依赖文件）
mapfile -t OBJS < <(find "$BUILD_DIR" -type f -name '*.o' | sort)

if [[ ${#OBJS[@]} -eq 0 ]]; then
    echo "错误: 没有找到任何 .o 文件" >&2
    exit 1
fi

echo "找到 ${#OBJS[@]} 个目标文件"
echo

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

DEFS="$TMPDIR/defs.txt"     # 已定义全局符号:  name
REFS="$TMPDIR/refs.txt"     # 未定义引用:        name

# 1) 已定义的全局符号（外部可见）
#    --extern-only   只看全局符号（排除 static / local）
#    --defined-only  只看已定义
#    -g              去掉调试符号
#    输出格式: "0000000000000000 T name"  -> 取最后一列 + 类型列
"$NM" --extern-only --defined-only -g "${OBJS[@]}" 2>/dev/null \
    | awk 'NF>=3 { print $NF, $2 }' \
    | sort -u > "$TMPDIR/defs.raw"

# 2) 未定义引用（被使用但未在当前对象中定义）
"$NM" --undefined-only -g "${OBJS[@]}" 2>/dev/null \
    | awk 'NF>=2 { print $NF }' \
    | sort -u > "$REFS"

# 3) 提取已定义符号名
awk '{ print $1 }' "$TMPDIR/defs.raw" | sort -u > "$DEFS"

echo "已定义全局符号: $(wc -l < "$DEFS")"
echo "未定义引用符号: $(wc -l < "$REFS")"
echo

# 4) 差集: 在 DEFS 中但不在 REFS 中
comm -23 "$DEFS" "$REFS" > "$TMPDIR/unused.txt"

UNUSED_COUNT=$(wc -l < "$TMPDIR/unused.txt")
echo "疑似未被引用的全局符号: $UNUSED_COUNT"
echo

if [[ "$UNUSED_COUNT" -eq 0 ]]; then
    echo "没有发现未引用的全局符号。"
    exit 0
fi

# 5) 输出带类型和来源文件的报告
#    把 defs.raw 里每个符号的 (类型, 文件) 都关联出来
echo "===== 疑似未被引用的全局符号 ====="
printf '%-8s %-40s %s\n' "TYPE" "SYMBOL" "DEFINED_IN"
printf '%-8s %-40s %s\n' "----" "------" "----------"

while read -r sym; do
    # 找出这个符号在哪些 .o 中被定义（可能有多个，比如 built-in.o 和原 .o 重复）
    grep -E "^${sym} " "$TMPDIR/defs.raw" \
        | awk -v s="$sym" '{ print $2, s }' \
        | sort -u \
        | while read -r type _; do
            # 找到包含该符号定义的具体文件
            for obj in "${OBJS[@]}"; do
                if "$NM" --extern-only --defined-only -g "$obj" 2>/dev/null \
                    | awk 'NF>=3 { print $NF, $2 }' \
                    | grep -qE "^${sym} "; then
                    printf '%-8s %-40s %s\n' "$type" "$sym" "$obj"
                fi
            done
        done
done < "$TMPDIR/unused.txt" | sort -u

echo
echo "完整符号列表（含类型）见: $TMPDIR/defs.raw"
echo "提示: 函数指针、回调注册、外部链接等间接引用不会被 nm 捕获，需人工确认。"
