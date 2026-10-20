#include "vacuum/fft.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace vacuum {
namespace {

constexpr double kPi = 3.14159265358979323846;

std::size_t log2Exact(std::size_t n) {
  std::size_t bits = 0;
  while ((std::size_t{1} << bits) < n) ++bits;
  if((std::size_t{1} << bits) != n) {
    throw std::invalid_argument("Fft size must be a power of two");
  }
  return bits;
}

}  // namespace

Fft::Fft(std::size_t n) : n_(n) {
  if (n < 2) throw std::invalid_argument("Fft size must be at least 2");
  const std::size_t bits = log2Exact(n);

  bitrev_.resize(n);
  for (std::size_t i = 0; i < n; ++i) {
    std::size_t reversed = 0;
    for (std::size_t b = 0; b < bits; ++b) {
      if (i & (std::size_t{1} << b)) reversed |= std::size_t{1} << (bits - 1 - b);
    }
    bitrev_[i] = reversed;
  }

  twiddle_.resize(n / 2);
  for (std::size_t k = 0; k < n / 2; ++k) {
    const double angle = -2.0 * kPi * static_cast<double>(k) / static_cast<double>(n);
    twiddle_[k] = Complex(static_cast<float>(std::cos(angle)),
                          static_cast<float>(std::sin(angle)));
  }
}

void Fft::forward(std::vector<Complex>& x) const { transform(x, false); }

void Fft::inverse(std::vector<Complex>& x) const { transform(x, true); }

void Fft::transform(std::vector<Complex>& x, bool inverse) const {
  if (x.size() != n_) throw std::invalid_argument("buffer size does not match Fft size");

  for (std::size_t i = 0; i < n_; ++i) {
    if (bitrev_[i] > i) std::swap(x[i], x[bitrev_[i]]);
  }

  for (std::size_t len = 2; len <= n_; len <<= 1) {
    const std::size_t half = len >> 1;
    const std::size_t step = n_ / len;
    for (std::size_t i = 0; i < n_; i += len) {
      for (std::size_t j = 0; j < half; ++j) {
        Complex w = twiddle_[j * step];
        if (inverse) w = std::conj(w);
        const Complex u = x[i + j];
        const Complex v = x[i + j + half] * w;
        x[i + j] = u + v;
        x[i + j + half] = u - v;
      }
    }
  }
}

}  // namespace vacuum
