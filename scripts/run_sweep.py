#!/usr/bin/env python3
"""Run paired policy measurements across relation sizes, skews, and seeds."""

import argparse
import subprocess
import tempfile
from pathlib import Path

import pandas as pd


def values(text, cast):
    return [cast(part.strip()) for part in text.split(",") if part.strip()]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default="build/hashjoin")
    parser.add_argument("--output", default="results/raw_measurements.csv")
    parser.add_argument("--sizes", default="100000,500000")
    parser.add_argument("--skews", default="0,0.75,1.25,1.5")
    parser.add_argument("--seeds", default="11,29,47")
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--key-range", type=int, default=1000000)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--q", type=int, default=32)
    parser.add_argument("--coarse-batch", type=int, default=8)
    parser.add_argument("--pack-threshold", type=float, default=16.0)
    parser.add_argument("--chunk-size", type=int, default=4096)
    parser.add_argument("--split-factor", type=float, default=2.5)
    args = parser.parse_args()

    binary = Path(args.binary).resolve()
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    frames = []
    with tempfile.TemporaryDirectory(prefix="hashjoin-sweep-") as temporary:
        run_index = 0
        for size in values(args.sizes, int):
            for skew in values(args.skews, float):
                for seed in values(args.seeds, int):
                    csv_path = Path(temporary) / f"run-{run_index}.csv"
                    command = [
                        str(binary), "--r-size", str(size), "--s-size", str(size),
                        "--key-range", str(args.key_range), "--skew", str(skew),
                        "--seed", str(seed), "--threads", str(args.threads),
                        "--repetitions", str(args.repetitions), "--warmups", str(args.warmups),
                        "--q", str(args.q), "--chunk-size", str(args.chunk_size),
                        "--coarse-batch", str(args.coarse_batch),
                        "--pack-threshold", str(args.pack_threshold),
                        "--split-factor", str(args.split_factor), "--policy", "all",
                        "--csv", str(csv_path),
                    ]
                    print(f"[{run_index + 1}] size={size} skew={skew} seed={seed}", flush=True)
                    subprocess.run(command, check=True)
                    frame = pd.read_csv(csv_path)
                    if not frame["correct"].astype(str).str.lower().isin(["true", "1"]).all():
                        raise RuntimeError(f"incorrect output in sweep run: {command}")
                    frames.append(frame)
                    run_index += 1
    pd.concat(frames, ignore_index=True).to_csv(output, index=False)
    print(f"Wrote {sum(len(frame) for frame in frames)} measurements to {output}")


if __name__ == "__main__":
    main()
