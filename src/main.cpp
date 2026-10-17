#include "hash_join/data.hpp"
#include "hash_join/join.hpp"
#include "hash_join/types.hpp"

#include <cmath>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <omp.h>

namespace {

struct Arguments {
    hj::Config config;
    std::vector<hj::Policy> policies{hj::Policy::sequential, hj::Policy::static_coarse,
                                     hj::Policy::dynamic_coarse, hj::Policy::paper_fine,
                                     hj::Policy::novelty_budgeted};
    std::size_t repetitions{3};
    std::size_t warmups{1};
    std::string csv_path;
    bool diagnostics{false};
};

std::string take_value(int& index, int argc, char** argv) {
    if (index + 1 >= argc) {
        throw std::invalid_argument(std::string("missing value for ") + argv[index]);
    }
    return argv[++index];
}

std::size_t as_size(const std::string& value, const std::string& option) {
    if (value.empty() || value.front() == '-') {
        throw std::invalid_argument("invalid non-negative integer for " + option + ": " + value);
    }
    std::size_t consumed = 0;
    const unsigned long long parsed = std::stoull(value, &consumed);
    if (consumed != value.size() || parsed > std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument("invalid non-negative integer for " + option + ": " + value);
    }
    return static_cast<std::size_t>(parsed);
}

int as_int(const std::string& value, const std::string& option) {
    std::size_t consumed = 0;
    const long parsed = std::stol(value, &consumed);
    if (consumed != value.size() || parsed < std::numeric_limits<int>::min() ||
        parsed > std::numeric_limits<int>::max()) {
        throw std::invalid_argument("invalid integer for " + option + ": " + value);
    }
    return static_cast<int>(parsed);
}

double as_double(const std::string& value, const std::string& option) {
    std::size_t consumed = 0;
    const double parsed = std::stod(value, &consumed);
    if (consumed != value.size() || !std::isfinite(parsed)) {
        throw std::invalid_argument("invalid number for " + option + ": " + value);
    }
    return parsed;
}

void usage(std::ostream& out) {
    out << "Usage: hashjoin [options]\n"
           "  --r-size N --s-size N --key-range N --skew Z --seed N\n"
           "  --threads N --radix-bits N --pass1-bits N --chunk-size N\n"
           "  --split-factor F --q N --coarse-batch N --pack-threshold F --dynamic-chunk N\n"
           "  --repetitions N --warmups N\n"
           "  --policy all|sequential|static|dynamic|paper_fine|novelty_budgeted|novelty_packed\n"
           "  --csv PATH --diagnostics\n";
}

Arguments parse_arguments(int argc, char** argv) {
    Arguments arguments;
    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--help" || option == "-h") {
            usage(std::cout);
            std::exit(0);
        } else if (option == "--r-size") {
            arguments.config.build_size = as_size(take_value(i, argc, argv), option);
        } else if (option == "--s-size") {
            arguments.config.probe_size = as_size(take_value(i, argc, argv), option);
        } else if (option == "--key-range") {
            const auto parsed = as_size(take_value(i, argc, argv), option);
            if (parsed > std::numeric_limits<std::uint32_t>::max()) {
                throw std::invalid_argument("key range exceeds 32-bit keys");
            }
            arguments.config.key_range = static_cast<std::uint32_t>(parsed);
        } else if (option == "--skew") {
            arguments.config.zipf_skew = as_double(take_value(i, argc, argv), option);
        } else if (option == "--seed") {
            arguments.config.seed = static_cast<std::uint64_t>(as_size(take_value(i, argc, argv), option));
        } else if (option == "--threads") {
            arguments.config.threads = as_int(take_value(i, argc, argv), option);
        } else if (option == "--radix-bits") {
            arguments.config.radix_bits = as_int(take_value(i, argc, argv), option);
        } else if (option == "--pass1-bits") {
            arguments.config.first_pass_bits = as_int(take_value(i, argc, argv), option);
        } else if (option == "--chunk-size") {
            arguments.config.chunk_size = as_size(take_value(i, argc, argv), option);
        } else if (option == "--split-factor") {
            arguments.config.split_factor = as_double(take_value(i, argc, argv), option);
        } else if (option == "--q") {
            arguments.config.chunks_per_thread = as_size(take_value(i, argc, argv), option);
        } else if (option == "--coarse-batch") {
            arguments.config.coarse_batch_size = as_size(take_value(i, argc, argv), option);
        } else if (option == "--pack-threshold") {
            arguments.config.packing_imbalance_threshold =
                as_double(take_value(i, argc, argv), option);
        } else if (option == "--dynamic-chunk") {
            arguments.config.dynamic_chunk = as_int(take_value(i, argc, argv), option);
        } else if (option == "--repetitions") {
            arguments.repetitions = as_size(take_value(i, argc, argv), option);
        } else if (option == "--warmups") {
            arguments.warmups = as_size(take_value(i, argc, argv), option);
        } else if (option == "--csv") {
            arguments.csv_path = take_value(i, argc, argv);
        } else if (option == "--diagnostics") {
            arguments.diagnostics = true;
        } else if (option == "--policy") {
            const std::string value = take_value(i, argc, argv);
            if (value == "all") {
                arguments.policies = {hj::Policy::sequential, hj::Policy::static_coarse,
                                      hj::Policy::dynamic_coarse, hj::Policy::paper_fine,
                                      hj::Policy::novelty_budgeted, hj::Policy::novelty_packed};
            } else {
                arguments.policies = {hj::parse_policy(value)};
            }
        } else {
            throw std::invalid_argument("unknown option: " + option);
        }
    }
    if (arguments.repetitions == 0) {
        throw std::invalid_argument("repetitions must be positive");
    }
    if (arguments.config.threads < 1 || arguments.config.dynamic_chunk < 1) {
        throw std::invalid_argument("thread count and dynamic chunk must be positive");
    }
    if (arguments.config.build_size > std::numeric_limits<std::uint32_t>::max() ||
        arguments.config.probe_size > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("relation sizes exceed supported row-id range");
    }
    return arguments;
}

constexpr const char* csv_header =
    "policy,seed,build_size,probe_size,key_range,skew,threads,radix_bits,pass1_bits,"
    "chunk_size,split_factor,q,coarse_batch,pack_threshold,policy_order_position,repetition,"
    "partition_ms,planning_ms,build_ms,probe_ms,end_to_end_ms,"
    "sequential_ms,speedup,parallel_efficiency,max_to_average,candidates,selected,"
    "lambda_partition,avg_probe_partition_tuples,max_probe_partition_tuples,"
    "fine_chunks,chunk_budget,coarse_tasks,total_tasks,worker_task_total,"
    "max_worker_tasks,worker_task_max_to_average,workers_with_probe_work,"
    "max_worker_probe_tuples,worker_probe_max_to_average,steal_count,"
    "steal_count_status,join_cardinality,correct";

void write_row(std::ostream& out, const Arguments& args, hj::Policy policy, std::size_t repetition,
               std::size_t order_position, const hj::JoinResult& result,
               double sequential_ms, bool correct) {
    const double speedup = result.metrics.end_to_end_ms > 0.0
        ? sequential_ms / result.metrics.end_to_end_ms : 0.0;
    const double efficiency = args.config.threads > 0
        ? speedup / static_cast<double>(args.config.threads) : 0.0;
    out << hj::policy_name(policy) << ',' << args.config.seed << ','
        << args.config.build_size << ',' << args.config.probe_size << ','
        << args.config.key_range << ',' << args.config.zipf_skew << ','
        << args.config.threads << ',' << args.config.radix_bits << ','
        << args.config.first_pass_bits << ',' << args.config.chunk_size << ','
        << args.config.split_factor << ',' << args.config.chunks_per_thread << ','
        << args.config.coarse_batch_size << ',' << args.config.packing_imbalance_threshold
        << ',' << order_position << ',' << repetition << ','
        << result.metrics.partition_ms << ',' << result.metrics.planning_ms << ','
        << result.metrics.build_ms << ','
        << result.metrics.probe_ms << ',' << result.metrics.end_to_end_ms << ','
        << sequential_ms << ',' << speedup << ',' << efficiency << ','
        << result.metrics.max_to_average << ',' << result.metrics.candidate_partitions << ','
        << result.metrics.selected_partitions << ','
        << result.metrics.max_to_average << ','
        << result.metrics.average_probe_partition_tuples << ','
        << result.metrics.max_partition_probe_tuples << ','
        << result.metrics.fine_chunks << ',' << result.metrics.budget << ','
        << result.metrics.coarse_tasks << ','
        << result.metrics.total_tasks << ',' << result.metrics.worker_task_total << ','
        << result.metrics.max_worker_tasks << ','
        << result.metrics.worker_task_max_to_average << ','
        << result.metrics.workers_with_probe_work << ','
        << result.metrics.max_worker_probe_tuples << ','
        << result.metrics.worker_probe_max_to_average
        << ",,unavailable_openmp_runtime,"
        << result.cardinality << ',' << (correct ? "true" : "false") << '\n';
}

void diagnostics(std::ostream& out, const Arguments& args, hj::Policy policy,
                 std::size_t repetition, const hj::JoinResult& result, bool correct) {
    out << "policy=" << hj::policy_name(policy) << " rep=" << repetition
        << " imbalance(max/avg)=" << result.metrics.max_to_average
        << " lambda_partition=" << result.metrics.max_to_average
        << " planning_ms=" << result.metrics.planning_ms
        << " worker_probe_lambda=" << result.metrics.worker_probe_max_to_average
        << " max_worker_tasks=" << result.metrics.max_worker_tasks
        << " max_worker_probe_tuples=" << result.metrics.max_worker_probe_tuples
        << " steal_count=unavailable_openmp_runtime"
        << " workers_with_probe_work=" << result.metrics.workers_with_probe_work
        << " candidates=" << result.metrics.candidate_partitions
        << " selected=" << result.metrics.selected_partitions
        << " chunk_budget=" << result.metrics.budget
        << " packing_enabled=" << (result.metrics.packing_enabled ? "true" : "false")
        << " fine_chunks=" << result.metrics.fine_chunks
        << " coarse_tasks=" << result.metrics.coarse_tasks
        << " tasks=" << result.metrics.total_tasks
        << " cardinality=" << result.cardinality
        << " correct=" << (correct ? "true" : "false")
        << " runtime_ms=" << result.metrics.end_to_end_ms << '\n';
    (void)args;
}

} // namespace

int main(int argc, char** argv) {
    try {
        Arguments arguments = parse_arguments(argc, argv);
        omp_set_dynamic(0);
        const hj::Relations relations = hj::generate_relations(arguments.config);
        const hj::OracleResult oracle = hj::sequential_oracle(relations);

        std::ofstream csv_file;
        std::ostream* output = &std::cout;
        if (!arguments.csv_path.empty()) {
            csv_file.open(arguments.csv_path, std::ios::out | std::ios::trunc);
            if (!csv_file) {
                throw std::runtime_error("cannot open CSV output: " + arguments.csv_path);
            }
            output = &csv_file;
        }
        *output << csv_header << '\n' << std::setprecision(10);

        for (std::size_t warmup = 0; warmup < arguments.warmups; ++warmup) {
            for (const auto policy : arguments.policies) {
                const auto result = hj::run_join(relations, arguments.config, policy);
                if (result.cardinality != oracle.cardinality) {
                    throw std::runtime_error(std::string("warm-up correctness failure for ") +
                                             hj::policy_name(policy));
                }
            }
        }

        for (std::size_t repetition = 0; repetition < arguments.repetitions; ++repetition) {
            double sequential_ms = 0.0;
            bool has_sequential_policy = false;
            for (const auto policy : arguments.policies) {
                has_sequential_policy = has_sequential_policy || policy == hj::Policy::sequential;
            }
            if (!has_sequential_policy) {
                const auto baseline = hj::run_join(relations, arguments.config, hj::Policy::sequential);
                sequential_ms = baseline.metrics.end_to_end_ms;
                if (baseline.cardinality != oracle.cardinality) {
                    throw std::runtime_error("sequential baseline correctness failure");
                }
            }
            std::vector<hj::Policy> execution_order = arguments.policies;
            if (!execution_order.empty()) {
                const std::size_t offset = static_cast<std::size_t>(
                    (arguments.config.seed + repetition) % execution_order.size());
                std::rotate(execution_order.begin(), execution_order.begin() +
                            static_cast<std::ptrdiff_t>(offset), execution_order.end());
            }
            std::vector<std::pair<hj::Policy, hj::JoinResult>> run_results;
            run_results.reserve(execution_order.size());
            for (const auto policy : execution_order) {
                const auto result = hj::run_join(relations, arguments.config, policy);
                const bool correct = result.cardinality == oracle.cardinality;
                if (!correct) {
                    throw std::runtime_error(std::string("correctness failure for ") +
                                             hj::policy_name(policy));
                }
                if (policy == hj::Policy::sequential) {
                    sequential_ms = result.metrics.end_to_end_ms;
                }
                run_results.emplace_back(policy, result);
            }
            for (std::size_t position = 0; position < run_results.size(); ++position) {
                const auto& [policy, result] = run_results[position];
                const bool correct = result.cardinality == oracle.cardinality;
                write_row(*output, arguments, policy, repetition, position,
                          result, sequential_ms, correct);
                if (arguments.diagnostics) {
                    diagnostics(std::cerr, arguments, policy, repetition, result, correct);
                }
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        usage(std::cerr);
        return 2;
    }
}
