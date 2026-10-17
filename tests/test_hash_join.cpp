#include "hash_join/data.hpp"
#include "hash_join/join.hpp"
#include "hash_join/partition.hpp"
#include "hash_join/scheduler.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <omp.h>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

hj::Relations rows(std::initializer_list<std::uint32_t> build_keys,
                   std::initializer_list<std::uint32_t> probe_keys) {
    hj::Relations result;
    std::uint32_t id = 0;
    for (const auto key : build_keys) result.build.push_back({key, id++});
    id = 0;
    for (const auto key : probe_keys) result.probe.push_back({key, id++});
    return result;
}

void compare_all_policies(const hj::Relations& input, int threads = 4) {
    const auto expected = hj::sequential_oracle(input, true);
    hj::Config config;
    config.threads = threads;
    config.radix_bits = 5;
    config.first_pass_bits = 2;
    config.chunk_size = 2;
    config.chunks_per_thread = 4;
    config.split_factor = 1.2;
    const std::vector<hj::Policy> policies{
        hj::Policy::sequential, hj::Policy::static_coarse, hj::Policy::dynamic_coarse,
        hj::Policy::paper_fine, hj::Policy::novelty_budgeted, hj::Policy::novelty_packed
    };
    for (const auto policy : policies) {
        const auto actual = hj::run_join(input, config, policy, {true});
        require(actual.cardinality == expected.cardinality,
                std::string(hj::policy_name(policy)) + " cardinality differs from oracle");
        require(actual.metrics.worker_task_total == actual.metrics.total_tasks,
                std::string(hj::policy_name(policy)) +
                    " worker task accounting differs from planned tasks");
        require(actual.metrics.workers_with_probe_work <=
                    static_cast<std::size_t>(threads),
                std::string(hj::policy_name(policy)) +
                    " reported more active workers than configured");
        require(actual.metrics.max_partition_probe_tuples >=
                    actual.metrics.average_probe_partition_tuples,
                "maximum partition size is below average partition size");
        require(actual.metrics.max_to_average >= 1.0 ||
                    actual.metrics.average_probe_partition_tuples == 0.0,
                "partition imbalance lambda is below one");
        auto actual_pairs = actual.pairs;
        auto expected_pairs = expected.pairs;
        const auto less = [](const hj::JoinPair& a, const hj::JoinPair& b) {
            return a.build_id < b.build_id ||
                   (a.build_id == b.build_id && a.probe_id < b.probe_id);
        };
        std::sort(actual_pairs.begin(), actual_pairs.end(), less);
        std::sort(expected_pairs.begin(), expected_pairs.end(), less);
        require(actual_pairs == expected_pairs,
                std::string(hj::policy_name(policy)) + " output pairs differ from oracle");
    }
}

void test_task_coverage() {
    hj::Relation raw;
    for (std::uint32_t i = 0; i < 1000; ++i) raw.push_back({i, i});
    hj::Config config;
    const auto partitioned = hj::radix_partition(raw, 4, 2);
    const auto plan = hj::make_task_plan(partitioned, hj::Policy::novelty_budgeted,
                                         2, 8, 0.2, 2);
    require(plan.fine_chunk_count <= plan.budget, "novelty exceeds its fine-chunk budget");
    std::vector<std::vector<unsigned int>> seen(partitioned.partition_count());
    for (std::size_t partition = 0; partition < partitioned.partition_count(); ++partition) {
        seen[partition].assign(partitioned.size(partition), 0);
    }
    for (const auto& task : plan.tasks) {
        const std::size_t partition_end = task.partition_end == 0
            ? task.partition + 1 : task.partition_end;
        for (std::size_t partition = task.partition; partition < partition_end; ++partition) {
            const std::size_t begin = task.coarse_bundle ? 0 : task.begin;
            const std::size_t end = task.coarse_bundle
                ? partitioned.size(partition) : task.end;
            require(begin < end, "scheduler emitted an empty task range");
            for (std::size_t i = begin; i < end; ++i) {
                ++seen[partition][i];
            }
        }
    }
    for (const auto& partition : seen) {
        for (const unsigned int count : partition) {
            require(count == 1, "task plan omitted or duplicated a probe tuple");
        }
    }
}

void test_packed_coarse_task_coverage() {
    hj::Relation raw;
    for (std::uint32_t i = 0; i < 10000; ++i) {
        raw.push_back({i < 5000 ? 7U : i, i});
    }
    const auto partitioned = hj::radix_partition(raw, 6, 3);
    const auto plan = hj::make_task_plan(partitioned, hj::Policy::novelty_packed,
                                         4, 4096, 2.5, 8, 5);
    require(plan.packing_enabled, "high-skew input did not enable coarse packing");
    require(plan.fine_chunk_count <= plan.budget,
            "packed novelty exceeds fine-chunk budget");
    require(plan.coarse_tasks > 0, "packed novelty emitted no coarse bundles");
    std::vector<std::vector<unsigned int>> seen(partitioned.partition_count());
    for (std::size_t partition = 0; partition < partitioned.partition_count(); ++partition) {
        seen[partition].assign(partitioned.size(partition), 0);
    }
    for (const auto& task : plan.tasks) {
        const std::size_t partition_end = task.partition_end == 0
            ? task.partition + 1 : task.partition_end;
        for (std::size_t partition = task.partition; partition < partition_end; ++partition) {
            const std::size_t begin = task.coarse_bundle ? 0 : task.begin;
            const std::size_t end = task.coarse_bundle
                ? partitioned.size(partition) : task.end;
            require(begin < end, "packed novelty emitted an empty tuple range");
            for (std::size_t i = begin; i < end; ++i) {
                ++seen[partition][i];
            }
        }
    }
    for (const auto& partition : seen) {
        for (const unsigned int count : partition) {
            require(count == 1, "packed task plan omitted or duplicated a probe tuple");
        }
    }
}

void test_packed_fallback_on_low_imbalance() {
    hj::Relation raw;
    for (std::uint32_t i = 0; i < 10000; ++i) raw.push_back({i, i});
    const auto partitioned = hj::radix_partition(raw, 6, 3);
    const auto plan = hj::make_task_plan(partitioned, hj::Policy::novelty_packed,
                                         4, 4096, 2.5, 8, 5);
    require(!plan.packing_enabled, "low-imbalance input unexpectedly enabled packing");
    require(plan.budget == 32, "configured fine-chunk budget was not retained in fallback");
    require(plan.fine_chunk_count == 0,
            "low-imbalance input should fall back to unsplit coarse tasks");
    require(plan.tasks.size() == plan.coarse_tasks,
            "low-imbalance fallback should schedule individual coarse tasks");
}

void test_scheduler_limits_and_large_chunks() {
    hj::Relation raw;
    for (std::uint32_t i = 0; i < 10000; ++i) raw.push_back({7, i});
    const auto partitioned = hj::radix_partition(raw, 4, 2);

    const auto no_split = hj::make_task_plan(partitioned, hj::Policy::novelty_budgeted,
                                             1, 100000, 0.1, 1);
    require(no_split.budget == 1, "unexpected small chunk budget");
    require(no_split.fine_chunk_count == 0, "budget below two created a split");
    require(no_split.tasks.size() == 1, "unsplit probe partition must be one coarse task");

    const auto one_selected = hj::make_task_plan(partitioned, hj::Policy::novelty_budgeted,
                                                 1, 100000, 0.1, 2);
    require(one_selected.budget == 2, "unexpected two-chunk budget");
    require(one_selected.fine_chunk_count == 2, "two-chunk budget not used exactly");
    require(one_selected.tasks.size() == 2, "selected partition did not become two tasks");

    const auto oversized_chunk = hj::make_task_plan(partitioned, hj::Policy::paper_fine,
                                                    4, 20000, 0.1, 32);
    require(oversized_chunk.fine_chunk_count == 0,
            "paper-style fixed chunk larger than partition should not split it");
    require(oversized_chunk.tasks.size() == 1, "oversized fixed chunk produced multiple tasks");
    require(oversized_chunk.tasks.front().begin == 0 &&
            oversized_chunk.tasks.front().end == raw.size(),
            "coarse task failed to cover large partition");
}

void test_generated_distributions() {
    hj::Config config;
    config.build_size = 2000;
    config.probe_size = 8000;
    config.key_range = 2000;
    config.seed = 42;
    config.threads = 1;
    for (const double skew : {0.0, 0.75, 1.5}) {
        config.zipf_skew = skew;
        const auto input = hj::generate_relations(config);
        compare_all_policies(input, 1);
        const auto again = hj::generate_relations(config);
        require(input.probe.size() == again.probe.size(), "generator changed relation size");
        for (std::size_t i = 0; i < input.probe.size(); ++i) {
            require(input.probe[i].key == again.probe[i].key,
                    "generator is not reproducible for a fixed seed");
        }
    }
    config.zipf_skew = 1.5;
    compare_all_policies(hj::generate_relations(config), 4);
}

} // namespace

int main() {
    try {
        omp_set_dynamic(0);
        compare_all_policies({}, 1);
        compare_all_policies(rows({1}, {1}), 1);
        compare_all_policies(rows({1, 1, 2}, {1, 1, 1, 3}), 1);
        compare_all_policies(rows({1, 2, 3}, {99, 100}), 4);
        compare_all_policies(rows({0, 31, 32, 63, 64, 1000}, {0, 31, 32, 63, 64, 1000}), 4);
        test_task_coverage();
        test_packed_coarse_task_coverage();
        test_packed_fallback_on_low_imbalance();
        test_scheduler_limits_and_large_chunks();
        test_generated_distributions();
        std::cout << "PASS: correctness, pair multisets, generator reproducibility, and task coverage\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
