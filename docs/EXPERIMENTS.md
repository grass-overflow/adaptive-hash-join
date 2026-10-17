# Experimental protocol

## Commands

Windows/MinGW:

```powershell
.\scripts\build.ps1
.\scripts\smoke_test.ps1
.\scripts\run_benchmark.ps1 -Size 100000 -Skew 1.25 -Seed 11
.\.venv\Scripts\python.exe scripts\run_sweep.py --binary build\hashjoin.exe --output results\raw_measurements.csv
.\.venv\Scripts\python.exe scripts\analyze.py --input results\raw_measurements.csv --output-dir results
```

Linux/WSL with GCC, CMake, OpenMP, Python, pandas, and matplotlib:

```bash
bash scripts/build.sh
ctest --test-dir build --output-on-failure
bash scripts/run_benchmark.sh 100000 1.25 11 4 3
python3 scripts/run_sweep.py --binary build/hashjoin --output results/raw_measurements.csv
python3 scripts/analyze.py --input results/raw_measurements.csv --output-dir results
```

The sweep defaults to equal 100k and 500k relation sizes, z={0, 0.75, 1.25, 1.5}, seeds={11, 29, 47}, four workers, one warm-up, three timed repetitions, and a maximum of 8 non-empty coarse partitions per packed task. `novelty_packed` uses q=32 (B=128 at four workers) and continues to respect that fine-chunk budget. This may take several minutes. Use smaller settings for quick runs, e.g. `--sizes 20000 --skews 0,1.5 --seeds 11,29 --repetitions 2`. For five million rows per side, pass `--sizes 5000000`; memory headroom depends on the machine and the novelty candidate chunk vectors are not the dominant memory cost.

The CLI can run an individual diagnostic trial:

```powershell
.\build\hashjoin.exe --r-size 100000 --s-size 100000 --key-range 1000000 --skew 1.25 --seed 11 --threads 4 --policy all --repetitions 3 --warmups 1 --diagnostics --csv results\trial.csv
```

`--policy novelty_packed --coarse-batch 8 --pack-threshold 16` selects the adaptive policy. Below the max/average imbalance threshold it falls back to unsplit coarse dynamic tasks; at or above the threshold it applies bounded chunk allocation and bundles unselected partitions. Its gate state, fine chunks, and coarse bundles are reported; total task count remains separate from the fine-chunk budget.

The q/batch tuning helper should run on development seeds first, then final comparisons should use previously unused confirmation seeds:

```powershell
.\.venv\Scripts\python.exe scripts\tune_novelty.py --binary build\hashjoin.exe --output results\packed_tuning.csv --size 1000000 --skews 0,0.75,1.5 --seeds 801,803,805 --q-values 32 --batch-values 4,8,16,32
```

The final adaptive-policy dataset in `results/adaptive_gate_final.csv` uses seeds 1401, 1403, 1407, 1409, and 1411, 10 repetitions, and `--pack-threshold 16`. Since the threshold was selected after exploratory measurements, describe this as fresh-seed validation of a heuristic rather than a preregistered threshold experiment.

## Measurement definitions and controls

- Generator seed and input relations remain identical across policies in an invocation; each sweep cell uses the same seed and parameters for every policy.
- `partition_ms`, `build_ms`, and `probe_ms` are phase wall times. End-to-end starts immediately before radix partitioning and stops after join probing; generation and oracle time are excluded.
- Build hash tables are built once for each partition in a parallel static loop for all parallel policies. Probe tasks distinguish static, coarse dynamic, paper-style chunks, and budgeted chunks. Thus comparisons isolate probe scheduling and do not reproduce the paper's fused per-partition build/probe cache order.
- The sequential policy uses the same radix partitioning and local hash-table representation, but sequentially.
- `max_to_average` is maximum probe partition cardinality divided by mean cardinality over every final partition (including empty partitions).
- `lambda_partition` is a CSV alias for the same partition skew metric: `max_partition_probe_tuples / average_probe_partition_tuples`. It is an input-imbalance diagnostic and is shared by scheduling policies for the same generated input. `worker_probe_max_to_average` is the maximum worker-assigned probe tuple count divided by the mean across configured workers; `worker_task_max_to_average` analogously compares assigned task counts. These worker metrics describe observed task assignment, not task stealing.
- `planning_ms` times task-plan construction and is included in end-to-end time. `planning_overhead_pct` can be derived as `100 * planning_ms / end_to_end_ms`. `probe_ms` combines tuple probe work with OpenMP dispatch; dispatch-only overhead is not isolated.
- Actual steal counts are unavailable: OpenMP dynamic work-sharing does not expose portable per-loop steal counters. CSV marks `steal_count_status=unavailable_openmp_runtime`; do not infer steal counts from worker assignments.
- `speedup` is sequential end-to-end time divided by policy end-to-end time for the same repetition; parallel efficiency divides by configured worker count. A one-thread result is useful for overhead comparisons, not a parallel-efficiency claim.
- CSV rows include correctness status, phase times, skew, seed, tasks, candidate counts, chunk budget, and actual fine chunks. Invalid rows abort rather than being plotted.
- `novelty_packed` combines consecutive unselected coarse partitions into bounded bundles; per bundle, at most `coarse_batch` non-empty partitions are processed serially. This reduces queue entries but may increase within-task imbalance, so report both task count and runtime.
- The `novelty_packed` imbalance gate uses `max_to_average >= pack_threshold`; default threshold is 16.0. It was introduced after an exploratory holdout showed a regression when bundling at low/moderate skew. The threshold is a heuristic informed by that exploratory data, not an independently optimized value; fresh-seed results must be described as validation of the gated design, not as a preregistered threshold study.
- `analyze.py` groups medians and means, writes paired runtime comparisons, and bootstraps improvements by resampling independent seed clusters when at least two seeds exist. With fewer than two seeds it labels the interval not estimable.
- The Python plotting step consumes the supplied CSV; plots are not drawn from hand-entered example data.
- For a robust paired metrics report, run `python scripts/robust_evaluate.py --input results/robust_adaptive_evaluation.csv --output-dir results/robust_adaptive_analysis`. It refuses incorrect rows or incomplete policy sets, summarizes paired improvements by independent seed, and produces the full skew/imbalance/speedup/worker-load/overhead plot.
- A fresh q=64 high-skew confirmation is in `results/robust_q64_confirmation.csv` with generated output in `results/robust_q64_confirmation_analysis/`. It uses 12 independent seeds and 10 repetitions per seed. This setting improved the probe phase versus static and coarse-dynamic baselines, but did not improve end-to-end time versus fine-grained baselines; see [RESULTS.md](RESULTS.md).

For final confirmation, use tuning runs separate from confirmation seeds, pin/record the physical machine, compiler version, OS, and power policy, and repeat on Linux as required by the proposal. The current available environment is Windows MinGW; it does not provide WSL, so Linux-specific performance behavior remains unverified. The supplied four-core target is a design target, not a claim about this machine's CPU.
