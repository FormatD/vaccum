#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "vacuum/fft.h"
#include "vacuum/masking.h"
#include "vacuum/modem.h"

namespace vacuum {

/// Configuration for the masked multicarrier modem.
///
/// The scheme is CP-OFDM in shape but non-coherent MFSK in detection: each
/// symbol picks one tone group out of `groups` inside each of `subBands`
/// independent frequency slots, and the receiver decides by comparing band
/// energies. Non-coherent detection is what makes this survive an unknown air
/// channel phase and small clock drift between playback and capture devices.
struct SpreadConfig {
  int sampleRate = 48000;
  int fftSize = 1024;
  int cyclicPrefix = 256;

  /// Carrier band. Kept below ~9.5 kHz for the air path, not the file path.
  /// `--high-hz 12000` measured better on a clean loopback (BER 2.84e-03 against
  /// 9.42e-03) and the five MP3-only rows still pass 10/10, but two rows the
  /// default passes drop to 8/10 -- `+ noise 12 dB` and `mp3 128k + lowpass 8k +
  /// noise 18 dB` -- and the lowpass rows fall apart: 5/10 at a 6 kHz cutoff
  /// against 10/10 here, 0/10 at 4 kHz against 4/10. Earbud drivers and phone
  /// microphone capsules are what roll off up there. See README
  /// "Measured results".
  float lowHz = 1100.0f;
  float highHz = 9400.0f;

  int subBands = 8;
  int groups = 4;  ///< tones per sub-band, must be a power of two
  int carriersPerGroup = 2;

  /// Physical symbols sent per logical symbol, combined as summed energies.
  /// Not a "more is better" knob. Doubling it to 6 halves delivered throughput
  /// (76.6 bps to 38.6 bps for a 120-byte payload, 24.85 s of audio against
  /// 12.53 s) and lowers every row's raw BER -- 0.4% to 19.5% per row, 7.5%
  /// averaged over the fifteen -- but loses message success on four of them:
  /// `--mode matrix --payload 120 --trials 10 --repeat 6` gives 7/10 at a 6 kHz
  /// lowpass against 10/10 here, 6/10 at 5 kHz against 9/10, 2/10 at 4 kHz
  /// against 4/10, and 9/10 at 12 dB SNR against 10/10. The other eleven rows
  /// hold 10/10 either way.
  int repeat = 3;

  int preambleSymbols = 8;
  float maskingMarginDb = 6.0f;
  /// Tone amplitude where the host audio is silent, so that a carrier with
  /// nothing masking it still clears the detection floor. Against a silent host
  /// the watermark aggregates to -39.6 dBFS, which is quieter in absolute terms
  /// than the -10.5 dB below host it reaches under music, where the masking
  /// model pushes it up toward `peakAmplitude`. Silence is also the easiest
  /// channel measured: 6/6 on all fifteen matrix rows at BER 0.00e+00.
  float floorAmplitude = 0.004f;
  /// Lower bound on the tone amplitude as a multiple of the host's own-bin
  /// amplitude. The receiver finds a tone by subtracting its estimate of the
  /// host energy at the same bin, which leaves |T|^2 + 2 Re(H T*); that is only
  /// guaranteed positive when the tone dominates the host there. Masking alone
  /// does not promise it, because the threshold is a spread sum that can be well
  /// above a single strong partial. 2.0 is the smallest safe value.
  ///
  /// In practice most carriers hit `peakAmplitude` long before this multiple is
  /// reached, so the ratio mostly controls how many of them do. Measured with
  /// `vacuum_bench --mode matrix --payload 32 --trials 3` over synthetic music,
  /// baselineWindow = 4, clean file loopback row:
  ///
  ///     detectFloorRatio   2.0    4.0    6.0    8.0   10.0   14.0
  ///     clean bit error    3.3e-2 2.9e-2 2.2e-2 1.5e-2 8.5e-3 5.9e-3
  ///     payloads back         1/3    3/3    3/3    3/3    3/3    3/3
  ///     watermark/host dB  -20.6  -15.2  -12.4  -10.9  -10.3   -9.6
  ///
  /// 5.6x fewer errors across the sweep for 11 dB more level, most of it between
  /// 6 and 10. 2.0 satisfies the inequality above but is not usable: two of three
  /// payloads never came back even on a clean loopback, because a Reed-Solomon
  /// block is an all-or-nothing threshold on top of the bit error rate.
  float detectFloorRatio = 10.0f;
  /// Per-carrier cap. Only subBands * carriersPerGroup carriers are active at
  /// once, so this must stay well below peakBudget / sqrt(active carriers) or
  /// the symbols will be rescaled and lose SNR.
  float peakAmplitude = 0.06f;
  /// Hard ceiling on the summed watermark within one symbol. Scaling a symbol
  /// down to fit is always safe: quieter than the masking threshold is still
  /// inaudible. Prevents clipping when many carriers add in phase.
  float peakBudget = 0.30f;
  int minBinGap = 2;
  /// Samples searched either side of the nominal symbol start by the cyclic
  /// prefix tracker, which absorbs playback/capture clock drift. 0 disables the
  /// tracker and uses the acquisition offset for every symbol.
  int timingSearch = 192;
  /// How far either side of a symbol, in symbols, to look for one where a given
  /// carrier is dark. The median host level at that carrier over the dark
  /// symbols found, scaled by each symbol's own broadband level, is what gets
  /// subtracted. When the window holds no dark symbol at all the receiver falls
  /// back to the neighbouring spectral probe bins. Wider windows give more
  /// samples but span real changes in the music; 2 and 4 measured within noise of
  /// each other and both beat 8.
  int baselineWindow = 4;
};

class SpreadModem final : public Modem {
 public:
  explicit SpreadModem(SpreadConfig config);

  int sampleRate() const noexcept override { return cfg_.sampleRate; }
  std::size_t preambleBitCount() const noexcept override;
  double netBitRate() const noexcept override;
  std::size_t samplesForBits(std::size_t bitCount) const noexcept override;

  Audio embed(const std::vector<std::uint8_t>& bits,
              const Audio& host,
              std::size_t offset) const override;
  Extract extract(const Audio& rx) const override;

  const SpreadConfig& config() const noexcept { return cfg_; }
  int bitsPerSymbol() const noexcept { return bitsPerSymbol_; }
  int totalCarriers() const noexcept { return totalCarriers_; }
  std::size_t symbolPeriod() const noexcept { return symbolPeriod_; }
  const std::vector<int>& carrierBins() const noexcept { return carrierBins_; }

  /// Preamble as bits, in transmission order.
  std::vector<std::uint8_t> preambleBits() const;

  /// Reference waveform of the preamble at unit amplitude. Exposed because the
  /// receiver correlates against it and diagnostics want to see it too.
  Audio preambleReference() const;

 private:
  /// Per-sub-band tone group selection for one symbol.
  void symbolGroups(const std::vector<std::uint8_t>& bits, std::size_t bitOffset,
                    std::vector<int>& groups) const;

  SpreadConfig cfg_;
  int totalCarriers_;
  int bitsPerSymbol_;
  int bitsPerGroup_;
  std::size_t symbolPeriod_;
  std::vector<int> carrierBins_;
  std::vector<int> carrierSubBand_;
  std::vector<int> carrierGroup_;
  /// Bins beside each carrier used to estimate the host level there. Excludes
  /// bins that carry a tone, so the estimate never contains watermark energy.
  std::vector<std::vector<int>> probeBins_;
  MaskingModel masking_;
  Fft fft_;
};

}  // namespace vacuum
