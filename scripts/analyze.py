#!/usr/bin/env python3
"""Summarize measured CSV rows, paired comparisons, seed bootstrap CIs, and plots."""

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


def bootstrap_seed_ci(seed_values, iterations=5000, seed=2025):
    if len(seed_values) < 2:
        return np.nan, np.nan
    rng = np.random.default_rng(seed)
    values = np.asarray(seed_values, dtype=float)
    samples = rng.choice(values, size=(iterations, len(values)), replace=True).mean(axis=1)
    return tuple(np.quantile(samples, [0.025, 0.975]))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", default="results/raw_measurements.csv")
    parser.add_argument("--output-dir", default="results")
    args = parser.parse_args()
    output = Path(args.output_dir)
    output.mkdir(parents=True, exist_ok=True)
    data = pd.read_csv(args.input)
    data = data[data["correct"].astype(str).str.lower().isin(["true", "1"])].copy()
    if data.empty:
        raise ValueError("No correct measurement rows found; refusing to analyze invalid data")

    group_columns = ["build_size", "probe_size", "skew", "threads", "policy"]
    summary = data.groupby(group_columns, as_index=False).agg(
        runs=("end_to_end_ms", "count"),
        runtime_median_ms=("end_to_end_ms", "median"),
        runtime_mean_ms=("end_to_end_ms", "mean"),
        runtime_std_ms=("end_to_end_ms", "std"),
        partition_median_ms=("partition_ms", "median"),
        build_median_ms=("build_ms", "median"),
        probe_median_ms=("probe_ms", "median"),
        imbalance_median=("max_to_average", "median"),
        candidates_median=("candidates", "median"),
        selected_median=("selected", "median"),
        fine_chunks_median=("fine_chunks", "median"),
        coarse_tasks_median=("coarse_tasks", "median"),
        tasks_median=("total_tasks", "median"),
    )
    summary.to_csv(output / "summary.csv", index=False)

    paired_keys = ["build_size", "probe_size", "skew", "threads", "seed", "repetition"]
    comparison_rows = []
    for measurement in ["end_to_end_ms", "probe_ms"]:
        timings = data.pivot_table(index=paired_keys, columns="policy",
                                   values=measurement, aggfunc="first").reset_index()
        for method in ["novelty_budgeted", "novelty_packed"]:
            for baseline in ["static", "dynamic", "paper_fine", "novelty_budgeted"]:
                if baseline == method:
                    continue
                if baseline not in timings or method not in timings:
                    continue
                paired = timings.dropna(subset=[baseline, method]).copy()
                paired["improvement_pct"] = (
                    (paired[baseline] - paired[method]) / paired[baseline] * 100.0
                )
                by_seed = paired.groupby(
                    ["build_size", "probe_size", "skew", "threads", "seed"],
                    as_index=False,
                )["improvement_pct"].mean()
                for key, group in by_seed.groupby(
                    ["build_size", "probe_size", "skew", "threads"]
                ):
                    low, high = bootstrap_seed_ci(group["improvement_pct"].tolist())
                    comparison_rows.append({
                        "build_size": key[0], "probe_size": key[1], "skew": key[2],
                        "threads": key[3], "measurement": measurement,
                        "method": method, "baseline": baseline,
                        "paired_seed_count": group["seed"].nunique(),
                        "mean_improvement_pct": group["improvement_pct"].mean(),
                        "bootstrap_95_low_pct": low, "bootstrap_95_high_pct": high,
                        "ci_status": "seed-bootstrap" if group["seed"].nunique() >= 2
                                     else "not estimable: fewer than two seeds",
                    })
    comparisons = pd.DataFrame(comparison_rows)
    comparisons.to_csv(output / "paired_comparisons.csv", index=False)
    if not comparisons.empty:
        baselines = ["static", "dynamic", "paper_fine", "novelty_budgeted"]
        for measurement, filename, label in [
            ("end_to_end_ms", "paired_novelty_comparison.png", "End-to-end runtime reduction"),
            ("probe_ms", "paired_novelty_probe_comparison.png", "Probe-phase runtime reduction"),
        ]:
            fig, axes = plt.subplots(
                1, len(baselines), figsize=(18, 4.5), sharey=True, squeeze=False
            )
            axes = axes[0]
            subset_all = comparisons[comparisons["measurement"] == measurement]
            for axis, baseline in zip(axes, baselines):
                panel = subset_all[subset_all["baseline"] == baseline]
                for (method, size), curve in panel.groupby(["method", "build_size"]):
                    curve = curve.sort_values("skew")
                    means = curve["mean_improvement_pct"].to_numpy()
                    lows = curve["bootstrap_95_low_pct"].to_numpy()
                    highs = curve["bootstrap_95_high_pct"].to_numpy()
                    series_label = f"{method}, n={size:,}"
                    if np.isfinite(lows).all() and np.isfinite(highs).all():
                        axis.errorbar(curve["skew"], means,
                                      yerr=np.vstack([means - lows, highs - means]),
                                      marker="o", capsize=3, label=series_label)
                    else:
                        axis.plot(curve["skew"], means, marker="o", label=series_label)
                axis.axhline(0.0, color="black", linewidth=0.8, linestyle="--")
                axis.set(title=f"vs {baseline}", xlabel="Probe Zipf parameter")
                axis.grid(True, alpha=0.3)
                axis.legend(fontsize=8)
            axes[0].set_ylabel(f"{label} (%)\n(positive = faster)")
            fig.suptitle(
                f"Paired {measurement} comparison; intervals bootstrap independent seeds"
            )
            fig.tight_layout()
            fig.savefig(output / filename, dpi=160)
            plt.close(fig)

    size_values = sorted(data["build_size"].unique())
    for size in size_values:
        subset = summary[summary["build_size"] == size]
        fig, ax = plt.subplots(figsize=(8, 5))
        for policy in POLICIES:
            curve = subset[subset["policy"] == policy].sort_values("skew")
            if not curve.empty:
                ax.plot(curve["skew"], curve["runtime_median_ms"], marker="o", label=policy)
        ax.set(title=f"Measured end-to-end time (R and S = {size:,} tuples)",
               xlabel="Probe Zipf parameter", ylabel="Median end-to-end time (ms)")
        ax.grid(True, alpha=0.3)
        ax.legend()
        fig.tight_layout()
        fig.savefig(output / f"runtime_skew_{size}.png", dpi=160)
        plt.close(fig)

    novelty = data[data["policy"].isin(["novelty_budgeted", "novelty_packed"])]
    task_summary = novelty.groupby(["build_size", "skew", "policy"], as_index=False).agg(
        actual_fine_chunks=("fine_chunks", "median"),
        configured_budget=("chunk_budget", "median"),
        total_tasks=("total_tasks", "median"),
        selected_partitions=("selected", "median"),
        candidates=("candidates", "median"),
    )
    task_summary.to_csv(output / "novelty_task_diagnostics.csv", index=False)
    if not task_summary.empty:
        fig, axes = plt.subplots(1, 2, figsize=(11, 4.5))
        for (size, policy), curve in task_summary.groupby(["build_size", "policy"]):
            curve = curve.sort_values("skew")
            axes[0].plot(curve["skew"], curve["actual_fine_chunks"], marker="o",
                         label=f"{policy} fine chunks, n={size:,}")
            axes[0].plot(curve["skew"], curve["configured_budget"], linestyle="--",
                         alpha=0.65, label=f"{policy} budget, n={size:,}")
            axes[1].plot(curve["skew"], curve["total_tasks"], marker="o",
                         label=f"{policy}, n={size:,}")
        axes[0].set(xlabel="Probe Zipf parameter", ylabel="Chunk count",
                    title="Budgeted fine chunks")
        axes[1].set(xlabel="Probe Zipf parameter", ylabel="Scheduled tasks",
                    title="Actual task count")
        for axis in axes:
            axis.grid(True, alpha=0.3)
            axis.legend(fontsize=8)
        fig.tight_layout()
        fig.savefig(output / "novelty_task_counts.png", dpi=160)
        plt.close(fig)
    print(f"Analyzed {len(data)} correct timed rows; results in {output}")


if __name__ == "__main__":
    main()
