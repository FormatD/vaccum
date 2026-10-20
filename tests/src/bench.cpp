// End-to-end and bit-level benchmarks under simulated channels.
//
// The point of this tool is to replace guesses with measurements: every claim
// about robustness in the README comes from a row in one of these tables.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "audio_gen.h"
#include "vacuum/physical.h"
#include "vacuum/rs.h"
#include "vacuum/ss_modem.h"
#include "vacuum/transmit.h"
#include "vacuum_io/audio_file.h"

using namespace vacuum;

namespace {

struct Options {
  std::string mode = "matrix";
  std::size_t payload = 32;
  int trials = 3;
  std::string host = "music";
  int repeat = 0;          // 0 = keep the default
  int baselineWindow = -1;  // -1 = keep the default
  int timingSearch = -1;
  float marginDb = 0.0f;   // 0 = keep the default
  float detectFloor = -1.0f;
  float lowHz = -1.0f;
  float highHz = -1.0f;
  int fragmentBytes = 0;  // 0 = let planFrames choose
  int parity = 0;         // 0 = keep the default
  bool softErasure = true;
};

Options parseArgs(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s needs a value\n", a.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--mode") o.mode = next();
    else if (a == "--payload") o.payload = std::strtoul(next().c_str(), nullptr, 10);
    else if (a == "--trials") o.trials = std::atoi(next().c_str());
    else if (a == "--host") o.host = next();
    else if (a == "--repeat") o.repeat = std::atoi(next().c_str());
    else if (a == "--baseline-window") o.baselineWindow = std::atoi(next().c_str());
    else if (a == "--timing-search") o.timingSearch = std::atoi(next().c_str());
    else if (a == "--margin") o.marginDb = std::atof(next().c_str());
    else if (a == "--detect-floor") o.detectFloor = static_cast<float>(std::atof(next().c_str()));
    else if (a == "--low-hz") o.lowHz = static_cast<float>(std::atof(next().c_str()));
    else if (a == "--high-hz") o.highHz = static_cast<float>(std::atof(next().c_str()));
    else if (a == "--fragment-size") o.fragmentBytes = std::atoi(next().c_str());
    else if (a == "--parity") o.parity = std::atoi(next().c_str());
    else if (a == "--no-soft-erasure") o.softErasure = false;
    else if (a == "--help" || a == "-h") {
      std::printf(
          "vacuum_bench --mode matrix|modem|fec|sweep\n"
          "  --payload BYTES         payload size per trial (default 32)\n"
          "  --trials N              trials per scenario (default 3)\n"
          "  --host music|silence    host audio to hide the watermark in\n"
          "  --repeat N              physical symbols per logical symbol\n"
          "  --baseline-window N     symbols each side used for host removal\n"
          "  --timing-search N       samples each side of nominal symbol start\n"
          "  --margin DB             masking safety margin\n"
          "  --detect-floor RATIO    tone amplitude floor as a multiple of the host bin\n"
          "  --low-hz HZ             bottom of the carrier band\n"
          "  --high-hz HZ            top of the carrier band\n"
          "  --fragment-size BYTES   cap the RS block payload at 8, 24, 64, 128 or 196\n"
          "  --parity N              Reed-Solomon parity symbols per block\n"
          "  --no-soft-erasure       disable the confidence-ranked erasure retry\n");
      std::exit(0);
    } else {
      std::fprintf(stderr, "unknown option %s\n", a.c_str());
      std::exit(2);
    }
  }
  return o;
}

SpreadConfig makeConfig(const Options& o) {
  SpreadConfig c;
  if (o.repeat > 0) c.repeat = o.repeat;
  if (o.baselineWindow >= 0) c.baselineWindow = o.baselineWindow;
  if (o.timingSearch >= 0) c.timingSearch = o.timingSearch;
  if (o.marginDb > 0.0f) c.maskingMarginDb = o.marginDb;
  if (o.detectFloor >= 0.0f) c.detectFloorRatio = o.detectFloor;
  if (o.lowHz > 0.0f) c.lowHz = o.lowHz;
  if (o.highHz > 0.0f) c.highHz = o.highHz;
  return c;
}

FragmentSize fragmentSizeFor(int bytes) {
  for (std::uint8_t code = 0; code <= static_cast<std::uint8_t>(FragmentSize::Bytes196); ++code) {
    const auto size = static_cast<FragmentSize>(code);
    if (static_cast<int>(fragmentPayloadSize(size)) >= bytes) return size;
  }
  return FragmentSize::Bytes196;
}

TransmitConfig makeTransmitConfig(const Options& o) {
  TransmitConfig t;
  if (o.fragmentBytes > 0) t.maxFragmentSize = fragmentSizeFor(o.fragmentBytes);
  if (o.parity > 0) t.rsParitySymbols = o.parity;
  t.softErasureRetry = o.softErasure;
  return t;
}

Bytes makePayload(std::size_t len, unsigned seed) {
  Bytes p(len);
  std::uint32_t s = seed * 2654435761u + 1u;
  for (std::size_t i = 0; i < len; ++i) {
    s = s * 1103515245u + 12345u;
    p[i] = static_cast<std::uint8_t>((s >> 16) & 0xFF);
  }
  return p;
}

/// Round trip through a real MP3 encoder and decoder, which is the impairment
/// that actually matters for this project's stated goal.
bool mp3RoundTrip(Audio& audio, int sampleRate, int kbps, std::string& error) {
  namespace fs = std::filesystem;
  const auto dir = fs::temp_directory_path();
  std::random_device rd;
  const auto stem = dir / ("vacuum_bench_" + std::to_string(rd()));
  const std::string wavPath = (stem.string() + ".wav");
  const std::string mp3Path = (stem.string() + ".mp3");

  if (!io::writeWav(wavPath, audio, sampleRate, io::WavFormat::Int16, error)) return false;
  if (!io::writeMp3(mp3Path, audio, sampleRate, kbps, error)) {
    std::remove(wavPath.c_str());
    return false;
  }
  Audio decoded;
  int rate = 0;
  if (!io::readAudioFile(mp3Path, decoded, rate, error)) {
    std::remove(wavPath.c_str());
    std::remove(mp3Path.c_str());
    return false;
  }
  std::remove(wavPath.c_str());
  std::remove(mp3Path.c_str());

  if (rate != sampleRate) {
    error = "MP3 decoder returned " + std::to_string(rate) + " Hz, expected " +
            std::to_string(sampleRate);
    return false;
  }
  audio = std::move(decoded);
  return true;
}

struct Scenario {
  std::string name;
  double snrDb = 1e9;      // effectively infinite = no noise
  double lowpassHz = 0.0;  // 0 = off
  int mp3Kbps = 0;         // 0 = off
  double delaySamples = 0.0;
};

Audio applyChannel(const Audio& tx, const Scenario& s, int sampleRate, unsigned seed,
                   std::string& error) {
  Audio a = tx;
  if (s.delaySamples != 0.0) a = testutil::fractionalDelay(a, s.delaySamples);
  if (s.lowpassHz > 0.0) a = testutil::lowpass(a, sampleRate, s.lowpassHz);
  if (s.mp3Kbps > 0 && !mp3RoundTrip(a, sampleRate, s.mp3Kbps, error)) return {};
  if (s.snrDb < 100.0) a = testutil::addNoise(a, s.snrDb, seed);
  error.clear();
  return a;
}

std::size_t countBitErrors(const std::vector<std::uint8_t>& sent,
                           const std::vector<std::uint8_t>& got) {
  const std::size_t n = std::min(sent.size(), got.size());
  std::size_t e = sent.size() - n;
  for (std::size_t i = 0; i < n; ++i) {
    if (sent[i] != got[i]) ++e;
  }
  return e;
}

double rms(const Audio& a) {
  double s = 0;
  for (float v : a) s += static_cast<double>(v) * v;
  return std::sqrt(s / static_cast<double>(std::max<std::size_t>(a.size(), 1)));
}

/// How loud the watermark is, in the only sense that means anything per host.
/// Against a silent host a ratio is meaningless -- there is no level to hide
/// under -- so report the absolute figure, which is what `floorAmplitude` sets.
std::string watermarkLevel(const Audio& watermark, const Audio& host) {
  char buf[96];
  const double wm = rms(watermark);
  const double hs = rms(host);
  if (hs < 1e-6) {
    std::snprintf(buf, sizeof buf, "%.1f dBFS", 20.0 * std::log10(std::max(1e-12, wm)));
  } else {
    std::snprintf(buf, sizeof buf, "%.1f dB below host",
                  20.0 * std::log10(std::max(1e-12, wm) / hs));
  }
  return buf;
}

Audio makeHost(const Options& o, double seconds, int sampleRate, unsigned seed) {
  if (o.host == "silence") {
    return Audio(static_cast<std::size_t>(seconds * sampleRate), 0.0f);
  }
  return testutil::synthHost(seconds, sampleRate, seed);
}

/// The host has to cover the whole watermark. `embed` zero-pads a short host,
/// and a message whose tail rides on silence decodes far more easily than one
/// buried in music for its entire length, which would flatter every result
/// below. Two seconds of slack covers the preamble and the channel delay.
double hostSeconds(const Options& o, const SpreadModem& modem) {
  return estimateSeconds(o.payload, modem, makeTransmitConfig(o)) + 2.0;
}

struct Row {
  std::string label;
  int trials = 0;
  int messageOk = 0;
  double berSum = 0.0;
  double rsSum = 0.0;
  double confSum = 0.0;
  double secondsSum = 0.0;
};

void printRow(const Row& r) {
  const double n = std::max(1, r.trials);
  std::printf(
      "%-34s %3d/%-3d  BER %8.2e  rs_corrected %5.1f  conf %.3f  audio %5.2fs\n", r.label.c_str(),
      r.messageOk, r.trials, r.berSum / n, r.rsSum / n, r.confSum / n, r.secondsSum / n);
}

/// Run `trials` end-to-end messages through one scenario. `t` is a parameter
/// rather than derived from `o` so that a sweep can vary the coding knobs too.
Row runScenario(const std::string& label, const Scenario& scenario, const Options& o,
                const SpreadConfig& cfg, const TransmitConfig& t) {
  const SpreadModem modem(cfg);
  Row row;
  row.label = label;
  row.trials = o.trials;

  for (int trial = 0; trial < o.trials; ++trial) {
    const Bytes payload = makePayload(o.payload, static_cast<unsigned>(trial) + 1);
    const Audio host = makeHost(o, hostSeconds(o, modem), modem.sampleRate(), static_cast<unsigned>(trial) + 100);

    EncodeResult enc;
    std::string error;
    if (!encodeMessage(payload, host, modem, t, enc, error)) {
      std::printf("    encode failed: %s\n", error.c_str());
      continue;
    }
    row.secondsSum += enc.seconds;

    const Audio rx = applyChannel(enc.audio, scenario, modem.sampleRate(),
                                  static_cast<unsigned>(trial) + 7, error);
    if (rx.empty()) {
      std::printf("    channel failed: %s\n", error.c_str());
      continue;
    }

    const auto extraction = modem.extract(rx);
    const std::size_t errors =
        extraction.synced ? countBitErrors(enc.txBits, extraction.bits) : enc.txBits.size();
    row.berSum += static_cast<double>(errors) /
                  static_cast<double>(std::max<std::size_t>(enc.txBits.size(), 1));

    const auto dec = decodeMessage(rx, modem, t);
    if (dec.ok && dec.payload == payload) ++row.messageOk;
    row.rsSum += dec.rsCorrectedSymbols;
    row.confSum += dec.meanBitConfidence;

    if (trial == 0) {
      // Only the span where the host actually exists: a long payload makes the
      // watermark outgrow the synthetic host, which `embed` zero-pads, and the
      // padding is not part of either signal.
      const std::size_t n = std::min({rx.size(), enc.audio.size(), host.size()});
      Audio watermark(n);
      for (std::size_t i = 0; i < n; ++i) watermark[i] = enc.audio[i] - host[i];
      std::printf("    payload %zu B, %zu fragments, %.2f s audio, %.0f bps delivered, watermark %s\n",
                  o.payload, enc.fragmentCount, enc.seconds, enc.payloadBitRate,
                  watermarkLevel(watermark, host).c_str());
    }
  }
  return row;
}

/// Block-level accounting for the soft erasure retry. Message success in the
/// matrix is all-or-nothing per fragment, so it hides whether the retry is
/// doing any work; this counts the blocks it actually rescues.
void runFecReport(const Options& o, const SpreadConfig& cfg) {
  const SpreadModem modem(cfg);
  TransmitConfig t = makeTransmitConfig(o);
  // A fixed stream id so the expected blocks can be rebuilt and each decode
  // checked for correctness. Reed-Solomon can return a valid codeword that is
  // not the one that was sent once the error load exceeds its capacity, and
  // counting those as successes would overstate the retry.
  t.streamId = 0x5A5A;
  const ReedSolomon rs(t.rsParitySymbols);

  const std::vector<Scenario> scenarios = {
      {"clean file loopback", 1e9, 0, 0, 3333.3},
      {"air delay + lowpass 11k", 1e9, 11000, 0, 3333.3},
      {"+ noise 18 dB", 18, 11000, 0, 3333.3},
      {"+ lowpass 8 kHz", 18, 8000, 0, 3333.3},
      {"mp3 128 kbps", 1e9, 0, 128, 3333.3},
  };

  // Erasure counts to compare, as fractions of the parity budget. Flagging k
  // of the least confident symbols leaves room for (parity - k) / 2 further
  // errors, so this is a trade rather than "as many as possible".
  const int parity = t.rsParitySymbols;
  const std::vector<int> steps = {0, parity / 4, parity / 2, (3 * parity) / 4, parity};

  std::printf("%-26s %4s %7s %8s %8s %8s  %8s\n", "scenario", "k", "blocks", "correct",
              "miscorr", "failed", "damaged/blk");

  for (const auto& s : scenarios) {
    std::size_t blocks = 0, damaged = 0;
    std::vector<std::size_t> correct(steps.size(), 0), miscorr(steps.size(), 0),
        failed(steps.size(), 0);
    std::size_t cascadeOk = 0, cascadeRescued = 0, cascadeMiscorr = 0, cascadeFailed = 0;

    for (int trial = 0; trial < o.trials; ++trial) {
      const Bytes payload = makePayload(o.payload, static_cast<unsigned>(trial) + 1);
      const Audio host = makeHost(o, hostSeconds(o, modem), modem.sampleRate(), static_cast<unsigned>(trial) + 100);
      EncodeResult enc;
      std::string error;
      if (!encodeMessage(payload, host, modem, t, enc, error)) continue;
      const Audio rx = applyChannel(enc.audio, s, modem.sampleRate(),
                                    static_cast<unsigned>(trial) + 7, error);
      if (rx.empty()) continue;

      const auto ex = modem.extract(rx);
      if (!ex.synced) continue;

      const auto plan = planFrames(payload.size(), t.rsParitySymbols, t.maxFragmentSize);
      if (!plan) continue;
      const auto expected = packetize(payload, t.streamId, *plan, t.rsParitySymbols);

      const std::size_t headerBits = static_cast<std::size_t>(t.physicalHeaderRepeat) * 32;
      if (ex.bits.size() <= headerBits) continue;
      Bytes coded;
      bitsToBytes({ex.bits.begin() + static_cast<std::ptrdiff_t>(headerBits), ex.bits.end()}, coded);

      std::vector<float> conf;
      conf.reserve(ex.confidence.size() / 8);
      for (std::size_t i = 0; i + 8 <= ex.confidence.size(); i += 8) {
        float weakest = ex.confidence[i];
        for (std::size_t k = 1; k < 8; ++k) weakest = std::min(weakest, ex.confidence[i + k]);
        conf.push_back(weakest);
      }

      const std::size_t blockBytes = plan->blockBytes;
      const std::size_t n = std::min(coded.size() / blockBytes, plan->fragmentCount);
      const std::size_t confBase = static_cast<std::size_t>(t.physicalHeaderRepeat) * 4;

      for (std::size_t b = 0; b < n; ++b) {
        const std::size_t off = b * blockBytes;
        const Bytes block(coded.begin() + static_cast<std::ptrdiff_t>(off),
                          coded.begin() + static_cast<std::ptrdiff_t>(off + blockBytes));
        ++blocks;

        std::size_t bad = 0;
        for (std::size_t j = 0; j < blockBytes; ++j) {
          const std::size_t bit = headerBits + (off + j) * 8;
          for (std::size_t k = 0; k < 8; ++k) {
            if (bit + k >= enc.txBits.size() || bit + k >= ex.bits.size() ||
                ex.bits[bit + k] != enc.txBits[bit + k]) {
              ++bad;
              break;
            }
          }
        }
        damaged += bad;

        // packetize returns RS-encoded blocks; decode returns the systematic
        // data part, which is the frame without its parity tail.
        const Bytes truth =
            b < expected.size()
                ? Bytes(expected[b].begin(),
                        expected[b].begin() + static_cast<std::ptrdiff_t>(blockBytes) -
                            t.rsParitySymbols)
                : Bytes{};
        auto isCorrect = [&](const ReedSolomon::DecodeResult& d) {
          return d.ok && d.data == truth;
        };

        // Least confident positions first, shared by every step.
        std::vector<int> order(blockBytes);
        std::iota(order.begin(), order.end(), 0);
        const std::size_t base = confBase + off;
        const bool haveConf = base + blockBytes <= conf.size();
        if (haveConf) {
          std::stable_sort(order.begin(), order.end(), [&](int x, int y) {
            return conf[base + static_cast<std::size_t>(x)] <
                   conf[base + static_cast<std::size_t>(y)];
          });
        }

        for (std::size_t si = 0; si < steps.size(); ++si) {
          std::vector<int> erasures;
          if (steps[si] > 0 && haveConf) {
            erasures.assign(order.begin(),
                            order.begin() + std::min<std::size_t>(steps[si], order.size()));
          }
          const auto d = rs.decode(block, erasures);
          if (isCorrect(d)) ++correct[si];
          else if (d.ok) ++miscorr[si];
          else ++failed[si];
        }

        // The policy transmit.cpp actually uses: plain decoding, and only on an
        // outright failure a retry with half the parity budget flagged.
        const auto plain = rs.decode(block);
        if (isCorrect(plain)) {
          ++cascadeOk;
        } else if (plain.ok) {
          ++cascadeMiscorr;
        } else if (haveConf) {
          std::vector<int> erasures(
              order.begin(), order.begin() + std::min<std::size_t>(parity / 2, order.size()));
          const auto soft = rs.decode(block, erasures);
          if (isCorrect(soft)) ++cascadeRescued;
          else if (soft.ok) ++cascadeMiscorr;
          else ++cascadeFailed;
        } else {
          ++cascadeFailed;
        }
      }
    }

    const double perBlock =
        blocks ? static_cast<double>(damaged) / static_cast<double>(blocks) : 0.0;
    for (std::size_t si = 0; si < steps.size(); ++si) {
      std::printf("%-26s %4d %7zu %8zu %8zu %8zu  %8.1f\n", si == 0 ? s.name.c_str() : "",
                  steps[si], blocks, correct[si], miscorr[si], failed[si], perBlock);
    }
    std::printf("%-26s %4s %7zu %8zu %8zu %8zu  %8.1f\n", "", "csc", blocks,
                cascadeOk + cascadeRescued, cascadeMiscorr, cascadeFailed, perBlock);
    std::printf("%-26s      (of which %zu rescued by the erasure retry)\n", "", cascadeRescued);
  }
}

}  // namespace

int run(int argc, char** argv) {
  const Options o = parseArgs(argc, argv);
  const SpreadConfig cfg = makeConfig(o);

  if (o.mode == "sweep") {
    std::printf("\n== parameter sweep, host=%s, payload=%zu B, %d trial(s) ==\n", o.host.c_str(),
                o.payload, o.trials);
    std::printf("baseline: %.0f-%.0f Hz repeat=%d detectFloor=%.1f parity=%d fragment<=%zu\n",
                cfg.lowHz, cfg.highHz, cfg.repeat, cfg.detectFloorRatio,
                makeTransmitConfig(o).rsParitySymbols,
                fragmentPayloadSize(makeTransmitConfig(o).maxFragmentSize));

    // Sweeping on a clean channel is useless: every setting passes. These two
    // bracket the range where the knobs actually discriminate -- the harder one
    // is past what the default coding can carry, so a setting that helps shows
    // up as recovered messages rather than as noise in the bit error rate.
    const Scenario air{"+ lowpass 8 kHz, noise 18 dB", 18, 8000, 0, 3333.3};
    const Scenario hard{"+ lowpass 6 kHz, noise 12 dB", 12, 6000, 0, 3333.3};
    const auto both = [&](const std::string& label, const SpreadConfig& c, const TransmitConfig& t) {
      printRow(runScenario(label + "  [8 kHz/18 dB]", air, o, c, t));
      printRow(runScenario(label + "  [6 kHz/12 dB]", hard, o, c, t));
    };
    const TransmitConfig base = makeTransmitConfig(o);

    std::printf("\n-- Reed-Solomon parity (the strongest knob) --\n");
    for (int p : {16, 32, 48, 64, 80, 96}) {
      TransmitConfig t = base;
      t.rsParitySymbols = p;
      both("parity=" + std::to_string(p), cfg, t);
    }

    std::printf("\n-- fragment size at parity %d --\n", base.rsParitySymbols);
    for (int f : {8, 24, 64, 128}) {
      TransmitConfig t = base;
      t.maxFragmentSize = fragmentSizeFor(f);
      // A block is fragment + 18 + parity and must fit in 255 bytes; planFrames
      // silently skips the combinations that do not, which would make the row
      // look like a fragment-size result when it is really a capacity limit.
      const auto plan = planFrames(o.payload, t.rsParitySymbols, t.maxFragmentSize);
      if (!plan) {
        std::printf("%-34s  no block fits parity %d\n", ("fragment=" + std::to_string(f)).c_str(),
                    t.rsParitySymbols);
        continue;
      }
      both("fragment=" + std::to_string(fragmentPayloadSize(t.maxFragmentSize)), cfg, t);
    }

    std::printf("\n-- soft erasure retry at parity %d --\n", base.rsParitySymbols);
    for (bool retry : {false, true}) {
      TransmitConfig t = base;
      t.softErasureRetry = retry;
      both(retry ? "erasureRetry=on" : "erasureRetry=off", cfg, t);
    }

    std::printf("\n-- detection floor (louder tone = more SNR, less hiding) --\n");
    for (float f : {2.0f, 4.0f, 6.0f, 8.0f, 10.0f, 14.0f}) {
      SpreadConfig c = cfg;
      c.detectFloorRatio = f;
      char label[32];
      std::snprintf(label, sizeof(label), "detectFloor=%.0f", f);
      both(label, c, base);
    }

    std::printf("\n-- carrier band top (earbuds and microphones roll off here) --\n");
    for (float hz : {6000.0f, 7500.0f, 8500.0f, 9400.0f, 11000.0f}) {
      SpreadConfig c = cfg;
      c.highHz = hz;
      char label[32];
      std::snprintf(label, sizeof(label), "highHz=%.0f", hz);
      try {
        both(label, c, base);
      } catch (const std::exception& e) {
        std::printf("%-34s  %s\n", label, e.what());
      }
    }

    std::printf("\n-- host baseline removal window --\n");
    for (int w : {0, 2, 4, 8, 16}) {
      SpreadConfig c = cfg;
      c.baselineWindow = w;
      both("baselineWindow=" + std::to_string(w), c, base);
    }

    std::printf("\n-- cyclic prefix timing tracker --\n");
    for (int s : {0, 32, 96, 192, 384}) {
      SpreadConfig c = cfg;
      c.timingSearch = s;
      both("timingSearch=" + std::to_string(s), c, base);
    }

    std::printf("\n-- time repetition --\n");
    for (int r : {1, 2, 3, 6}) {
      SpreadConfig c = cfg;
      c.repeat = r;
      both("repeat=" + std::to_string(r), c, base);
    }

    std::printf("\n-- masking margin --\n");
    for (float m : {0.0f, 3.0f, 6.0f, 12.0f, 20.0f}) {
      SpreadConfig c = cfg;
      c.maskingMarginDb = m;
      char label[32];
      std::snprintf(label, sizeof(label), "marginDb=%.0f", m);
      both(label, c, base);
    }
    return 0;
  }

  if (o.mode == "fec") {
    std::printf("\n== soft erasure retry, block level, host=%s, payload=%zu B, %d trial(s) ==\n",
                o.host.c_str(), o.payload, o.trials);
    runFecReport(o, cfg);
    return 0;
  }

  if (o.mode == "modem") {
    std::printf("\n== raw bit error rate, host=%s, %d trial(s) ==\n", o.host.c_str(), o.trials);
    const SpreadModem modem(cfg);
    std::printf("%-22s %10s %10s %10s\n", "channel", "sent bits", "errors", "BER");
    const std::vector<Scenario> scenarios = {
        {"clean", 1e9, 0, 0, 0},
        {"snr 30 dB", 30, 0, 0, 0},
        {"snr 20 dB", 20, 0, 0, 0},
        {"snr 10 dB", 10, 0, 0, 0},
        {"snr 5 dB", 5, 0, 0, 0},
        {"lowpass 10 kHz", 1e9, 10000, 0, 0},
        {"mp3 320 kbps", 1e9, 0, 320, 0},
        {"mp3 128 kbps", 1e9, 0, 128, 0},
        {"delay 4321.5", 1e9, 0, 0, 4321.5},
    };
    for (const auto& s : scenarios) {
      std::size_t totalErrors = 0, totalBits = 0;
      for (int trial = 0; trial < o.trials; ++trial) {
        const Bytes payload = makePayload(o.payload, static_cast<unsigned>(trial) + 1);
        const Audio host = makeHost(o, hostSeconds(o, modem), modem.sampleRate(), static_cast<unsigned>(trial) + 100);
        EncodeResult enc;
        std::string error;
        TransmitConfig t = makeTransmitConfig(o);
        if (!encodeMessage(payload, host, modem, t, enc, error)) continue;
        const Audio rx = applyChannel(enc.audio, s, modem.sampleRate(),
                                      static_cast<unsigned>(trial) + 7, error);
        if (rx.empty()) {
          std::printf("%-22s  channel error: %s\n", s.name.c_str(), error.c_str());
          break;
        }
        const auto ex = modem.extract(rx);
        totalBits += enc.txBits.size();
        totalErrors += ex.synced ? countBitErrors(enc.txBits, ex.bits) : enc.txBits.size();
      }
      std::printf("%-22s %10zu %10zu %10.3e\n", s.name.c_str(), totalBits, totalErrors,
                  totalBits ? static_cast<double>(totalErrors) / static_cast<double>(totalBits)
                            : 0.0);
    }
    return 0;
  }

  // matrix: the headline result. Message success across the impairments that
  // the real headphone -> air -> microphone -> MP3 path produces.
  std::printf("\n== message success matrix, host=%s, payload=%zu B, %d trial(s) ==\n",
              o.host.c_str(), o.payload, o.trials);
  std::printf("config: %.0f-%.0f Hz fftSize=%d cp=%d repeat=%d baselineWindow=%d "
              "timingSearch=%d margin=%.1fdB detectFloor=%.1f\n",
              cfg.lowHz, cfg.highHz, cfg.fftSize, cfg.cyclicPrefix, cfg.repeat,
              cfg.baselineWindow, cfg.timingSearch, cfg.maskingMarginDb, cfg.detectFloorRatio);
  const TransmitConfig tx = makeTransmitConfig(o);
  std::printf("coding: parity=%d fragment<=%zu erasureRetry=%s\n", tx.rsParitySymbols,
              fragmentPayloadSize(tx.maxFragmentSize), tx.softErasureRetry ? "on" : "off");

  const auto start = std::chrono::steady_clock::now();
  const std::vector<Scenario> scenarios = {
      {"clean file loopback", 1e9, 0, 0, 3333.3},
      {"air delay + lowpass 11k", 1e9, 11000, 0, 3333.3},
      {"+ noise 25 dB", 25, 11000, 0, 3333.3},
      {"+ noise 18 dB", 18, 11000, 0, 3333.3},
      {"+ noise 12 dB", 12, 11000, 0, 3333.3},
      {"+ lowpass 8 kHz", 18, 8000, 0, 3333.3},
      {"+ lowpass 6 kHz", 18, 6000, 0, 3333.3},
      {"+ lowpass 5 kHz", 18, 5000, 0, 3333.3},
      {"+ lowpass 4 kHz", 18, 4000, 0, 3333.3},
      {"mp3 320 kbps", 1e9, 0, 320, 3333.3},
      {"mp3 192 kbps", 1e9, 0, 192, 3333.3},
      {"mp3 128 kbps", 1e9, 0, 128, 3333.3},
      {"mp3 320k + noise 18 dB", 18, 0, 320, 3333.3},
      {"mp3 128k + noise 18 dB", 18, 0, 128, 3333.3},
      {"mp3 128k + lp 8k + 18 dB", 18, 8000, 128, 3333.3},
  };

  for (const auto& s : scenarios) printRow(runScenario(s.name, s, o, cfg, makeTransmitConfig(o)));

  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
          .count();
  std::printf("\ncompleted in %.1f s\n", static_cast<double>(elapsed) / 1000.0);
  return 0;
}

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& e) {
    // A rejected SpreadConfig is a normal outcome when sweeping the band.
    std::fprintf(stderr, "vacuum_bench: %s\n", e.what());
    return 2;
  }
}
