# Results status

This document reports only measured results. Published results from the supplied paper are summarized separately in [PAPER_REPRODUCTION.md](./PAPER_REPRODUCTION.md).

## Current conclusion

There is **no robust evidence that adaptive packing improves end-to-end join time over the best parallel baselines**. The strongest supported finding is narrower: at high skew, q=64 adaptive packing improves the probe phase and worker-assigned probe-load balance relative to static and coarse-dynamic scheduling, while cutting scheduled task count. That probe-phase gain did not translate to an end-to-end win over paper-style fine or the unbundled budgeted policy. Do not claim that the adaptive novelty is faster overall than all baselines.

## Fresh-seed q=64 high-skew confirmation

The separately held-out confirmation is `results/robust_q64_confirmation.csv` with analysis in [ROBUST_EVALUATION.md](../results/robust_q64_confirmation_analysis/ROBUST_EVALUATION.md) and [robust_evaluation_metrics.png](../results/robust_q64_confirmation_analysis/robust_evaluation_metrics.png).

Configuration: 1,000,000 tuples per relation; z=1.5; 12 independent seeds {4103, 4111, 4127, 4129, 4133, 4139, 4153, 4157, 4159, 4177, 4201, 4211}; 10 timed repetitions per seed; four threads; q=64 (B=256); coarse batch=8; packing threshold=16. The dataset contains 720 correct rows, 120 per policy. The q=64 choice followed a separate three-seed development sweep (`results/adaptive_q_tuning.csv`); this confirmation uses new seeds.

Paired improvement is `(baseline_time - novelty_packed_time) / baseline_time`; positive is faster. CIs bootstrap independent seed clusters after averaging paired repetitions within each seed.

| Baseline | End-to-end improvement (95% CI) | Probe-phase improvement (95% CI) |
|---|---:|---:|
| Static | -0.48% [-1.86%, +1.02%] | **+28.39% [+21.59%, +35.18%]** |
| Coarse dynamic | -0.62% [-1.91%, +0.64%] | **+21.21% [+13.55%, +28.10%]** |
| Paper-style fine | -1.67% [-3.54%, -0.01%] | -0.75% [-4.38%, +2.89%] |
| `novelty_budgeted` (unpacked) | -1.60% [-3.06%, -0.14%] | +2.26% [-2.30%, +6.12%] |

The end-to-end intervals versus static and coarse dynamic include zero; versus paper-style fine and unbundled budgeted scheduling, the observed difference favors those baselines. Thus the clear advantage here is limited to the **probe phase against static/coarse dynamic**, not the full join or all fine-grained policies.

### Other evaluation metrics for q=64

| Metric | Adaptive packed result | Interpretation |
|---|---:|---|
| Median end-to-end time | 64.31 ms | Versus 63.41 ms static, 64.13 ms dynamic, 63.45 ms paper-style fine, and 63.45 ms budgeted-unpacked |
| Median speedup vs sequential | 1.95x | Sequential end-to-end / adaptive end-to-end; not speedup over parallel baselines |
| Partition λ (max/average) | 392.44 | Maximum 383,246 probe tuples / mean 976.6 over 1,024 partitions |
| Worker probe-load λ (max/average) | 1.05 | Worker-assigned probe tuples; lower is more balanced. Static: 1.96; dynamic: 1.79 |
| Median scheduled tasks | 394 | Static/dynamic: 1,024; paper-style fine: 1,220; unpacked budgeted: 1,250.5 |
| Task-plan construction | 0.1308 ms (0.206% of end-to-end) | Explicitly measured planning phase; does not isolate OpenMP dispatch overhead |
| Correctness | 720/720 rows correct | CLI sequential-oracle cardinality check; exact pair multisets and task coverage also pass unit tests |
| Steal count | Unavailable | OpenMP dynamic work sharing exposes no portable steal counter here; worker assignments are not steal counts |

At high skew, adaptive packing used 256 fine chunks within budget B=256. It scheduled about 61.5% fewer tasks than static/dynamic, 67.7% fewer than paper-style fine, and 68.5% fewer than unbundled budgeted scheduling. Lower task count and better worker λ are diagnostic results, not by themselves a full-query speedup.

## Broader q=32 paired run

`results/robust_adaptive_final.csv` has 2,160 correct rows: 12 independent seeds, 10 repetitions, z={0, 0.75, 1.5}, four threads, q=32, coarse batch=8, and packing threshold=16. Its report and all-skew metrics plot are in `results/robust_adaptive_final_analysis/`.

At z=1.5, q=32 produced median 266 tasks and worker probe λ=1.14, but paired end-to-end improvement was -4.65% versus static (95% CI [-10.91%, +1.21%]), -2.69% versus dynamic ([-8.41%, +2.99%]), -6.19% versus paper-style fine ([-11.73%, -1.09%]), and -6.78% versus unbundled budgeted ([-11.23%, -2.69%]). Probe time improved versus static by +14.76% ([+6.93%, +22.74%]); other probe comparisons were mixed. This result is why the final claim is restricted to the q=64 probe-phase comparisons.

## Tuning and interpretation

The development tuning sweep used q={32, 64, 96, 128}, coarse batch=8, and three seeds {3301, 3307, 3313}. q=64 looked promising on those seeds, but the independent confirmation did **not** find an end-to-end advantage; it did find a probe-phase gain versus static and dynamic scheduling. The separate batch-size tuning used three different development seeds and is also exploratory. Neither tuning sweep establishes a globally optimal parameter.

Partition λ is defined as maximum probe tuples in any final radix partition divided by average probe tuples across all final partitions, including empty partitions. Worker probe λ uses the maximum per-worker assigned probe tuples divided by the mean over configured workers. `planning_ms` is measured inside end-to-end. Probe time combines tuple processing and OpenMP dispatch. Per-worker counters are cache-line padded, but their measurement perturbation was not separately calibrated.

The source paper's numeric results are not reproduced. `paper_fine` is an approximation because the supplied paper does not specify this implementation's split threshold/chunk size. The measurements are from Windows/MinGW on an Intel i5-1035G1, with uncontrolled CPU affinity/power state. Linux/WSL, ASan/UBSan, and 5m-tuple runs remain unverified. Treat all claims as machine/workload-specific.

Regenerate the q=64 report and plot from its CSV:

```powershell
.\.venv\Scripts\python.exe scripts\robust_evaluate.py --input results\robust_q64_confirmation.csv --output-dir results\robust_q64_confirmation_analysis
```
