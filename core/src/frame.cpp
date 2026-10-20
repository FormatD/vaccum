#include "vacuum/frame.h"

#include <algorithm>
#include <cstring>

#include "vacuum/crc.h"
#include "vacuum/rs.h"

namespace vacuum {
namespace {

constexpr std::size_t kFragmentSizes[] = {8, 24, 64, 128, 196};
constexpr std::size_t kMaxFragmentCount = 4095;

void putU16(Bytes& out, std::uint16_t v) {
  out.push_back(static_cast<std::uint8_t>(v & 0xFF));
  out.push_back(static_cast<std::uint8_t>(v >> 8));
}

void putU32(Bytes& out, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}

std::uint16_t readU16(const std::uint8_t* p) {
  return static_cast<std::uint16_t>(p[0]) | static_cast<std::uint16_t>(p[1] << 8);
}

std::uint32_t readU32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

}  // namespace

std::size_t fragmentPayloadSize(FragmentSize size) noexcept {
  const auto code = static_cast<std::uint8_t>(size);
  return code < std::size(kFragmentSizes) ? kFragmentSizes[code] : 0;
}

std::optional<FragmentSize> fragmentSizeFromCode(std::uint8_t code) noexcept {
  if (code >= std::size(kFragmentSizes)) return std::nullopt;
  return static_cast<FragmentSize>(code);
}

std::optional<FramePlan> planFrames(std::size_t payloadLen, int rsParitySymbols,
                                    FragmentSize maxSize) {
  if (payloadLen == 0) return std::nullopt;

  std::optional<FramePlan> best;
  std::size_t bestBits = std::size_t(-1);

  for (std::uint8_t code = 0; code <= static_cast<std::uint8_t>(maxSize); ++code) {
    const auto size = static_cast<FragmentSize>(code);
    const std::size_t frag = fragmentPayloadSize(size);
    const std::size_t count = (payloadLen + frag - 1) / frag;
    if (count == 0 || count > kMaxFragmentCount) continue;

    const std::size_t frameBytes = kFrameHeaderSize + frag + kFrameCrcSize;
    if (frameBytes + static_cast<std::size_t>(rsParitySymbols) > ReedSolomon::kMaxBlock) continue;

    const std::size_t blockBytes = frameBytes + static_cast<std::size_t>(rsParitySymbols);
    const std::size_t bits = count * blockBytes * 8;
    if (bits < bestBits) {
      bestBits = bits;
      best = FramePlan{size, count, blockBytes};
    }
  }
  return best;
}

std::vector<Bytes> packetize(const Bytes& payload,
                             std::uint16_t streamId,
                             const FramePlan& plan,
                             int rsParitySymbols) {
  const ReedSolomon rs(rsParitySymbols);
  const std::size_t fragSize = fragmentPayloadSize(plan.size);
  const std::uint32_t totalCrc = Crc32::compute(payload.data(), payload.size());

  std::vector<Bytes> blocks;
  blocks.reserve(plan.fragmentCount);

  for (std::size_t i = 0; i < plan.fragmentCount; ++i) {
    const std::size_t offset = i * fragSize;
    const std::size_t take = std::min(fragSize, payload.size() - offset);

    Bytes frame;
    frame.reserve(kFrameHeaderSize + fragSize + kFrameCrcSize);
    frame.push_back(kFrameVersion);
    frame.push_back(static_cast<std::uint8_t>(i));
    putU16(frame, static_cast<std::uint16_t>(plan.fragmentCount));
    putU16(frame, streamId);
    putU16(frame, static_cast<std::uint16_t>(payload.size()));
    putU16(frame, static_cast<std::uint16_t>(take));
    putU32(frame, totalCrc);
    frame.insert(frame.end(), payload.begin() + offset, payload.begin() + offset + take);
    frame.resize(kFrameHeaderSize + fragSize, 0);
    putU32(frame, Crc32::compute(frame.data(), frame.size()));

    blocks.push_back(rs.encode(frame));
  }
  return blocks;
}

void Reassembler::reset() noexcept {
  *this = Reassembler{};
}

bool Reassembler::add(const Bytes& b) {
  if (complete_) return false;
  if (b.size() < kFrameHeaderSize + kFrameCrcSize) return false;
  if (b[0] != kFrameVersion) return false;

  const std::size_t bodyLen = b.size() - kFrameCrcSize;
  if (readU32(b.data() + bodyLen) != Crc32::compute(b.data(), bodyLen)) return false;

  const std::uint16_t streamId = readU16(b.data() + 4);
  const std::size_t fragmentCount = readU16(b.data() + 2);
  const std::size_t totalLen = readU16(b.data() + 6);
  const std::size_t fragLen = readU16(b.data() + 8);
  const std::uint32_t totalCrc = readU32(b.data() + 10);
  const std::size_t index = b[1];

  if (fragmentCount == 0 || index >= fragmentCount) return false;
  if (started_ && streamId != streamId_) return false;

  if (!started_) {
    started_ = true;
    streamId_ = streamId;
    fragmentCount_ = fragmentCount;
    totalPayloadLen_ = totalLen;
    payloadCrc_ = totalCrc;
    fragments_.assign(fragmentCount, Bytes{});
    seen_.assign(fragmentCount, 0);
  }
  if (fragmentCount != fragmentCount_ || totalLen != totalPayloadLen_) return false;

  const std::size_t fragSize = bodyLen - kFrameHeaderSize;
  if (fragLen > fragSize) return false;
  const std::size_t start = index * fragSize;
  if (start > totalPayloadLen_ || start + fragLen > totalPayloadLen_) return false;
  if (b.size() < kFrameHeaderSize + fragLen) return false;

  if (!seen_[index]) {
    fragments_[index].assign(b.begin() + kFrameHeaderSize,
                             b.begin() + kFrameHeaderSize + fragLen);
    seen_[index] = 1;
    ++received_;
  }

  if (received_ == fragmentCount_) {
    Bytes out;
    out.reserve(totalPayloadLen_);
    for (const Bytes& f : fragments_) out.insert(out.end(), f.begin(), f.end());
    out.resize(totalPayloadLen_);
    if (Crc32::compute(out.data(), out.size()) != payloadCrc_) return false;
    payload_ = std::move(out);
    complete_ = true;
    return true;
  }
  return false;
}

std::vector<std::size_t> Reassembler::missingFragments() const {
  std::vector<std::size_t> out;
  for (std::size_t i = 0; i < seen_.size(); ++i) {
    if (!seen_[i]) out.push_back(i);
  }
  return out;
}

}  // namespace vacuum
