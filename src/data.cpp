#include "hash_join/data.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <stdexcept>
#include <unordered_map>

namespace hj {

Relations generate_relations(const Config& config) {
    if (config.key_range == 0) {
        if (config.build_size != 0 || config.probe_size != 0) {
            throw std::invalid_argument("key range must be positive for non-empty relations");
        }
        return {};
    }
    if (!std::isfinite(config.zipf_skew) || config.zipf_skew < 0.0) {
        throw std::invalid_argument("Zipf skew must be finite and non-negative");
    }
    if (config.build_size > std::numeric_limits<std::uint32_t>::max() ||
        config.probe_size > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("relation sizes exceed the supported row-id range");
    }

    std::mt19937_64 rng(config.seed);
    Relations result;
    result.build.reserve(config.build_size);
    result.probe.reserve(config.probe_size);

    std::vector<std::uint32_t> keys(config.key_range);
    for (std::size_t key = 0; key < config.key_range; ++key) {
        keys[key] = static_cast<std::uint32_t>(key);
    }
    std::shuffle(keys.begin(), keys.end(), rng);

    for (std::size_t i = 0; i < config.build_size; ++i) {
        const auto key = keys[i % keys.size()];
        result.build.push_back({key, static_cast<std::uint32_t>(i)});
    }

    const std::size_t referenced_keys = std::min<std::size_t>(config.key_range, config.build_size);
    if (config.probe_size == 0) {
        return result;
    }
    if (referenced_keys == 0) {
        throw std::invalid_argument("non-empty probe relation needs at least one build key");
    }

    std::uniform_int_distribution<std::size_t> uniform_key(0, referenced_keys - 1);
    std::uniform_real_distribution<double> unit_interval(0.0, 1.0);
    std::vector<double> cumulative;
    if (config.zipf_skew > 0.0) {
        cumulative.resize(referenced_keys);
        long double total = 0.0L;
        for (std::size_t rank = 1; rank <= referenced_keys; ++rank) {
            total += 1.0L / std::pow(static_cast<long double>(rank), config.zipf_skew);
        }
        long double running = 0.0L;
        for (std::size_t rank = 1; rank <= referenced_keys; ++rank) {
            running += 1.0L / std::pow(static_cast<long double>(rank), config.zipf_skew);
            cumulative[rank - 1] = static_cast<double>(running / total);
        }
        cumulative.back() = 1.0;
    }

    for (std::size_t i = 0; i < config.probe_size; ++i) {
        const std::size_t rank = config.zipf_skew == 0.0
            ? uniform_key(rng)
            : static_cast<std::size_t>(
                  std::lower_bound(cumulative.begin(), cumulative.end(), unit_interval(rng)) -
                  cumulative.begin());
        result.probe.push_back({keys[rank], static_cast<std::uint32_t>(i)});
    }
    return result;
}

OracleResult sequential_oracle(const Relations& relations, bool collect_pairs) {
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> build_ids;
    build_ids.reserve(relations.build.size());
    for (const Row& row : relations.build) {
        build_ids[row.key].push_back(row.payload);
    }

    OracleResult result;
    for (const Row& probe : relations.probe) {
        const auto found = build_ids.find(probe.key);
        if (found == build_ids.end()) {
            continue;
        }
        result.cardinality += static_cast<std::uint64_t>(found->second.size());
        if (collect_pairs) {
            for (const std::uint32_t build_id : found->second) {
                result.pairs.push_back({build_id, probe.payload});
            }
        }
    }
    return result;
}

} // namespace hj
