#include "vacuum/rs.h"

#include <stdexcept>

#include "vacuum/gf256.h"

namespace vacuum {
namespace {

using Poly = std::vector<std::uint8_t>;  // ascending: p[i] = coeff of x^i

std::uint8_t polyEval(const Poly& p, std::uint8_t x) {
  std::uint8_t acc = 0;
  for (auto it = p.rbegin(); it != p.rend(); ++it) {
    acc = gf::add(gf::mul(acc, x), *it);
  }
  return acc;
}

Poly polyMulTrunc(const Poly& a, const Poly& b, std::size_t maxDegree) {
  Poly out(maxDegree, 0);
  for (std::size_t i = 0; i < a.size() && i < maxDegree; ++i) {
    if (a[i] == 0) continue;
    for (std::size_t j = 0; j < b.size() && i + j < maxDegree; ++j) {
      if (b[j] == 0) continue;
      out[i + j] = gf::add(out[i + j], gf::mul(a[i], b[j]));
    }
  }
  return out;
}

/// Formal derivative. In characteristic 2 only odd powers survive.
Poly polyDerivative(const Poly& p) {
  if (p.size() <= 1) return Poly{0};
  Poly out(p.size() - 1, 0);
  for (std::size_t i = 1; i < p.size(); ++i) {
    if (i & 1) out[i - 1] = p[i];
  }
  return out;
}

std::size_t polyDegree(const Poly& p) {
  std::size_t d = p.size();
  while (d > 0 && p[d - 1] == 0) --d;
  return d == 0 ? 0 : d - 1;
}

/// Berlekamp-Massey: given syndromes s[0..nsym-1], return the minimal-degree
/// error locator Lambda(x) with Lambda(0) = 1.
///
/// The caller checks the resulting degree against its budget. It cannot be
/// checked during the iteration: L is only monotone at the end and transiently
/// overshoots the final degree partway through, which would reject correct
/// solutions.
Poly berlekampMassey(const Poly& s) {
  const int nsym = static_cast<int>(s.size());
  Poly locator{1};
  Poly previous{1};
  int L = 0;
  int shift = 1;
  std::uint8_t b = 1;

  for (int n = 0; n < nsym; ++n) {
    std::uint8_t d = s[n];
    for (int i = 1; i <= L && i < static_cast<int>(locator.size()); ++i) {
      d = gf::add(d, gf::mul(locator[i], s[n - i]));
    }

    if (d == 0) {
      ++shift;
      continue;
    }

    const Poly before = locator;
    const std::uint8_t scale = gf::div(d, b);
    const std::size_t needed = previous.size() + shift;
    if (locator.size() < needed) locator.resize(needed, 0);
    for (std::size_t i = 0; i < previous.size(); ++i) {
      locator[i + shift] = gf::add(locator[i + shift], gf::mul(previous[i], scale));
    }

    if (2 * L <= n) {
      L = n + 1 - L;
      previous = before;
      b = d;
      shift = 1;
    } else {
      ++shift;
    }
  }
  return locator;
}

}  // namespace

ReedSolomon::ReedSolomon(int paritySymbols) : nsym_(paritySymbols) {
  if (nsym_ < 2 || nsym_ > 254 || (nsym_ & 1) != 0) {
    throw std::invalid_argument("paritySymbols must be even and in [2, 254]");
  }
  // g(x) = product over i of (x - alpha^i), stored descending so the encoder
  // shift register can index it directly.
  Poly ascending{1};
  for (int i = 0; i < nsym_; ++i) {
    const Poly factor{gf::exp(i), 1};  // alpha^i + x
    Poly product(ascending.size() + 1, 0);
    for (std::size_t a = 0; a < ascending.size(); ++a) {
      for (std::size_t b = 0; b < factor.size(); ++b) {
        product[a + b] = gf::add(product[a + b], gf::mul(ascending[a], factor[b]));
      }
    }
    ascending.swap(product);
  }
  generatorDesc_.assign(ascending.rbegin(), ascending.rend());
}

Bytes ReedSolomon::encode(const Bytes& data) const {
  if (data.empty()) throw std::invalid_argument("cannot encode an empty block");
  if (static_cast<int>(data.size()) + nsym_ > kMaxBlock) {
    throw std::invalid_argument("block exceeds 255 symbols");
  }

  // Shift-register division of data(x) * x^nsym by g(x).
  std::vector<std::uint8_t> reg(nsym_, 0);
  for (std::uint8_t byte : data) {
    const std::uint8_t feedback = gf::add(byte, reg[0]);
    for (int i = 0; i < nsym_ - 1; ++i) {
      reg[i] = gf::add(reg[i + 1], gf::mul(generatorDesc_[i + 1], feedback));
    }
    reg[nsym_ - 1] = gf::mul(generatorDesc_[nsym_], feedback);
  }

  Bytes out = data;
  out.insert(out.end(), reg.begin(), reg.end());
  return out;
}

ReedSolomon::DecodeResult ReedSolomon::decode(const Bytes& codeword,
                                              const std::vector<int>& erasures) const {
  DecodeResult result;
  const int n = static_cast<int>(codeword.size());
  if (n <= nsym_ || n > kMaxBlock) return result;

  Bytes buf = codeword;  // buf[i] is the coefficient of x^(n-1-i)

  const auto computeSyndromes = [&]() {
    Poly r(n);
    for (int i = 0; i < n; ++i) r[n - 1 - i] = buf[i];
    Poly s(nsym_);
    for (int k = 0; k < nsym_; ++k) s[k] = polyEval(r, gf::exp(k));
    return s;
  };

  const Poly s = computeSyndromes();
  bool clean = true;
  for (std::uint8_t v : s) {
    if (v != 0) { clean = false; break; }
  }
  if (clean) {
    result.ok = true;
    result.data.assign(buf.begin(), buf.begin() + (n - nsym_));
    return result;
  }

  // Erasure locator: the product of (1 - X x) over the flagged positions, where
  // X = alpha^p and p is the position's power in the codeword polynomial.
  Poly erasureLocator{1};
  for (int pos : erasures) {
    if (pos < 0 || pos >= n) return result;
    const Poly factor{1, gf::exp(n - 1 - pos)};
    Poly product(erasureLocator.size() + 1, 0);
    for (std::size_t i = 0; i < erasureLocator.size(); ++i) {
      for (std::size_t j = 0; j < factor.size(); ++j) {
        product[i + j] = gf::add(product[i + j], gf::mul(erasureLocator[i], factor[j]));
      }
    }
    erasureLocator.swap(product);
  }
  const int erasureCount = static_cast<int>(polyDegree(erasureLocator));
  if (erasureCount > nsym_) return result;

  // Fold the erasures out of the syndromes. What is left describes only the
  // unflagged errors, and each erasure has consumed one parity symbol, so the
  // locator for the rest may be at most half as long again.
  const int remaining = nsym_ - erasureCount;
  Poly locator;
  if (remaining > 0) {
    // Fold the erasure locator into the syndromes. The coefficient of x^m in
    // S(x) * Lambda_erasure(x) equals sum_i Lambda_i S_{m-i}, which only reaches
    // the full erasure locator once m >= erasureCount; below that the sum is
    // truncated and still carries erasure energy. So the sequence describing
    // the unflagged errors alone is the tail, not the head, and it is exactly
    // `remaining` terms long.
    const Poly product = polyMulTrunc(s, erasureLocator, nsym_);
    const Poly folded(product.begin() + erasureCount, product.end());
    const Poly errorLocator = berlekampMassey(folded);
    if (static_cast<int>(polyDegree(errorLocator)) > remaining / 2) return result;
    locator = errorLocator;
  } else {
    locator = Poly{1};
  }

  // Full errata locator.
  Poly combined(erasureLocator.size() + locator.size() - 1, 0);
  for (std::size_t i = 0; i < erasureLocator.size(); ++i) {
    if (erasureLocator[i] == 0) continue;
    for (std::size_t j = 0; j < locator.size(); ++j) {
      if (locator[j] == 0) continue;
      combined[i + j] = gf::add(combined[i + j], gf::mul(erasureLocator[i], locator[j]));
    }
  }

  const int degree = static_cast<int>(polyDegree(combined));
  if (degree == 0 || degree > nsym_) return result;

  // Chien search: Lambda(alpha^-p) == 0 marks an erratum at power p.
  std::vector<int> errorPowers;
  errorPowers.reserve(degree);
  for (int p = 0; p < n; ++p) {
    if (polyEval(combined, gf::exp(-p)) == 0) errorPowers.push_back(p);
  }
  if (static_cast<int>(errorPowers.size()) != degree) return result;

  const Poly omega = polyMulTrunc(s, combined, nsym_);
  const Poly locatorDerivative = polyDerivative(combined);

  int corrected = 0;
  for (int p : errorPowers) {
    const std::uint8_t x = gf::exp(p);
    const std::uint8_t xInv = gf::exp(-p);
    const std::uint8_t den = polyEval(locatorDerivative, xInv);
    if (den == 0) return result;
    const std::uint8_t magnitude = gf::div(gf::mul(x, polyEval(omega, xInv)), den);
    if (magnitude != 0) ++corrected;
    buf[n - 1 - p] = gf::add(buf[n - 1 - p], magnitude);
  }

  for (std::uint8_t v : computeSyndromes()) {
    if (v != 0) return result;
  }

  result.ok = true;
  result.data.assign(buf.begin(), buf.begin() + (n - nsym_));
  result.correctedSymbols = corrected;
  return result;
}

}  // namespace vacuum
