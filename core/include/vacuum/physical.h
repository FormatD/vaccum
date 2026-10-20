#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "vacuum/types.h"

namespace vacuum {

/// Physical-layer header. Tells the receiver how to slice the demodulated bit
/// stream into Reed-Solomon blocks; without it the receiver cannot know the
/// message length.
struct PhysicalHeader {
  std::uint8_t sizeCode = 0;  ///< FragmentSize code
  std::uint16_t fragmentCount = 0;
};

/// 4 bytes: [4-bit sizeCode | 12-bit fragmentCount] then CRC-16, big-endian
/// bit order within the packed field.
Bytes encodePhysicalHeader(const PhysicalHeader& header);
std::optional<PhysicalHeader> decodePhysicalHeader(const Bytes& fourBytes);

void bytesToBits(const Bytes& in, std::vector<std::uint8_t>& bits);
/// Trailing bits fewer than 8 are discarded.
void bitsToBytes(const std::vector<std::uint8_t>& bits, Bytes& out);

/// 16-bit Galois LFSR (taps x^16+x^14+x^13+x^11+1). Used for the acquisition
/// preamble, where a long pseudo-random run gives a sharp correlation peak.
class Lfsr {
 public:
  explicit Lfsr(std::uint32_t seed = 0xACE1u) : state_(seed & 0xFFFFu) {
    if (state_ == 0) state_ = 1;
  }
  int next() noexcept;
  std::vector<std::uint8_t> bits(std::size_t count);
  std::uint32_t state() const noexcept { return state_; }

 private:
  std::uint32_t state_;
};

/// Deterministic per-carrier phase, so transmitter and receiver agree without
/// transmitting it and so the tones do not add coherently into audible peaks.
float carrierPhase(int carrierIndex, int symbolIndex, int carrierCount) noexcept;

}  // namespace vacuum
