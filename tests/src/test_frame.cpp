#include "testing.h"
#include "vacuum/crc.h"
#include "vacuum/frame.h"
#include "vacuum/rs.h"

#include <random>

using namespace vacuum;

TEST(frame_plan_prefers_cheapest_total_bits) {
  // A 40-byte payload fits one 64-byte fragment (114 coded bytes) or five
  // 8-byte fragments (290 coded bytes); the single large fragment must win.
  const auto plan = planFrames(40, 32);
  CHECK(plan.has_value());
  if (!plan) return;
  CHECK(plan->size == FragmentSize::Bytes64);
  CHECK_EQ(plan->fragmentCount, std::size_t{1});
  CHECK_EQ(plan->blockBytes, kFrameHeaderSize + 64 + kFrameCrcSize + 32);
}

TEST(frame_plan_handles_multi_fragment_payloads) {
  const auto plan = planFrames(500, 32);
  CHECK(plan.has_value());
  if (!plan) return;
  CHECK_EQ(plan->fragmentCount * fragmentPayloadSize(plan->size) >= 500, true);
  CHECK(plan->blockBytes <= std::size_t{ReedSolomon::kMaxBlock});

  // The planner's contract is minimum total transmitted bits, not a particular
  // fragment size, so verify it against every admissible size.
  std::size_t best = std::size_t(-1);
  for (std::uint8_t code = 0; code <= 4; ++code) {
    const std::size_t frag = fragmentPayloadSize(static_cast<FragmentSize>(code));
    const std::size_t count = (500 + frag - 1) / frag;
    if (count > 4095) continue;
    const std::size_t block = kFrameHeaderSize + frag + kFrameCrcSize + 32;
    if (block > std::size_t{ReedSolomon::kMaxBlock}) continue;
    best = std::min(best, count * block);
  }
  CHECK_EQ(plan->fragmentCount * plan->blockBytes, best);
}

TEST(frame_plan_rejects_empty_and_oversized) {
  CHECK(!planFrames(0, 32).has_value());
  // 4095 fragments is the ceiling the 12-bit physical header field allows.
  CHECK(planFrames(4095 * 196, 32).has_value());
  CHECK(!planFrames(4095 * 196 + 1, 32).has_value());
}

TEST(frame_packetize_reassemble_roundtrip) {
  std::mt19937 rng(2024);
  const int parity = 32;
  const ReedSolomon rs(parity);

  for (std::size_t len : {1, 7, 8, 9, 24, 100, 250, 500, 999}) {
    Bytes payload(len);
    for (auto& b : payload) b = static_cast<std::uint8_t>(rng());

    const auto plan = planFrames(len, parity);
    CHECK(plan.has_value());
    if (!plan) continue;

    const auto blocks = packetize(payload, 0x1234, *plan, parity);
    CHECK_EQ(blocks.size(), plan->fragmentCount);

    Reassembler r;
    for (const Bytes& block : blocks) {
      const auto decoded = rs.decode(block);
      CHECK(decoded.ok);
      if (decoded.ok) r.add(decoded.data);
    }
    CHECK(r.complete());
    CHECK(r.payload() == payload);
  }
}

TEST(frame_reassembly_survives_symbol_errors) {
  std::mt19937 rng(55);
  const int parity = 32;
  const ReedSolomon rs(parity);
  const Bytes payload(300, 0);

  const auto plan = planFrames(payload.size(), parity);
  CHECK(plan.has_value());
  if (!plan) return;

  auto blocks = packetize(payload, 0x77, *plan, parity);
  // 16 wrong symbols per block is exactly the correction capacity.
  for (Bytes& block : blocks) {
    for (int e = 0; e < 16; ++e) block[rng() % block.size()] ^= static_cast<std::uint8_t>(rng() | 1);
  }

  Reassembler r;
  for (const Bytes& block : blocks) {
    const auto decoded = rs.decode(block);
    CHECK(decoded.ok);
    if (decoded.ok) r.add(decoded.data);
  }
  CHECK(r.complete());
  CHECK(r.payload() == payload);
}

TEST(frame_rejects_corrupt_frame_crc) {
  const int parity = 32;
  const Bytes payload(20, 0xAB);
  const auto plan = planFrames(payload.size(), parity);
  CHECK(plan.has_value());
  if (!plan) return;

  const auto blocks = packetize(payload, 1, *plan, parity);
  const ReedSolomon rs(parity);
  const auto decoded = rs.decode(blocks[0]);
  CHECK(decoded.ok);
  if (!decoded.ok) return;

  Bytes tampered = decoded.data;
  tampered[kFrameHeaderSize] ^= 0xFF;  // payload byte, frame CRC now wrong
  Reassembler r;
  CHECK(!r.add(tampered));
  CHECK(!r.complete());
}

TEST(frame_reports_missing_fragments) {
  const int parity = 32;
  Bytes payload(600);
  for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<std::uint8_t>(i);

  const auto plan = planFrames(payload.size(), parity);
  CHECK(plan.has_value());
  if (!plan) return;

  const auto blocks = packetize(payload, 9, *plan, parity);
  CHECK(blocks.size() >= 3);
  const ReedSolomon rs(parity);

  Reassembler r;
  for (std::size_t i = 0; i < blocks.size(); ++i) {
    if (i == 1) continue;  // drop one fragment
    const auto decoded = rs.decode(blocks[i]);
    if (decoded.ok) r.add(decoded.data);
  }
  CHECK(!r.complete());
  CHECK_EQ(r.fragmentsExpected(), blocks.size());
  const auto missing = r.missingFragments();
  CHECK_EQ(missing.size(), std::size_t{1});
  if (!missing.empty()) CHECK_EQ(missing[0], std::size_t{1});
}

TEST(frame_ignores_foreign_stream_id) {
  const int parity = 32;
  Bytes payload(400);
  for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<std::uint8_t>(i * 3);

  const auto plan = planFrames(payload.size(), parity);
  CHECK(plan.has_value());
  if (!plan) return;

  const auto mine = packetize(payload, 111, *plan, parity);
  const auto theirs = packetize(payload, 222, *plan, parity);
  CHECK(mine.size() >= 2);
  const ReedSolomon rs(parity);

  // One frame establishes the stream; everything from another stream id that
  // follows must be dropped rather than filling in the gaps.
  Reassembler r;
  const auto first = rs.decode(mine[0]);
  CHECK(first.ok);
  if (first.ok) r.add(first.data);

  for (const Bytes& block : theirs) {
    const auto d = rs.decode(block);
    if (d.ok) r.add(d.data);
  }
  CHECK(!r.complete());
  CHECK_EQ(r.fragmentsReceived(), std::size_t{1});

  // The right stream resumes and completes it.
  for (std::size_t i = 1; i < mine.size(); ++i) {
    const auto d = rs.decode(mine[i]);
    if (d.ok) r.add(d.data);
  }
  CHECK(r.complete());
  CHECK(r.payload() == payload);
}

TEST(frame_payload_crc_catches_wrong_reassembly) {
  const int parity = 32;
  Bytes payload(600, 0x5A);
  const auto plan = planFrames(payload.size(), parity);
  CHECK(plan.has_value());
  if (!plan) return;

  auto blocks = packetize(payload, 42, *plan, parity);
  const ReedSolomon rs(parity);

  // Flip a byte inside a fragment's payload and fix up the frame CRC, so the
  // frame passes its own check but the end-to-end payload CRC must catch it.
  Bytes frame = rs.decode(blocks[0]).data;
  frame[kFrameHeaderSize] ^= 0x01;
  frame.resize(frame.size() - kFrameCrcSize);
  const std::uint32_t crc = Crc32::compute(frame.data(), frame.size());
  for (int i = 0; i < 4; ++i) frame.push_back(static_cast<std::uint8_t>(crc >> (8 * i)));

  Reassembler r;
  r.add(frame);
  for (std::size_t i = 1; i < blocks.size(); ++i) {
    const auto d = rs.decode(blocks[i]);
    if (d.ok) r.add(d.data);
  }
  CHECK(!r.complete());
}
