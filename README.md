# Adaptive Fine-Grained Scheduling for Skew-Resilient Parallel Hash Joins

A compact C++17/OpenMP implementation and experiment harness for radix-partitioned equi-joins. It compares sequential, static partition scheduling, coarse dynamic scheduling, paper-style fine-grained probe partitioning, the bounded size-proportional novelty, and its adaptive `novelty_packed` variant that only enables fine splitting and coarse bundling above an observed imbalance threshold.

## Source material and claim boundary

The supplied paper is Balkesen et al., “Main-Memory Hash Joins on Multi-Core CPUs: Tuning to the Underlying Hardware” (ICDE 2013). It studies hardware-conscious radix join tuning and describes chunking oversized skewed probe partitions, but it does **not** introduce the bounded proportional chunk budget in this project. The PPT and supplied methodology images specify the course-project configuration and proposed budgeted scheduler. The code and distinction are documented in [docs/PAPER_REPRODUCTION.md](docs/PAPER_REPRODUCTION.md) and [docs/NOVELTY.md](docs/NOVELTY.md).

This is not a complete reproduction of the paper's C++ implementation or historical experiments. In particular, this project defaults to 10 radix bits (two 5-bit passes / 1,024 partitions), uses synthetic workloads, and builds partition hash tables in a common phase before probing. This enables safe shared read-only table probes across chunks, but differs from the paper's cache-aware per-partition build-then-probe order.

## Requirements

- C++17 compiler with OpenMP (GCC/G++ recommended), CMake, and Ninja (optional; only used by the Windows helper).
- Python 3 with pandas and matplotlib for sweeps and analysis.
- Linux/WSL is the intended research runtime. The current workspace was verified on Windows with MinGW GCC/OpenMP; WSL was unavailable. Windows timings are useful for exercising the program, not a substitute for confirmation on the target Linux machine.

## Build and validate

Windows PowerShell with the installed MinGW toolchain:

```powershell
.\scripts\build.ps1
ctest --test-dir build --output-on-failure
.\scripts\smoke_test.ps1
```

Linux:

```bash
bash scripts/build.sh
ctest --test-dir build --output-on-failure
```

Optional sanitizer build on GCC platforms with sanitizer runtimes:

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DHJ_ENABLE_SANITIZERS=ON
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure
```

Python packages:

```powershell
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
```

Or on Linux: `python3 -m pip install -r requirements.txt`.

## Run an individual diagnostic

```powershell
.\build\hashjoin.exe --r-size 100000 --s-size 100000 --key-range 1000000 --skew 1.25 --seed 11 --threads 4 --policy all --warmups 1 --repetitions 3 --diagnostics --csv results\trial.csv
```

Available policies are `sequential`, `static`, `dynamic`, `paper_fine`, `novelty_budgeted`, and `novelty_packed`; `--policy all` runs each on the same generated input. Useful options include `--radix-bits`, `--pass1-bits`, `--chunk-size`, `--split-factor`, `--q`, `--coarse-batch`, and `--pack-threshold` (default 16.0 for `novelty_packed`). See `hashjoin --help`.

The default build/probe sizes are 100,000 each, key range 100,000, four threads, 10 radix bits, 5+5 pass split, split factor 2.5, q=32, coarse bundle size 8, and adaptive packing threshold 16.0. Below the threshold, `novelty_packed` falls back to unsplit coarse dynamic tasks; the threshold is a heuristic, not an established optimum. The synthetic generator samples probe keys from build keys, so every generated probe row matches at least one build row. The CLI independently checks the result cardinality before it writes each row.

## Reproducible sweep and analysis

Windows with the configured Python environment:

```powershell
.\.venv\Scripts\python.exe scripts\run_sweep.py --binary build\hashjoin.exe --output results\raw_measurements.csv
.\.venv\Scripts\python.exe scripts\analyze.py --input results\raw_measurements.csv --output-dir results
```

The sweep defaults to two sizes, four skew values, three seeds, one warm-up, and three timed repetitions. `--coarse-batch` configures the maximum number of non-empty unselected partitions in one packed coarse task. For a shorter run:

```powershell
.\.venv\Scripts\python.exe scripts\run_sweep.py --binary build\hashjoin.exe --sizes 20000 --skews 0,1.5 --seeds 11,29 --repetitions 2 --warmups 1 --output results\smoke_sweep.csv
.\.venv\Scripts\python.exe scripts\analyze.py --input results\smoke_sweep.csv --output-dir results\smoke_analysis
```

Linux uses the analogous `python3` commands with `build/hashjoin`. For separate novelty-budget tuning, run `.\.venv\Scripts\python.exe scripts\tune_novelty.py --binary build\hashjoin.exe --output results\packed_tuning.csv --size 1000000 --skews 0,0.75,1.5 --seeds 801,803,805 --q-values 32 --batch-values 4,8,16,32`. Keep tuning seeds separate from the final confirmation seeds.

The analyzer produces data-derived summary/paired CSVs and PNG plots; paired seed-bootstrap intervals are labeled unavailable for fewer than two seeds. Details and metric definitions are in [docs/EXPERIMENTS.md](docs/EXPERIMENTS.md).

For a larger paired validation (for example 12 independent seeds, 10 repetitions, 1m tuples/side, three skew levels), analyze raw rows with:

```powershell
.\.venv\Scripts\python.exe scripts\robust_evaluate.py --input results\robust_adaptive_evaluation.csv --output-dir results\robust_adaptive_analysis
```

This report includes speedup, partition imbalance λ, per-worker assigned probe-load imbalance, plan-construction overhead, task counts, and correctness validation. Actual OpenMP steal counts are not exposed; the report marks them unavailable rather than estimating them.

For the fresh high-skew q=64 confirmation:

```powershell
.\.venv\Scripts\python.exe scripts\run_sweep.py --binary build\hashjoin.exe --output results\robust_q64_confirmation.csv --sizes 1000000 --skews 1.5 --seeds 4103,4111,4127,4129,4133,4139,4153,4157,4159,4177,4201,4211 --repetitions 10 --warmups 1 --threads 4 --q 64 --coarse-batch 8 --pack-threshold 16
.\.venv\Scripts\python.exe scripts\robust_evaluate.py --input results\robust_q64_confirmation.csv --output-dir results\robust_q64_confirmation_analysis
```

For one CLI benchmark from PowerShell, run `.\scripts\run_benchmark.ps1 -Size 100000 -Skew 1.25 -Seed 11`; Linux uses `bash scripts/run_benchmark.sh 100000 1.25 11 4 3`.

## Correctness and output

`hashjoin_tests` exercises empty/tiny inputs, duplicate keys, no matches, all matches, boundary keys, uniform and Zipf-like generated data, one/multiple threads, small task budgets, oversized chunk sizes, exact pair multisets, and task coverage. Build and probe IDs are payload IDs; `JoinResult::pairs` is optional so benchmark runs count matches without materializing potentially large output relations.

Diagnostic stderr includes max/mean probe partition size, oversized candidate/selected counts, configured fine-chunk budget, actual fine chunks, total tasks, cardinality, correctness, and elapsed time. CSV rows include phase and end-to-end times, speedup/efficiency, configuration, task diagnostics, cardinality, and correctness. End-to-end timing includes radix partitioning and local table build/probe, but excludes data generation and the oracle.

## Current results

See [docs/RESULTS.md](docs/RESULTS.md) for the current data-backed interpretation. The latest q=64 confirmation figure is [robust_evaluation_metrics.png](results/robust_q64_confirmation_analysis/robust_evaluation_metrics.png), with its generated [report](results/robust_q64_confirmation_analysis/ROBUST_EVALUATION.md).

## Project map

- `include/hash_join/`, `src/`: data generation, independent sequential oracle, two-pass radix partitioning, task planning, join execution, CLI.
- `tests/`: CTest correctness and scheduler coverage.
- `scripts/`: PowerShell/bash builds and smoke, Python parameter sweep and statistical analysis.
- `docs/`: source-paper boundary, novelty specification, experimental protocol, results status, and viva notes.
