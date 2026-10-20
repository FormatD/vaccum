#include "vacuum_io/audio_file.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <vector>

#ifdef VACUUM_HAVE_LAME
#include <lame/lame.h>
#endif

namespace vacuum::io {

bool writeMp3(const std::string& path, const Audio& mono, int sampleRate, int kbps,
              std::string& error) {
#ifndef VACUUM_HAVE_LAME
  (void)path;
  (void)mono;
  (void)sampleRate;
  (void)kbps;
  error = "this build has no libmp3lame; write WAV instead or reconfigure CMake";
  return false;
#else
  if (sampleRate <= 0 || kbps <= 0 || mono.empty()) {
    error = "invalid arguments for MP3 encoding";
    return false;
  }

  lame_global_flags* g = lame_init();
  if (!g) {
    error = "lame_init failed";
    return false;
  }
  lame_set_in_samplerate(g, sampleRate);
  lame_set_out_samplerate(g, sampleRate);
  lame_set_num_channels(g, 1);
  lame_set_mode(g, MONO);
  lame_set_brate(g, kbps);
  lame_set_quality(g, 0);  // slowest, highest quality psychoacoustic model
  lame_set_copyright(g, 0);
  lame_set_original(g, 1);
  // Let LAME choose its own lowpass. That is the point of the exercise: the
  // watermark must survive what a normal encoder does, not a tuned one.
  if (lame_init_params(g) < 0) {
    lame_close(g);
    error = "lame rejected the encoding parameters (sample rate " +
            std::to_string(sampleRate) + ", " + std::to_string(kbps) + " kbps)";
    return false;
  }

  const std::size_t frames = mono.size();
  std::vector<std::int16_t> pcm(frames);
  for (std::size_t i = 0; i < frames; ++i) {
    pcm[i] = static_cast<std::int16_t>(
        std::lround(std::clamp(mono[i], -1.0f, 1.0f) * 32767.0f));
  }

  std::vector<std::uint8_t> mp3(frames * 5 / 4 + 7200);
  const int written = lame_encode_buffer(
      g, pcm.data(), pcm.data(), static_cast<int>(frames), mp3.data(),
      static_cast<int>(mp3.size()));
  if (written < 0) {
    lame_close(g);
    error = "lame_encode_buffer failed with " + std::to_string(written);
    return false;
  }
  const int flushed = lame_encode_flush(g, mp3.data() + written,
                                        static_cast<int>(mp3.size()) - written);
  if (flushed < 0) {
    lame_close(g);
    error = "lame_encode_flush failed with " + std::to_string(flushed);
    return false;
  }
  const std::size_t total = static_cast<std::size_t>(written) + static_cast<std::size_t>(flushed);
  lame_close(g);

  std::ofstream out(path, std::ios::binary);
  if (!out) {
    error = "cannot open " + path + " for writing";
    return false;
  }
  out.write(reinterpret_cast<const char*>(mp3.data()), static_cast<std::streamsize>(total));
  if (!out) {
    error = "write failed for " + path;
    return false;
  }
  error.clear();
  return true;
#endif
}

}  // namespace vacuum::io
