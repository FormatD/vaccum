#pragma once

#include <cstddef>
#include <utility>
#include <vector>

namespace vacuum {

/// Simplified simultaneous-masking model.
///
/// Given the magnitude spectrum of one block of host audio, it reports how loud
/// a watermark tone may be at each carrier bin before it becomes audible. This
/// is what lets the payload hide inside ordinary music: the tone energy sits
/// below the masking threshold the host audio itself creates, so it survives
/// MP3 encoding (which preserves everything above that threshold) without being
/// heard.
class MaskingModel {
 public:
  /// @param fftSize      transform size the host is analysed with
  /// @param sampleRate   Hz
  /// @param marginDb     safety margin below the computed threshold
  /// @param floorAmplitude tone amplitude used where the host is silent
  /// @param peakAmplitude  hard cap on tone amplitude
  MaskingModel(std::size_t fftSize, int sampleRate, float marginDb,
               float floorAmplitude, float peakAmplitude);

  /// Lock the set of carrier bins the model will be queried about. Must be
  /// called before `carrierAmplitudes`.
  void setCarrierBins(std::vector<int> bins);

  const std::vector<int>& carrierBins() const noexcept { return carrierBins_; }

  /// @param magnitudes host magnitude spectrum, length fftSize/2 + 1
  /// @param out        allowed linear tone amplitude per carrier bin
  void carrierAmplitudes(const std::vector<float>& magnitudes,
                         std::vector<float>& out) const;

  std::size_t binCount() const noexcept { return binCount_; }
  float binHz() const noexcept { return binHz_; }

  static float hzToBark(float hz) noexcept;

 private:
  std::size_t binCount_;
  float binHz_;
  float marginLinear_;
  float floorAmplitude_;
  float peakAmplitude_;
  std::vector<float> bark_;
  std::vector<int> carrierBins_;
  /// For each carrier, the (source bin, linear gain) pairs that mask it.
  std::vector<std::vector<std::pair<std::size_t, float>>> spread_;
};

}  // namespace vacuum
