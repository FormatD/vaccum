#pragma once

#include <complex>
#include <cstddef>
#include <vector>

namespace vacuum {

using Complex = std::complex<float>;

/// Iterative radix-2 Cooley-Tukey FFT. Self contained, no third-party code.
class Fft {
 public:
  /// @param n transform size, must be a power of two and at least 2.
  explicit Fft(std::size_t n);

  std::size_t size() const noexcept { return n_; }

  /// Forward transform, in place.
  void forward(std::vector<Complex>& x) const;

  /// Inverse transform, in place and unnormalised: divide by size() yourself.
  void inverse(std::vector<Complex>& x) const;

 private:
  void transform(std::vector<Complex>& x, bool inverse) const;

  std::size_t n_;
  std::vector<std::size_t> bitrev_;
  std::vector<Complex> twiddle_;  ///< exp(-2*pi*i*k/n) for k in [0, n/2)
};

}  // namespace vacuum
