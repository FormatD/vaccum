// Desktop command line for embedding and recovering payloads in ordinary audio.
//
// The DSP lives in vacuum_core and the file/device handling in vacuum_io, so
// this program is only argument parsing and reporting: the Android build shares
// the same two libraries and neither depends on anything here.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "vacuum/frame.h"
#include "vacuum/ss_modem.h"
#include "vacuum/transmit.h"
#include "vacuum_io/audio_file.h"

using namespace vacuum;

namespace {

struct Args {
  std::string command;
  std::string host;      // carrier audio the watermark hides in
  std::string input;     // audio to decode
  std::string data;      // payload source, or "-" for stdin
  std::string out;
  std::string hex;

  int kbps = 192;
  int rate = 0;  // 0 = follow the audio file, or 48 kHz when there is none
  int parity = 0;
  int fragment = 0;
  int streamId = 0;
  int repeat = 0;
  float lowHz = 0.0f;
  float highHz = 0.0f;
  float detectFloor = -1.0f;
  float marginDb = 0.0f;
  double seconds = 0.0;
  double offsetSeconds = 0.0;
  std::string saveWav;  // mic: keep the capture so it can be decoded again offline
  bool noSoftErasure = false;
  bool quiet = false;
};

[[noreturn]] void fail(const std::string& message) {
  std::fprintf(stderr, "vacuum: %s\n", message.c_str());
  std::exit(1);
}

void usage() {
  std::printf(
      "vacuum -- binary payloads carried by audio, hidden inside ordinary music\n"
      "\n"
      "  vacuum watermark --host SONG.mp3 --data PAYLOAD --out TAGGED.mp3\n"
      "  vacuum extract   --in TAGGED.mp3 [--out PAYLOAD]\n"
      "  vacuum mic       --seconds 20 [--out PAYLOAD]\n"
      "  vacuum devices\n"
      "  vacuum info      --in AUDIO\n"
      "\n"
      "Payload source (watermark): --data FILE, --data - for stdin, or --hex 00ff..\n"
      "Payload sink (extract/mic): --out FILE, otherwise hex on stdout.\n"
      "\n"
      "Link options, and they must match on both ends:\n"
      "  --rate N              sample rate; watermark and extract follow the audio\n"
      "                        file unless this is given, mic defaults to 48000\n"
      "  --parity N            Reed-Solomon parity symbols per block (default 64)\n"
      "  --fragment BYTES      cap the block payload at 8, 24, 64, 128 or 196\n"
      "                        (default 64; raising --parity is the stronger\n"
      "                        knob, and cheaper in airtime than shrinking this)\n"
      "  --stream-id N         receiver ignores frames from any other stream\n"
      "  --no-soft-erasure     disable the confidence-ranked erasure retry\n"
      "\n"
      "Modem options:\n"
      "  --low-hz HZ           bottom of the carrier band (default 1100)\n"
      "  --high-hz HZ          top of the carrier band (default 9400)\n"
      "  --repeat N            physical symbols per logical symbol (default 3)\n"
      "  --detect-floor RATIO  tone floor as a multiple of the host bin (default 10)\n"
      "  --margin DB           masking safety margin (default 6)\n"
      "\n"
      "Output options:\n"
      "  --kbps N              MP3 bitrate when --out names an .mp3 (default 192)\n"
      "  --offset SECONDS      where in the host track the watermark starts\n"
      "  --seconds N           how long to record (mic)\n"
      "  --save-wav PATH       mic: also keep the capture, so it can be decoded\n"
      "                        again offline without playing it back a second time\n"
      "  --quiet               only report errors\n");
}

std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

bool endsWith(const std::string& path, const std::string& suffix) {
  const std::string lower = toLower(path);
  return lower.size() >= suffix.size() &&
         lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0;
}

Bytes parseHex(const std::string& text) {
  std::string clean;
  clean.reserve(text.size());
  for (char c : text) {
    if (std::isxdigit(static_cast<unsigned char>(c))) clean.push_back(c);
  }
  if (clean.size() % 2 != 0) fail("--hex needs an even number of hex digits");
  Bytes out;
  out.reserve(clean.size() / 2);
  for (std::size_t i = 0; i < clean.size(); i += 2) {
    out.push_back(static_cast<std::uint8_t>(std::strtoul(clean.substr(i, 2).c_str(), nullptr, 16)));
  }
  return out;
}

std::string toHex(const Bytes& data) {
  static const char* kDigits = "0123456789abcdef";
  std::string out;
  out.reserve(data.size() * 2);
  for (std::uint8_t b : data) {
    out.push_back(kDigits[b >> 4]);
    out.push_back(kDigits[b & 0x0F]);
  }
  return out;
}

/// "-" means stdin, which is what makes the tool composable with a pipe.
bool readDataFile(const std::string& path, Bytes& out, std::string& error) {
  out.clear();
  std::FILE* f = path == "-" ? stdin : std::fopen(path.c_str(), "rb");
  if (!f) {
    error = "cannot open " + path;
    return false;
  }
  char buf[8192];
  std::size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
    out.insert(out.end(), buf, buf + n);
  }
  const bool eof = std::feof(f) != 0;
  if (f != stdin) std::fclose(f);
  if (!eof) {
    error = "read error on " + path;
    return false;
  }
  return true;
}

bool writeDataFile(const std::string& path, const Bytes& data, std::string& error) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) {
    error = "cannot open " + path + " for writing";
    return false;
  }
  const std::size_t written = data.empty() ? 0 : std::fwrite(data.data(), 1, data.size(), f);
  std::fclose(f);
  if (written != data.size()) {
    error = "short write on " + path;
    return false;
  }
  return true;
}

Args parseArgs(int argc, char** argv) {
  Args a;
  if (argc < 2) {
    usage();
    std::exit(argc == 1 ? 0 : 2);
  }
  a.command = argv[1];
  if (a.command == "-h" || a.command == "--help" || a.command == "help") {
    usage();
    std::exit(0);
  }

  auto next = [&](int& i) -> std::string {
    if (i + 1 >= argc) fail(std::string(argv[i]) + " needs a value");
    return argv[++i];
  };

  for (int i = 2; i < argc; ++i) {
    const std::string k = argv[i];
    if (k == "--host") a.host = next(i);
    else if (k == "--in") a.input = next(i);
    else if (k == "--data") a.data = next(i);
    else if (k == "--out") a.out = next(i);
    else if (k == "--hex") a.hex = next(i);
    else if (k == "--kbps") a.kbps = std::atoi(next(i).c_str());
    else if (k == "--rate") a.rate = std::atoi(next(i).c_str());
    else if (k == "--parity") a.parity = std::atoi(next(i).c_str());
    else if (k == "--fragment") a.fragment = std::atoi(next(i).c_str());
    else if (k == "--stream-id") a.streamId = std::atoi(next(i).c_str());
    else if (k == "--repeat") a.repeat = std::atoi(next(i).c_str());
    else if (k == "--low-hz") a.lowHz = static_cast<float>(std::atof(next(i).c_str()));
    else if (k == "--high-hz") a.highHz = static_cast<float>(std::atof(next(i).c_str()));
    else if (k == "--detect-floor") a.detectFloor = static_cast<float>(std::atof(next(i).c_str()));
    else if (k == "--margin") a.marginDb = static_cast<float>(std::atof(next(i).c_str()));
    else if (k == "--seconds") a.seconds = std::atof(next(i).c_str());
    else if (k == "--offset") a.offsetSeconds = std::atof(next(i).c_str());
    else if (k == "--save-wav") a.saveWav = next(i);
    else if (k == "--no-soft-erasure") a.noSoftErasure = true;
    else if (k == "--quiet") a.quiet = true;
    else if (k == "-h" || k == "--help") {
      usage();
      std::exit(0);
    } else {
      fail("unknown option " + k);
    }
  }
  return a;
}

FragmentSize fragmentFor(int bytes) {
  for (std::uint8_t code = 0; code <= static_cast<std::uint8_t>(FragmentSize::Bytes196); ++code) {
    const auto size = static_cast<FragmentSize>(code);
    if (static_cast<int>(fragmentPayloadSize(size)) >= bytes) return size;
  }
  fail("--fragment must be at most 196");
}

SpreadConfig makeSpreadConfig(const Args& a) {
  SpreadConfig c;
  if (a.rate > 0) c.sampleRate = a.rate;
  if (a.lowHz > 0.0f) c.lowHz = a.lowHz;
  if (a.highHz > 0.0f) c.highHz = a.highHz;
  if (a.repeat > 0) c.repeat = a.repeat;
  if (a.detectFloor >= 0.0f) c.detectFloorRatio = a.detectFloor;
  if (a.marginDb > 0.0f) c.maskingMarginDb = a.marginDb;
  return c;
}

TransmitConfig makeTransmitConfig(const Args& a) {
  TransmitConfig t;
  if (a.parity > 0) t.rsParitySymbols = a.parity;
  if (a.fragment > 0) t.maxFragmentSize = fragmentFor(a.fragment);
  if (a.streamId > 0) t.streamId = static_cast<std::uint16_t>(a.streamId);
  t.softErasureRetry = !a.noSoftErasure;
  return t;
}

/// Read an audio file and, unless --rate pinned it, run the modem at the rate
/// the file actually uses. Most music is 44.1 kHz and resampling it to 48 kHz
/// first would be a needless extra step for the user.
Audio loadAudio(const Args& a, const std::string& path, SpreadConfig& cfg) {
  Audio rx;
  int rate = 0;
  std::string error;
  if (!io::readAudioFile(path, rx, rate, error)) fail(error);
  if (a.rate == 0) cfg.sampleRate = rate;
  if (rate != cfg.sampleRate) {
    fail(path + " is " + std::to_string(rate) + " Hz but --rate asked for " +
         std::to_string(cfg.sampleRate) + " Hz");
  }
  return rx;
}

int cmdWatermark(const Args& a) {
  if (a.out.empty()) fail("watermark needs --out");

  Bytes payload;
  std::string error;
  if (!a.hex.empty()) {
    payload = parseHex(a.hex);
  } else if (!a.data.empty()) {
    if (!readDataFile(a.data, payload, error)) fail(error);
  } else {
    fail("watermark needs --data FILE or --hex BYTES");
  }
  if (payload.empty()) fail("payload is empty");

  Audio host;
  SpreadConfig cfg = makeSpreadConfig(a);
  if (!a.host.empty()) host = loadAudio(a, a.host, cfg);

  const SpreadModem modem(cfg);
  TransmitConfig t = makeTransmitConfig(a);
  if (a.offsetSeconds > 0.0) {
    t.embedOffsetSamples = static_cast<std::size_t>(a.offsetSeconds * cfg.sampleRate);
  }

  EncodeResult enc;
  if (!encodeMessage(payload, host, modem, t, enc, error)) fail(error);

  if (endsWith(a.out, ".mp3")) {
    if (!io::writeMp3(a.out, enc.audio, cfg.sampleRate, a.kbps, error)) fail(error);
  } else {
    const auto format = endsWith(a.out, ".f32.wav") ? io::WavFormat::Float32 : io::WavFormat::Int16;
    if (!io::writeWav(a.out, enc.audio, cfg.sampleRate, format, error)) fail(error);
  }

  if (!a.quiet) {
    std::printf("payload   %zu B in %zu fragment(s)\n", payload.size(), enc.fragmentCount);
    std::printf("watermark %.2f s of audio at %d Hz, %.0f bps delivered\n", enc.seconds,
                cfg.sampleRate, enc.payloadBitRate);
    std::printf("out       %s, %.2f s\n", a.out.c_str(),
                static_cast<double>(enc.audio.size()) / cfg.sampleRate);
    if (host.empty()) {
      std::printf("host      none, the watermark stands alone\n");
    } else {
      std::printf("host      %s, %.2f s\n", a.host.c_str(),
                  static_cast<double>(host.size()) / cfg.sampleRate);
      if (enc.audio.size() > host.size()) {
        // Past the end of the host there is nothing to mask the tones: the tail
        // runs at `floorAmplitude`, which measures -39.6 dBFS against silence.
        // Whether that is audible depends on the playback chain, so state the
        // fact and let the user judge.
        std::printf("warning   the watermark is %.2f s longer than the host; that tail has\n"
                    "          no masking material and plays at floorAmplitude instead.\n"
                    "          Use --offset or a longer host, or a smaller --parity to\n"
                    "          shorten it.\n",
                    static_cast<double>(enc.audio.size() - host.size()) / cfg.sampleRate);
      }
    }
  }
  return 0;
}

void reportDecode(const DecodeResult& dec, const Args& a) {
  if (a.quiet) return;
  std::printf("sync      %s, score %.3f\n", dec.synced ? "found" : "not found", dec.syncScore);
  // The framing comes from the physical header, so it is reported even when
  // the payload did not -- a wrong --parity shows up here as a block count that
  // does not add up, which is a far faster diagnosis than a bare failure.
  if (dec.fragmentBytes > 0) {
    std::printf("sent with %zu B fragments, %zu of %zu recovered\n", dec.fragmentBytes,
                dec.framesRecovered, dec.framesExpected);
  }
  std::printf("rs        %d corrected, %d erased, %d blocks failed\n", dec.rsCorrectedSymbols,
              dec.rsErasedSymbols, dec.rsFailedBlocks);
  std::printf("bit conf  %.3f mean\n", dec.meanBitConfidence);
}

/// Send the payload where the user asked for it, or hex-dump it so the command
/// is useful without a destination file.
void emitPayload(const Bytes& payload, const Args& a) {
  if (a.out.empty()) {
    std::printf("%s\n", toHex(payload).c_str());
    return;
  }
  std::string error;
  if (!writeDataFile(a.out, payload, error)) fail(error);
  if (!a.quiet) std::printf("payload   %zu B -> %s\n", payload.size(), a.out.c_str());
}

int cmdExtract(const Args& a) {
  if (a.input.empty()) fail("extract needs --in AUDIO");
  SpreadConfig cfg = makeSpreadConfig(a);
  const Audio rx = loadAudio(a, a.input, cfg);
  const SpreadModem modem(cfg);

  const auto dec = decodeMessage(rx, modem, makeTransmitConfig(a));
  reportDecode(dec, a);
  if (!dec.ok) {
    std::fprintf(stderr, "vacuum: %s\n", dec.error.c_str());
    return 1;
  }
  emitPayload(dec.payload, a);
  return 0;
}

int cmdMic(const Args& a) {
  if (!io::toolAvailable("ffmpeg")) {
    fail("recording needs ffmpeg on PATH; see `vacuum devices`");
  }
  if (a.seconds <= 0.0) {
    fail("mic needs --seconds N; an open-ended recording cannot be stopped without discarding it");
  }
  const SpreadConfig cfg = makeSpreadConfig(a);
  const SpreadModem modem(cfg);

  Audio rx;
  std::string error;
  if (!a.quiet) {
    // Measured here: `mic --seconds 2` blocked for two minutes and returned
    // nothing. macOS gates the microphone behind a consent prompt aimed at the
    // calling terminal, which never appears in a headless shell, so name the
    // likely blocker instead of letting the command look frozen.
    std::printf("recording %.0f s at %d Hz (waits on the microphone permission prompt)...\n",
                a.seconds, cfg.sampleRate);
    std::fflush(stdout);
  }
  if (!io::captureMicrophone(a.seconds, cfg.sampleRate, rx, error)) fail(error);
  if (!a.quiet) std::printf("captured  %.2f s\n", static_cast<double>(rx.size()) / cfg.sampleRate);

  if (!a.saveWav.empty()) {
    // Float32 rather than Int16: a capture is evidence, and quantising it would
    // change the very signal a second decode attempt depends on.
    if (!io::writeWav(a.saveWav, rx, cfg.sampleRate, io::WavFormat::Float32, error)) fail(error);
    if (!a.quiet) std::printf("saved     %s\n", a.saveWav.c_str());
  }

  const auto dec = decodeMessage(rx, modem, makeTransmitConfig(a));
  reportDecode(dec, a);
  if (!dec.ok) {
    std::fprintf(stderr, "vacuum: %s\n", dec.error.c_str());
    return 1;
  }
  emitPayload(dec.payload, a);
  return 0;
}

int cmdInfo(const Args& a) {
  if (a.input.empty()) fail("info needs --in AUDIO");
  SpreadConfig cfg = makeSpreadConfig(a);
  const Audio rx = loadAudio(a, a.input, cfg);
  const SpreadModem modem(cfg);

  // Decoding rather than just acquiring is the point: the physical header
  // survives even when the payload does not, so this reports what the sender
  // actually used, which is what a mismatched receiver needs to know.
  const auto dec = decodeMessage(rx, modem, makeTransmitConfig(a));
  std::printf("file      %s\n", a.input.c_str());
  std::printf("duration  %.2f s at %d Hz\n", static_cast<double>(rx.size()) / cfg.sampleRate,
              cfg.sampleRate);
  std::printf("band      %.0f-%.0f Hz, %d sub-bands x %d groups, repeat %d\n", cfg.lowHz,
              cfg.highHz, cfg.subBands, cfg.groups, cfg.repeat);
  // `netBitRate` is the modem layer before framing and parity, so quote both:
  // the second figure is what a caller actually gets, and it comes from the same
  // estimator `watermark` uses to size the recording.
  const double kbSeconds = estimateSeconds(1024, modem, makeTransmitConfig(a));
  std::printf("rate      %.0f bps modem layer, %.0f bps delivered (%.1f s per kB)\n",
              modem.netBitRate(), 1024.0 * 8.0 / std::max(1e-9, kbSeconds), kbSeconds);
  std::printf("parity    %d (this run's setting; it is not transmitted)\n",
              makeTransmitConfig(a).rsParitySymbols);
  reportDecode(dec, a);
  return dec.synced ? 0 : 1;
}

int cmdDevices() {
  if (!io::toolAvailable("ffmpeg")) {
    fail("ffmpeg is not on PATH");
  }
  // ffmpeg lists devices by failing to open an empty input, so its own error
  // lines are part of a successful listing. Keep only the device entries.
  const std::string raw = io::listInputDevices();
  std::size_t start = 0;
  while (start <= raw.size()) {
    const std::size_t end = raw.find('\n', start);
    const std::string line = raw.substr(start, end == std::string::npos ? end : end - start);
    if (line.find("Error opening input") == std::string::npos) {
      std::printf("%s\n", line.c_str());
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const Args a = parseArgs(argc, argv);
  try {
    if (a.command == "watermark") return cmdWatermark(a);
    if (a.command == "extract") return cmdExtract(a);
    if (a.command == "mic") return cmdMic(a);
    if (a.command == "info") return cmdInfo(a);
    if (a.command == "devices") return cmdDevices();
    std::fprintf(stderr, "vacuum: unknown command '%s'\n\n", a.command.c_str());
    usage();
    return 2;
  } catch (const std::exception& e) {
    // A SpreadConfig that cannot fit its carriers in the requested band throws,
    // which is a normal outcome when the user narrows the band by hand.
    std::fprintf(stderr, "vacuum: %s\n", e.what());
    return 2;
  }
}
