#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "vacuum/types.h"

namespace vacuum {

/// Allowed payload sizes per fragment. Kept as a small enumerated set so the
/// receiver can learn the block geometry from a 4-bit physical header instead
/// of a variable-length one.
enum class FragmentSize : std::uint8_t {
  Bytes8 = 0,
  Bytes24 = 1,
  Bytes64 = 2,
  Bytes128 = 3,
  Bytes196 = 4,
};

std::size_t fragmentPayloadSize(FragmentSize size) noexcept;
std::optional<FragmentSize> fragmentSizeFromCode(std::uint8_t code) noexcept;

struct FramePlan {
  FragmentSize size = FragmentSize::Bytes8;
  std::size_t fragmentCount = 1;
  std::size_t blockBytes = 0;  ///< length of one RS-encoded block
};

/// Pick the fragment size that minimises total transmitted bits for a payload
/// of `payloadLen` bytes, using no size larger than `maxSize`. Returns nullopt
/// when the payload cannot fit.
///
/// `maxSize` trades airtime for resilience: a Reed-Solomon block absorbs a fixed
/// number of bad symbols, so the smaller the block the higher the bit error rate
/// it survives. Bytes196 is the cheapest per payload byte; Bytes64 roughly
/// doubles the block-level error tolerance for about 25% more airtime.
std::optional<FramePlan> planFrames(std::size_t payloadLen, int rsParitySymbols,
                                    FragmentSize maxSize = FragmentSize::Bytes196);

// Frame layout, little-endian, before RS encoding:
//   [0]      version
//   [1]      fragmentIndex
//   [2..3]   fragmentCount
//   [4..5]   streamId
//   [6..7]   totalPayloadLen
//   [8..9]   fragmentPayloadLen (actual bytes in this fragment, <= size)
//   [10..13] crc32 of the fully reassembled payload
//   [14..]   fragment payload, zero padded to fragmentPayloadSize(size)
//   [last 4] crc32 of every preceding byte
constexpr std::size_t kFrameHeaderSize = 14;
constexpr std::size_t kFrameCrcSize = 4;
constexpr std::uint8_t kFrameVersion = 1;

/// Split `payload` into RS-encoded blocks ready for the modem.
std::vector<Bytes> packetize(const Bytes& payload,
                             std::uint16_t streamId,
                             const FramePlan& plan,
                             int rsParitySymbols);

/// Accumulates decoded frames and reassembles the payload.
class Reassembler {
 public:
  Reassembler() = default;

  /// Feed one RS-decoded frame. Returns true when the message just became
  /// complete. Frames with a bad version, bad CRC or a stream id that does not
  /// match the first accepted one are ignored.
  bool add(const Bytes& frameBytes);

  bool complete() const noexcept { return complete_; }
  const Bytes& payload() const noexcept { return payload_; }

  std::size_t fragmentsReceived() const noexcept { return received_; }
  std::size_t fragmentsExpected() const noexcept { return fragmentCount_; }
  std::vector<std::size_t> missingFragments() const;

  void reset() noexcept;

 private:
  bool started_ = false;
  bool complete_ = false;
  std::uint16_t streamId_ = 0;
  std::size_t fragmentCount_ = 0;
  std::size_t received_ = 0;
  std::size_t totalPayloadLen_ = 0;
  std::uint32_t payloadCrc_ = 0;
  std::vector<Bytes> fragments_;
  std::vector<char> seen_;
  Bytes payload_;
};

}  // namespace vacuum
