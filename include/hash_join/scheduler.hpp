#pragma once

#include "hash_join/types.hpp"

namespace hj {

TaskPlan make_task_plan(const PartitionedRelation& probe, Policy policy, int threads,
                        std::size_t chunk_size, double split_factor,
                        std::size_t chunks_per_thread, std::size_t coarse_batch_size = 1,
                        double packing_imbalance_threshold = 16.0);

} // namespace hj
