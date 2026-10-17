# Consolidated evaluation report

## Scope and claim

This report consolidates the measured full-join and probe-scheduling evidence in
this archive. The proposed novelty is bounded, size-aware fine-grained scheduling
with partition-task packing (`novelty_packed`). Results support improved
high-skew probe scheduling relative to static and coarse-dynamic policies in one
confirmation configuration. They do **not** support a general end-to-end speedup
claim: partitioning and hash-table build dominate total time, and the strongest
fine-grained baselines are competitive or faster overall.

All times below are medians for 1,000,000 build and 1,000,000 probe tuples on four
threads. Correctness was true for all rows in the corresponding confirmation
datasets. Paired confidence intervals use seed-level bootstrap estimates from the
generated analysis reports.

## Full join: fresh q=64 high-skew confirmation

The q=64 confirmation used 12 fresh seeds and 10 repetitions per seed at skew
`z=1.5` (720 correct timed rows across six policies).

| Policy | End-to-end (ms) | Speedup vs sequential | Planning (ms) | Build (ms) | Probe (ms) | Total tasks |
|---|---:|---:|---:|---:|---:|---:|
| Sequential | 127.18 | 1.00x | 0.148 | 88.11 | 7.88 | 1,024 |
| Static | 63.41 | 2.01x | 0.132 | 29.29 | 3.84 | 1,024 |
| Coarse dynamic | 64.13 | 1.98x | 0.138 | 30.18 | 3.46 | 1,024 |
| Paper-style fine | 63.45 | 2.00x | 0.207 | 30.08 | 2.57 | 1,220 |
| Budgeted, unpacked | 63.45 | 2.00x | 0.212 | 30.07 | 2.74 | 1,250.5 |
| **Adaptive packed (q=64)** | **64.31** | **1.98x** | **0.131** | **31.18** | **2.56** | **394** |

The adaptive packed probe phase was faster than static by **28.39%**
(95% CI 21.54% to 35.09%) and coarse dynamic by **21.21%** (13.51% to
28.17%). Against paper-style fine scheduling, the probe difference was not
resolved (−0.75%, CI −4.39% to 2.91%); against budgeted-unpacked it was also
not resolved (+2.26%, CI −2.31% to 6.13%). Here, positive improvement means
adaptive is faster.

That probe result did not translate into a full-join gain. Adaptive packed
end-to-end improvement was −0.48% versus static (CI −1.86% to 1.02%) and
−0.62% versus dynamic (CI −1.91% to 0.64%). It was slower than paper-style
fine by 1.67% (CI 0.01% to 3.54%) and budgeted-unpacked by 1.60%
(CI 0.14% to 3.06%). The phase medians show why caution is needed: partition
and build together account for about 96% of adaptive packed end-to-end time.
The adaptive packed build median was 31.18 ms, compared with 29.29–30.18 ms
for the parallel baselines; the benchmark does not establish that this difference
was caused by the probe scheduler.

## Broad q=32 confirmation

The q=32 dataset covers 12 seeds × 10 repetitions at `z={0, 0.75, 1.5}`
(2,160 correct timed rows). At high skew, the adaptive packed policy reduced
median scheduled tasks to **266**, versus 1,024 for static/dynamic, 1,220 for
paper-style fine, and 1,122 for budgeted-unpacked. Its worker probe-load ratio
was **1.14**, compared with 2.00 for static and 1.58 for coarse dynamic.

These load/task improvements did not demonstrate an end-to-end win. At `z=1.5`,
median end-to-end time was 80.52 ms for adaptive packed, 79.62 ms static,
78.76 ms coarse dynamic, 79.55 ms paper-style fine, and 78.82 ms
budgeted-unpacked. Paired seed-level adaptive-packed improvement estimates
(95% CI) were:

| Comparison | End-to-end improvement |
|---|---:|
| vs static | −4.65% (−10.99%, 1.34%) |
| vs coarse dynamic | −2.69% (−8.34%, 3.10%) |
| vs paper-style fine | −6.19% (−11.84%, −1.05%) |
| vs budgeted-unpacked | −6.78% (−11.23%, −2.63%) |

The broad run therefore shows lower task count and better assignment balance
under severe skew, but no statistically supported improvement over static or
dynamic end-to-end. Adaptive packed was slower than both fine-grained
comparators in this run.

## Skew and partition metrics

At `z=1.5`, the measured partition imbalance ratio (maximum partition size /
average across all 1,024 partitions, including empty partitions) was about
**392.4**. Maximum partition size was approximately **383,000 tuples** and the
average was **976.6 tuples**. This confirms a highly skewed input; it does not
imply the scheduler can reduce the cost of partitioning or hash-table building.

The q=32 all-skew report shows end-to-end regressions for adaptive packed versus
static, dynamic, paper-style fine, and budgeted-unpacked at `z=0` and `z=0.75`.
Do not present the high-skew probe result as an all-workload improvement.

## Metric limitations

- Worker probe-load ratio measures assigned tuples, not actual runtime or steals.
- A portable steal count is **unavailable** through the OpenMP dynamic-loop
  interface in this implementation; no worker assignment is mislabeled as a steal.
- Probe time includes OpenMP dispatch; dispatch-only overhead is not isolated.
- Planning time is small (about 0.13 ms for adaptive packed in the q=64 run);
  relative percentage comparisons for such small values are noisy.
- The paper-style fine policy is an approximation, not a claimed exact paper
  reproduction.
- Results are from Windows/MinGW on one CPU. Linux/WSL, sanitizer, and
  five-million-tuple confirmation runs remain unverified.

## Artifacts

- q=64 raw data, generated report, phase tables, and plots:
  [`01_final_q64_confirmation/`](./01_final_q64_confirmation/)
- q=32 all-skew raw data, generated report, phase tables, and plots:
  [`02_broad_q32_confirmation/`](./02_broad_q32_confirmation/)
- Earlier reproduction, tuning, legacy, and smoke data are separated in the
  sibling numbered directories. They are retained for traceability and must not
  be pooled with the final confirmations.
