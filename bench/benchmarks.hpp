#pragma once

#include <nanobench.h>

namespace vx::bench {

// Each adds its measurements to `bench`, which prints a row for each.
void job_system(ankerl::nanobench::Bench& bench);
void packed_array(ankerl::nanobench::Bench& bench);
void chunk_storage(ankerl::nanobench::Bench& bench);
void chunk_map(ankerl::nanobench::Bench& bench);
void block_registry(ankerl::nanobench::Bench& bench);
void block_textures(ankerl::nanobench::Bench& bench);
void block_pack(ankerl::nanobench::Bench& bench);
void padded_chunk(ankerl::nanobench::Bench& bench);
void mesh(ankerl::nanobench::Bench& bench);
void movement(ankerl::nanobench::Bench& bench);

} // namespace vx::bench
