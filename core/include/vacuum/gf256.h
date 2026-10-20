#pragma once

#include <cstdint>

namespace vacuum {

/// GF(256) arithmetic with primitive polynomial 0x11D
/// (x^8 + x^4 + x^3 + x^2 + 1) and generator element 2. This is the field used
/// by CCSDS / QR-Code Reed-Solomon, so the tables match published vectors.
namespace gf {

inline constexpr int kFieldSize = 256;
inline constexpr std::uint16_t kPrimPoly = 0x11D;
inline constexpr std::uint8_t kGenerator = 2;

std::uint8_t exp(int power) noexcept;  ///< 2^power, power may be any int
int log(std::uint8_t x) noexcept;      ///< undefined for x == 0

std::uint8_t mul(std::uint8_t a, std::uint8_t b) noexcept;
std::uint8_t div(std::uint8_t a, std::uint8_t b) noexcept;
std::uint8_t pow(std::uint8_t a, int n) noexcept;
std::uint8_t inverse(std::uint8_t a) noexcept;

/// a + b in GF(256) is just xor.
inline constexpr std::uint8_t add(std::uint8_t a, std::uint8_t b) noexcept {
  return static_cast<std::uint8_t>(a ^ b);
}
inline constexpr std::uint8_t sub(std::uint8_t a, std::uint8_t b) noexcept {
  return static_cast<std::uint8_t>(a ^ b);
}

}  // namespace gf
}  // namespace vacuum
