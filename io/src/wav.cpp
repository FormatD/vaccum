#include "vacuum_io/audio_file.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace vacuum::io {
namespace {

void putU32(std::vector<std::uint8_t>& v, std::uint32_t x) {
  for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>((x >> (8 * i)) & 0xFF));
}
void putU16(std::vector<std::uint8_t>& v, std::uint16_t x) {
  v.push_back(static_cast<std::uint8_t>(x & 0xFF));
  v.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFF));
}
std::uint32_t getU32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint16_t getU16(const std::uint8_t* p) {
  return static_cast<std::uint16_t>(p[0]) | static_cast<std::uint16_t>(p[1] << 8);
}
void putTag(std::vector<std::uint8_t>& v, const char tag[4]) {
  v.insert(v.end(), tag, tag + 4);
}
bool tagIs(const std::uint8_t* p, const char tag[4]) { return std::memcmp(p, tag, 4) == 0; }

std::int16_t clampToInt16(float v) {
  const float scaled = std::clamp(v, -1.0f, 1.0f) * 32767.0f;
  return static_cast<std::int16_t>(std::lround(scaled));
}

}  // namespace

bool writeWav(const std::string& path, const Audio& mono, int sampleRate, WavFormat format,
              std::string& error) {
  if (sampleRate <= 0) {
    error = "sample rate must be positive";
    return false;
  }

  const std::uint16_t bits = format == WavFormat::Float32 ? 32 : 16;
  const std::uint16_t audioFormat = format == WavFormat::Float32 ? 3 : 1;
  const std::uint32_t bytesPerSample = bits / 8;
  const std::uint32_t dataSize = static_cast<std::uint32_t>(mono.size()) * bytesPerSample;

  std::vector<std::uint8_t> header;
  header.reserve(44);
  putTag(header, "RIFF");
  putU32(header, 36 + dataSize);
  putTag(header, "WAVE");
  putTag(header, "fmt ");
  putU32(header, 16);
  putU16(header, audioFormat);
  putU16(header, 1);  // mono
  putU32(header, static_cast<std::uint32_t>(sampleRate));
  putU32(header, static_cast<std::uint32_t>(sampleRate) * bytesPerSample);
  putU16(header, static_cast<std::uint16_t>(bytesPerSample));
  putU16(header, bits);
  putTag(header, "data");
  putU32(header, dataSize);

  std::ofstream out(path, std::ios::binary);
  if (!out) {
    error = "cannot open " + path + " for writing";
    return false;
  }
  out.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));

  if (format == WavFormat::Int16) {
    std::vector<std::int16_t> pcm(mono.size());
    for (std::size_t i = 0; i < mono.size(); ++i) pcm[i] = clampToInt16(mono[i]);
    out.write(reinterpret_cast<const char*>(pcm.data()),
              static_cast<std::streamsize>(pcm.size() * 2));
  } else {
    out.write(reinterpret_cast<const char*>(mono.data()),
              static_cast<std::streamsize>(mono.size() * sizeof(float)));
  }

  if (!out) {
    error = "write failed for " + path;
    return false;
  }
  error.clear();
  return true;
}

bool readWav(const std::string& path, Audio& monoOut, int& sampleRateOut, std::string& error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    error = "cannot open " + path;
    return false;
  }
  std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
  if (file.size() < 12 || !tagIs(file.data(), "RIFF") || !tagIs(file.data() + 8, "WAVE")) {
    error = path + " is not a RIFF/WAVE file";
    return false;
  }

  std::uint16_t audioFormat = 0, channels = 0, bits = 0;
  std::uint32_t sampleRate = 0;
  const std::uint8_t* dataPtr = nullptr;
  std::uint32_t dataSize = 0;

  std::size_t pos = 12;
  while (pos + 8 <= file.size()) {
    const std::uint8_t* chunk = file.data() + pos;
    const std::uint32_t size = getU32(chunk + 4);
    const std::uint8_t* body = chunk + 8;
    if (pos + 8 + size > file.size()) break;

    if (tagIs(chunk, "fmt ")) {
      if (size < 16) {
        error = path + " has a truncated fmt chunk";
        return false;
      }
      audioFormat = getU16(body);
      channels = getU16(body + 2);
      sampleRate = getU32(body + 4);
      bits = getU16(body + 14);
    } else if (tagIs(chunk, "data")) {
      dataPtr = body;
      dataSize = size;
    }
    pos += 8 + size + (size & 1u);  // chunks are word aligned
  }

  if (!dataPtr || channels == 0 || sampleRate == 0) {
    error = path + " has no usable fmt/data chunks";
    return false;
  }
  if ((audioFormat != 1 && audioFormat != 3) || (bits != 16 && bits != 32) ||
      (audioFormat == 1 && bits != 16) || (audioFormat == 3 && bits != 32)) {
    error = path + ": only 16-bit PCM and 32-bit float WAV are supported";
    return false;
  }

  const std::size_t bytesPerSample = bits / 8;
  const std::size_t frameBytes = bytesPerSample * channels;
  const std::size_t frames = dataSize / frameBytes;
  monoOut.assign(frames, 0.0f);

  for (std::size_t f = 0; f < frames; ++f) {
    double sum = 0.0;
    for (std::uint16_t c = 0; c < channels; ++c) {
      const std::uint8_t* p = dataPtr + f * frameBytes + c * bytesPerSample;
      if (bits == 16) {
        std::int16_t v;
        std::memcpy(&v, p, 2);
        sum += static_cast<double>(v) / 32768.0;
      } else {
        float v;
        std::memcpy(&v, p, 4);
        sum += static_cast<double>(v);
      }
    }
    monoOut[f] = static_cast<float>(sum / channels);
  }

  sampleRateOut = static_cast<int>(sampleRate);
  error.clear();
  return true;
}

}  // namespace vacuum::io
