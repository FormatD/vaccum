#pragma once

#include <functional>
#include <string>

#include "vacuum/types.h"

/// File and device audio I/O for the desktop tooling. Kept out of `vacuum_core`
/// so the Android JNI build links only the DSP and not stdio-backed helpers.
namespace vacuum::io {

enum class WavFormat { Int16, Float32 };

struct CaptureProgress {
  double elapsedSeconds = 0.0;
  double totalSeconds = 0.0;
  float currentPeak = 0.0f;
  std::size_t samplesCaptured = 0;
};
using CaptureProgressCallback = std::function<void(const CaptureProgress&)>;

/// Write mono PCM. Float32 avoids the 16-bit quantisation floor, which matters
/// when measuring a watermark that sits ~25 dB below the host.
bool writeWav(const std::string& path, const Audio& mono, int sampleRate, WavFormat format,
              std::string& error);

/// Read a WAV file and downmix to mono. Does not resample: `sampleRateOut`
/// reports what the file actually contains, and callers must check it matches
/// the modem configuration.
bool readWav(const std::string& path, Audio& monoOut, int& sampleRateOut, std::string& error);

/// Encode to MP3 with libmp3lame, constant bitrate.
bool writeMp3(const std::string& path, const Audio& mono, int sampleRate, int kbps,
              std::string& error);

/// Decode anything ffmpeg can read, to mono float32 at the file's own rate.
bool readAudioFile(const std::string& path, Audio& out, int& sampleRateOut, std::string& error);

/// Record from the default microphone. `seconds <= 0` records until interrupted.
bool captureMicrophone(double seconds, int sampleRate, Audio& out, std::string& error,
                       CaptureProgressCallback onProgress = nullptr);

/// Input specification for the default microphone on this platform ('<format> <device>').
std::string defaultAudioInput();

/// List ffmpeg input devices, for picking a non-default microphone.
std::string listInputDevices();

bool runCommand(const std::string& command, std::string& output, std::string& error);
bool toolAvailable(const std::string& tool);

}  // namespace vacuum::io
