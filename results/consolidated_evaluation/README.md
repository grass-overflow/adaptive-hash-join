# Consolidated hash-join evaluation package

This directory gathers generated measurements, analyses, plots, and reports into one place. Files are copied from the original `results/` paths; original artifacts remain unchanged. The most recent/strongest validated conclusions are in **01_final_q64_confirmation**. Do not merge exploratory or legacy data into the final confirmation sample.

See [CONSOLIDATED_REPORT.md](./CONSOLIDATED_REPORT.md) for the measured phase-by-phase results, paired comparisons, task/load metrics, and claim boundaries.

For a presentation-ready plot sequence and accurate progress/claim wording, see [PRESENTATION_GUIDE.md](./PRESENTATION_GUIDE.md).

## Contents

### `01_final_q64_confirmation/`

- `robust_q64_confirmation.csv`: 720 correct rows, 12 fresh seeds × 10 repetitions, z=1.5, q=64.
- `analysis/`: phase metrics, paired bootstrap comparisons, all-metric plots, phase-decomposition plot, and generated report.
- Full-join conclusion: no resolved end-to-end gain over static/dynamic; slower than fine-grained baselines in this confirmation. Probe phase is faster than static and coarse-dynamic scheduling, not clearly better than fine-grained baselines.

### `02_broad_q32_confirmation/`

- `robust_adaptive_final.csv`: 2,160 correct rows, 12 seeds × 10 repetitions, z={0, 0.75, 1.5}, q=32.
- `analysis/`: all-skew summaries, paired intervals, phase breakdown, and plots.
- Shows why task-count reduction or worker-load balance must not be treated as a full-join speedup.

### `03_reproduction_and_earlier_adaptive/`

- The five-seed q=32 reproduction dataset and its focused plot/report, plus an earlier five-seed q=32 dataset and analysis.
- These are useful for traceability, but were superseded by the larger 12-seed confirmations.

### `04_exploratory_tuning/`

- Batch and q development sweeps and summaries.
- Earlier balanced packed holdout/tuning and novelty tuning outputs.
- Tuning data are exploratory and are not independent confirmation data.

### `05_legacy_baselines/`

- Original baseline sweeps, scale component, summaries, and plots from before the final adaptive instrumentation.
- `robust_adaptive_evaluation.csv` is an earlier run; use `02_broad_q32_confirmation/` for the final q=32 comparison.

### `06_smoke_tests/`

- Small CLI/schema smoke measurements and smoke-test analysis only; never use these as performance evidence.

## Metric definitions

- **End-to-end**: radix partition + scheduler plan + hash-table build + probe; data generation and sequential oracle are excluded.
- **Speedup**: sequential end-to-end time divided by policy end-to-end time.
- **Partition λ**: maximum probe tuples in one radix partition divided by average probe tuples across all final partitions, including empty partitions.
- **Worker probe λ**: maximum worker-assigned probe tuples divided by the mean over configured workers. It measures assignment balance, not stealing.
- **Planning overhead**: task-plan construction time and its fraction of end-to-end; probe timing still combines probe work and OpenMP dispatch.
- **Correctness**: every benchmark CSV row must report `correct=true`; CTest also checks exact pair multisets and scheduler coverage.
- **Steal count**: unavailable from the portable OpenMP dynamic-loop interface used here. Do not infer steals from worker assignments.

## Reproduce analyses

From the project root in PowerShell:

```powershell
.\.venv\Scripts\python.exe scripts\robust_evaluate.py --input results\consolidated_evaluation\01_final_q64_confirmation\robust_q64_confirmation.csv --output-dir results\consolidated_evaluation\01_final_q64_confirmation\analysis
.\.venv\Scripts\python.exe scripts\robust_evaluate.py --input results\consolidated_evaluation\02_broad_q32_confirmation\robust_adaptive_final.csv --output-dir results\consolidated_evaluation\02_broad_q32_confirmation\analysis
```

## Claim boundary

Current evidence supports a workload-specific probe-phase improvement versus static and coarse-dynamic scheduling at high skew. It does not support the claim that adaptive packing improves total join time over all strong parallel baselines. Measurements are Windows/MinGW results on one CPU; Linux/WSL, sanitizer, and 5m-tuple confirmation remain unverified. See each generated report for confidence intervals and per-phase values.
