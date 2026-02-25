#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

CC="${CC:-cc}"
CFLAGS="${CFLAGS:--std=c11 -O2 -Wall -Wextra -Wpedantic}"

TRACE_DIR="${TESTCASE_DIR:-./testcases}"
TRACE_PATTERN="${TRACE_PATTERN:-*.rep}"

TRACE_FILES=()
while IFS= read -r trace; do
    TRACE_FILES+=("$trace")
done < <(find "$TRACE_DIR" -maxdepth 1 -type f -name "$TRACE_PATTERN" | LC_ALL=C sort)

if [[ ${#TRACE_FILES[@]} -eq 0 ]]; then
    echo "no traces found in: $TRACE_DIR matching pattern: $TRACE_PATTERN" >&2
    echo "set TESTCASE_DIR=/path/to/traces and TRACE_PATTERN='*.rep' when running benchmark.sh" >&2
    exit 1
fi

UMALLOC_BIN="benchmark_umalloc"
LIBC_BIN="benchmark_libc"
CSV_FILE="benchmark_results.csv"

$CC $CFLAGS -o "$UMALLOC_BIN" benchmark.c umalloc.c
$CC $CFLAGS -DUSE_LIBC -o "$LIBC_BIN" benchmark.c

{
    echo "mode,workload,ops,ms,ops_per_sec,ns_per_op,allocs,frees,avg_utilization"
    for trace in "${TRACE_FILES[@]}"; do
        ./"$UMALLOC_BIN" "$trace" "$(basename "$trace")"
    done
    for trace in "${TRACE_FILES[@]}"; do
        ./"$LIBC_BIN" "$trace" "$(basename "$trace")"
    done
} > "$CSV_FILE"

rm -f "$UMALLOC_BIN" "$LIBC_BIN"

echo "Wrote $CSV_FILE"
