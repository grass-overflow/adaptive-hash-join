#!/usr/bin/env python3
"""Create a focused high-skew performance figure and data-derived report."""

import argparse
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

BASELINES = ["static", "dynamic", "paper_fine", "novelty_budgeted"]
ALL_POLICIES = ["sequential", *BASELINES, "novelty_packed"]
POLICY_LABELS = {
    "sequential": "Sequential",
    "static": "Static",
    "dynamic": "Coarse dynamic",
    "paper_fine": "Paper-style fine",
    "novelty_budgeted": "Budgeted (unpacked)",
    "novelty_packed": "Adaptive packed",
}


def paired_seed_improvements(data, measurement, baseline, method="novelty_packed"):
    keys = ["build_size", "probe_size", "skew", "threads", "seed", "repetition"]
    pivot = data.pivot_table(index=keys, columns="policy", values=measurement, aggfunc="first")
    if baseline not in pivot or method not in pivot:
        raise ValueError(f"Missing policy for {measurement}: {method} vs {baseline}")
    paired = pivot[[baseline, method]].dropna().reset_index()
    if paired.empty:
        raise ValueError(f"No paired measurements for {measurement}: {baseline}")
    paired["improvement_pct"] = (
        (paired[baseline] - paired[method]) / paired[baseline] * 100.0
    )
    by_seed = paired.groupby("seed")["improvement_pct"].mean()
    values = by_seed.to_numpy(dtype=float)
    if len(values) < 2:
        raise ValueError("At least two independent seeds are required for bootstrap intervals")
    rng = np.random.default_rng(2025)
    samples = rng.choice(values, size=(20000, len(values)), replace=True).mean(axis=1)
    low, high = np.quantile(samples, [0.025, 0.975])
    return {
        "mean": float(values.mean()),
        "low": float(low),
        "high": float(high),
        "seed_count": int(len(values)),
        "paired_repetitions": int(len(paired)),
    }


def fmt_ms(value):
    return f"{value:.2f} ms"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--input", default="results/adaptive_gate_reproduction.csv",
        help="Correct raw CSV produced by run_sweep.py",
    )
    parser.add_argument(
        "--output-dir", default="results/adaptive_gate_reproduction_analysis",
    )
    args = parser.parse_args()

    output = Path(args.output_dir)
    output.mkdir(parents=True, exist_ok=True)
    data = pd.read_csv(args.input)
    required = {
        "correct", "policy", "seed", "skew", "build_size", "probe_size", "threads",
        "repetition", "end_to_end_ms", "probe_ms", "total_tasks", "fine_chunks",
        "chunk_budget", "max_to_average", "pack_threshold",
        "planning_ms", "speedup", "max_partition_probe_tuples",
        "average_probe_partition_tuples", "workers_with_probe_work",
        "max_worker_tasks", "max_worker_probe_tuples",
        "worker_task_total", "worker_task_max_to_average",
        "worker_probe_max_to_average",
    }
    missing = required - set(data.columns)
    if missing:
        raise ValueError(f"Input CSV lacks required columns: {sorted(missing)}")
    if data.empty or not data["correct"].astype(str).str.lower().isin(["true", "1"]).all():
        raise ValueError("Refusing to report an empty CSV or incorrect measurement rows")

    paired_id = ["build_size", "probe_size", "skew", "threads", "seed", "repetition"]
    if data.duplicated(paired_id + ["policy"]).any():
        raise ValueError("Duplicate policy measurement found for a paired run")
    skew = float(data["skew"].max())
    high = data[data["skew"] == skew].copy()
    policies = set(high["policy"].unique())
    expected = set(ALL_POLICIES)
    if policies != expected:
        raise ValueError(f"High-skew slice policies differ: expected {expected}, got {policies}")
    if high.groupby(paired_id)["policy"].nunique().ne(len(expected)).any():
        raise ValueError("High-skew measurements are not fully paired across policies")
    if (high["worker_task_total"] != high["total_tasks"]).any():
        raise ValueError("Worker task counts do not sum to planned task count")
    if (high["max_to_average"] < 1.0).any():
        raise ValueError("Partition lambda must be >= 1 for non-empty relations")

    comparisons = {}
    for measurement in ["end_to_end_ms", "probe_ms"]:
        comparisons[measurement] = {
            baseline: paired_seed_improvements(high, measurement, baseline)
            for baseline in BASELINES
        }

    summary = high.groupby("policy", as_index=True).agg(
        median_end_to_end_ms=("end_to_end_ms", "median"),
        median_probe_ms=("probe_ms", "median"),
        median_tasks=("total_tasks", "median"),
        median_fine_chunks=("fine_chunks", "median"),
        median_budget=("chunk_budget", "median"),
        median_imbalance=("max_to_average", "median"),
        median_max_partition_probe_tuples=("max_probe_partition_tuples", "median"),
        median_average_probe_partition_tuples=("avg_probe_partition_tuples", "median"),
        median_speedup=("speedup", "median"),
        median_planning_ms=("planning_ms", "median"),
        median_worker_probe_lambda=("worker_probe_max_to_average", "median"),
        median_worker_task_lambda=("worker_task_max_to_average", "median"),
        median_workers_active=("workers_with_probe_work", "median"),
        median_max_worker_tasks=("max_worker_tasks", "median"),
        median_max_worker_probe_tuples=("max_worker_probe_tuples", "median"),
    )
    seeds = sorted(int(seed) for seed in high["seed"].unique())
    repetitions = int(high["repetition"].nunique())
    sizes = sorted(int(size) for size in high["build_size"].unique())
    threads = sorted(int(value) for value in high["threads"].unique())
    if len(sizes) != 1 or threads != [4]:
        raise ValueError("Focused report expects one relation size and four worker threads")
    size = sizes[0]

    fig, axes = plt.subplots(4, 2, figsize=(16, 17))
    fig.suptitle(
        f"Adaptive packed scheduling at high skew (z={skew:g}, {size:,} tuples/relation)",
        fontsize=17,
        fontweight="bold",
        y=0.98,
    )
    colors = {"significant": "#16855b", "uncertain": "#c57a12", "negative": "#b54747"}
    y_positions = np.arange(len(BASELINES))
    labels = [f"Adaptive packed vs {POLICY_LABELS[baseline]}" for baseline in BASELINES]

    for axis, measurement, title in [
        (axes[0, 0], "end_to_end_ms", "Adaptive packed vs baseline: end-to-end"),
        (axes[0, 1], "probe_ms", "Adaptive packed vs baseline: probe phase"),
    ]:
        stats = comparisons[measurement]
        for position, baseline in zip(y_positions, BASELINES):
            item = stats[baseline]
            color = (
                colors["significant"] if item["low"] > 0
                else colors["negative"] if item["high"] < 0
                else colors["uncertain"]
            )
            axis.errorbar(
                item["mean"],
                position,
                xerr=[[item["mean"] - item["low"]], [item["high"] - item["mean"]]],
                fmt="o",
                color=color,
                capsize=4,
                markersize=7,
                linewidth=2,
            )
            axis.annotate(
                f'{item["mean"]:+.1f}% [{item["low"]:+.1f}, {item["high"]:+.1f}]',
                (item["high"], position),
                xytext=(6, 0),
                textcoords="offset points",
                va="center",
                fontsize=8,
            )
        axis.axvline(0, color="#333333", linestyle="--", linewidth=1)
        axis.set_yticks(y_positions, labels)
        axis.invert_yaxis()
        axis.set_title(title, fontweight="bold")
        axis.set_xlabel("Runtime reduction vs baseline (%)\n(positive = adaptive packed is faster)")
        axis.grid(axis="x", alpha=0.25)
        axis.margins(x=0.28)

    task_axis = axes[1, 0]
    task_order = ALL_POLICIES
    task_values = [float(summary.loc[name, "median_tasks"]) for name in task_order]
    task_colors = ["#8d99ae"] * (len(task_order) - 1) + ["#16855b"]
    task_bars = task_axis.barh(
        [POLICY_LABELS[name] for name in task_order],
        task_values,
        color=task_colors,
    )
    task_axis.invert_yaxis()
    task_axis.set_title("Scheduled tasks (median)", fontweight="bold")
    task_axis.set_xlabel("OpenMP scheduled tasks")
    task_axis.grid(axis="x", alpha=0.25)
    task_axis.bar_label(task_bars, fmt="%.0f", padding=4, fontsize=9)
    max_tasks = max(task_values)
    task_axis.set_xlim(0, max_tasks * 1.24)

    time_axis = axes[1, 1]
    time_order = ALL_POLICIES
    time_values = [float(summary.loc[name, "median_end_to_end_ms"]) for name in time_order]
    time_bars = time_axis.barh(
        [POLICY_LABELS[name] for name in time_order],
        time_values,
        color=["#8d99ae"] * (len(time_order) - 1) + ["#16855b"],
    )
    time_axis.invert_yaxis()
    time_axis.set_title("End-to-end runtime (median)", fontweight="bold")
    time_axis.set_xlabel("Milliseconds (lower is faster)")
    time_axis.grid(axis="x", alpha=0.25)
    time_axis.bar_label(time_bars, fmt="%.2f ms", padding=4, fontsize=9)
    time_axis.set_xlim(0, max(time_values) * 1.25)

    speedup_axis = axes[2, 0]
    speedup_values = [float(summary.loc[name, "median_speedup"]) for name in ALL_POLICIES]
    speedup_bars = speedup_axis.barh(
        [POLICY_LABELS[name] for name in ALL_POLICIES],
        speedup_values,
        color=["#8d99ae"] * (len(ALL_POLICIES) - 1) + ["#16855b"],
    )
    speedup_axis.invert_yaxis()
    speedup_axis.axvline(1.0, color="#333333", linestyle="--", linewidth=1)
    speedup_axis.set_title("Speedup vs sequential (median)", fontweight="bold")
    speedup_axis.set_xlabel("Sequential end-to-end time / policy end-to-end time")
    speedup_axis.grid(axis="x", alpha=0.25)
    speedup_axis.bar_label(speedup_bars, fmt="%.2fx", padding=4, fontsize=9)
    speedup_axis.set_xlim(0, max(speedup_values) * 1.25)

    imbalance_axis = axes[2, 1]
    imbalance_values = [
        float(summary.loc[name, "median_worker_probe_lambda"])
        for name in ALL_POLICIES
    ]
    imbalance_bars = imbalance_axis.barh(
        [POLICY_LABELS[name] for name in ALL_POLICIES],
        imbalance_values,
        color=["#8d99ae"] * (len(ALL_POLICIES) - 1) + ["#16855b"],
    )
    imbalance_axis.invert_yaxis()
    imbalance_axis.axvline(1.0, color="#333333", linestyle="--", linewidth=1)
    imbalance_axis.set_title("Probe work imbalance across workers", fontweight="bold")
    imbalance_axis.set_xlabel("Worker λ = max probe tuples / mean over configured workers")
    imbalance_axis.grid(axis="x", alpha=0.25)
    imbalance_axis.bar_label(imbalance_bars, fmt="%.2f", padding=4, fontsize=9)
    imbalance_axis.set_xlim(0, max(imbalance_values) * 1.25)

    plan_axis = axes[3, 0]
    planning_values = [float(summary.loc[name, "median_planning_ms"]) for name in ALL_POLICIES]
    plan_bars = plan_axis.barh(
        [POLICY_LABELS[name] for name in ALL_POLICIES],
        planning_values,
        color=["#8d99ae"] * (len(ALL_POLICIES) - 1) + ["#16855b"],
    )
    plan_axis.invert_yaxis()
    plan_axis.set_title("Task-plan construction time (median)", fontweight="bold")
    plan_axis.set_xlabel("Milliseconds (included in end-to-end)")
    plan_axis.grid(axis="x", alpha=0.25)
    plan_axis.bar_label(plan_bars, fmt="%.3f ms", padding=4, fontsize=9)
    plan_axis.set_xlim(0, max(planning_values) * 1.5 if max(planning_values) else 1)

    overhead_axis = axes[3, 1]
    overhead_values = [
        float(summary.loc[name, "median_planning_ms"]) /
        float(summary.loc[name, "median_end_to_end_ms"]) * 100.0
        for name in ALL_POLICIES
    ]
    overhead_bars = overhead_axis.barh(
        [POLICY_LABELS[name] for name in ALL_POLICIES],
        overhead_values,
        color=["#8d99ae"] * (len(ALL_POLICIES) - 1) + ["#16855b"],
    )
    overhead_axis.invert_yaxis()
    overhead_axis.set_title("Planning overhead share (median ratio)", fontweight="bold")
    overhead_axis.set_xlabel("Task-plan construction / end-to-end (%)")
    overhead_axis.grid(axis="x", alpha=0.25)
    overhead_axis.bar_label(overhead_bars, fmt="%.2f%%", padding=4, fontsize=9)
    overhead_axis.set_xlim(0, max(overhead_values) * 1.5 if max(overhead_values) else 1)

    fig.text(
        0.5,
        0.015,
        f"Five independent seeds ({', '.join(map(str, seeds))}); {repetitions} timed repetitions per seed. "
        "Error bars: 95% bootstrap CI over seed-level paired means. "
        "Green: CI entirely faster; amber: CI overlaps zero. Partition λ=max/mean probe partition size. "
        "OpenMP steal counts are not exposed; worker assignment is a proxy only. Windows/MinGW.",
        ha="center",
        fontsize=9,
        color="#444444",
    )
    fig.tight_layout(rect=[0.02, 0.045, 0.98, 0.95])
    figure = output / "high_skew_novelty_result.png"
    fig.savefig(figure, dpi=200, bbox_inches="tight")
    plt.close(fig)

    packed = summary.loc["novelty_packed"]
    static_tasks = float(summary.loc["static", "median_tasks"])
    task_reductions = {
        baseline: (float(summary.loc[baseline, "median_tasks"]) -
                   float(packed["median_tasks"])) /
                  float(summary.loc[baseline, "median_tasks"]) * 100.0
        for baseline in BASELINES
    }
    comparison_lines = []
    for baseline in BASELINES:
        end = comparisons["end_to_end_ms"][baseline]
        probe = comparisons["probe_ms"][baseline]
        comparison_lines.append(
            f'| {POLICY_LABELS[baseline]} | {end["mean"]:+.2f}% '
            f'[{end["low"]:+.2f}%, {end["high"]:+.2f}%] | '
            f'{probe["mean"]:+.2f}% [{probe["low"]:+.2f}%, {probe["high"]:+.2f}%] |'
        )
    report = f"""# High-skew adaptive scheduling result

![High-skew comparison](./high_skew_novelty_result.png)

## Experiment

This report is generated from `{Path(args.input).as_posix()}`. It includes only rows with `correct=true` and compares fully paired policies on identical inputs. The selected high-skew condition is Zipf z={skew:g}, with {size:,} tuples in each relation, {threads[0]} OpenMP threads, seeds {', '.join(map(str, seeds))}, and {repetitions} timed repetitions per seed. There are {len(high)} measurements in this high-skew slice. The CSV configuration is q=32, coarse batch=8, and packing threshold=16.

The 95% intervals bootstrap independent seed clusters: timed paired improvements are averaged within each seed, then seeds are resampled. They are not bootstraps over individual repetitions.

## Paired performance

Positive values indicate that `novelty_packed` is faster. End-to-end includes radix partitioning, build, and probe; probe-phase time isolates the probe stage.

| Baseline | End-to-end reduction (95% seed-bootstrap CI) | Probe reduction (95% seed-bootstrap CI) |
|---|---:|---:|
{chr(10).join(comparison_lines)}

At this high-skew setting, the policy reduced median end-to-end time to {fmt_ms(float(packed["median_end_to_end_ms"]))}. Median task count was {int(float(packed["median_tasks"]))} versus {int(static_tasks)} for static scheduling ({task_reductions["static"]:.1f}% fewer), {int(float(summary.loc["paper_fine", "median_tasks"]))} for paper-style fine scheduling ({task_reductions["paper_fine"]:.1f}% fewer), and {int(float(summary.loc["novelty_budgeted", "median_tasks"]))} for the unbundled budgeted policy ({task_reductions["novelty_budgeted"]:.1f}% fewer). The adaptive policy used {int(float(packed["median_fine_chunks"]))} fine chunks of a configured budget of {int(float(packed["median_budget"]))}; median max/average partition imbalance was {float(packed["median_imbalance"]):.2f}.

## Evaluation metrics

| Metric | Adaptive packed median | Definition |
|---|---:|---|
| Sequential speedup | {float(packed["median_speedup"]):.2f}x | Sequential end-to-end time / adaptive end-to-end time |
| Partition λ | {float(packed["median_imbalance"]):.2f} | Maximum probe partition tuples / average over all radix partitions |
| Largest probe partition | {int(float(summary.loc["novelty_packed", "median_max_partition_probe_tuples"])):,} tuples | Maximum cardinality of any probe partition |
| Average probe partition | {float(summary.loc["novelty_packed", "median_average_probe_partition_tuples"]):,.1f} tuples | Probe tuple count / radix partition count |
| Worker probe λ | {float(packed["median_worker_probe_lambda"]):.2f} | Maximum worker-assigned probe tuples / mean over configured workers |
| Workers with probe work | {int(float(packed["median_workers_active"]))} of {threads[0]} | Number of workers assigned at least one probe tuple |
| Max worker task count | {int(float(packed["median_max_worker_tasks"]))} | Highest number of probe tasks assigned to one worker |
| Task-plan construction | {float(packed["median_planning_ms"]):.4f} ms ({float(packed["median_planning_ms"]) / float(packed["median_end_to_end_ms"]) * 100.0:.3f}% of end-to-end) | Measured CPU-side task-plan construction time |

All {len(data)} raw rows passed cardinality correctness validation. Per-worker assignment metrics are observational load-balance proxies; they do not identify which worker initiated or stole a task. Actual OpenMP steal counts are unavailable from this runtime path: it uses a dynamic work-sharing loop and the OpenMP runtime does not expose a portable steal counter. No steal count is inferred or fabricated.

The plan-construction metric is not total scheduler overhead: `probe_ms` includes both tuple processing and OpenMP dispatch, which cannot be separated by these measurements. Do not subtract planning time and describe the remainder as pure computation.

## Interpretation and limits

This is evidence of a high-skew benefit on this specific measured machine and workload, not proof of general superiority. An interval crossing zero means this experiment did not resolve an end-to-end difference from that baseline. The adaptive threshold was selected after exploratory runs, the five-seed interval is preliminary, Windows/MinGW is not the target Linux environment, and the `paper_fine` policy is an approximation rather than an exact historical reproduction.

The figure is generated directly from the measured CSV; it does not contain hand-entered runtime or task-count values. Recreate it with:

```powershell
.\\.venv\\Scripts\\python.exe scripts\\make_final_visual.py --input results\\adaptive_gate_reproduction.csv --output-dir results\\adaptive_gate_reproduction_analysis
```
"""
    report_path = output / "HIGH_SKEW_REPORT.md"
    report_path.write_text(report, encoding="utf-8")
    print(f"Wrote {figure}")
    print(f"Wrote {report_path}")


if __name__ == "__main__":
    main()
