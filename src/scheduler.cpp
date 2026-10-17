#include "hash_join/scheduler.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace hj {
namespace {

std::size_t ceil_div(std::size_t value, std::size_t divisor) {
    return value / divisor + static_cast<std::size_t>(value % divisor != 0);
}

} // namespace

TaskPlan make_task_plan(const PartitionedRelation& probe, Policy policy, int threads,
                        std::size_t chunk_size, double split_factor,
                        std::size_t chunks_per_thread, std::size_t coarse_batch_size,
                        double packing_imbalance_threshold) {
    if (threads < 1) {
        throw std::invalid_argument("thread count must be positive");
    }
    if (chunk_size == 0) {
        throw std::invalid_argument("chunk size must be positive");
    }
    if (!std::isfinite(split_factor) || split_factor <= 0.0) {
        throw std::invalid_argument("split factor must be finite and positive");
    }
    if (coarse_batch_size == 0) {
        throw std::invalid_argument("coarse batch size must be positive");
    }
    if (!std::isfinite(packing_imbalance_threshold) || packing_imbalance_threshold <= 0.0) {
        throw std::invalid_argument("packing imbalance threshold must be finite and positive");
    }

    TaskPlan plan;
    const std::size_t partition_count = probe.partition_count();
    std::size_t total_probe = 0;
    std::size_t max_size = 0;
    for (std::size_t partition = 0; partition < partition_count; ++partition) {
        const std::size_t size = probe.size(partition);
        total_probe += size;
        max_size = std::max(max_size, size);
    }
    if (partition_count != 0) {
        plan.average_probe_size =
            static_cast<double>(total_probe) / static_cast<double>(partition_count);
    }
    if (plan.average_probe_size > 0.0) {
        plan.max_to_average = static_cast<double>(max_size) / plan.average_probe_size;
    }
    plan.packing_enabled = policy == Policy::novelty_packed &&
        plan.max_to_average >= packing_imbalance_threshold;
    const bool budgeted = policy == Policy::novelty_budgeted ||
                          policy == Policy::novelty_packed;
    const bool allocate_chunks = policy == Policy::novelty_budgeted ||
                                 plan.packing_enabled;
    const bool packed = plan.packing_enabled;

    if (budgeted) {
        if (chunks_per_thread > std::numeric_limits<std::size_t>::max() /
                                    static_cast<std::size_t>(threads)) {
            throw std::overflow_error("fine-chunk budget overflows size_t");
        }
        plan.budget = static_cast<std::size_t>(threads) * chunks_per_thread;
    }

    if (policy == Policy::paper_fine || budgeted) {
        for (std::size_t partition = 0; partition < partition_count; ++partition) {
            const std::size_t size = probe.size(partition);
            if (size != 0 && static_cast<double>(size) > split_factor * plan.average_probe_size) {
                plan.candidate_partitions.push_back(partition);
            }
        }
    }

    if (allocate_chunks) {
        std::sort(plan.candidate_partitions.begin(), plan.candidate_partitions.end(),
                  [&probe](std::size_t left, std::size_t right) {
                      if (probe.size(left) != probe.size(right)) {
                          return probe.size(left) > probe.size(right);
                      }
                      return left < right;
                  });
        const std::size_t selectable = plan.budget / 2;
        for (const std::size_t partition : plan.candidate_partitions) {
            if (plan.selected_partitions.size() == selectable) {
                break;
            }
            if (probe.size(partition) >= 2) {
                plan.selected_partitions.push_back(partition);
            }
        }

        plan.chunks_per_partition.assign(partition_count, 0);
        for (const std::size_t partition : plan.selected_partitions) {
            plan.chunks_per_partition[partition] = 2;
        }
        const std::size_t total_selected_tuples = [&]() {
            std::size_t total = 0;
            for (const std::size_t partition : plan.selected_partitions) {
                total += probe.size(partition);
            }
            return total;
        }();
        const std::size_t target = std::min(plan.budget, total_selected_tuples);
        std::size_t extra = target - 2 * plan.selected_partitions.size();

        while (extra > 0) {
            std::vector<std::size_t> eligible;
            long double total_weight = 0.0L;
            for (const std::size_t partition : plan.selected_partitions) {
                if (plan.chunks_per_partition[partition] < probe.size(partition)) {
                    eligible.push_back(partition);
                    total_weight += static_cast<long double>(probe.size(partition));
                }
            }
            if (eligible.empty()) {
                break;
            }

            std::size_t allocated = 0;
            struct Fraction {
                std::size_t partition;
                long double remainder;
            };
            std::vector<Fraction> fractions;
            for (const std::size_t partition : eligible) {
                const long double quota = static_cast<long double>(extra) *
                    static_cast<long double>(probe.size(partition)) / total_weight;
                const std::size_t capacity =
                    probe.size(partition) - plan.chunks_per_partition[partition];
                const std::size_t share = std::min(capacity, static_cast<std::size_t>(quota));
                plan.chunks_per_partition[partition] += share;
                allocated += share;
                fractions.push_back({partition, quota - std::floor(quota)});
            }
            extra -= allocated;
            if (extra == 0) {
                break;
            }

            std::sort(fractions.begin(), fractions.end(), [](const Fraction& left, const Fraction& right) {
                if (left.remainder != right.remainder) {
                    return left.remainder > right.remainder;
                }
                return left.partition < right.partition;
            });
            std::size_t round_allocated = 0;
            for (const Fraction& fraction : fractions) {
                if (extra == 0) {
                    break;
                }
                if (plan.chunks_per_partition[fraction.partition] < probe.size(fraction.partition)) {
                    ++plan.chunks_per_partition[fraction.partition];
                    --extra;
                    ++round_allocated;
                }
            }
            if (allocated == 0 && round_allocated == 0) {
                break;
            }
        }
    }

    std::vector<bool> selected(partition_count, false);
    for (const std::size_t partition : plan.selected_partitions) {
        selected[partition] = true;
    }
    std::size_t coarse_begin = 0;
    std::size_t coarse_end = 0;
    std::size_t coarse_members = 0;
    std::size_t coarse_probe_tuple_count = 0;
    const auto flush_coarse = [&]() {
        if (coarse_members == 0) {
            return;
        }
        plan.tasks.push_back({coarse_begin, 0, 0, false, coarse_end, true,
                              coarse_probe_tuple_count});
        ++plan.coarse_tasks;
        coarse_members = 0;
        coarse_probe_tuple_count = 0;
    };
    for (std::size_t partition = 0; partition < partition_count; ++partition) {
        const std::size_t size = probe.size(partition);
        if (packed && selected[partition]) {
            flush_coarse();
        }
        if (size == 0) {
            if (packed && coarse_members != 0) {
                coarse_end = partition + 1;
            }
            continue;
        }
        if (packed && !selected[partition]) {
            if (coarse_members == 0) {
                coarse_begin = partition;
            }
            coarse_end = partition + 1;
            ++coarse_members;
            coarse_probe_tuple_count += size;
            if (coarse_members == coarse_batch_size) {
                flush_coarse();
            }
            continue;
        }
        if (packed) {
            flush_coarse();
        }

        std::size_t chunk_count = 1;
        if (policy == Policy::paper_fine &&
            static_cast<double>(size) > split_factor * plan.average_probe_size) {
            chunk_count = ceil_div(size, chunk_size);
        } else if (allocate_chunks && selected[partition]) {
            chunk_count = plan.chunks_per_partition[partition];
        }

        const bool is_fine = chunk_count > 1;
        if (is_fine) {
            plan.fine_chunk_count += chunk_count;
        } else {
            ++plan.coarse_tasks;
        }
        const std::size_t base = size / chunk_count;
        const std::size_t remainder = size % chunk_count;
        std::size_t offset = 0;
        for (std::size_t chunk = 0; chunk < chunk_count; ++chunk) {
            const std::size_t length = base + static_cast<std::size_t>(chunk < remainder);
            plan.tasks.push_back({partition, offset, offset + length, is_fine,
                                  partition + 1, false, length});
            offset += length;
        }
        if (offset != size) {
            throw std::logic_error("scheduler plan failed to cover a probe partition");
        }
    }
    if (packed) {
        flush_coarse();
    }
    return plan;
}

} // namespace hj
