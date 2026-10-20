#include "vacuum/ss_modem.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "vacuum/correlate.h"
#include "vacuum/physical.h"

namespace vacuum {
namespace {

int log2Int(int v) {
  int b = 0;
  while ((1 << b) < v) ++b;
  if ((1 << b) != v) throw std::invalid_argument("value must be a power of two");
  return b;
}

/// Spread `count` carrier bins evenly over [binLow, binHigh], then push them
/// apart until every gap is at least `minGap`.
std::vector<int> chooseBins(int binLow, int binHigh, int count, int minGap) {
  if (count <= 0) throw std::invalid_argument("carrier count must be positive");
  if (binHigh < binLow) throw std::invalid_argument("empty carrier band");

  std::vector<int> bins(count);
  for (int i = 0; i < count; ++i) {
    const double t = count == 1 ? 0.0 : static_cast<double>(i) / static_cast<double>(count - 1);
    bins[i] = static_cast<int>(std::lround(binLow + t * (binHigh - binLow)));
  }
  for (int i = 1; i < count; ++i) bins[i] = std::max(bins[i], bins[i - 1] + minGap);
  return bins;
}

/// Fill a length-`n` spectrum with the selected tone groups at the given
/// amplitudes, ready for an unnormalised inverse FFT.
void buildSymbolSpectrum(const std::vector<int>& groups, const std::vector<int>& carrierBins,
                         const std::vector<int>& carrierSubBand,
                         const std::vector<int>& carrierGroup, const std::vector<float>& amplitude,
                         int carrierCount, int symbolIndex, std::vector<Complex>& x) {
  const int n = static_cast<int>(x.size());
  std::fill(x.begin(), x.end(), Complex(0.0f, 0.0f));
  for (int c = 0; c < carrierCount; ++c) {
    if (groups[carrierSubBand[c]] != carrierGroup[c]) continue;
    const float ph = carrierPhase(c, symbolIndex, carrierCount);
    const float a = 0.5f * amplitude[c];
    const int bin = carrierBins[c];
    x[bin] += Complex(a * std::cos(ph), a * std::sin(ph));
    x[n - bin] += Complex(a * std::cos(ph), -a * std::sin(ph));
  }
}

/// Per-symbol energy at every carrier, before host-baseline removal.
struct CarrierEnergies {
  std::size_t symbols = 0;
  std::size_t carriers = 0;
  std::vector<float> e;  ///< [symbol][carrier]
  /// Host-only energy estimated from the bins next to each carrier in the same
  /// symbol. Independent of what data was sent, which the time-domain estimate
  /// is not.
  std::vector<float> probe;  ///< [symbol][carrier]

  float at(std::size_t sym, std::size_t carrier) const {
    return e[sym * carriers + carrier];
  }
  float probeAt(std::size_t sym, std::size_t carrier) const {
    return probe[sym * carriers + carrier];
  }
};

/// Per-symbol, per-sub-band tone-group energies.
struct GroupEnergies {
  std::size_t symbols = 0;
  int subBands = 0;
  int groups = 0;
  std::vector<float> e;  ///< [symbol][subBand * groups + group]

  float& at(std::size_t sym, int subBand, int group) {
    return e[(sym * subBands + subBand) * groups + group];
  }
  float at(std::size_t sym, int subBand, int group) const {
    return e[(sym * subBands + subBand) * groups + group];
  }
};

CarrierEnergies collectCarrierEnergies(const Audio& rx, std::ptrdiff_t start,
                                       std::size_t symbolCount, std::size_t symbolPeriod, int n,
                                       int cp, int timingSearch,
                                       const std::vector<int>& carrierBins,
                                       const std::vector<std::vector<int>>& probeBins,
                                       const Fft& fft) {
  CarrierEnergies out;
  out.symbols = symbolCount;
  out.carriers = carrierBins.size();
  out.e.assign(symbolCount * carrierBins.size(), 0.0f);
  out.probe.assign(symbolCount * carrierBins.size(), 0.0f);

  std::vector<Complex> z(static_cast<std::size_t>(n));
  std::vector<float> scratch;
  const std::size_t rxLen = rx.size();

  for (std::size_t p = 0; p < symbolCount; ++p) {
    const std::ptrdiff_t nominal = start + static_cast<std::ptrdiff_t>(p * symbolPeriod);

    // The cyclic prefix is a copy of the symbol tail, so its autocorrelation
    // peaks at the true symbol start independently of the payload. This is what
    // absorbs playback/capture clock drift over a long message.
    std::ptrdiff_t bestM = nominal;
    if (timingSearch > 0) {
      double bestMetric = -1.0;
      bool found = false;
      for (int d = -timingSearch; d <= timingSearch; ++d) {
        const std::ptrdiff_t m = nominal + d;
        if (m < 0 || static_cast<std::size_t>(m) + cp + n > rxLen) continue;
        double metric = 0.0;
        for (int i = 0; i < cp; ++i) {
          metric += static_cast<double>(rx[m + i]) * static_cast<double>(rx[m + n + i]);
        }
        metric = std::abs(metric);
        if (!found || metric > bestMetric) {
          bestMetric = metric;
          bestM = m;
          found = true;
        }
      }
      if (!found) continue;
    }
    if (bestM < 0 || static_cast<std::size_t>(bestM) + cp + n > rxLen) continue;

    for (int i = 0; i < n; ++i) z[i] = Complex(rx[bestM + cp + i], 0.0f);
    fft.forward(z);

    float* row = &out.e[p * out.carriers];
    float* probeRow = &out.probe[p * out.carriers];
    for (std::size_t c = 0; c < carrierBins.size(); ++c) {
      const Complex v = z[carrierBins[c]];
      row[c] = v.real() * v.real() + v.imag() * v.imag();

      // Tones land exactly on bin centres, so the bins beside a carrier hold
      // host energy only. Their median is an estimate of the host level at the
      // carrier itself that does not care which carriers are lit.
      scratch.clear();
      for (int bin : probeBins[c]) {
        const Complex u = z[bin];
        scratch.push_back(u.real() * u.real() + u.imag() * u.imag());
      }
      if (scratch.empty()) {
        probeRow[c] = row[c];
      } else {
        const std::size_t mid = scratch.size() / 2;
        std::nth_element(scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(mid),
                         scratch.end());
        probeRow[c] = scratch[mid];
      }
    }
  }
  return out;
}

/// Remove the host audio's contribution.
///
/// This is the step that makes masking-based hiding detectable at all. The
/// watermark is placed under the psychoacoustic masking threshold, so at a
/// carrier's own bin the tone can sit below a strong host partial, and raw
/// non-coherent detection would fail. What saves it is that the receiver can
/// estimate the host-only level at that bin and subtract it.
///
/// The estimate is a median over the symbols in which `litMask` says the carrier
/// is dark. A median, because it rejects the odd symbol where the host happens
/// to move; dark symbols only, because a lit one measures host plus tone and
/// would cancel the very thing being detected.
///
/// Each sample is divided by its symbol's overall host level before the median
/// and rescaled afterwards. Music swells and decays as a whole and that envelope
/// moves faster than the window can follow, so without the normalisation the
/// estimate carries the drift of the neighbouring symbols instead of the current
/// one. Only 1 in `carriersPerGroup` carriers is lit per symbol, so the median
/// across carriers is a dark-dominated measure of level.
GroupEnergies subtractHostBaseline(const CarrierEnergies& raw, const std::vector<int>& carrierSubBand,
                                  const std::vector<int>& carrierGroup, int subBands, int groups,
                                  int window, const std::vector<char>* litMask) {
  GroupEnergies out;
  out.symbols = raw.symbols;
  out.subBands = subBands;
  out.groups = groups;
  out.e.assign(raw.symbols * static_cast<std::size_t>(subBands) * static_cast<std::size_t>(groups),
               0.0f);
  if (raw.symbols == 0) return out;

  const std::size_t carriers = raw.carriers;
  std::vector<float> level(raw.symbols, 1.0f);
  {
    std::vector<float> row(carriers);
    const std::size_t mid = carriers / 2;
    for (std::size_t p = 0; p < raw.symbols; ++p) {
      for (std::size_t c = 0; c < carriers; ++c) row[c] = raw.at(p, c);
      std::nth_element(row.begin(), row.begin() + static_cast<std::ptrdiff_t>(mid), row.end());
      level[p] = std::max(row[mid], 1e-20f);
    }
  }

  const int half = std::max(0, window);
  std::vector<float> buf;
  buf.reserve(2 * static_cast<std::size_t>(half) + 1);

  for (std::size_t p = 0; p < raw.symbols; ++p) {
    const std::size_t lo =
        p > static_cast<std::size_t>(half) ? p - static_cast<std::size_t>(half) : 0;
    const std::size_t hi = std::min(raw.symbols - 1, p + static_cast<std::size_t>(half));

    for (std::size_t c = 0; c < carriers; ++c) {
      buf.clear();
      if (half > 0) {
        for (std::size_t q = lo; q <= hi; ++q) {
          if (litMask && (*litMask)[q * carriers + c]) continue;
          buf.push_back(raw.at(q, c) / level[q]);
        }
      }

      float baseline = raw.probeAt(p, c);
      if (buf.size() >= 3) {
        const std::size_t m = buf.size() / 2;
        std::nth_element(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(m), buf.end());
        baseline = buf[m] * level[p];
      } else if (!buf.empty()) {
        baseline = buf[0] * level[p];
      }

      const float residual = raw.at(p, c) - baseline;
      out.at(p, carrierSubBand[c], carrierGroup[c]) += residual;
    }
  }
  return out;
}

/// Argmax over tone groups per sub-band, with a soft-decision confidence.
void decide(const GroupEnergies& energies, std::size_t symbol, std::vector<int>& values,
            std::vector<float>& confidence) {
  values.resize(energies.subBands);
  confidence.resize(energies.subBands);
  for (int s = 0; s < energies.subBands; ++s) {
    float best = 0.0f, second = 0.0f;
    int bestG = 0;
    bool first = true, secondSet = false;
    for (int g = 0; g < energies.groups; ++g) {
      const float v = energies.at(symbol, s, g);
      if (first || v > best) {
        second = best;
        secondSet = !first;
        best = v;
        bestG = g;
        first = false;
      } else if (!secondSet || v > second) {
        second = v;
        secondSet = true;
      }
    }
    values[s] = bestG;
    const float span = best - second;
    const float scale = std::abs(best) + std::abs(second);
    confidence[s] = scale > 0.0f ? std::clamp(span / scale, 0.0f, 1.0f) : 0.0f;
  }
}

/// Which carriers hold tone energy in each symbol. A group value of -1 marks a
/// symbol whose data is not yet known, so all of its carriers are treated as lit
/// and drop out of every baseline estimate.
std::vector<char> buildLitMask(const std::vector<int>& groupsPerSymbol, int subBands,
                              const std::vector<int>& carrierSubBand,
                              const std::vector<int>& carrierGroup) {
  const std::size_t symbols = groupsPerSymbol.size() / static_cast<std::size_t>(subBands);
  const std::size_t carriers = carrierSubBand.size();
  std::vector<char> mask(symbols * carriers, 1);
  for (std::size_t p = 0; p < symbols; ++p) {
    for (std::size_t c = 0; c < carriers; ++c) {
      const int g = groupsPerSymbol[p * static_cast<std::size_t>(subBands) +
                                   static_cast<std::size_t>(carrierSubBand[c])];
      mask[p * carriers + c] = (g < 0 || g == carrierGroup[c]) ? 1 : 0;
    }
  }
  return mask;
}

/// Sum the `repeat` physical symbols that carry one logical symbol. For
/// non-coherent detection this is maximal-ratio combining, and it is what buys
/// the noise margin.
GroupEnergies combineRepeats(const GroupEnergies& perSymbol, std::size_t preSymbols, int repeat,
                            std::size_t logicalCount) {
  GroupEnergies out;
  out.symbols = logicalCount;
  out.subBands = perSymbol.subBands;
  out.groups = perSymbol.groups;
  out.e.assign(logicalCount * static_cast<std::size_t>(perSymbol.subBands) *
                   static_cast<std::size_t>(perSymbol.groups),
               0.0f);
  const auto step = static_cast<std::size_t>(repeat);
  for (std::size_t l = 0; l < logicalCount; ++l) {
    for (std::size_t r = 0; r < step; ++r) {
      const std::size_t p = preSymbols + l * step + r;
      for (int s = 0; s < perSymbol.subBands; ++s) {
        for (int g = 0; g < perSymbol.groups; ++g) out.at(l, s, g) += perSymbol.at(p, s, g);
      }
    }
  }
  return out;
}

/// Argmax over tone groups for every symbol, flattened to [symbol][subBand].
std::vector<int> decideGroups(const GroupEnergies& energies) {
  std::vector<int> out(energies.symbols * static_cast<std::size_t>(energies.subBands));
  std::vector<int> values;
  std::vector<float> conf;
  for (std::size_t p = 0; p < energies.symbols; ++p) {
    decide(energies, p, values, conf);
    for (int s = 0; s < energies.subBands; ++s) {
      out[p * static_cast<std::size_t>(energies.subBands) + static_cast<std::size_t>(s)] = values[s];
    }
  }
  return out;
}

}  // namespace

SpreadModem::SpreadModem(SpreadConfig cfg)
    : cfg_(cfg),
      masking_(static_cast<std::size_t>(cfg.fftSize), cfg.sampleRate, cfg.maskingMarginDb,
               cfg.floorAmplitude, cfg.peakAmplitude),
      fft_(static_cast<std::size_t>(cfg.fftSize)) {
  if (cfg_.sampleRate <= 0) throw std::invalid_argument("sampleRate must be positive");
  if (cfg_.cyclicPrefix <= 0 || cfg_.cyclicPrefix >= cfg_.fftSize) {
    throw std::invalid_argument("cyclicPrefix must be in (0, fftSize)");
  }
  if (cfg_.repeat <= 0) throw std::invalid_argument("repeat must be positive");
  if (cfg_.preambleSymbols <= 0) throw std::invalid_argument("preambleSymbols must be positive");
  if (cfg_.subBands <= 0 || cfg_.carriersPerGroup <= 0) {
    throw std::invalid_argument("subBands and carriersPerGroup must be positive");
  }
  if (!(cfg_.detectFloorRatio >= 0.0f)) {
    throw std::invalid_argument("detectFloorRatio must not be negative");
  }
  if (!(cfg_.peakBudget > 0.0f) || !(cfg_.peakAmplitude > 0.0f)) {
    throw std::invalid_argument("peakBudget and peakAmplitude must be positive");
  }

  bitsPerGroup_ = log2Int(cfg_.groups);
  bitsPerSymbol_ = cfg_.subBands * bitsPerGroup_;
  totalCarriers_ = cfg_.subBands * cfg_.groups * cfg_.carriersPerGroup;
  symbolPeriod_ =
      static_cast<std::size_t>(cfg_.fftSize) + static_cast<std::size_t>(cfg_.cyclicPrefix);

  const float binHz = masking_.binHz();
  const int maxBin = cfg_.fftSize / 2 - 1;
  const int binLow = std::max(1, static_cast<int>(std::ceil(cfg_.lowHz / binHz)));
  const int binHigh =
      std::min(static_cast<int>(std::floor(cfg_.highHz / binHz)), maxBin);

  carrierBins_ = chooseBins(binLow, binHigh, totalCarriers_, cfg_.minBinGap);
  if (carrierBins_.back() > binHigh) {
    throw std::invalid_argument(
        "carrier band too narrow: widen lowHz/highHz, raise fftSize, or lower "
        "subBands * groups * carriersPerGroup");
  }

  carrierSubBand_.resize(totalCarriers_);
  carrierGroup_.resize(totalCarriers_);
  for (int c = 0; c < totalCarriers_; ++c) {
    // The carriersPerGroup carriers that compete in one decision are adjacent in
    // frequency, so the host audio under them is essentially the same and
    // largely cancels when they are compared. Spreading a decision's arms apart
    // would make the host level at each arm unrelated, and then the whole
    // decision rests on how well the host can be estimated and subtracted.
    // Sub-bands still interleave across the band, so one masked-out region costs
    // a single sub-band rather than the whole symbol.
    const int group = c / cfg_.carriersPerGroup;
    carrierSubBand_[c] = group % cfg_.subBands;
    carrierGroup_[c] = group / cfg_.subBands;
  }
  masking_.setCarrierBins(carrierBins_);

  // Bins within +-kProbeSpan of each carrier that no tone ever occupies. Tones
  // are exactly bin-centred and the symbol body is exactly one FFT long, so
  // those bins contain host audio and nothing else.
  // Bins within +-kProbeSpan of each carrier that no tone ever occupies. Tones
  // are exactly bin-centred and the analysis block is exactly one transform long,
  // so those bins hold host audio and nothing else.
  constexpr int kProbeSpan = 4;
  std::vector<char> isCarrier(static_cast<std::size_t>(cfg_.fftSize), 0);
  for (int bin : carrierBins_) isCarrier[static_cast<std::size_t>(bin)] = 1;
  probeBins_.resize(totalCarriers_);
  for (int c = 0; c < totalCarriers_; ++c) {
    for (int d = 1; d <= kProbeSpan && probeBins_[c].size() < 4; ++d) {
      for (int bin : {carrierBins_[c] - d, carrierBins_[c] + d}) {
        if (bin < 1 || bin > maxBin) continue;
        if (isCarrier[static_cast<std::size_t>(bin)]) continue;
        probeBins_[c].push_back(bin);
        if (probeBins_[c].size() >= 4) break;
      }
    }
  }
}

std::size_t SpreadModem::preambleBitCount() const noexcept {
  return static_cast<std::size_t>(cfg_.preambleSymbols) * static_cast<std::size_t>(bitsPerSymbol_);
}

double SpreadModem::netBitRate() const noexcept {
  const double symbolRate =
      static_cast<double>(cfg_.sampleRate) / static_cast<double>(symbolPeriod_);
  return symbolRate * static_cast<double>(bitsPerSymbol_) / static_cast<double>(cfg_.repeat);
}

std::size_t SpreadModem::samplesForBits(std::size_t bitCount) const noexcept {
  const auto bits = static_cast<std::size_t>(bitsPerSymbol_);
  const std::size_t logical = bitCount == 0 ? 0 : (bitCount + bits - 1) / bits;
  const std::size_t physical = static_cast<std::size_t>(cfg_.preambleSymbols) +
                               logical * static_cast<std::size_t>(cfg_.repeat);
  return physical * symbolPeriod_;
}

std::vector<std::uint8_t> SpreadModem::preambleBits() const {
  Lfsr lfsr(0xACE1u);
  return lfsr.bits(preambleBitCount());
}

void SpreadModem::symbolGroups(const std::vector<std::uint8_t>& bits, std::size_t bitOffset,
                               std::vector<int>& groups) const {
  groups.resize(cfg_.subBands);
  for (int s = 0; s < cfg_.subBands; ++s) {
    int v = 0;
    for (int b = 0; b < bitsPerGroup_; ++b) {
      const std::size_t idx = bitOffset + static_cast<std::size_t>(s * bitsPerGroup_ + b);
      const int bit = idx < bits.size() ? (bits[idx] & 1) : 0;
      v = (v << 1) | bit;
    }
    groups[s] = v;
  }
}

Audio SpreadModem::preambleReference() const {
  const int n = cfg_.fftSize;
  const int cp = cfg_.cyclicPrefix;
  const auto pre = preambleBits();

  Audio out(static_cast<std::size_t>(cfg_.preambleSymbols) * symbolPeriod_, 0.0f);
  std::vector<Complex> x(static_cast<std::size_t>(n));
  const std::vector<float> amp(totalCarriers_, 1.0f);
  std::vector<int> groups;

  for (int p = 0; p < cfg_.preambleSymbols; ++p) {
    symbolGroups(pre, static_cast<std::size_t>(p) * bitsPerSymbol_, groups);
    buildSymbolSpectrum(groups, carrierBins_, carrierSubBand_, carrierGroup_, amp, totalCarriers_, p,
                        x);
    fft_.inverse(x);
    const std::size_t base = static_cast<std::size_t>(p) * symbolPeriod_;
    for (int i = 0; i < cp; ++i) out[base + i] = x[n - cp + i].real();
    for (int i = 0; i < n; ++i) out[base + cp + i] = x[i].real();
  }
  return out;
}

Audio SpreadModem::embed(const std::vector<std::uint8_t>& bits, const Audio& host,
                         std::size_t offset) const {
  const int n = cfg_.fftSize;
  const int cp = cfg_.cyclicPrefix;
  const std::size_t binCount = static_cast<std::size_t>(n) / 2 + 1;

  const auto bitsPerSym = static_cast<std::size_t>(bitsPerSymbol_);
  const std::size_t logicalSymbols = bits.empty() ? 0 : (bits.size() + bitsPerSym - 1) / bitsPerSym;
  const std::size_t physicalSymbols = static_cast<std::size_t>(cfg_.preambleSymbols) +
                                      logicalSymbols * static_cast<std::size_t>(cfg_.repeat);

  Audio out = host;
  const std::size_t needed = offset + physicalSymbols * symbolPeriod_;
  if (out.size() < needed) out.resize(needed, 0.0f);

  const auto pre = preambleBits();
  std::vector<Complex> h(static_cast<std::size_t>(n));
  std::vector<Complex> x(static_cast<std::size_t>(n));
  std::vector<float> mags(binCount);
  std::vector<float> amp(totalCarriers_);
  std::vector<int> groups;

  for (std::size_t p = 0; p < physicalSymbols; ++p) {
    const bool inPreamble = p < static_cast<std::size_t>(cfg_.preambleSymbols);
    const std::size_t bitOffset = inPreamble ? p * bitsPerSym
                                             : ((p - static_cast<std::size_t>(cfg_.preambleSymbols)) /
                                                static_cast<std::size_t>(cfg_.repeat)) *
                                                   bitsPerSym;
    symbolGroups(inPreamble ? pre : bits, bitOffset, groups);

    // Masking is estimated from the host audio overlapping this symbol, so the
    // tone level tracks the music instead of being fixed for the whole track.
    // The analysis window has to be the symbol body, not the prefix in front of
    // it: that is the exact span the receiver will FFT, and both the masking
    // threshold and the detection floor below are per-bin quantities.
    const std::size_t wStart = offset + p * symbolPeriod_;
    const std::size_t bodyStart = wStart + static_cast<std::size_t>(cp);
    std::fill(h.begin(), h.end(), Complex(0.0f, 0.0f));
    for (int i = 0; i < n; ++i) {
      const std::size_t idx = bodyStart + static_cast<std::size_t>(i);
      h[i] = Complex(idx < host.size() ? host[idx] : 0.0f, 0.0f);
    }
    fft_.forward(h);
    for (std::size_t k = 0; k < binCount; ++k) {
      mags[k] = 2.0f * std::abs(h[k]) / static_cast<float>(n);
    }
    masking_.carrierAmplitudes(mags, amp);

    // Masking says how loud a tone may be before it is heard; detection says how
    // loud it must be before it can be found. Where a host partial sits exactly
    // on a carrier bin, the second constraint binds. Spending the extra energy
    // there is the cheapest option: it lands on top of an existing partial and
    // changes its level instead of adding a new tone to the spectrum.
    if (cfg_.detectFloorRatio > 0.0f) {
      for (int c = 0; c < totalCarriers_; ++c) {
        const float host = mags[static_cast<std::size_t>(carrierBins_[c])];
        amp[c] = std::max(amp[c], cfg_.detectFloorRatio * host);
      }
    }

    buildSymbolSpectrum(groups, carrierBins_, carrierSubBand_, carrierGroup_, amp, totalCarriers_,
                        static_cast<int>(p), x);
    fft_.inverse(x);

    // Many carriers can add in phase and exceed full scale. Rescaling the whole
    // symbol is always safe: quieter than the masking threshold is still
    // inaudible.
    float peak = 0.0f;
    for (int i = 0; i < n; ++i) peak = std::max(peak, std::abs(x[i].real()));
    const float scale =
        peak > cfg_.peakBudget && peak > 0.0f ? cfg_.peakBudget / peak : 1.0f;

    for (int i = 0; i < cp; ++i) out[wStart + i] += scale * x[n - cp + i].real();
    for (int i = 0; i < n; ++i) out[wStart + cp + i] += scale * x[i].real();
  }
  return out;
}

Modem::Extract SpreadModem::extract(const Audio& rx) const {
  Extract result;
  const int n = cfg_.fftSize;
  const int cp = cfg_.cyclicPrefix;
  const std::size_t preSymbols = static_cast<std::size_t>(cfg_.preambleSymbols);
  const std::size_t subBands = static_cast<std::size_t>(cfg_.subBands);
  const std::size_t window = static_cast<std::size_t>(std::max(0, cfg_.baselineWindow));
  const auto expected = preambleBits();

  // The preamble is known to the receiver, so the carriers it lights are known
  // too and can be excluded from the baseline right away.
  std::vector<int> preGroups(preSymbols * subBands);
  {
    std::vector<int> g;
    for (std::size_t p = 0; p < preSymbols; ++p) {
      symbolGroups(expected, p * static_cast<std::size_t>(bitsPerSymbol_), g);
      for (std::size_t s = 0; s < subBands; ++s) preGroups[p * subBands + s] = g[s];
    }
  }

  const Audio ref = preambleReference();
  const auto candidates = correlateTopOffsets(rx, ref, 6, static_cast<std::ptrdiff_t>(symbolPeriod_));
  if (candidates.empty()) return result;

  // Rank candidate sync points by how well the preamble decodes at each.
  std::ptrdiff_t bestStart = -1;
  double bestAgreement = -1.0;
  for (const auto& [score, lag] : candidates) {
    (void)score;
    if (lag < 0 || static_cast<std::size_t>(lag) + preSymbols * symbolPeriod_ > rx.size()) continue;

    // Look past the preamble so its first symbols have a two-sided window.
    const std::size_t available = (rx.size() - static_cast<std::size_t>(lag)) / symbolPeriod_;
    const std::size_t span = std::min(available, preSymbols + 2 * window);
    if (span < preSymbols) continue;

    const auto raw = collectCarrierEnergies(rx, lag, span, symbolPeriod_, n, cp, cfg_.timingSearch,
                                           carrierBins_, probeBins_, fft_);
    std::vector<int> groups(span * subBands, -1);
    std::copy(preGroups.begin(), preGroups.end(), groups.begin());
    const auto mask = buildLitMask(groups, cfg_.subBands, carrierSubBand_, carrierGroup_);
    const auto energies = subtractHostBaseline(raw, carrierSubBand_, carrierGroup_, cfg_.subBands,
                                               cfg_.groups, cfg_.baselineWindow, &mask);

    const auto decided = decideGroups(energies);
    std::size_t matched = 0;
    for (std::size_t p = 0; p < preSymbols; ++p) {
      for (std::size_t s = 0; s < subBands; ++s) {
        int v = 0;
        for (int b = 0; b < bitsPerGroup_; ++b) {
          const std::size_t idx = p * static_cast<std::size_t>(bitsPerSymbol_) +
                                  s * static_cast<std::size_t>(bitsPerGroup_) +
                                  static_cast<std::size_t>(b);
          v = (v << 1) | (idx < expected.size() ? expected[idx] : 0);
        }
        if (v == decided[p * subBands + s]) matched += static_cast<std::size_t>(bitsPerGroup_);
      }
    }
    const double agreement = static_cast<double>(matched) / static_cast<double>(preambleBitCount());
    if (agreement > bestAgreement) {
      bestAgreement = agreement;
      bestStart = lag;
    }
  }

  if (bestStart < 0 || bestAgreement < 0.5) return result;

  const std::size_t availSymbols =
      (rx.size() - static_cast<std::size_t>(bestStart)) / symbolPeriod_;
  result.synced = true;
  result.syncSample = bestStart;
  result.syncScore = bestAgreement;
  if (availSymbols <= preSymbols) return result;

  const auto raw = collectCarrierEnergies(rx, bestStart, availSymbols, symbolPeriod_, n, cp,
                                          cfg_.timingSearch, carrierBins_, probeBins_, fft_);
  const std::size_t logicalCount =
      (availSymbols - preSymbols) / static_cast<std::size_t>(cfg_.repeat);
  if (logicalCount == 0) return result;

  std::vector<int> groups(availSymbols * subBands, -1);
  std::copy(preGroups.begin(), preGroups.end(), groups.begin());

  // Two passes. The first has nothing but the preamble to go on, so its host
  // estimate is contaminated by every data symbol that lights a carrier. The
  // second feeds the first pass's decisions back in, which lets the estimate use
  // only symbols where the carrier is genuinely dark. That is the difference
  // between a biased and an unbiased baseline, and it is what makes detection
  // work when the tone sits below a strong host partial.
  GroupEnergies combined;
  for (int pass = 0; pass < 2; ++pass) {
    const auto mask = buildLitMask(groups, cfg_.subBands, carrierSubBand_, carrierGroup_);
    const auto energies = subtractHostBaseline(raw, carrierSubBand_, carrierGroup_, cfg_.subBands,
                                               cfg_.groups, cfg_.baselineWindow, &mask);
    combined = combineRepeats(energies, preSymbols, cfg_.repeat, logicalCount);
    if (pass == 0) {
      const auto decided = decideGroups(combined);
      for (std::size_t l = 0; l < logicalCount; ++l) {
        for (int r = 0; r < cfg_.repeat; ++r) {
          const std::size_t p = preSymbols + l * static_cast<std::size_t>(cfg_.repeat) +
                                static_cast<std::size_t>(r);
          for (std::size_t s = 0; s < subBands; ++s) {
            groups[p * subBands + s] = decided[l * subBands + s];
          }
        }
      }
      // Preamble agreement is now measurable against an unbiased baseline.
      std::size_t matched = 0;
      const auto preDecided = decideGroups(energies);
      for (std::size_t p = 0; p < preSymbols; ++p) {
        for (std::size_t s = 0; s < subBands; ++s) {
          if (preDecided[p * subBands + s] == preGroups[p * subBands + s]) {
            matched += static_cast<std::size_t>(bitsPerGroup_);
          }
        }
      }
      result.syncScore = static_cast<double>(matched) / static_cast<double>(preambleBitCount());
    }
  }

  std::vector<int> values;
  std::vector<float> conf;
  for (std::size_t l = 0; l < logicalCount; ++l) {
    decide(combined, l, values, conf);
    for (std::size_t s = 0; s < subBands; ++s) {
      for (int b = bitsPerGroup_ - 1; b >= 0; --b) {
        result.bits.push_back(static_cast<std::uint8_t>((values[s] >> b) & 1));
        result.confidence.push_back(conf[s]);
      }
    }
  }
  return result;
}

}  // namespace vacuum
