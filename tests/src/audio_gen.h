#pragma once

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

#include "vacuum/types.h"

namespace testutil {

constexpr double kPi = 3.14159265358979323846;

/// A stand-in for music: a harmonic stack with slow amplitude movement plus a
/// noise floor. Enough spectral structure to exercise the masking model
/// without needing a fixture file.
inline vacuum::Audio synthHost(double seconds, int sampleRate, unsigned seed = 7) {
  std::mt19937 rng(seed);
  std::normal_distribution<float> noise(0.0f, 0.02f);

  const std::size_t n = static_cast<std::size_t>(seconds * sampleRate);
  vacuum::Audio h(n);

  const float freqs[] = {110.f, 220.f, 330.f, 440.f, 880.f, 1760.f, 3200.f};
  const float amps[] = {0.30f, 0.22f, 0.15f, 0.12f, 0.06f, 0.03f, 0.015f};

  for (std::size_t i = 0; i < n; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sampleRate);
    float v = noise(rng);
    for (int k = 0; k < 7; ++k) {
      const float lfo = 0.6f + 0.4f * std::sin(static_cast<float>(2 * kPi) * (0.3f + 0.17f * k) * t);
      v += amps[k] * lfo * std::sin(static_cast<float>(2 * kPi) * freqs[k] * t + k);
    }
    h[i] = std::clamp(v, -1.0f, 1.0f);
  }
  return h;
}

/// Add white Gaussian noise scaled so the result has the requested SNR
/// relative to `signal`'s own power.
inline vacuum::Audio addNoise(const vacuum::Audio& signal, double snrDb, unsigned seed = 11) {
  double power = 0.0;
  for (float v : signal) power += static_cast<double>(v) * v;
  power /= static_cast<double>(std::max<std::size_t>(signal.size(), 1));

  const double noisePower = power / std::pow(10.0, snrDb / 10.0);
  const float sigma = static_cast<float>(std::sqrt(noisePower));

  std::mt19937 rng(seed);
  std::normal_distribution<float> d(0.0f, sigma);
  vacuum::Audio out = signal;
  for (float& v : out) v += d(rng);
  return out;
}

/// Shift by a fractional number of samples with linear interpolation, to
/// emulate the delay and slight smearing of an air path.
inline vacuum::Audio fractionalDelay(const vacuum::Audio& in, double delaySamples) {
  const auto whole = static_cast<long long>(std::floor(delaySamples));
  const float frac = static_cast<float>(delaySamples - static_cast<double>(whole));
  vacuum::Audio out(in.size() + static_cast<std::size_t>(std::max(0LL, whole)), 0.0f);
  for (std::size_t i = 0; i < in.size(); ++i) {
    const long long t = static_cast<long long>(i) + whole;
    if (t < 0 || static_cast<std::size_t>(t) >= out.size()) continue;
    out[t] += in[i] * (1.0f - frac);
    if (t + 1 < static_cast<long long>(out.size())) out[t + 1] += in[i] * frac;
  }
  return out;
}

/// First-order low-pass, standing in for earbud and microphone roll-off.
inline vacuum::Audio lowpass(const vacuum::Audio& in, int sampleRate, double cutoffHz) {
  const double rc = 1.0 / (2 * kPi * cutoffHz);
  const double dt = 1.0 / sampleRate;
  const double alpha = dt / (rc + dt);
  vacuum::Audio out = in;
  for (std::size_t i = 1; i < out.size(); ++i) {
    out[i] = static_cast<float>(out[i - 1] + alpha * (out[i] - out[i - 1]));
  }
  return out;
}

}  // namespace testutil
