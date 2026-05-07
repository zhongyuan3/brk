#!/bin/bash

DEFAULT_DIRS=("boot" "drivers" "fs" "include" "kernel" "lib" "mm")

if [ $# -gt 0 ]; then
    target_dirs=("$@")
else
    target_dirs=("${DEFAULT_DIRS[@]}")
fi

total_lines=0

for dir in "${target_dirs[@]}"; do
    if [ ! -d "$dir" ]; then
        echo "❌ Error: Directory '$dir' does not exist, skipped."
        continue
    fi

    echo "📃 Counting [$dir]..."

    count=$(find "$dir" -type f \( -name "*.c" -o -name "*.h" -o -name "*.S" \) -exec wc -l {} + 2>/dev/null | awk 'END { print $1+0 }')

    echo "📂 Directory [$dir]: $count lines"

    total_lines=$((total_lines + count))
done

echo "--------------------------------"
echo "✅ Total lines: $total_lines"
