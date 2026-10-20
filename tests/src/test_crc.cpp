#include "testing.h"
#include "vacuum/crc.h"

using namespace vacuum;

// Reference check values for the standard "123456789" test vector.
TEST(crc32_known_vector) {
  const char* s = "123456789";
  CHECK_EQ(Crc32::compute(s, 9), 0xCBF43926u);
}

TEST(crc32_incremental_matches_one_shot) {
  const std::uint8_t data[] = {0x00, 0xFF, 0x10, 0x7F, 0x80, 0xAB, 0xCD};
  Crc32 streaming;
  for (std::uint8_t b : data) streaming.update(b);
  CHECK_EQ(streaming.value(), Crc32::compute(data, sizeof(data)));
}

TEST(crc32_detects_single_bit_flip) {
  std::uint8_t data[64];
  for (int i = 0; i < 64; ++i) data[i] = static_cast<std::uint8_t>(i * 7);
  const std::uint32_t good = Crc32::compute(data, sizeof(data));
  for (int i = 0; i < 64; ++i) {
    data[i] ^= 0x01;
    CHECK(Crc32::compute(data, sizeof(data)) != good);
    data[i] ^= 0x01;
  }
}

TEST(crc16_known_vector) {
  // CRC-16/CCITT-FALSE of "123456789" is 0x29B1.
  const char* s = "123456789";
  CHECK_EQ(crc16(s, 9), 0x29B1u);
}
