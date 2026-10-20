#include "testing.h"
#include "vacuum/gf256.h"

#include <vector>

using namespace vacuum;

TEST(gf256_primitive_element_cycles) {
  // 2 is primitive in GF(256) with 0x11D, so alpha^255 == 1 and the 255 powers
  // are all distinct and non-zero.
  CHECK_EQ(gf::exp(0), 1);
  CHECK_EQ(gf::exp(255), 1);

  std::vector<int> seen(256, 0);
  for (int i = 0; i < 255; ++i) {
    const std::uint8_t v = gf::exp(i);
    CHECK(v != 0);
    CHECK(seen[v] == 0);
    seen[v] = 1;
    CHECK_EQ(gf::log(v), i);
  }
  CHECK(seen[0] == 0);
}

TEST(gf256_mul_div_inverse) {
  for (int a = 1; a < 256; ++a) {
    const std::uint8_t x = static_cast<std::uint8_t>(a);
    CHECK_EQ(gf::mul(x, gf::inverse(x)), 1);
    for (int b = 1; b < 256; ++b) {
      const std::uint8_t y = static_cast<std::uint8_t>(b);
      CHECK_EQ(gf::div(gf::mul(x, y), y), x);
    }
  }
  CHECK_EQ(gf::mul(0, 200), 0);
  CHECK_EQ(gf::mul(200, 0), 0);
}

TEST(gf256_addition_is_xor) {
  CHECK_EQ(gf::add(0xA5, 0x5A), 0xFF);
  CHECK_EQ(gf::add(0x33, 0x33), 0);
  CHECK_EQ(gf::sub(0xA5, 0xA5), 0);
}

TEST(gf256_pow_matches_repeated_mul) {
  for (int a = 1; a < 256; a += 7) {
    std::uint8_t acc = 1;
    for (int n = 0; n < 20; ++n) {
      CHECK_EQ(gf::pow(static_cast<std::uint8_t>(a), n), acc);
      acc = gf::mul(acc, static_cast<std::uint8_t>(a));
    }
  }
}
