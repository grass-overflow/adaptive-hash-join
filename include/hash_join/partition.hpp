#pragma once

#include "hash_join/types.hpp"

namespace hj {

PartitionedRelation radix_partition(const Relation& relation, int radix_bits, int first_pass_bits);
PartitionedInput radix_partition(const Relations& relations, int radix_bits, int first_pass_bits);
std::uint32_t radix_hash(std::uint32_t key);

} // namespace hj
