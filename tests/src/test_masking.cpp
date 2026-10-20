#include "testing.h"
#include "audio_gen.h"
#include "vacuum/masking.h"

#include <cmath>

using namespace vacuum;

namespace {

constexpr int kFft = 1024;
constexpr int kRate = 48000;
constexpr std::size_t kBins = kFft / 2 + 1;

MaskingModel makeModel(float marginDb = 6.0f, float floor = 0.004f, float peak = 0.2f) {
  return MaskingModel(kFft, kRate, marginDb, floor, peak);
}

std::vector<float> spectrumWithTone(int bin, float amplitude) {
  std::vector<float> mags(kBins, 0.0f);
  mags[bin] = amplitude;
  return mags;
}

}  // namespace

TEST(masking_silence_yields_floor_amplitude) {
  MaskingModel model = makeModel();
  model.setCarrierBins({24, 48, 96, 144});

  std::vector<float> out;
  model.carrierAmplitudes(std::vector<float>(kBins, 0.0f), out);
  CHECK_EQ(out.size(), std::size_t{4});
  for (float v : out) CHECK_NEAR(v, 0.004, 1e-6);
}

TEST(masking_louder_host_allows_louder_tone) {
  MaskingModel model = makeModel();
  model.setCarrierBins({100});

  std::vector<float> prev = {0.0f};
  for (float amp : {0.01f, 0.05f, 0.1f, 0.3f, 0.8f}) {
    std::vector<float> out;
    model.carrierAmplitudes(spectrumWithTone(98, amp), out);
    CHECK(out[0] > prev[0]);
    prev = out;
  }
}

TEST(masking_respects_peak_cap) {
  MaskingModel model = makeModel(6.0f, 0.004f, 0.2f);
  model.setCarrierBins({60});
  std::vector<float> out;
  model.carrierAmplitudes(spectrumWithTone(60, 100.0f), out);
  CHECK_NEAR(out[0], 0.2, 1e-6);
}

TEST(masking_tighter_margin_gives_quieter_tone) {
  // Use a host level low enough that neither setting hits the peak cap,
  // otherwise the ratio measures the cap rather than the margin.
  MaskingModel loud = makeModel(0.0f, 0.0001f, 0.5f);
  MaskingModel quiet = makeModel(20.0f, 0.0001f, 0.5f);
  loud.setCarrierBins({80});
  quiet.setCarrierBins({80});

  const auto host = spectrumWithTone(78, 0.05f);
  std::vector<float> a, b;
  loud.carrierAmplitudes(host, a);
  quiet.carrierAmplitudes(host, b);
  CHECK(a[0] > b[0]);
  CHECK_NEAR(a[0] / b[0], std::pow(10.0, 20.0 / 20.0), 1e-3);
}

TEST(masking_is_asymmetric_upward) {
  // A low-frequency masker hides a higher-frequency tone far better than the
  // reverse; this is the property the Schroeder spreading function encodes.
  MaskingModel model = makeModel();
  model.setCarrierBins({100});

  const float barkStep = MaskingModel::hzToBark(100 * 46.875f + 46.875f) -
                         MaskingModel::hzToBark(100 * 46.875f);
  const int offset = std::max(1, static_cast<int>(std::round(2.0f / barkStep)));

  std::vector<float> up, down;
  model.carrierAmplitudes(spectrumWithTone(100 - offset, 0.5f), up);
  model.carrierAmplitudes(spectrumWithTone(100 + offset, 0.5f), down);
  CHECK(up[0] > down[0]);
}

TEST(masking_nearby_bins_mask_more_than_distant_bins) {
  MaskingModel model = makeModel();
  model.setCarrierBins({100});

  std::vector<float> near, far;
  model.carrierAmplitudes(spectrumWithTone(99, 0.5f), near);
  model.carrierAmplitudes(spectrumWithTone(20, 0.5f), far);
  CHECK(near[0] > far[0]);
}

TEST(masking_bark_scale_is_monotonic) {
  float prev = MaskingModel::hzToBark(0.0f);
  for (float f = 50.0f; f < 20000.0f; f *= 1.2f) {
    const float z = MaskingModel::hzToBark(f);
    CHECK(z > prev);
    prev = z;
  }
  // Sanity anchors for the Bark formula.
  CHECK_NEAR(MaskingModel::hzToBark(1000.0f), 8.5, 0.5);
}

TEST(masking_rejects_out_of_range_carrier_bin) {
  MaskingModel model = makeModel();
  bool threw = false;
  try {
    model.setCarrierBins({static_cast<int>(kBins + 10)});
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
}

TEST(masking_rejects_non_power_of_two_fft) {
  bool threw = false;
  try {
    MaskingModel model(1000, kRate, 6.0f, 0.004f, 0.2f);
    (void)model;
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
}

TEST(masking_rejects_short_spectrum) {
  MaskingModel model = makeModel();
  model.setCarrierBins({24});
  bool threw = false;
  try {
    std::vector<float> out;
    model.carrierAmplitudes(std::vector<float>(10, 0.0f), out);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
}

TEST(masking_real_music_produces_usable_headroom) {
  // With a realistic host the model must allow tones loud enough to be
  // detected, otherwise the whole hiding scheme carries no energy.
  MaskingModel model = makeModel();
  model.setCarrierBins({40, 80, 120, 160});

  const Audio host = testutil::synthHost(0.05, kRate);
  std::vector<float> mags(kBins, 0.0f);
  // Crude magnitude estimate from a single window; the test only needs the
  // order of magnitude, not an exact spectrum.
  for (std::size_t i = 0; i < host.size() && i < static_cast<std::size_t>(kFft); ++i) {
    mags[i % kBins] += std::abs(host[i]);
  }

  std::vector<float> out;
  model.carrierAmplitudes(mags, out);
  int usable = 0;
  for (float v : out) {
    if (v > 0.004f) ++usable;
  }
  CHECK(usable >= 1);
}
