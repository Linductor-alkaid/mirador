#!/usr/bin/env bash
# M5-06 (SCOPE-08, design section 20): static-library and binary size report
# for one configured build directory. Numbers are only meaningful for the
# machine/toolchain that produced the build (DEC-011); CI never asserts them.
#
# Usage: benchmarks/measure_sizes.sh [build-dir]   (default: build/release)
set -euo pipefail

build_dir="${1:-build/release}"
if [ ! -d "$build_dir" ]; then
    echo "error: build directory '$build_dir' does not exist (configure it first)" >&2
    exit 1
fi

printf '%-42s %12s\n' "artifact" "bytes"
total=0
for artifact in \
    "$build_dir"/libmirador_core.a \
    "$build_dir"/libmirador_image.a \
    "$build_dir"/libmirador_cache.a \
    "$build_dir"/libmirador_geometry.a \
    "$build_dir"/libmirador_fusion.a \
    "$build_dir"/libmirador_render.a \
    "$build_dir"/benchmarks/mirador_bench_change_detection \
    "$build_dir"/benchmarks/mirador_bench_cache_backend \
    "$build_dir"/examples/mirador_example_*; do
    # Modules stay optional (MIRADOR_BUILD_*): absent artifacts are skipped,
    # everything found is summed into the total.
    [ -f "$artifact" ] || continue
    size=$(stat -c %s "$artifact")
    total=$((total + size))
    printf '%-42s %12d\n' "${artifact#"$build_dir"/}" "$size"
done
printf '%-42s %12d\n' "TOTAL (listed artifacts)" "$total"
