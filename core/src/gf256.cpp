#include "vacuum/gf256.h"

#include <array>

namespace vacuum::gf {
namespace {

struct Tables {
  std::array<std::uint16_t, 512> exp{};  ///< doubled so no modulo is needed
  std::array<std::uint8_t, 256> log{};

  Tables() {
    std::uint16_t x = 1;
    for (int i = 0; i < 255; ++i) {
      exp[i] = x;
      log[x] = static_cast<std::uint8_t>(i);
      x <<= 1;
      if (x & 0x100) x ^= kPrimPoly;
    }
    for (int i = 255; i < 512; ++i) exp[i] = exp[i - 255];
  }
};

const Tables& tables() {
  static const Tables t;
  return t;
}

}  // namespace

std::uint8_t exp(int power) noexcept {
  const auto& t = tables();
  return static_cast<std::uint8_t>(t.exp[((power % 255) + 255) % 255]);
}

int log(std::uint8_t x) noexcept { return tables().log[x]; }

std::uint8_t mul(std::uint8_t a, std::uint8_t b) noexcept {
  if (a == 0 || b == 0) return 0;
  const auto& t = tables();
  return static_cast<std::uint8_t>(t.exp[t.log[a] + t.log[b]]);
}

std::uint8_t div(std::uint8_t a, std::uint8_t b) noexcept {
  if (b == 0) return 0;  // caller guarantees non-zero; keep this total
  if (a == 0) return 0;
  const auto& t = tables();
  int diff = static_cast<int>(t.log[a]) - static_cast<int>(t.log[b]);
  if (diff < 0) diff += 255;
  return static_cast<std::uint8_t>(t.exp[diff]);
}

std::uint8_t pow(std::uint8_t a, int n) noexcept {
  if (a == 0) return 0;
  const auto& t = tables();
  int p = (static_cast<int>(t.log[a]) * n) % 255;
  if (p < 0) p += 255;
  return static_cast<std::uint8_t>(t.exp[p]);
}

std::uint8_t inverse(std::uint8_t a) noexcept {
  const auto& t = tables();
  return static_cast<std::uint8_t>(t.exp[255 - t.log[a]]);
}

}  // namespace vacuum::gf
