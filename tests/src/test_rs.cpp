#include "testing.h"
#include "vacuum/rs.h"

#include <algorithm>
#include <numeric>
#include <random>
#include <stdexcept>

using namespace vacuum;

TEST(rs_encode_is_systematic) {
  const ReedSolomon rs(16);
  Bytes data(50);
  for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<std::uint8_t>(i * 13 + 7);

  const Bytes cw = rs.encode(data);
  CHECK_EQ(cw.size(), data.size() + 16);
  CHECK(std::equal(data.begin(), data.end(), cw.begin()));

  const auto r = rs.decode(cw);
  CHECK(r.ok);
  CHECK_EQ(r.correctedSymbols, 0);
  CHECK(r.data == data);
}

TEST(rs_corrects_up_to_capacity) {
  std::mt19937 rng(12345);
  const ReedSolomon rs(32);  // corrects 16
  constexpr int kCapacity = 16;

  for (int trial = 0; trial < 300; ++trial) {
    Bytes data(100);
    for (auto& b : data) b = static_cast<std::uint8_t>(rng());
    Bytes cw = rs.encode(data);

    const int errors = static_cast<int>(rng() % (kCapacity + 1));
    for (int e = 0; e < errors; ++e) {
      cw[rng() % cw.size()] ^= static_cast<std::uint8_t>(1 + rng() % 255);
    }

    const auto r = rs.decode(cw);
    CHECK(r.ok);
    if (r.ok) {
      CHECK(r.data == data);
      CHECK(r.correctedSymbols <= kCapacity);
    }
  }
}

TEST(rs_erasures_double_the_capacity) {
  // An erasure costs one parity symbol where an unlocated error costs two, so a
  // block with `nsym` parity absorbs any mix satisfying k + 2e <= nsym. The
  // demodulator hands the decoder the positions it is least sure about, which is
  // what makes that budget reachable in practice.
  std::mt19937 rng(2024);
  const int parity = 32;
  const ReedSolomon rs(parity);

  Bytes data(82);
  for (auto& b : data) b = static_cast<std::uint8_t>(rng());
  const Bytes coded = rs.encode(data);

  int cases = 0;
  for (int k = 0; k <= parity; k += 2) {
    for (int e = 0; k + 2 * e <= parity; ++e) {
      std::vector<int> positions(coded.size());
      std::iota(positions.begin(), positions.end(), 0);
      std::shuffle(positions.begin(), positions.end(), rng);

      Bytes rx = coded;
      std::vector<int> erasures;
      for (int i = 0; i < k; ++i) {
        erasures.push_back(positions[i]);
        rx[positions[i]] ^= static_cast<std::uint8_t>(1 + rng() % 255);
      }
      for (int i = k; i < k + e; ++i) {
        rx[positions[i]] ^= static_cast<std::uint8_t>(1 + rng() % 255);
      }

      const auto r = rs.decode(rx, erasures);
      CHECK(r.ok);
      if (!r.ok) {
        std::printf("    k=%d e=%d failed to decode\n", k, e);
        return;
      }
      CHECK(r.data == data);
      CHECK_EQ(r.correctedSymbols, k + e);
      ++cases;
    }
  }
  std::printf("    %d erasure/error combinations within k + 2e <= %d\n", cases, parity);
}

TEST(rs_rejects_more_erasures_than_parity) {
  const ReedSolomon rs(8);
  Bytes data(20, 0x31);
  Bytes cw = rs.encode(data);

  std::vector<int> erasures;
  for (int i = 0; i < 9; ++i) {
    erasures.push_back(i);
    cw[i] ^= 0x7E;
  }
  CHECK(!rs.decode(cw, erasures).ok);
}

TEST(rs_never_returns_wrong_data_as_success) {
  std::mt19937 rng(999);
  const ReedSolomon rs(16);  // corrects 8

  // 40 errors is far past capacity. The decoder must fail, or land on a
  // different valid codeword. It must never report success with the original
  // payload replaced by something else that the CRC layer would then miss.
  int wrongDataClaimedOk = 0;
  int failed = 0;
  for (int trial = 0; trial < 300; ++trial) {
    Bytes data(60);
    for (auto& b : data) b = static_cast<std::uint8_t>(rng());
    Bytes cw = rs.encode(data);
    for (int e = 0; e < 40; ++e) cw[rng() % cw.size()] = static_cast<std::uint8_t>(rng());

    const auto r = rs.decode(cw);
    if (!r.ok) {
      ++failed;
    } else if (r.data != data) {
      ++wrongDataClaimedOk;
    }
  }
  CHECK(failed > 250);
  CHECK_EQ(wrongDataClaimedOk, 0);
}

TEST(rs_shortened_lengths) {
  const ReedSolomon rs(8);
  for (std::size_t len = 1; len <= 40; ++len) {
    Bytes data(len);
    for (std::size_t i = 0; i < len; ++i) data[i] = static_cast<std::uint8_t>(i + len);
    Bytes cw = rs.encode(data);
    cw[len / 2] ^= 0x5A;
    cw.back() ^= 0xA5;
    const auto r = rs.decode(cw);
    CHECK(r.ok);
    if (r.ok) CHECK(r.data == data);
  }
}

TEST(rs_rejects_oversized_block) {
  const ReedSolomon rs(32);
  bool threw = false;
  try {
    rs.encode(Bytes(224, 1));
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
  CHECK_EQ(rs.maxDataBytes(), 223);
}

TEST(rs_rejects_bad_parity_count) {
  bool threw = false;
  try {
    const ReedSolomon rs(7);
    (void)rs;
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
}
