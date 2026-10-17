#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hj {

struct Row {
    std::uint32_t key{};
    std::uint32_t payload{};
};

using Relation = std::vector<Row>;

struct Relations {
    Relation build;
    Relation probe;
};

enum class Policy {
    sequential,
    static_coarse,
    dynamic_coarse,
    paper_fine,
    novelty_budgeted,
    novelty_packed
};

inline const char* policy_name(Policy policy) {
    switch (policy) {
    case Policy::sequential: return "sequential";
    case Policy::static_coarse: return "static";
    case Policy::dynamic_coarse: return "dynamic";
    case Policy::paper_fine: return "paper_fine";
    case Policy::novelty_budgeted: return "novelty_budgeted";
    case Policy::novelty_packed: return "novelty_packed";
    }
    return "unknown";
}

struct Config {
    std::size_t build_size{100'000};
    std::size_t probe_size{100'000};
    std::uint32_t key_range{100'000};
    double zipf_skew{0.0};
    std::uint64_t seed{1};
    int threads{4};
    int radix_bits{10};
    int first_pass_bits{5};
    std::size_t chunk_size{4096};
    double split_factor{2.5};
    double packing_imbalance_threshold{16.0};
    std::size_t chunks_per_thread{32};
    std::size_t coarse_batch_size{8};
    int dynamic_chunk{1};
};

struct PartitionedRelation {
    Relation tuples;
    std::vector<std::size_t> offsets;

    std::size_t partition_count() const {
        return offsets.empty() ? 0 : offsets.size() - 1;
    }

    std::size_t size(std::size_t partition) const {
        return offsets[partition + 1] - offsets[partition];
    }

    const Row* begin(std::size_t partition) const {
        return tuples.empty() ? tuples.data() : tuples.data() + offsets[partition];
    }

    const Row* end(std::size_t partition) const {
        return tuples.empty() ? tuples.data() : tuples.data() + offsets[partition + 1];
    }
};

struct PartitionedInput {
    PartitionedRelation build;
    PartitionedRelation probe;
};

struct ProbeTask {
    std::size_t partition{};
    std::size_t begin{};
    std::size_t end{};
    bool fine_grained{};
    std::size_t partition_end{};
    bool coarse_bundle{};
    std::size_t probe_tuple_count{};
};

struct TaskPlan {
    std::vector<ProbeTask> tasks;
    std::vector<std::size_t> candidate_partitions;
    std::vector<std::size_t> selected_partitions;
    std::vector<std::size_t> chunks_per_partition;
    std::size_t fine_chunk_count{};
    std::size_t budget{};
    std::size_t coarse_tasks{};
    double average_probe_size{};
    double max_to_average{};
    bool packing_enabled{};
};

struct JoinOptions {
    bool collect_pairs{false};
};

struct JoinMetrics {
    double partition_ms{};
    double planning_ms{};
    double build_ms{};
    double probe_ms{};
    double end_to_end_ms{};
    std::size_t candidate_partitions{};
    std::size_t selected_partitions{};
    std::size_t fine_chunks{};
    std::size_t total_tasks{};
    std::size_t budget{};
    std::size_t coarse_tasks{};
    std::size_t max_partition_probe_tuples{};
    std::size_t workers_with_probe_work{};
    std::size_t worker_task_total{};
    std::size_t max_worker_tasks{};
    std::size_t max_worker_probe_tuples{};
    double average_probe_partition_tuples{};
    double max_to_average{};
    double worker_task_max_to_average{};
    double worker_probe_max_to_average{};
    bool packing_enabled{};
};

struct JoinPair {
    std::uint32_t build_id{};
    std::uint32_t probe_id{};

    bool operator==(const JoinPair& other) const {
        return build_id == other.build_id && probe_id == other.probe_id;
    }
};

struct JoinResult {
    std::uint64_t cardinality{};
    std::vector<JoinPair> pairs;
    JoinMetrics metrics;
};

struct OracleResult {
    std::uint64_t cardinality{};
    std::vector<JoinPair> pairs;
};

std::string policy_name_string(Policy policy);
Policy parse_policy(const std::string& value);

} // namespace hj
