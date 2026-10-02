#pragma once

#include <nanobench.h>

namespace vx::bench {

// Each adds its measurements to `bench`, which prints a row for each.
void packed_array(ankerl::nanobench::Bench& bench);

} // namespace vx::bench
