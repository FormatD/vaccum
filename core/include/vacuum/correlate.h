#pragma once

#include <cstddef>
#include <utility>
#include <vector>

#include "vacuum/types.h"

namespace vacuum {

/// FFT-based search for the integer lag that maximises
/// |sum_n rx[lag + n] * ref[n]|.
///
/// Non-coherent (magnitude only) because the air channel applies an unknown
/// phase rotation. Runs in blocks so memory stays bounded for long recordings.
///
/// @return the `k` best lags, best first. Lags closer than `minSeparation` to
///         an already-accepted stronger lag are suppressed, so the result is a
///         set of genuinely distinct candidate sync points.
std::vector<std::pair<double, std::ptrdiff_t>> correlateTopOffsets(
    const Audio& rx, const Audio& ref, std::size_t k, std::ptrdiff_t minSeparation);

}  // namespace vacuum
