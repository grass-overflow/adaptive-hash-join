# Viva notes

## Core idea

A radix hash join hashes the same way on both relations, so matching keys land in the same partition. Each build partition gets a local hash table, and its probe tuples look up only that table. Two 5-bit passes produce 32 intermediate groups and 1,024 final partitions in the project configuration.

## Why scheduling matters

With skew, probe tuple counts vary between radix partitions. Assigning whole partitions statically can leave some workers idle while one handles a large partition. Coarse dynamic scheduling redistributes whole partitions but cannot subdivide a single straggler. Paper-style fine-grained decomposition splits sufficiently oversized probe partitions into fixed-size chunks. The proposed novelty adds a cap on fine chunks and apportions that cap according to candidate sizes.

## Precisely what the novelty guarantees

For the always-on `novelty_budgeted` policy, and for `novelty_packed` when imbalance reaches its configurable gate, selected partition chunks are contiguous and non-empty and form an exact disjoint cover. Below the `novelty_packed` gate, it falls back to unsplit coarse dynamic tasks. Actual fine chunks never exceed `B=T*q`; the total task count may exceed B because coarse tasks are additional. When there is insufficient budget for at least two chunks per candidate, lower-ranked candidates remain coarse.

## Correctness argument

Both radix passes use bits from one deterministic key hash, identically for R and S; equal keys therefore share a final partition. A build row belongs to one partition and enters one local table once. Probe task ranges divide each probe partition without overlap or gaps. Every matching probe visits its partition's table; duplicate build keys contribute their full multiplicity. Tests compare the resulting cardinality and exact pair multiset with a separate sequential oracle.

## How to explain result interpretation

Use end-to-end time, not task count alone. A lower task count may reduce scheduling overhead, but it may also make stragglers worse. Break out partition/build/probe time, report max/mean probe partition size, compare against static, coarse dynamic, and paper-style fine tasks on the same generated input, and show variation across independent seeds. Distinguish project measurements from the paper's historical hardware results.

The larger confirmation supports a limited high-skew probe-phase claim: at z=1.5 with q=64, `novelty_packed` reduced probe time versus static by 28.39% (95% CI 21.59% to 35.18%) and coarse dynamic by 21.21% (13.55% to 28.10%) across 12 seed clusters. It did not improve end-to-end time versus static/dynamic with resolved evidence, and was slower than paper-style fine and unbundled budgeted scheduling in that run. Do not claim universal end-to-end superiority; Linux confirmation remains pending.

## Limitations to disclose

The paper does not give a numerical threshold/chunk size, so `paper_fine` uses tunable implementation choices rather than asserting exact reproduction. Build tables are prepared as a separate common phase; this sacrifices the paper's build-then-probe cache locality but makes concurrent chunk probes safe without rebuilding each hash table. The current proposal's cost coefficients, cache-migration-aware stealing, and custom work-stealing queues are not implemented. Performance on Linux/WSL and five-million-row runs must be explicitly marked unverified unless executed.
