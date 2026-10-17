#pragma once

#include "hash_join/types.hpp"

namespace hj {

Relations generate_relations(const Config& config);
OracleResult sequential_oracle(const Relations& relations, bool collect_pairs = false);

} // namespace hj
