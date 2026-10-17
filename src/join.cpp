#include "hash_join/join.hpp"

#include "hash_join/partition.hpp"
#include "hash_join/scheduler.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include <omp.h>

namespace hj {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::uint32_t no_row = std::numeric_limits<std::uint32_t>::max();

struct Entry {
    std::uint64_t count{};
    std::uint32_t head{no_row};
};

struct LocalHashTable {
    std::unordered_map<std::uint32_t, Entry> entries;
    std::vector<std::uint32_t> next;
};

struct alignas(64) WorkerProbeMetrics {
    std::size_t tasks{};
    std::size_t probe_tuples{};
};

double elapsed_ms(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

void build_one(const PartitionedRelation& build, std::size_t partition, LocalHashTable& table) {
    const std::size_t count = build.size(partition);
    table.entries.clear();
    table.entries.max_load_factor(0.7F);
    table.entries.reserve(count);
    table.next.resize(count, no_row);
    std::size_t local_index = 0;
    for (const Row* row = build.begin(partition); row != build.end(partition); ++row, ++local_index) {
        auto [iterator, inserted] = table.entries.emplace(row->key, Entry{0, no_row});
        if (!inserted) {
            table.next[local_index] = iterator->second.head;
            iterator->second.head = static_cast<std::uint32_t>(local_index);
        } else {
            iterator->second.head = static_cast<std::uint32_t>(local_index);
        }
        ++iterator->second.count;
    }
}

void probe_task(const PartitionedInput& input, const std::vector<LocalHashTable>& tables,
                const ProbeTask& task, bool collect_pairs, JoinResult& result) {
    const std::size_t partition_end = task.partition_end == 0
        ? task.partition + 1 : task.partition_end;
    for (std::size_t partition = task.partition; partition < partition_end; ++partition) {
        const LocalHashTable& table = tables[partition];
        const Row* probe_begin = input.probe.begin(partition);
        const std::size_t begin = task.coarse_bundle ? 0 : task.begin;
        const std::size_t end = task.coarse_bundle
            ? input.probe.size(partition) : task.end;
        for (std::size_t index = begin; index < end; ++index) {
            const Row& probe_row = probe_begin[index];
            const auto found = table.entries.find(probe_row.key);
            if (found == table.entries.end()) {
                continue;
            }
            result.cardinality += found->second.count;
            if (collect_pairs) {
                std::uint32_t build_index = found->second.head;
                while (build_index != no_row) {
                    const Row& build_row = input.build.begin(partition)[build_index];
                    result.pairs.push_back({build_row.payload, probe_row.payload});
                    build_index = table.next[build_index];
                }
            }
        }
    }
}

void record_worker_metrics(JoinMetrics& metrics,
                           const std::vector<WorkerProbeMetrics>& workers) {
    std::size_t total_tasks = 0;
    std::size_t total_probe_tuples = 0;
    std::size_t max_tasks = 0;
    std::size_t max_probe_tuples = 0;
    std::size_t active_workers = 0;
    for (const WorkerProbeMetrics& worker : workers) {
        total_tasks += worker.tasks;
        total_probe_tuples += worker.probe_tuples;
        max_tasks = std::max(max_tasks, worker.tasks);
        max_probe_tuples = std::max(max_probe_tuples, worker.probe_tuples);
        active_workers += static_cast<std::size_t>(worker.probe_tuples != 0);
    }
    metrics.max_worker_tasks = max_tasks;
    metrics.max_worker_probe_tuples = max_probe_tuples;
    metrics.workers_with_probe_work = active_workers;
    metrics.worker_task_total = total_tasks;
    if (!workers.empty() && total_tasks != 0) {
        metrics.worker_task_max_to_average =
            static_cast<double>(max_tasks) * static_cast<double>(workers.size()) /
            static_cast<double>(total_tasks);
    }
    if (!workers.empty() && total_probe_tuples != 0) {
        metrics.worker_probe_max_to_average =
            static_cast<double>(max_probe_tuples) *
                static_cast<double>(workers.size()) /
            static_cast<double>(total_probe_tuples);
    }
}

} // namespace

std::string policy_name_string(Policy policy) {
    return policy_name(policy);
}

Policy parse_policy(const std::string& value) {
    if (value == "sequential") return Policy::sequential;
    if (value == "static") return Policy::static_coarse;
    if (value == "dynamic") return Policy::dynamic_coarse;
    if (value == "paper_fine") return Policy::paper_fine;
    if (value == "novelty_budgeted") return Policy::novelty_budgeted;
    if (value == "novelty_packed") return Policy::novelty_packed;
    throw std::invalid_argument("unknown policy: " + value);
}

JoinResult run_join(const Relations& relations, const Config& config, Policy policy,
                    JoinOptions options) {
    if (config.threads < 1 || config.dynamic_chunk < 1) {
        throw std::invalid_argument("thread count and dynamic chunk must be positive");
    }
    const auto overall_start = Clock::now();
    JoinResult result;
    const auto partition_start = Clock::now();
    const PartitionedInput input =
        radix_partition(relations, config.radix_bits, config.first_pass_bits);
    const auto partition_end = Clock::now();
    result.metrics.partition_ms = elapsed_ms(partition_start, partition_end);

    const std::size_t partition_count = input.probe.partition_count();
    const auto planning_start = Clock::now();
    const TaskPlan plan = make_task_plan(input.probe, policy, config.threads, config.chunk_size,
                                         config.split_factor, config.chunks_per_thread,
                                         config.coarse_batch_size,
                                         config.packing_imbalance_threshold);
    const auto planning_end = Clock::now();
    result.metrics.planning_ms = elapsed_ms(planning_start, planning_end);
    result.metrics.candidate_partitions = plan.candidate_partitions.size();
    result.metrics.selected_partitions = plan.selected_partitions.size();
    result.metrics.fine_chunks = plan.fine_chunk_count;
    result.metrics.total_tasks = plan.tasks.size();
    result.metrics.budget = plan.budget;
    result.metrics.coarse_tasks = plan.coarse_tasks;
    result.metrics.max_partition_probe_tuples = 0;
    for (std::size_t partition = 0; partition < partition_count; ++partition) {
        result.metrics.max_partition_probe_tuples = std::max(
            result.metrics.max_partition_probe_tuples, input.probe.size(partition));
    }
    result.metrics.average_probe_partition_tuples = plan.average_probe_size;
    result.metrics.max_to_average = plan.max_to_average;
    result.metrics.packing_enabled = plan.packing_enabled;

    std::vector<WorkerProbeMetrics> worker_metrics(static_cast<std::size_t>(config.threads));
    std::vector<LocalHashTable> tables(partition_count);
    const auto build_start = Clock::now();
    if (policy == Policy::sequential) {
        for (std::size_t partition = 0; partition < partition_count; ++partition) {
            build_one(input.build, partition, tables[partition]);
        }
    } else {
        const int threads = config.threads;
        const auto bound = static_cast<std::int64_t>(partition_count);
#pragma omp parallel for schedule(static) num_threads(threads)
        for (std::int64_t partition = 0; partition < bound; ++partition) {
            build_one(input.build, static_cast<std::size_t>(partition),
                      tables[static_cast<std::size_t>(partition)]);
        }
    }
    const auto build_end = Clock::now();
    result.metrics.build_ms = elapsed_ms(build_start, build_end);

    const auto probe_start = Clock::now();
    if (options.collect_pairs) {
        std::vector<std::vector<JoinPair>> task_pairs(plan.tasks.size());
        std::vector<std::uint64_t> task_counts(plan.tasks.size(), 0);
        if (policy == Policy::sequential) {
            for (std::size_t task_index = 0; task_index < plan.tasks.size(); ++task_index) {
                JoinResult local;
                probe_task(input, tables, plan.tasks[task_index], true, local);
                ++worker_metrics[0].tasks;
                worker_metrics[0].probe_tuples += plan.tasks[task_index].probe_tuple_count;
                task_counts[task_index] = local.cardinality;
                task_pairs[task_index] = std::move(local.pairs);
            }
        } else if (policy == Policy::static_coarse) {
            const auto bound = static_cast<std::int64_t>(plan.tasks.size());
#pragma omp parallel for schedule(static) num_threads(config.threads)
            for (std::int64_t i = 0; i < bound; ++i) {
                JoinResult local;
                probe_task(input, tables, plan.tasks[static_cast<std::size_t>(i)], true, local);
                const std::size_t worker = static_cast<std::size_t>(omp_get_thread_num());
                ++worker_metrics[worker].tasks;
                worker_metrics[worker].probe_tuples +=
                    plan.tasks[static_cast<std::size_t>(i)].probe_tuple_count;
                task_counts[static_cast<std::size_t>(i)] = local.cardinality;
                task_pairs[static_cast<std::size_t>(i)] = std::move(local.pairs);
            }
        } else {
            const auto bound = static_cast<std::int64_t>(plan.tasks.size());
#pragma omp parallel for schedule(dynamic, config.dynamic_chunk) num_threads(config.threads)
            for (std::int64_t i = 0; i < bound; ++i) {
                JoinResult local;
                probe_task(input, tables, plan.tasks[static_cast<std::size_t>(i)], true, local);
                const std::size_t worker = static_cast<std::size_t>(omp_get_thread_num());
                ++worker_metrics[worker].tasks;
                worker_metrics[worker].probe_tuples +=
                    plan.tasks[static_cast<std::size_t>(i)].probe_tuple_count;
                task_counts[static_cast<std::size_t>(i)] = local.cardinality;
                task_pairs[static_cast<std::size_t>(i)] = std::move(local.pairs);
            }
        }
        for (std::size_t i = 0; i < task_pairs.size(); ++i) {
            result.cardinality += task_counts[i];
            result.pairs.insert(result.pairs.end(),
                                std::make_move_iterator(task_pairs[i].begin()),
                                std::make_move_iterator(task_pairs[i].end()));
        }
    } else if (policy == Policy::sequential) {
        for (const ProbeTask& task : plan.tasks) {
            probe_task(input, tables, task, false, result);
            ++worker_metrics[0].tasks;
            worker_metrics[0].probe_tuples += task.probe_tuple_count;
        }
    } else if (policy == Policy::static_coarse) {
        std::vector<std::uint64_t> task_counts(plan.tasks.size(), 0);
        const auto bound = static_cast<std::int64_t>(plan.tasks.size());
#pragma omp parallel for schedule(static) num_threads(config.threads)
        for (std::int64_t i = 0; i < bound; ++i) {
            JoinResult local;
            probe_task(input, tables, plan.tasks[static_cast<std::size_t>(i)], false, local);
            const std::size_t worker = static_cast<std::size_t>(omp_get_thread_num());
            ++worker_metrics[worker].tasks;
            worker_metrics[worker].probe_tuples +=
                plan.tasks[static_cast<std::size_t>(i)].probe_tuple_count;
            task_counts[static_cast<std::size_t>(i)] = local.cardinality;
        }
        for (const std::uint64_t count : task_counts) {
            result.cardinality += count;
        }
    } else {
        std::vector<std::uint64_t> task_counts(plan.tasks.size(), 0);
        const auto bound = static_cast<std::int64_t>(plan.tasks.size());
#pragma omp parallel for schedule(dynamic, config.dynamic_chunk) num_threads(config.threads)
        for (std::int64_t i = 0; i < bound; ++i) {
            JoinResult local;
            probe_task(input, tables, plan.tasks[static_cast<std::size_t>(i)], false, local);
            const std::size_t worker = static_cast<std::size_t>(omp_get_thread_num());
            ++worker_metrics[worker].tasks;
            worker_metrics[worker].probe_tuples +=
                plan.tasks[static_cast<std::size_t>(i)].probe_tuple_count;
            task_counts[static_cast<std::size_t>(i)] = local.cardinality;
        }
        for (const std::uint64_t count : task_counts) {
            result.cardinality += count;
        }
    }
    record_worker_metrics(result.metrics, worker_metrics);
    const auto probe_end = Clock::now();
    result.metrics.probe_ms = elapsed_ms(probe_start, probe_end);
    result.metrics.end_to_end_ms = elapsed_ms(overall_start, probe_end);
    return result;
}

} // namespace hj
