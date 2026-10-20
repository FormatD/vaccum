#include "testing.h"
#include "vacuum/fft.h"

#include <cmath>
#include <random>

using namespace vacuum;

TEST(fft_impulse_is_flat_spectrum) {
  Fft fft(64);
  std::vector<Complex> x(64, Complex(0, 0));
  x[0] = Complex(1, 0);
  fft.forward(x);
  for (const auto& v : x) {
    CHECK_NEAR(v.real(), 1.0, 1e-4);
    CHECK_NEAR(v.imag(), 0.0, 1e-4);
  }
}

TEST(fft_single_bin_sinusoid) {
  const int n = 256;
  const int bin = 37;
  const float amp = 0.4f;
  Fft fft(n);
  std::vector<Complex> x(n);
  for (int i = 0; i < n; ++i) {
    const double a = 2.0 * 3.14159265358979323846 * bin * i / n;
    x[i] = Complex(amp * static_cast<float>(std::cos(a)), 0.0f);
  }
  fft.forward(x);

  // A real cosine of amplitude A lands as N*A/2 in the bin and its mirror.
  CHECK_NEAR(std::abs(x[bin]), n * amp / 2.0, 1e-2);
  CHECK_NEAR(std::abs(x[n - bin]), n * amp / 2.0, 1e-2);
  for (int k = 0; k < n; ++k) {
    if (k == bin || k == n - bin) continue;
    CHECK(std::abs(x[k]) < 1e-2f);
  }
}

TEST(fft_forward_inverse_roundtrip) {
  std::mt19937 rng(4);
  std::uniform_real_distribution<float> d(-1.0f, 1.0f);
  const int n = 512;
  Fft fft(n);

  std::vector<Complex> original(n);
  for (auto& v : original) v = Complex(d(rng), d(rng));

  std::vector<Complex> x = original;
  fft.forward(x);
  fft.inverse(x);
  for (int i = 0; i < n; ++i) x[i] /= static_cast<float>(n);

  for (int i = 0; i < n; ++i) {
    CHECK_NEAR(x[i].real(), original[i].real(), 1e-3);
    CHECK_NEAR(x[i].imag(), original[i].imag(), 1e-3);
  }
}

TEST(fft_conjugate_symmetric_input_gives_real_output) {
  const int n = 128;
  Fft fft(n);
  std::vector<Complex> x(n, Complex(0, 0));
  for (int k = 1; k < n / 2; ++k) {
    const float re = 0.1f * k, im = 0.03f * k;
    x[k] = Complex(re, im);
    x[n - k] = Complex(re, -im);
  }
  fft.inverse(x);
  for (int i = 0; i < n; ++i) {
    CHECK(std::abs(x[i].imag()) < 1e-2f);
  }
}

TEST(fft_matches_naive_dft) {
  const int n = 32;
  std::mt19937 rng(9);
  std::uniform_real_distribution<float> d(-1.0f, 1.0f);
  std::vector<Complex> x(n);
  for (auto& v : x) v = Complex(d(rng), 0.0f);

  std::vector<Complex> expected(n, Complex(0, 0));
  for (int k = 0; k < n; ++k) {
    for (int i = 0; i < n; ++i) {
      const double a = -2.0 * 3.14159265358979323846 * k * i / n;
      expected[k] += x[i] * Complex(static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)));
    }
  }

  std::vector<Complex> y = x;
  Fft fft(n);
  fft.forward(y);
  for (int k = 0; k < n; ++k) {
    CHECK_NEAR(y[k].real(), expected[k].real(), 1e-3);
    CHECK_NEAR(y[k].imag(), expected[k].imag(), 1e-3);
  }
}

TEST(fft_rejects_non_power_of_two) {
  bool threw = false;
  try {
    const Fft fft(100);
    (void)fft;
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
}
