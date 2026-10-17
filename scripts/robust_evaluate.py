#!/usr/bin/env python3
"""Validate paired hash-join measurements and generate robust evaluation outputs."""

import argparse
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

POLICIES = [
    "sequential", "static", "dynamic", "paper_fine",
    "novelty_budgeted", "novelty_packed",
]
BASELINES = ["static", "dynamic", "paper_fine", "novelty_budgeted"]
LABELS = {
    "sequential": "Sequential",
    "static": "Static",
    "dynamic": "Coarse dynamic",
    "paper_fine": "Paper-style fine",
    "novelty_budgeted": "Budgeted, unpacked",
    "novelty_packed": "Adaptive packed",
}
COLORS = {
    "sequential": "#7a869a",
    "static": "#4c78a8",
    "dynamic": "#f2a541",
    "paper_fine": "#9c6ade",
    "novelty_budgeted": "#36a269",
    "novelty_packed": "#c23b32",
}


def bootstrap_ci(seed_values, rng, iterations):
    values = np.asarray(seed_values, dtype=float)
    if values.size < 2:
        return np.nan, np.nan
    samples = rng.choice(values, size=(iterations, values.size), replace=True).mean(axis=1)
    return tuple(np.quantile(samples, [0.025, 0.975]))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--bootstrap-iterations", type=int, default=20000)
    args = parser.parse_args()

    output = Path(args.output_dir)
    output.mkdir(parents=True, exist_ok=True)
    data = pd.read_csv(args.input)
    required = {
        "policy", "seed", "build_size", "probe_size", "skew", "threads", "repetition",
        "correct", "end_to_end_ms", "partition_ms", "planning_ms", "build_ms", "probe_ms",
        "sequential_ms", "speedup", "parallel_efficiency", "max_to_average",
        "avg_probe_partition_tuples", "max_probe_partition_tuples", "total_tasks",
        "worker_task_total", "max_worker_tasks", "worker_task_max_to_average",
        "workers_with_probe_work", "max_worker_probe_tuples",
        "worker_probe_max_to_average", "coarse_batch", "q", "pack_threshold",
        "steal_count", "steal_count_status",
    }
    missing = required - set(data.columns)
    if missing:
        raise ValueError(f"Input CSV is missing columns: {sorted(missing)}")
    if data.empty:
        raise ValueError("Refusing to analyze an empty CSV")
    if not data["correct"].astype(str).str.lower().isin(["true", "1"]).all():
        raise ValueError("Refusing to analyze CSV containing incorrect results")
    if (data["worker_task_total"] != data["total_tasks"]).any():
        raise ValueError("Per-worker task counts do not sum to planned total tasks")
    if (data["max_to_average"] < 1.0).any():
        raise ValueError("Partition lambda must be >= 1")
    if data["steal_count"].notna().any() or not (
        data["steal_count_status"] == "unavailable_openmp_runtime"
    ).all():
        raise ValueError("Steal-count availability must be marked accurately")
    if data.duplicated(
        ["build_size", "probe_size", "skew", "threads", "seed", "repetition", "policy"]
    ).any():
        raise ValueError("Duplicate policy row for a paired measurement")

    paired_keys = ["build_size", "probe_size", "skew", "threads", "seed", "repetition"]
    policy_sets = data.groupby(paired_keys)["policy"].agg(lambda values: frozenset(values))
    if any(policies != frozenset(POLICIES) for policies in policy_sets):
        raise ValueError("Dataset has incomplete or unexpected paired policy sets")

    data["planning_overhead_pct"] = data["planning_ms"] / data["end_to_end_ms"] * 100.0
    data["probe_phase_fraction_pct"] = data["probe_ms"] / data["end_to_end_ms"] * 100.0

    summary_columns = ["build_size", "probe_size", "threads", "skew", "policy"]
    summary = data.groupby(summary_columns, as_index=False).agg(
        runs=("end_to_end_ms", "count"),
        runtime_median_ms=("end_to_end_ms", "median"),
        runtime_mean_ms=("end_to_end_ms", "mean"),
        runtime_std_ms=("end_to_end_ms", "std"),
        probe_median_ms=("probe_ms", "median"),
        build_median_ms=("build_ms", "median"),
        partition_median_ms=("partition_ms", "median"),
        planning_median_ms=("planning_ms", "median"),
        planning_overhead_pct_median=("planning_overhead_pct", "median"),
        speedup_median=("speedup", "median"),
        parallel_efficiency_median=("parallel_efficiency", "median"),
        lambda_partition_median=("max_to_average", "median"),
        average_partition_tuples_median=("avg_probe_partition_tuples", "median"),
        max_partition_tuples_median=("max_probe_partition_tuples", "median"),
        lambda_worker_probe_median=("worker_probe_max_to_average", "median"),
        worker_task_lambda_median=("worker_task_max_to_average", "median"),
        workers_with_probe_work_median=("workers_with_probe_work", "median"),
        max_worker_probe_tuples_median=("max_worker_probe_tuples", "median"),
        max_worker_tasks_median=("max_worker_tasks", "median"),
        total_tasks_median=("total_tasks", "median"),
        correctness_passes=("correct", "count"),
    )
    summary.to_csv(output / "metrics_by_skew_policy.csv", index=False)

    rng = np.random.default_rng(20261017)
    comparison_rows = []
    phase_measurements = [
        "end_to_end_ms", "partition_ms", "planning_ms", "build_ms", "probe_ms"
    ]
    for skew in sorted(data["skew"].unique()):
        cell = data[data["skew"] == skew]
        for measurement in phase_measurements:
            pivot = cell.pivot_table(
                index=paired_keys, columns="policy", values=measurement, aggfunc="first"
            )
            for baseline in BASELINES:
                paired = pivot[[baseline, "novelty_packed"]].dropna().reset_index()
                paired["improvement_pct"] = (
                    (paired[baseline] - paired["novelty_packed"]) /
                    paired[baseline] * 100.0
                )
                seed_effects = paired.groupby("seed")["improvement_pct"].mean()
                low, high = bootstrap_ci(
                    seed_effects.to_numpy(), rng, args.bootstrap_iterations
                )
                comparison_rows.append({
                    "skew": skew,
                    "measurement": measurement,
                    "method": "novelty_packed",
                    "baseline": baseline,
                    "independent_seed_count": int(seed_effects.size),
                    "paired_timed_comparisons": int(len(paired)),
                    "mean_seed_level_improvement_pct": float(seed_effects.mean()),
                    "bootstrap_95_low_pct": low,
                    "bootstrap_95_high_pct": high,
                    "correct_rows": int(len(paired)),
                })
    comparisons = pd.DataFrame(comparison_rows)
    comparisons.to_csv(output / "paired_comparisons.csv", index=False)
    comparisons.to_csv(output / "full_join_phase_comparisons.csv", index=False)

    figure, axes = plt.subplots(4, 2, figsize=(18, 19))
    skews = sorted(data["skew"].unique())
    plot_labels = [f"{float(value):g}" for value in skews]
    axes_flat = axes.ravel()

    ax = axes_flat[0]
    end = comparisons[comparisons["measurement"] == "end_to_end_ms"]
    for baseline in BASELINES:
        curve = end[end["baseline"] == baseline].sort_values("skew")
        means = curve["mean_seed_level_improvement_pct"].to_numpy()
        low = curve["bootstrap_95_low_pct"].to_numpy()
        high = curve["bootstrap_95_high_pct"].to_numpy()
        ax.errorbar(
            curve["skew"],
            means,
            yerr=np.vstack([means - low, high - means]),
            marker="o",
            capsize=3,
            label=LABELS[baseline],
        )
    ax.axhline(0, color="black", linestyle="--", linewidth=0.9)
    ax.set_title("Adaptive packed: paired end-to-end improvement")
    ax.set_xlabel("Zipf skew z")
    ax.set_ylabel("Improvement (%) — positive is faster")
    ax.legend(fontsize=8)
    ax.grid(alpha=0.25)

    ax = axes_flat[1]
    probe = comparisons[comparisons["measurement"] == "probe_ms"]
    for baseline in BASELINES:
        curve = probe[probe["baseline"] == baseline].sort_values("skew")
        means = curve["mean_seed_level_improvement_pct"].to_numpy()
        low = curve["bootstrap_95_low_pct"].to_numpy()
        high = curve["bootstrap_95_high_pct"].to_numpy()
        ax.errorbar(
            curve["skew"],
            means,
            yerr=np.vstack([means - low, high - means]),
            marker="o",
            capsize=3,
            label=LABELS[baseline],
        )
    ax.axhline(0, color="black", linestyle="--", linewidth=0.9)
    ax.set_title("Adaptive packed: paired probe-phase improvement")
    ax.set_xlabel("Zipf skew z")
    ax.set_ylabel("Improvement (%) — positive is faster")
    ax.legend(fontsize=8)
    ax.grid(alpha=0.25)

    metric_specs = [
        ("runtime_median_ms", "End-to-end time (median, ms)", "ms", False),
        ("speedup_median", "Speedup vs sequential (median)", "x", False),
        ("lambda_partition_median", "Partition imbalance λ=max/mean", "λ", False),
        ("lambda_worker_probe_median", "Worker probe-load λ=max/mean", "λ", False),
        ("total_tasks_median", "Scheduled tasks (median)", "tasks", False),
        ("planning_overhead_pct_median", "Plan construction overhead share", "% E2E", False),
    ]
    for ax, (metric, title, ylabel, _) in zip(axes_flat[2:], metric_specs):
        for policy in POLICIES:
            curve = summary[summary["policy"] == policy].sort_values("skew")
            ax.plot(
                curve["skew"],
                curve[metric],
                marker="o",
                label=LABELS[policy],
                color=COLORS[policy],
            )
        ax.set_title(title)
        ax.set_xlabel("Zipf skew z")
        ax.set_ylabel(ylabel)
        ax.set_xticks(skews, plot_labels)
        ax.grid(alpha=0.25)
        ax.legend(fontsize=8)

    figure.suptitle("Robust paired evaluation of adaptive hash-join scheduling", fontsize=17)
    figure.text(
        0.5,
        0.012,
        f"{data['seed'].nunique()} independent seeds, "
        f"{data['repetition'].nunique()} timed repetitions per seed/skew. "
        "95% CIs bootstrap seed-level paired means. "
        "Correctness checked against sequential oracle. "
        "OpenMP steal counts unavailable; worker λ is assignment only.",
        ha="center",
        fontsize=9,
    )
    figure.tight_layout(rect=[0.02, 0.035, 0.98, 0.96])
    plot_path = output / "robust_evaluation_metrics.png"
    figure.savefig(plot_path, dpi=190, bbox_inches="tight")
    plt.close(figure)

    phase_names = ["partition_ms", "planning_ms", "build_ms", "probe_ms"]
    phase_columns = {
        "partition_ms": "partition_median_ms",
        "planning_ms": "planning_median_ms",
        "build_ms": "build_median_ms",
        "probe_ms": "probe_median_ms",
    }
    phase_breakdown = summary[
        summary["policy"].isin(POLICIES)
    ][["build_size", "probe_size", "threads", "skew", "policy", "runtime_median_ms",
       *phase_columns.values()]].copy()
    phase_breakdown.to_csv(output / "full_join_phase_breakdown.csv", index=False)

    phase_figure, phase_axes = plt.subplots(
        1, len(skews), figsize=(7 * len(skews), 6), sharey=True
    )
    if len(skews) == 1:
        phase_axes = [phase_axes]
    for ax, skew_value in zip(phase_axes, skews):
        skew_summary = summary[summary["skew"] == skew_value].set_index("policy")
        y = np.arange(len(POLICIES))
        left = np.zeros(len(POLICIES), dtype=float)
        for phase, label in zip(
            phase_names, ["Radix partition", "Task planning", "Hash build", "Probe"]
        ):
            values = np.asarray([
                float(skew_summary.loc[policy, phase_columns[phase]])
                for policy in POLICIES
            ])
            ax.barh(y, values, left=left, label=label)
            left += values
        e2e_values = np.asarray([
            float(skew_summary.loc[policy, "runtime_median_ms"])
            for policy in POLICIES
        ])
        ax.scatter(
            e2e_values, y, marker="|", color="black", s=150, label="E2E median"
        )
        ax.set_yticks(y, [LABELS[policy] for policy in POLICIES])
        ax.invert_yaxis()
        ax.set_title(f"Zipf z={float(skew_value):g}")
        ax.set_xlabel("Median phase wall time (ms)")
        ax.grid(axis="x", alpha=0.25)
    phase_axes[0].set_ylabel("Policy")
    phase_axes[-1].legend(fontsize=8, loc="lower right")
    phase_figure.suptitle("Full-join phase decomposition (medians)", fontsize=15)
    phase_figure.tight_layout()
    phase_plot_path = output / "full_join_phase_breakdown.png"
    phase_figure.savefig(phase_plot_path, dpi=190, bbox_inches="tight")
    plt.close(phase_figure)

    high_skew = float(max(skews))
    thread_counts = sorted(int(value) for value in data["threads"].unique())
    q_values = sorted(int(value) for value in data["q"].unique())
    batch_values = sorted(int(value) for value in data["coarse_batch"].unique())
    threshold_values = sorted(float(value) for value in data["pack_threshold"].unique())
    packed_summary = summary[
        (summary["policy"] == "novelty_packed") & (summary["skew"] == high_skew)
    ].iloc[0]
    high_comparisons = comparisons[
        (comparisons["skew"] == high_skew) &
        (comparisons["measurement"] == "end_to_end_ms")
    ]
    comparison_report = "\n".join(
        f"| {LABELS[row.baseline]} | {row.mean_seed_level_improvement_pct:+.2f}% "
        f"[{row.bootstrap_95_low_pct:+.2f}%, {row.bootstrap_95_high_pct:+.2f}%] |"
        for row in high_comparisons.itertuples()
    )
    high_phase_rows = phase_breakdown[phase_breakdown["skew"] == high_skew].set_index("policy")
    phase_breakdown_report = "\n".join(
        f"| {LABELS[policy]} | "
        f"{float(high_phase_rows.loc[policy, 'partition_median_ms']):.2f} | "
        f"{float(high_phase_rows.loc[policy, 'planning_median_ms']):.3f} | "
        f"{float(high_phase_rows.loc[policy, 'build_median_ms']):.2f} | "
        f"{float(high_phase_rows.loc[policy, 'probe_median_ms']):.2f} | "
        f"{float(high_phase_rows.loc[policy, 'runtime_median_ms']):.2f} |"
        for policy in POLICIES
    )
    phase_comparison_rows = comparisons[
        (comparisons["skew"] == high_skew) &
        comparisons["measurement"].isin(
            ["partition_ms", "build_ms", "probe_ms"]
        )
    ]
    phase_comparison_report = "\n".join(
        f"| {row.measurement.removesuffix('_ms')} | {LABELS[row.baseline]} | "
        f"{row.mean_seed_level_improvement_pct:+.2f}% "
        f"[{row.bootstrap_95_low_pct:+.2f}%, {row.bootstrap_95_high_pct:+.2f}%] |"
        for row in phase_comparison_rows.itertuples()
    )
    metric_report = "\n".join(
        f"| z={row.skew:g} | {row.runtime_median_ms:.2f} ms | "
        f"{row.speedup_median:.2f}x | {row.lambda_partition_median:.2f} | "
        f"{row.max_partition_tuples_median:.0f} / "
        f"{row.average_partition_tuples_median:.1f} | "
        f"{row.lambda_worker_probe_median:.2f} | "
        f"{row.planning_median_ms:.4f} ms "
        f"({row.planning_overhead_pct_median:.3f}%) | "
        f"{row.total_tasks_median:.0f} | "
        f"{int(row.correctness_passes)} rows |"
        for row in summary[summary["policy"] == "novelty_packed"].sort_values("skew").itertuples()
    )
    seeds = ", ".join(str(int(value)) for value in sorted(data["seed"].unique()))
    report = f"""# Robust adaptive hash-join evaluation

![Evaluation metrics](./robust_evaluation_metrics.png)

## Design and validation

Raw measurements: `{Path(args.input).as_posix()}`. This evaluation uses {data['seed'].nunique()} independent seeds ({seeds}), {data['repetition'].nunique()} timed repetitions per seed/skew, {len(data)} rows, {thread_counts} thread(s), q={q_values}, coarse batch={batch_values}, and pack threshold={threshold_values}; every policy uses identical generated inputs. All {len(data)} rows passed the executable's independent sequential-oracle cardinality check. Worker task totals equal planned tasks on every row.

The 95% intervals resample independent seed clusters: policy-paired timed percent improvements are averaged within each seed, then seeds are bootstrap-resampled. Repetitions are not treated as independent samples. Positive improvement means `novelty_packed` is faster.

## High-skew end-to-end comparison (z={high_skew:g})

| Baseline | Adaptive packed improvement, 95% seed-bootstrap CI |
|---|---:|
{comparison_report}

## Full-join phase metrics at z={high_skew:g}

![Full-join phase breakdown](./full_join_phase_breakdown.png)

Median time by stage; E2E is measured separately and appears as a black tick in the plot.

| Policy | Partition (ms) | Planning (ms) | Build (ms) | Probe (ms) | End-to-end (ms) |
|---|---:|---:|---:|---:|---:|
{phase_breakdown_report}

Paired phase improvements for adaptive packed versus each baseline (positive means faster):

| Phase | Baseline | Improvement (95% seed-bootstrap CI) |
|---|---|---:|
{phase_comparison_report}

The dominant costs are partitioning and hash-table build. Reducing probe time therefore has limited leverage on total join time in this implementation. No end-to-end improvement should be claimed unless its paired interval supports it.

## Adaptive-policy diagnostics across skew

| Probe skew | Median E2E | Speedup vs sequential | Partition λ | Max / mean partition tuples | Worker probe λ | Plan time (plan/E2E) | Task count | Correct timed rows |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
{metric_report}

Partition λ is defined here as maximum probe tuples in any radix partition divided by the arithmetic mean over all final radix partitions (including empty partitions); equivalently, it is the max/average partition-size ratio. It is an input-skew diagnostic, not a scheduling result. Worker probe λ is the maximum worker-assigned probe tuples divided by the mean across the configured workers. Worker task/probe counts show assignment distribution, not stealing.

`planning_ms` measures task-plan construction and is included within end-to-end time. It is a narrow scheduler-planning overhead metric. `probe_ms` includes both probe work and OpenMP dynamic dispatch; these measurements cannot isolate dispatch-only overhead. Stage timings are separately measured wall times and their medians need not sum exactly to the separately measured E2E median.

Planning durations are sub-millisecond on this host; percentage comparisons at that scale are unstable and should not be interpreted as meaningful relative performance. Use their absolute median milliseconds and E2E fraction instead.

Per-worker assignment counters are updated in the probe loop; the counters are cache-line padded and each task's probe-row count is precomputed. This instrumentation is common to all policies but is not separately calibrated, so a small instrumentation perturbation remains possible. Treat timings as results from this instrumented build.

## Steal statistics and claims

Actual steal counts are **not available**. The implementation uses OpenMP `parallel for schedule(dynamic, ...)`; the OpenMP runtime does not expose a portable count of tasks stolen or re-queued to this program. Worker assignments are reported as load-balance diagnostics only and must not be labeled as steal counts.

Report as an empirical result for the stated host, workloads, and parameters. Do not claim universal improvement when a paired interval crosses zero. `paper_fine` is an approximation, not an exact reproduction of the source paper. Windows/MinGW measurements and the tested relation sizes do not replace Linux/WSL confirmation or a different-machine replication.
"""
    report_path = output / "ROBUST_EVALUATION.md"
    report_path.write_text(report, encoding="utf-8")
    print(f"Analyzed {len(data)} correct rows across {data['seed'].nunique()} seeds.")
    print(f"Wrote {plot_path}")
    print(f"Wrote {phase_plot_path}")
    print(f"Wrote {report_path}")


if __name__ == "__main__":
    main()
