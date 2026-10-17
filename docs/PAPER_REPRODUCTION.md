# Paper method and reproduction boundary

## Supplied source

The supplied PDF is C. Balkesen, J. Teubner, G. Alonso, and M. T. Ozsu, “Main-Memory Hash Joins on Multi-Core CPUs: Tuning to the Underlying Hardware,” ICDE 2013. Its focus is hardware-conscious versus hardware-oblivious in-memory hash joins, not a bounded chunk-budget scheduler. The proposal slides and methodology diagrams are separate project material, not claims made by that paper.

## Relevant algorithm in the paper

The paper's parallel radix join partitions both input relations with the same hash-derived radix bits. It uses multiple radix passes to keep each pass's fan-out within hardware/TLB constraints; then it creates a local hash table for each build partition and probes the corresponding probe partition. Its optimized implementation builds and probes each partition together to reduce cache eviction. Parallel partitioning is contention-avoiding: per-thread histograms, prefix sums, reserved output ranges, and scatter.

For skew, the paper reports that simple task queueing may leave barrier idle time. Its fine-granular decomposition breaks an oversized probe partition into smaller chunks when the partition is significantly larger than the expected uniform size; chunks are processed by workers in concert. The paper does not specify a numeric split threshold or a universal chunk size. It reports about 25% higher join throughput in the illustrated z=1.5 experiment after this modification. This published result is not a result from this project.

## What this project implements

The project uses two radix passes, by default 5+5 bits (1,024 final partitions), as specified in the project slides. Radix partitioning is a single-process histogram/prefix/scatter implementation; local build tables and probe ranges are parallelized with OpenMP. Static, coarse dynamic, paper-style fixed-size chunking, and the separately selectable budgeted novelty share the same partitioning/build implementation. To permit safe concurrent probe chunks without rebuilding the same hash table repeatedly, all local tables are built in one parallel phase and then treated as read-only during probe. This differs from the paper's cache-aware partition-local build-then-probe ordering.

`paper_fine` is therefore an explicit algorithmic approximation of the paper's described work decomposition, not a source-code or hardware-faithful reproduction. The threshold is configurable (default 2.5 times mean partition size) and the fixed chunk size is configurable (default 4,096 probe tuples); neither number is claimed to come from Balkesen et al. The paper's quantitative experiments used historical machines, workload A/B cardinalities, and compiler/runtime conditions documented in the paper. Our synthetic 5-million-tuple target, Windows MinGW runs, or any measured output here must not be presented as reproducing those published results.

## Published setup and reported observations

The paper's workloads use 8-byte key/payload tuples with |R|=16·2^20 and |S|=256·2^20 tuples (workload A), and 4-byte key/payload tuples with both relations at 128 million tuples (workload B). Probe keys reference the build relation; most experiments are uniform, with a separate Zipf skew sweep from z=0 through 1.75. Tested hardware includes Intel Nehalem, Intel Sandy Bridge, AMD Bulldozer, and Sun UltraSPARC T2, with GCC 4.4.3/4.6.3 and platform-specific settings.

Reported results include about 25% improvement from fine-granular task decomposition in the illustrated skew experiment, 4.6x speedup for the tuned radix join on a similar Nehalem setup, 3.5x radix-join advantage over no-partitioning on Intel for workload B, and peak throughput of 196 million tuples/s on the paper's Sandy Bridge system. These are source-paper measurements and hardware-specific; they are not measured by this code. The paper's tuned optimal radix widths also differ by architecture, so our required 10-bit configuration is a project configuration rather than a reproduction of its tuned settings.

## Validation standard

The sequential oracle is independently implemented over the original input relations, not the radix output. Tests compare both cardinality and the multiset of `(build row id, probe row id)` pairs. A run is included in measurements only after the CLI confirms the oracle cardinality. See [EXPERIMENTS.md](./EXPERIMENTS.md) for this implementation's verified platform and run boundary.
