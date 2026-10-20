#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "vacuum/types.h"

namespace vacuum {

/// Bit-level audio modem. Implementations turn a bit vector into a waveform
/// that can be added to host audio, and turn received audio back into bits.
class Modem {
 public:
  virtual ~Modem() = default;

  struct Extract {
    bool synced = false;             ///< acquisition preamble was found
    std::ptrdiff_t syncSample = 0;   ///< sample index of the first symbol
    double syncScore = 0.0;          ///< fraction of preamble bits recovered
    std::vector<std::uint8_t> bits;  ///< payload bits, preamble excluded
    std::vector<float> confidence;   ///< 0..1 per bit, from the soft decision
  };

  virtual int sampleRate() const noexcept = 0;

  /// Bits consumed by the acquisition preamble; the receiver skips them.
  virtual std::size_t preambleBitCount() const noexcept = 0;

  /// Modem-layer bits per second of audio, after paying for `repeat` but before
  /// framing, CRC and Reed-Solomon. Real throughput is lower -- see
  /// `EncodeResult::payloadBitRate`.
  virtual double netBitRate() const noexcept = 0;

  /// Samples of audio a transmission of `bitCount` bits occupies, including the
  /// acquisition preamble.
  virtual std::size_t samplesForBits(std::size_t bitCount) const noexcept = 0;

  /// Modulate `bits` and add them to `host` starting at sample `offset`.
  /// The host is zero-padded when it is shorter than the watermark.
  virtual Audio embed(const std::vector<std::uint8_t>& bits,
                      const Audio& host,
                      std::size_t offset) const = 0;

  /// Demodulate everything recoverable from `rx`. The caller decides how many
  /// of the returned bits are meaningful (see the physical header).
  virtual Extract extract(const Audio& rx) const = 0;
};

}  // namespace vacuum
