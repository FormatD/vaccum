#include "vacuum/masking.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace vacuum {
namespace {

/// Schroeder spreading function, in dB, for a Bark offset of dz.
/// Asymmetric: low frequencies mask high frequencies far better than the reverse.
float spreadingDb(float dz) {
  const float z = dz + 0.474f;
  return 15.81f + 7.5f * z - 17.5f * std::sqrt(1.0f + z * z);
}

constexpr float kMaxBarkSpan = 6.0f;   // beyond this the gain is under -70 dB
constexpr float kMinGainLinear = 1e-5f;
constexpr float kTonalityOffsetDb = 4.0f;  // host tones mask less than noise

}  // namespace

float MaskingModel::hzToBark(float hz) noexcept {
  const float f = hz / 1000.0f;
  return 13.0f * std::atan(0.76f * f) + 3.5f * std::atan((f * f) / 56.25f);
}

MaskingModel::MaskingModel(std::size_t fftSize, int sampleRate, float marginDb,
                           float floorAmplitude, float peakAmplitude)
    : binCount_(fftSize / 2 + 1),
      binHz_(static_cast<float>(sampleRate) / static_cast<float>(fftSize)),
      marginLinear_(std::pow(10.0f, -(marginDb + kTonalityOffsetDb) / 20.0f)),
      floorAmplitude_(floorAmplitude),
      peakAmplitude_(peakAmplitude) {
  if (fftSize < 2 || (fftSize & (fftSize - 1)) != 0) {
    throw std::invalid_argument("fftSize must be a power of two");
  }
  bark_.resize(binCount_);
  for (std::size_t k = 0; k < binCount_; ++k) {
    bark_[k] = hzToBark(static_cast<float>(k) * binHz_);
  }
}

void MaskingModel::setCarrierBins(std::vector<int> bins) {
  carrierBins_ = std::move(bins);
  spread_.assign(carrierBins_.size(), {});

  for (std::size_t c = 0; c < carrierBins_.size(); ++c) {
    const std::size_t k = static_cast<std::size_t>(carrierBins_[c]);
    if (k >= binCount_) throw std::invalid_argument("carrier bin out of range");

    const float zTarget = bark_[k];
    // Walk outwards until the Bark distance exceeds the useful span.
    std::size_t lo = k;
    while (lo > 0 && (zTarget - bark_[lo - 1]) < kMaxBarkSpan) --lo;
    std::size_t hi = k;
    while (hi + 1 < binCount_ && (bark_[hi + 1] - zTarget) < kMaxBarkSpan) ++hi;

    auto& entries = spread_[c];
    entries.reserve(hi - lo + 1);
    for (std::size_t j = lo; j <= hi; ++j) {
      const float gain = std::pow(10.0f, spreadingDb(zTarget - bark_[j]) / 10.0f);
      if (gain >= kMinGainLinear) entries.emplace_back(j, gain);
    }
  }
}

void MaskingModel::carrierAmplitudes(const std::vector<float>& magnitudes,
                                     std::vector<float>& out) const {
  if (magnitudes.size() < binCount_) {
    throw std::invalid_argument("magnitude spectrum shorter than binCount");
  }
  out.assign(carrierBins_.size(), floorAmplitude_);

  for (std::size_t c = 0; c < carrierBins_.size(); ++c) {
    float maskedPower = 0.0f;
    for (const auto& [j, gain] : spread_[c]) {
      const float m = magnitudes[j];
      maskedPower += gain * m * m;
    }
    const float allowed = marginLinear_ * std::sqrt(maskedPower);
    out[c] = std::clamp(allowed, floorAmplitude_, peakAmplitude_);
  }
}

}  // namespace vacuum
