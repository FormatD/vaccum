#include "vacuum_io/audio_file.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace vacuum::io {
namespace {

/// ffmpeg input specification for the default microphone on this platform.
/// Overridable with VACUUM_AUDIO_INPUT="<format> <device>", which is passed
/// straight through as `-f <format> -i <device>`.
std::string defaultInputSpec() {
  if (const char* env = std::getenv("VACUUM_AUDIO_INPUT"); env && *env) return env;
#if defined(__APPLE__)
  return "avfoundation :0";
#elif defined(__linux__)
  return "alsa default";
#elif defined(_WIN32)
  return "dshow audio=Microphone";
#else
  return "";
#endif
}

std::string quote(const std::string& s) {
#if defined(_WIN32)
  return "\"" + s + "\"";
#else
  return "'" + s + "'";
#endif
}

/// Read all of `stream` into a byte buffer.
std::vector<std::uint8_t> drain(FILE* stream) {
  std::vector<std::uint8_t> out;
  std::array<std::uint8_t, 65536> chunk{};
  while (true) {
    const std::size_t got = std::fread(chunk.data(), 1, chunk.size(), stream);
    if (got == 0) break;
    out.insert(out.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(got));
  }
  return out;
}

}  // namespace

bool runCommand(const std::string& command, std::string& output, std::string& error) {
  FILE* pipe = popen(command.c_str(), "r");
  if (!pipe) {
    error = "failed to spawn: " + command;
    return false;
  }
  const auto bytes = drain(pipe);
  const int status = pclose(pipe);
  output.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  if (status != 0) {
    error = "command exited with status " + std::to_string(status) + ": " + command;
    if (!output.empty()) error += "\n" + output;
    return false;
  }
  error.clear();
  return true;
}

bool toolAvailable(const std::string& tool) {
#if defined(_WIN32)
  const std::string cmd = "where " + tool + " > NUL 2>&1";
#else
  const std::string cmd = "command -v " + quote(tool) + " > /dev/null 2>&1";
#endif
  return std::system(cmd.c_str()) == 0;
}

std::string listInputDevices() {
#if defined(__APPLE__)
  const std::string cmd = "ffmpeg -hide_banner -f avfoundation -list_devices true -i \"\" 2>&1";
#elif defined(__linux__)
  const std::string cmd = "ffmpeg -hide_banner -sources alsa 2>&1";
#else
  const std::string cmd = "ffmpeg -hide_banner -list_options true -f dshow -i dummy 2>&1";
#endif
  std::string output, error;
  runCommand(cmd, output, error);
  return output.empty() ? error : output;
}

bool readAudioFile(const std::string& path, Audio& out, int& sampleRateOut, std::string& error) {
  if (!toolAvailable("ffprobe")) {
    error = "ffprobe not found on PATH; install ffmpeg to decode this format";
    return false;
  }

  std::string probeOutput, probeError;
  const std::string probe = "ffprobe -v error -select_streams a:0 -show_entries stream=sample_rate "
                            "-of default=noprint_wrappers=1:nokey=1 " +
                            quote(path) + " 2>&1";
  if (!runCommand(probe, probeOutput, probeError)) {
    error = "ffprobe failed on " + path + ": " + probeError;
    return false;
  }
  sampleRateOut = std::atoi(probeOutput.c_str());
  if (sampleRateOut <= 0) {
    error = "could not determine the sample rate of " + path;
    return false;
  }

  const std::string cmd = "ffmpeg -v error -i " + quote(path) +
                          " -map 0:a:0 -ac 1 -ar " + std::to_string(sampleRateOut) +
                          " -f f32le -acodec pcm_f32le - 2>&1";
  std::string decoded, decodeError;
  if (!runCommand(cmd, decoded, decodeError)) {
    error = "ffmpeg failed to decode " + path + ": " + decodeError;
    return false;
  }

  // ffmpeg prints errors to stderr, which 2>&1 merges in; a successful decode
  // yields raw float32 and nothing else.
  if (decoded.size() % sizeof(float) != 0) {
    error = "unexpected ffmpeg output for " + path + ": " + decoded.substr(0, 400);
    return false;
  }
  const std::size_t frames = decoded.size() / sizeof(float);
  out.resize(frames);
  std::memcpy(out.data(), decoded.data(), decoded.size());
  error.clear();
  return true;
}

bool captureMicrophone(double seconds, int sampleRate, Audio& out, std::string& error) {
  if (seconds <= 0.0) {
    error = "capture duration must be positive; pass --seconds";
    return false;
  }
  if (!toolAvailable("ffmpeg")) {
    error = "ffmpeg not found on PATH; it provides microphone capture";
    return false;
  }
  const std::string spec = defaultInputSpec();
  if (spec.empty()) {
    error = "no default microphone input for this platform; set VACUUM_AUDIO_INPUT";
    return false;
  }

  const auto space = spec.find(' ');
  if (space == std::string::npos) {
    error = "VACUUM_AUDIO_INPUT must be '<format> <device>', got: " + spec;
    return false;
  }
  const std::string format = spec.substr(0, space);
  const std::string device = spec.substr(space + 1);

  // `-nostdin` because popen leaves the child attached to the caller's terminal,
  // and ffmpeg's interactive key handler reads from it. stderr is deliberately
  // not redirected: a capture that produces nothing needs to say why, and the
  // only channel it says it on is that one. `-v error` keeps it to real errors.
  const std::string cmd =
      "ffmpeg -nostdin -v error -f " + format + " -i " + quote(device) + " -t " +
      std::to_string(seconds) + " -ac 1 -ar " + std::to_string(sampleRate) +
      " -f f32le -acodec pcm_f32le -";
  std::string captured, captureError;
  if (!runCommand(cmd, captured, captureError)) {
    error = "microphone capture failed (" + format + " " + device + "): " + captureError;
    return false;
  }
  if (captured.size() < sizeof(float)) {
    error = "microphone capture produced no audio; check the input device";
    return false;
  }

  out.resize(captured.size() / sizeof(float));
  std::memcpy(out.data(), captured.data(), out.size() * sizeof(float));
  error.clear();
  return true;
}

}  // namespace vacuum::io
