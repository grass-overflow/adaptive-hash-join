#!/usr/bin/env bash
set -euo pipefail
size="${1:-100000}"
skew="${2:-1.25}"
seed="${3:-11}"
threads="${4:-4}"
repetitions="${5:-3}"
mkdir -p results
output="results/benchmark_size${size}_skew${skew}_seed${seed}.csv"
./build/hashjoin --r-size "$size" --s-size "$size" --key-range 1000000 \
  --skew "$skew" --seed "$seed" --threads "$threads" --policy all \
  --warmups 1 --repetitions "$repetitions" --diagnostics --csv "$output"
printf 'Wrote %s\n' "$output"
