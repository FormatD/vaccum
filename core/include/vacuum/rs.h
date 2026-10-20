#pragma once

#include <cstdint>
#include <vector>

#include "vacuum/types.h"

namespace vacuum {

/// Systematic Reed-Solomon codec over GF(256), shortened-capable.
///
/// A block carries at most 255 symbols total; `paritySymbols` of them are
/// parity, so up to 255 - paritySymbols data bytes. Corrects up to
/// paritySymbols / 2 erroneous symbols per block, or flags failure.
class ReedSolomon {
 public:
  static constexpr int kMaxBlock = 255;

  /// @param paritySymbols must be even and in [2, 254].
  explicit ReedSolomon(int paritySymbols);

  int paritySymbols() const noexcept { return nsym_; }
  int maxCorrectableSymbols() const noexcept { return nsym_ / 2; }
  int maxDataBytes() const noexcept { return kMaxBlock - nsym_; }

  /// Returns data followed by parity. Throws if the block would exceed 255.
  Bytes encode(const Bytes& data) const;

  struct DecodeResult {
    bool ok = false;
    Bytes data;
    int correctedSymbols = 0;
  };

  /// Accepts a received block of any length in (paritySymbols, 255].
  ///
  /// `erasures` holds indices into `codeword` whose value the caller does not
  /// trust. A known position costs one parity symbol to fix rather than two, so
  /// a block survives nsym erasures but only nsym/2 unmarked errors. That
  /// matters here because the demodulator reports a confidence per bit, and the
  /// bits it is unsure about are far more likely to be wrong.
  ///
  /// `ok` is false when the block is uncorrectable; never returns silently
  /// corrupted data, because the result is re-checked against the syndromes.
  DecodeResult decode(const Bytes& codeword, const std::vector<int>& erasures = {}) const;

 private:
  int nsym_;
  std::vector<std::uint8_t> generatorDesc_;  ///< g(x), index i = coeff of x^(nsym-i)
};

}  // namespace vacuum
