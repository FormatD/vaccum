#pragma once

#include <cstddef>
#include <cstdint>

namespace vacuum {

/// CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320).
/// Used as the integrity check on every frame: the Reed-Solomon layer fixes
/// most corruption, the CRC catches whatever is left.
class Crc32 {
 public:
  Crc32() = default;

  void update(const void* data, std::size_t len) noexcept;
  void update(std::uint8_t byte) noexcept;

  /// Value after the final xor. Call `reset` to reuse the object.
  std::uint32_t value() const noexcept { return state_ ^ 0xFFFFFFFFu; }

  void reset() noexcept { state_ = 0xFFFFFFFFu; }

  static std::uint32_t compute(const void* data, std::size_t len) noexcept;

 private:
  std::uint32_t state_ = 0xFFFFFFFFu;
};

/// CRC-16/CCITT-FALSE (polynomial 0x1021, init 0xFFFF). Used on the short
/// physical-layer header, where a 32-bit CRC would be a large share of it.
std::uint16_t crc16(const void* data, std::size_t len) noexcept;

}  // namespace vacuum
