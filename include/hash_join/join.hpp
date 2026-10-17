#pragma once

#include "hash_join/types.hpp"

namespace hj {

JoinResult run_join(const Relations& relations, const Config& config, Policy policy,
                    JoinOptions options = {});

} // namespace hj
