#include "testing.h"
#include "audio_gen.h"
#include "vacuum/correlate.h"
#include "vacuum/frame.h"
#include "vacuum/ss_modem.h"
#include "vacuum/transmit.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <stdexcept>

using namespace vacuum;

namespace {

constexpr int kRate = 48000;

std::vector<std::uint8_t> randomBits(std::size_t n, unsigned seed) {
  std::mt19937 rng(seed);
  std::vector<std::uint8_t> b(n);
  for (auto& v : b) v = static_cast<std::uint8_t>(rng() & 1);
  return b;
}

std::size_t bitErrors(const std::vector<std::uint8_t>& sent,
                      const std::vector<std::uint8_t>& received) {
  const std::size_t n = std::min(sent.size(), received.size());
  std::size_t e = sent.size() - n;
  for (std::size_t i = 0; i < n; ++i) {
    if (sent[i] != received[i]) ++e;
  }
  return e;
}

double rms(const Audio& a) {
  double s = 0;
  for (float v : a) s += static_cast<double>(v) * v;
  return std::sqrt(s / static_cast<double>(std::max<std::size_t>(a.size(), 1)));
}

/// Raw physical-layer budget. The modem itself has no error correction, so the
/// loopback tests can only assert a bit error rate, not exact equality; the
/// Reed-Solomon layer above is what turns that into a correct payload. The
/// measured clean-over-music rate is 5.5e-3 (see SpreadConfig::detectFloorRatio),
/// and 2% leaves room for the extra impairment under test.
constexpr double kBerBudget = 0.02;

void checkBer(const char* label, const std::vector<std::uint8_t>& sent,
              const std::vector<std::uint8_t>& received) {
  const std::size_t errors = bitErrors(sent, received);
  const double ber = static_cast<double>(errors) / static_cast<double>(sent.size());
  std::printf("    %-22s %zu/%zu bits, BER %.2e\n", label, errors, sent.size(), ber);
  CHECK(received.size() >= sent.size());
  CHECK(ber <= kBerBudget);
}

}  // namespace

TEST(modem_rejects_invalid_config) {
  for (auto tweak : {0, 1, 2, 3, 4}) {
    SpreadConfig c;
    switch (tweak) {
      case 0: c.cyclicPrefix = 0; break;
      case 1: c.groups = 3; break;  // not a power of two
      case 2: c.repeat = 0; break;
      case 3: c.preambleSymbols = 0; break;
      case 4: c.peakBudget = 0.0f; break;
    }
    bool threw = false;
    try {
      const SpreadModem m(c);
      (void)m;
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    CHECK(threw);
  }
}

TEST(modem_rejects_band_too_narrow_for_carriers) {
  SpreadConfig c;
  c.lowHz = 2000.0f;
  c.highHz = 2100.0f;  // cannot fit 64 carriers at 2-bin spacing
  bool threw = false;
  try {
    const SpreadModem m(c);
    (void)m;
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
}

TEST(modem_carrier_layout) {
  SpreadConfig c;
  const SpreadModem m(c);

  CHECK_EQ(m.totalCarriers(), c.subBands * c.groups * c.carriersPerGroup);
  CHECK_EQ(m.carrierBins().size(), static_cast<std::size_t>(m.totalCarriers()));

  const float binHz = static_cast<float>(c.sampleRate) / static_cast<float>(c.fftSize);
  int previous = -1000;
  for (int bin : m.carrierBins()) {
    CHECK(bin > previous + c.minBinGap - 1);
    previous = bin;
    CHECK(bin >= 1);
    CHECK(bin < c.fftSize / 2);
    CHECK(bin * binHz >= c.lowHz - binHz);
    CHECK(bin * binHz <= c.highHz + binHz);
  }
}

TEST(modem_rate_and_duration_are_consistent) {
  SpreadConfig c;
  const SpreadModem m(c);

  CHECK_EQ(m.bitsPerSymbol(), c.subBands * 2);
  CHECK_EQ(m.sampleRate(), c.sampleRate);

  const double symbolRate = static_cast<double>(c.sampleRate) /
                            static_cast<double>(c.fftSize + c.cyclicPrefix);
  CHECK_NEAR(m.netBitRate(), symbolRate * m.bitsPerSymbol() / c.repeat, 1e-6);

  // 4 logical symbols worth of bits, plus the fixed preamble.
  const std::size_t bits = static_cast<std::size_t>(m.bitsPerSymbol()) * 4;
  const std::size_t expected =
      (static_cast<std::size_t>(c.preambleSymbols) + 4 * static_cast<std::size_t>(c.repeat)) *
      static_cast<std::size_t>(c.fftSize + c.cyclicPrefix);
  CHECK_EQ(m.samplesForBits(bits), expected);
  CHECK_EQ(m.samplesForBits(0),
           static_cast<std::size_t>(c.preambleSymbols) *
               static_cast<std::size_t>(c.fftSize + c.cyclicPrefix));
}

TEST(modem_loopback_clean_over_music) {
  SpreadConfig c;
  const SpreadModem m(c);
  const auto bits = randomBits(m.bitsPerSymbol() * 32, 1);

  const Audio host = testutil::synthHost(4.0, kRate);
  const Audio tx = m.embed(bits, host, 0);
  const auto rx = m.extract(tx);

  CHECK(rx.synced);
  CHECK(rx.syncScore > 0.9);
  checkBer("clean over music", bits, rx.bits);
}

TEST(modem_loopback_clean_over_silence) {
  // The hardest case for hiding and the easiest for detection: no masking
  // energy at all, so every tone falls back to floorAmplitude.
  SpreadConfig c;
  const SpreadModem m(c);
  const auto bits = randomBits(m.bitsPerSymbol() * 32, 2);

  const Audio tx = m.embed(bits, Audio(static_cast<std::size_t>(kRate) * 4, 0.0f), 0);
  const auto rx = m.extract(tx);

  CHECK(rx.synced);
  CHECK_EQ(bitErrors(bits, rx.bits), std::size_t{0});
}

TEST(modem_loopback_with_transmission_delay) {
  SpreadConfig c;
  const SpreadModem m(c);
  const auto bits = randomBits(m.bitsPerSymbol() * 32, 3);

  const Audio host = testutil::synthHost(4.0, kRate);
  const Audio tx = m.embed(bits, host, 0);

  // Prepend lead-in silence: the receiver has no idea where playback started.
  Audio delayed(static_cast<std::size_t>(7777) + tx.size(), 0.0f);
  std::copy(tx.begin(), tx.end(), delayed.begin() + 7777);

  const auto rx = m.extract(delayed);
  CHECK(rx.synced);
  CHECK(rx.syncSample >= 7777 - c.cyclicPrefix);
  CHECK(rx.syncSample <= 7777 + c.cyclicPrefix);
  checkBer("7777-sample delay", bits, rx.bits);
}

TEST(modem_loopback_with_fractional_delay) {
  SpreadConfig c;
  const SpreadModem m(c);
  const auto bits = randomBits(m.bitsPerSymbol() * 32, 4);

  const Audio host = testutil::synthHost(4.0, kRate);
  const Audio tx = m.embed(bits, host, 0);
  const Audio rxSignal = testutil::fractionalDelay(tx, 4321.5);

  const auto rx = m.extract(rxSignal);
  CHECK(rx.synced);
  checkBer("fractional delay", bits, rx.bits);
}

TEST(modem_loopback_survives_noise) {
  SpreadConfig c;
  const SpreadModem m(c);
  const auto bits = randomBits(m.bitsPerSymbol() * 32, 5);
  const Audio host = testutil::synthHost(4.0, kRate);
  const Audio tx = m.embed(bits, host, 0);

  for (double snr : {30.0, 20.0, 15.0}) {
    const Audio noisy = testutil::addNoise(tx, snr, static_cast<unsigned>(snr));
    const auto rx = m.extract(noisy);
    CHECK(rx.synced);
    if (rx.synced) {
      char label[32];
      std::snprintf(label, sizeof(label), "noise %.0f dB", snr);
      checkBer(label, bits, rx.bits);
    }
  }
}

TEST(modem_watermark_stays_within_peak_budget) {
  SpreadConfig c;
  const SpreadModem m(c);
  const auto bits = randomBits(m.bitsPerSymbol() * 4, 6);

  const Audio host = testutil::synthHost(0.8, kRate);
  const Audio tx = m.embed(bits, host, 0);
  CHECK_EQ(tx.size(), host.size());

  double peak = 0.0;
  for (std::size_t i = 0; i < tx.size(); ++i) {
    peak = std::max(peak, std::abs(static_cast<double>(tx[i] - host[i])));
  }
  CHECK(peak <= static_cast<double>(c.peakBudget) + 1e-4);

  // And the combined signal must not clip.
  double outPeak = 0.0;
  for (float v : tx) outPeak = std::max(outPeak, static_cast<double>(std::abs(v)));
  CHECK(outPeak <= 1.0);
}

TEST(modem_watermark_is_quieter_than_host) {
  // Proxy for inaudibility: the injected energy should be well below the host's.
  SpreadConfig c;
  const SpreadModem m(c);
  const auto bits = randomBits(m.bitsPerSymbol() * 16, 7);

  const Audio host = testutil::synthHost(1.5, kRate);
  const Audio tx = m.embed(bits, host, 0);

  Audio watermark(tx.size());
  for (std::size_t i = 0; i < tx.size(); ++i) watermark[i] = tx[i] - host[i];

  const double ratioDb = 20.0 * std::log10(rms(watermark) / std::max(1e-9, rms(host)));
  std::printf("    watermark-to-host ratio = %.1f dB\n", ratioDb);
  CHECK(ratioDb < -6.0);
}

TEST(modem_preamble_reference_correlates_at_zero_lag) {
  SpreadConfig c;
  const SpreadModem m(c);
  const Audio host = testutil::synthHost(0.8, kRate);
  const Audio tx = m.embed(randomBits(m.bitsPerSymbol() * 2, 8), host, 0);

  const Audio ref = m.preambleReference();
  CHECK_EQ(ref.size(), static_cast<std::size_t>(c.preambleSymbols) * m.symbolPeriod());

  const auto peaks = correlateTopOffsets(tx, ref, 3, static_cast<std::ptrdiff_t>(m.symbolPeriod()));
  CHECK(!peaks.empty());
  if (!peaks.empty()) CHECK(std::abs(peaks[0].second) <= 2);
}

TEST(message_roundtrip_over_music) {
  SpreadConfig c;
  const SpreadModem m(c);
  TransmitConfig t;

  Bytes payload;
  for (int i = 0; i < 40; ++i) payload.push_back(static_cast<std::uint8_t>(i * 11 + 3));

  const Audio host = testutil::synthHost(8.0, kRate);
  EncodeResult enc;
  std::string error;
  CHECK(encodeMessage(payload, host, m, t, enc, error));
  if (error.size() != 0) std::printf("    encode error: %s\n", error.c_str());
  CHECK(error.empty());
  CHECK_EQ(enc.fragmentCount, std::size_t{1});

  std::printf("    40 bytes -> %.2f s of audio at %.0f bps\n", enc.seconds, enc.netBitRate);

  const auto dec = decodeMessage(enc.audio, m, t);
  if (!dec.ok) std::printf("    decode error: %s\n", dec.error.c_str());
  CHECK(dec.ok);
  CHECK(dec.payload == payload);
  CHECK(dec.syncScore > 0.9);
  CHECK_EQ(dec.framesRecovered, dec.framesExpected);
}

TEST(message_roundtrip_through_channel_impairments) {
  SpreadConfig c;
  const SpreadModem m(c);
  TransmitConfig t;

  Bytes payload(120);
  std::mt19937 rng(31);
  for (auto& b : payload) b = static_cast<std::uint8_t>(rng());

  const Audio host = testutil::synthHost(14.0, kRate);
  EncodeResult enc;
  std::string error;
  CHECK(encodeMessage(payload, host, m, t, enc, error));
  CHECK(error.empty());

  // Earbud/microphone roll-off, an air-path delay, and background noise.
  Audio rxSignal = testutil::lowpass(enc.audio, kRate, 11000.0);
  rxSignal = testutil::fractionalDelay(rxSignal, 3333.3);
  rxSignal = testutil::addNoise(rxSignal, 18.0, 77);

  const auto dec = decodeMessage(rxSignal, m, t);
  if (!dec.ok) {
    std::printf("    decode error: %s (rs failed blocks=%d)\n", dec.error.c_str(),
                dec.rsFailedBlocks);
  }
  CHECK(dec.ok);
  CHECK(dec.payload == payload);
}

TEST(message_roundtrip_multi_fragment) {
  SpreadConfig c;
  const SpreadModem m(c);
  TransmitConfig t;

  Bytes payload(900);
  for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<std::uint8_t>(i & 0xFF);

  EncodeResult enc;
  std::string error;
  CHECK(encodeMessage(payload, Audio(), m, t, enc, error));
  CHECK(error.empty());
  CHECK(enc.fragmentCount > 1);

  const auto dec = decodeMessage(enc.audio, m, t);
  CHECK(dec.ok);
  CHECK(dec.payload == payload);
  CHECK_EQ(dec.framesRecovered, dec.framesExpected);
}

TEST(message_rejects_empty_and_oversized_payload) {
  SpreadConfig c;
  const SpreadModem m(c);
  TransmitConfig t;
  t.maxPayloadBytes = 64;

  EncodeResult enc;
  std::string error;
  CHECK(!encodeMessage(Bytes{}, Audio(), m, t, enc, error));
  CHECK(!error.empty());
  CHECK(!encodeMessage(Bytes(65, 1), Audio(), m, t, enc, error));
  CHECK(!error.empty());
}

TEST(message_fails_cleanly_on_truncated_recording) {
  SpreadConfig c;
  const SpreadModem m(c);
  TransmitConfig t;

  Bytes payload(400, 0x42);
  EncodeResult enc;
  std::string error;
  CHECK(encodeMessage(payload, Audio(), m, t, enc, error));
  CHECK(error.empty());

  Audio truncated(enc.audio.begin(), enc.audio.begin() + enc.audio.size() / 2);
  const auto dec = decodeMessage(truncated, m, t);
  CHECK(!dec.ok);
  CHECK(!dec.error.empty());
}

TEST(message_fails_on_pure_noise) {
  SpreadConfig c;
  const SpreadModem m(c);
  TransmitConfig t;

  std::mt19937 rng(5);
  std::normal_distribution<float> d(0.0f, 0.05f);
  Audio noise(static_cast<std::size_t>(kRate * 3));
  for (auto& v : noise) v = d(rng);

  const auto dec = decodeMessage(noise, m, t);
  CHECK(!dec.ok);
  CHECK(!dec.error.empty());
}

// Fragment size is not a receiver setting: the physical header carries it, so a
// receiver configured with the wrong one still reads the sender's framing. The
// parity is the opposite -- it is never transmitted -- so a parity mismatch is
// the one failure this reports as "N blocks failed" rather than a bare error.
TEST(decode_reports_the_senders_fragment_size) {
  SpreadConfig c;
  const SpreadModem m(c);
  TransmitConfig t;
  t.maxFragmentSize = FragmentSize::Bytes24;

  Bytes payload(64, 0x3C);
  EncodeResult enc;
  std::string error;
  CHECK(encodeMessage(payload, Audio(), m, t, enc, error));
  CHECK(error.empty());

  TransmitConfig receiver = t;
  receiver.maxFragmentSize = FragmentSize::Bytes64;
  const auto matched = decodeMessage(enc.audio, m, receiver);
  CHECK(matched.ok);
  CHECK_EQ(matched.fragmentBytes, fragmentPayloadSize(FragmentSize::Bytes24));
  CHECK_EQ(matched.framesExpected, enc.fragmentCount);

  // Now the case the field exists for: wrong parity, header still readable.
  TransmitConfig wrongParity = t;
  wrongParity.rsParitySymbols = 32;
  const auto failed = decodeMessage(enc.audio, m, wrongParity);
  CHECK(!failed.ok);
  CHECK_EQ(failed.fragmentBytes, fragmentPayloadSize(FragmentSize::Bytes24));
  CHECK_EQ(failed.framesExpected, enc.fragmentCount);
  CHECK(failed.rsFailedBlocks > 0);
}

TEST(message_estimate_seconds_is_close_to_actual) {
  SpreadConfig c;
  const SpreadModem m(c);
  TransmitConfig t;

  const double estimated = estimateSeconds(40, m, t);
  EncodeResult enc;
  std::string error;
  CHECK(encodeMessage(Bytes(40, 1), Audio(), m, t, enc, error));
  CHECK(error.empty());
  CHECK_NEAR(estimated, enc.seconds, enc.seconds * 0.15);
}

// The modem's own rate is quoted before framing, CRC and parity, so it is not
// throughput. This is the ratio that makes "~200 bps" a misleading claim and
// "77 bps delivered" the true one.
TEST(delivered_bit_rate_is_lower_than_the_modem_rate) {
  SpreadConfig c;
  const SpreadModem m(c);
  TransmitConfig t;

  EncodeResult enc;
  std::string error;
  CHECK(encodeMessage(Bytes(120, 7), Audio(), m, t, enc, error));
  CHECK(error.empty());
  CHECK_NEAR(enc.payloadBitRate, 120.0 * 8.0 / enc.seconds, 1e-6);
  CHECK(enc.payloadBitRate < enc.netBitRate);
  CHECK(enc.payloadBitRate > 0.3 * enc.netBitRate);
}
