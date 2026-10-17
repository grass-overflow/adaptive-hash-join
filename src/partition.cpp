#include "hash_join/partition.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace hj {
namespace {

PartitionedRelation partition_one(const Relation& relation, int radix_bits, int first_pass_bits) {
    if (radix_bits < 1 || radix_bits > 16) {
        throw std::invalid_argument("radix bits must be in [1, 16]");
    }
    if (first_pass_bits < 1 || first_pass_bits > radix_bits) {
        throw std::invalid_argument("first-pass bits must be in [1, radix bits]");
    }
    const int second_pass_bits = radix_bits - first_pass_bits;
    const std::size_t first_count = std::size_t{1} << first_pass_bits;
    const std::size_t second_count = std::size_t{1} << second_pass_bits;
    const std::size_t partition_count = std::size_t{1} << radix_bits;
    const std::uint32_t first_mask = static_cast<std::uint32_t>(first_count - 1);
    const std::uint32_t second_mask = static_cast<std::uint32_t>(second_count - 1);

    std::vector<std::size_t> first_histogram(first_count, 0);
    for (const Row& row : relation) {
        const std::size_t bucket = radix_hash(row.key) & first_mask;
        ++first_histogram[bucket];
    }
    std::vector<std::size_t> first_offsets(first_count + 1, 0);
    for (std::size_t i = 0; i < first_count; ++i) {
        first_offsets[i + 1] = first_offsets[i] + first_histogram[i];
    }
    Relation first_pass(relation.size());
    std::vector<std::size_t> first_cursor(first_offsets.begin(), first_offsets.end() - 1);
    for (const Row& row : relation) {
        const std::size_t bucket = radix_hash(row.key) & first_mask;
        first_pass[first_cursor[bucket]++] = row;
    }

    PartitionedRelation output;
    output.tuples.resize(relation.size());
    output.offsets.assign(partition_count + 1, 0);
    if (second_pass_bits == 0) {
        output.tuples = std::move(first_pass);
        output.offsets = std::move(first_offsets);
        return output;
    }

    for (std::size_t first = 0; first < first_count; ++first) {
        for (std::size_t index = first_offsets[first]; index < first_offsets[first + 1]; ++index) {
            const std::size_t second =
                (radix_hash(first_pass[index].key) >> first_pass_bits) & second_mask;
            ++output.offsets[first * second_count + second + 1];
        }
    }
    for (std::size_t i = 0; i < partition_count; ++i) {
        output.offsets[i + 1] += output.offsets[i];
    }

    std::vector<std::size_t> cursor(output.offsets.begin(), output.offsets.end() - 1);
    for (std::size_t first = 0; first < first_count; ++first) {
        for (std::size_t index = first_offsets[first]; index < first_offsets[first + 1]; ++index) {
            const std::size_t second =
                (radix_hash(first_pass[index].key) >> first_pass_bits) & second_mask;
            const std::size_t partition = first * second_count + second;
            output.tuples[cursor[partition]++] = first_pass[index];
        }
    }
    return output;
}

} // namespace

std::uint32_t radix_hash(std::uint32_t value) {
    value ^= value >> 16U;
    value *= 0x7feb352dU;
    value ^= value >> 15U;
    value *= 0x846ca68bU;
    value ^= value >> 16U;
    return value;
}

PartitionedRelation radix_partition(const Relation& relation, int radix_bits, int first_pass_bits) {
    return partition_one(relation, radix_bits, first_pass_bits);
}

PartitionedInput radix_partition(const Relations& relations, int radix_bits, int first_pass_bits) {
    PartitionedInput result;
    result.build = partition_one(relations.build, radix_bits, first_pass_bits);
    result.probe = partition_one(relations.probe, radix_bits, first_pass_bits);
    return result;
}

} // namespace hj
