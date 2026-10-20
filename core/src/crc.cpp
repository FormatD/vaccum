#include "vacuum/crc.h"

#include <array>

namespace vacuum {
namespace {

constexpr std::uint32_t kPolynomial = 0xEDB88320u;

const std::array<std::uint32_t, 256>& table() {
  static const std::array<std::uint32_t, 256> kTable = [] {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int bit = 0; bit < 8; ++bit) {
        c = (c & 1u) ? (kPolynomial ^ (c >> 1)) : (c >> 1);
      }
      t[i] = c;
    }
    return t;
  }();
  return kTable;
}

}  // namespace

void Crc32::update(std::uint8_t byte) noexcept {
  const auto& t = table();
  state_ = t[(state_ ^ byte) & 0xFFu] ^ (state_ >> 8);
}

void Crc32::update(const void* data, std::size_t len) noexcept {
  const auto* p = static_cast<const std::uint8_t*>(data);
  for (std::size_t i = 0; i < len; ++i) update(p[i]);
}

std::uint32_t Crc32::compute(const void* data, std::size_t len) noexcept {
  Crc32 crc;
  crc.update(data, len);
  return crc.value();
}

std::uint16_t crc16(const void* data, std::size_t len) noexcept {
  const auto* p = static_cast<const std::uint8_t*>(data);
  std::uint16_t crc = 0xFFFFu;
  for (std::size_t i = 0; i < len; ++i) {
    crc ^= static_cast<std::uint16_t>(p[i]) << 8;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x8000u) ? static_cast<std::uint16_t>((crc << 1) ^ 0x1021u)
                            : static_cast<std::uint16_t>(crc << 1);
    }
  }
  return crc;
}

}  // namespace vacuum
