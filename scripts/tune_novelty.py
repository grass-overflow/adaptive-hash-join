#!/usr/bin/env python3
"""Run an explicit, seed-separated q sweep for the bounded novelty budget."""

import argparse
import subprocess
import tempfile
from pathlib import Path

import pandas as pd


def parse_list(text, cast):
    return [cast(item.strip()) for item in text.split(",") if item.strip()]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default="build/hashjoin")
    parser.add_argument("--output", default="results/novelty_tuning.csv")
    parser.add_argument("--size", type=int, default=1000000)
    parser.add_argument("--skews", default="0,0.75,1.5")
    parser.add_argument("--seeds", default="201,203,205")
    parser.add_argument("--q-values", default="16,24,32,48,64")
    parser.add_argument("--batch-values", default="4,8,16,32")
    parser.add_argument("--pack-threshold", type=float, default=16.0)
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--key-range", type=int, default=1000000)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--chunk-size", type=int, default=4096)
    parser.add_argument("--split-factor", type=float, default=2.5)
    args = parser.parse_args()

    binary = Path(args.binary).resolve()
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    frames = []
    total = (len(parse_list(args.skews, float)) * len(parse_list(args.seeds, int)) *
             len(parse_list(args.q_values, int)) * len(parse_list(args.batch_values, int)))
    with tempfile.TemporaryDirectory(prefix="hashjoin-tuning-") as temporary:
        run = 0
        for skew in parse_list(args.skews, float):
            for seed in parse_list(args.seeds, int):
                for q in parse_list(args.q_values, int):
                    for batch in parse_list(args.batch_values, int):
                        csv_path = Path(temporary) / f"tuning-{run}.csv"
                        command = [
                            str(binary), "--r-size", str(args.size), "--s-size", str(args.size),
                            "--key-range", str(args.key_range), "--skew", str(skew),
                            "--seed", str(seed), "--threads", str(args.threads),
                            "--repetitions", str(args.repetitions), "--warmups", str(args.warmups),
                            "--q", str(q), "--chunk-size", str(args.chunk_size),
                            "--coarse-batch", str(batch),
                            "--pack-threshold", str(args.pack_threshold),
                            "--split-factor", str(args.split_factor), "--policy", "all",
                            "--csv", str(csv_path),
                        ]
                        print(f"[{run + 1}/{total}] z={skew} seed={seed} q={q} batch={batch}",
                              flush=True)
                        subprocess.run(command, check=True)
                        frame = pd.read_csv(csv_path)
                        if not frame["correct"].astype(str).str.lower().isin(["true", "1"]).all():
                            raise RuntimeError(f"incorrect output in tuning run: {command}")
                        frames.append(frame)
                        run += 1

    data = pd.concat(frames, ignore_index=True)
    data.to_csv(output, index=False)
    paired_keys = ["build_size", "probe_size", "skew", "seed", "repetition", "coarse_batch"]
    pivot = data.pivot_table(index=paired_keys, columns=["policy", "q"],
                             values="end_to_end_ms", aggfunc="first")
    rows = []
    for q in parse_list(args.q_values, int):
        for batch in parse_list(args.batch_values, int):
            for skew in parse_list(args.skews, float):
                subset = pivot[
                    (pivot.index.get_level_values("skew") == skew) &
                    (pivot.index.get_level_values("coarse_batch") == batch)
                ]
                for baseline in ("static", "dynamic", "paper_fine"):
                    improvements = []
                    for key, row in subset.iterrows():
                        novelty_time = row.get(("novelty_packed", q))
                        baseline_time = row.get((baseline, q))
                        if pd.notna(novelty_time) and pd.notna(baseline_time):
                            improvements.append(
                                (baseline_time - novelty_time) / baseline_time * 100.0
                            )
                    rows.append({
                        "size": args.size, "skew": skew, "q": q,
                        "coarse_batch": batch, "baseline": baseline,
                        "paired_runs": len(improvements),
                        "mean_runtime_reduction_pct": (
                            sum(improvements) / len(improvements) if improvements
                            else float("nan")
                        ),
                    })
    pd.DataFrame(rows).to_csv(output.with_name(output.stem + "_summary.csv"), index=False)
    print(f"Wrote {len(data)} tuning measurements to {output}")


if __name__ == "__main__":
    main()
