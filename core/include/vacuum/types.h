#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vacuum {

/// Mono audio sample, normalised to [-1, 1].
using Sample = float;
using Audio = std::vector<Sample>;
using Bytes = std::vector<std::uint8_t>;

struct AudioFormat {
  int sampleRate = 48000;
  int channels = 1;
};

}  // namespace vacuum
