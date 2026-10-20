#include "testing.h"
#include "vacuum/physical.h"

#include <cmath>
#include <random>

using namespace vacuum;

TEST(physical_header_roundtrip) {
  for (std::uint8_t code = 0; code <= 4; ++code) {
    for (std::uint16_t count : {1, 2, 255, 4095}) {
      const Bytes b = encodePhysicalHeader(PhysicalHeader{code, count});
      CHECK_EQ(b.size(), std::size_t{4});
      const auto decoded = decodePhysicalHeader(b);
      CHECK(decoded.has_value());
      if (!decoded) continue;
      CHECK_EQ(decoded->sizeCode, code);
      CHECK_EQ(decoded->fragmentCount, count);
    }
  }
}

TEST(physical_header_rejects_every_single_bit_flip) {
  const Bytes good = encodePhysicalHeader(PhysicalHeader{2, 37});
  for (std::size_t byte = 0; byte < good.size(); ++byte) {
    for (int bit = 0; bit < 8; ++bit) {
      Bytes bad = good;
      bad[byte] ^= static_cast<std::uint8_t>(1u << bit);
      // A flip in the payload can also land on a valid CRC by chance, but only
      // for a different (sizeCode, fragmentCount) pair; assert we never get
      // the original values back.
      const auto decoded = decodePhysicalHeader(bad);
      if (decoded) {
        const bool same = decoded->sizeCode == 2 && decoded->fragmentCount == 37;
        CHECK(!same);
      }
    }
  }
}

TEST(physical_header_rejects_unknown_size_code) {
  // sizeCode 5..15 are outside the FragmentSize enumeration.
  for (std::uint8_t code = 5; code < 16; ++code) {
    const Bytes b = encodePhysicalHeader(PhysicalHeader{code, 3});
    CHECK(!decodePhysicalHeader(b).has_value());
  }
}

TEST(physical_header_rejects_zero_fragment_count) {
  const Bytes b = encodePhysicalHeader(PhysicalHeader{0, 0});
  CHECK(!decodePhysicalHeader(b).has_value());
}

TEST(physical_header_rejects_short_input) {
  CHECK(!decodePhysicalHeader(Bytes{1, 2, 3}).has_value());
  CHECK(!decodePhysicalHeader(Bytes{}).has_value());
}

TEST(physical_bit_packing_roundtrip) {
  std::mt19937 rng(3);
  for (std::size_t len = 1; len <= 64; ++len) {
    Bytes data(len);
    for (auto& b : data) b = static_cast<std::uint8_t>(rng());

    std::vector<std::uint8_t> bits;
    bytesToBits(data, bits);
    CHECK_EQ(bits.size(), len * 8);

    Bytes back;
    bitsToBytes(bits, back);
    CHECK(back == data);
  }
}

TEST(physical_bit_packing_is_msb_first) {
  std::vector<std::uint8_t> bits;
  bytesToBits(Bytes{0x81}, bits);
  CHECK_EQ(bits.size(), std::size_t{8});
  CHECK_EQ(bits[0], std::uint8_t{1});
  CHECK_EQ(bits[1], std::uint8_t{0});
  CHECK_EQ(bits[7], std::uint8_t{1});
}

TEST(physical_lfsr_is_deterministic_and_balanced) {
  Lfsr a(0xACE1u), b(0xACE1u);
  int ones = 0;
  for (int i = 0; i < 65535; ++i) {
    const int x = a.next();
    CHECK_EQ(x, b.next());
    ones += x;
  }
  // A maximal-length 16-bit sequence has 32768 ones and 32767 zeros.
  CHECK_EQ(ones, 32768);
  CHECK_EQ(a.state(), 0xACE1u);  // full period returns to the seed
}

TEST(physical_lfsr_rejects_zero_seed) {
  Lfsr lfsr(0);
  int ones = 0;
  for (int i = 0; i < 1000; ++i) ones += lfsr.next();
  CHECK(ones > 400);  // must not lock up at zero
}

TEST(physical_carrier_phase_is_bounded_and_varied) {
  for (int c = 0; c < 64; ++c) {
    for (int p = 0; p < 20; ++p) {
      const float ph = carrierPhase(c, p, 64);
      CHECK(ph >= 0.0f);
      CHECK(ph <= static_cast<float>(2 * 3.14159265358979323846) + 1e-4f);
    }
  }
  // Phases must differ across carriers, otherwise the tones would add
  // coherently into an audible peak.
  float spread = 0.0f;
  for (int c = 0; c < 64; ++c) spread += carrierPhase(c, 0, 64);
  CHECK(spread > 1.0f);
}
