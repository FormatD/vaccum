#include "vacuum/correlate.h"

#include <algorithm>
#include <cmath>

#include "vacuum/fft.h"

namespace vacuum {
namespace {

std::size_t nextPow2(std::size_t v) {
  std::size_t n = 1;
  while (n < v) n <<= 1;
  return n;
}

constexpr std::size_t kBlockSamples = std::size_t{1} << 17;

/// Insert `cand` into a best-first list of at most `k` entries, dropping any
/// weaker entry that sits within `minSeparation` of the newcomer.
void insertCandidate(std::vector<std::pair<double, std::ptrdiff_t>>& list,
                     std::pair<double, std::ptrdiff_t> cand, std::size_t k,
                     std::ptrdiff_t minSeparation) {
  for (auto it = list.begin(); it != list.end(); ++it) {
    if (std::llabs(it->second - cand.second) < minSeparation) {
      if (it->first >= cand.first) return;  // a stronger neighbour already won
      list.erase(it);
      break;
    }
  }
  auto pos = std::lower_bound(list.begin(), list.end(), cand,
                              [](const auto& a, const auto& b) { return a.first > b.first; });
  list.insert(pos, cand);
  if (list.size() > k) list.resize(k);
}

}  // namespace

std::vector<std::pair<double, std::ptrdiff_t>> correlateTopOffsets(
    const Audio& rx, const Audio& ref, std::size_t k, std::ptrdiff_t minSeparation) {
  std::vector<std::pair<double, std::ptrdiff_t>> best;
  if (ref.empty() || rx.size() < ref.size() || k == 0) return best;

  const std::size_t refLen = ref.size();
  const std::size_t maxLag = rx.size() - refLen;
  const std::size_t step = std::min(kBlockSamples, maxLag + 1);
  const std::size_t m = nextPow2(step + refLen);

  Fft fft(m);
  std::vector<Complex> a(m), b(m);
  for (std::size_t i = 0; i < refLen; ++i) b[i] = Complex(ref[i], 0.0f);
  fft.forward(b);
  for (std::size_t i = 0; i < m; ++i) b[i] = std::conj(b[i]);

  const float invM = 1.0f / static_cast<float>(m);

  std::vector<double> cumEnergy(rx.size() + 1, 0.0);
  for (std::size_t i = 0; i < rx.size(); ++i) {
    cumEnergy[i + 1] = cumEnergy[i] + static_cast<double>(rx[i]) * rx[i];
  }

  for (std::size_t base = 0; base <= maxLag; base += step) {
    const std::size_t lags = std::min(step, maxLag - base + 1);
    std::fill(a.begin(), a.end(), Complex(0.0f, 0.0f));
    for (std::size_t i = 0; i < lags + refLen - 1; ++i) {
      a[i] = Complex(rx[base + i], 0.0f);
    }
    fft.forward(a);
    for (std::size_t i = 0; i < m; ++i) a[i] *= b[i];
    fft.inverse(a);

    for (std::size_t tau = 0; tau < lags; ++tau) {
      const double re = static_cast<double>(a[tau].real()) * invM;
      const double im = static_cast<double>(a[tau].imag()) * invM;
      const double mag = std::sqrt(re * re + im * im);
      const double windowEnergy = cumEnergy[base + tau + refLen] - cumEnergy[base + tau];
      const double norm = std::sqrt(std::max(1e-12, windowEnergy));
      const double score = mag / norm;
      insertCandidate(best, {score, static_cast<std::ptrdiff_t>(base + tau)}, k, minSeparation);
    }
  }
  return best;
}

}  // namespace vacuum
