// JNI bridge between the Kotlin app and the shared C++ core.
//
// Only vacuum_core is linked here, deliberately: vacuum_io shells out to
// ffmpeg and libmp3lame, which do not exist on Android. The app gets its PCM
// from AudioRecord/AudioTrack and its MP3 from MediaCodec, and hands the core
// plain float arrays.
//
// The interface avoids constructing Kotlin objects from C++. Reflecting on a
// Kotlin class means hard-coding a JNI signature string that nothing here can
// check, so results come back through caller-supplied arrays instead.

#include <jni.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "vacuum/frame.h"
#include "vacuum/ss_modem.h"
#include "vacuum/transmit.h"

using namespace vacuum;

namespace {

/// Per-thread so a failure on one thread cannot be reported as another's.
thread_local std::string g_lastError;

void setError(const std::string& message) { g_lastError = message; }

/// Mirror of the desktop CLI's mapping: a byte count to the smallest
/// FragmentSize that holds it.
FragmentSize fragmentFor(int bytes) {
  for (std::uint8_t code = 0; code <= static_cast<std::uint8_t>(FragmentSize::Bytes196); ++code) {
    const auto size = static_cast<FragmentSize>(code);
    if (static_cast<int>(fragmentPayloadSize(size)) >= bytes) return size;
  }
  return FragmentSize::Bytes196;
}

SpreadConfig spreadFor(jint sampleRate) {
  SpreadConfig c;
  c.sampleRate = static_cast<int>(sampleRate);
  return c;
}

TransmitConfig transmitFor(jint parity, jint fragmentBytes) {
  TransmitConfig t;
  if (parity > 0) t.rsParitySymbols = static_cast<int>(parity);
  if (fragmentBytes > 0) t.maxFragmentSize = fragmentFor(static_cast<int>(fragmentBytes));
  return t;
}

/// Copy a Java float array into an Audio, or report and return false. A
/// negative length or a null reference is a programming error on the Kotlin
/// side, not something to crash the process over.
bool toAudio(JNIEnv* env, jfloatArray array, Audio& out) {
  if (array == nullptr) {
    setError("audio array is null");
    return false;
  }
  const jsize n = env->GetArrayLength(array);
  out.resize(static_cast<std::size_t>(n));
  if (n == 0) return true;
  env->GetFloatArrayRegion(array, 0, n, out.data());
  return true;
}

bool toBytes(JNIEnv* env, jbyteArray array, Bytes& out) {
  if (array == nullptr) {
    setError("payload array is null");
    return false;
  }
  const jsize n = env->GetArrayLength(array);
  out.resize(static_cast<std::size_t>(n));
  if (n == 0) return true;
  env->GetByteArrayRegion(array, 0, n, reinterpret_cast<jbyte*>(out.data()));
  return true;
}

}  // namespace

extern "C" {

JNIEXPORT jstring JNICALL
Java_dev_vacuum_android_Vacuum_nativeLastError(JNIEnv* env, jobject /*self*/) {
  return env->NewStringUTF(g_lastError.c_str());
}

/// Watermark `host` with `payload` and return the mixed audio. Returns null on
/// failure, with the reason available from nativeLastError.
JNIEXPORT jfloatArray JNICALL
Java_dev_vacuum_android_Vacuum_nativeEncode(JNIEnv* env, jobject /*self*/, jbyteArray payloadArray,
                                            jfloatArray hostArray, jint sampleRate, jint parity,
                                            jint fragmentBytes) {
  g_lastError.clear();

  Bytes payload;
  if (!toBytes(env, payloadArray, payload)) return nullptr;
  Audio host;
  if (!toAudio(env, hostArray, host)) return nullptr;

  SpreadConfig cfg;
  try {
    cfg = spreadFor(sampleRate);
    const SpreadModem modem(cfg);
    const TransmitConfig t = transmitFor(parity, fragmentBytes);

    EncodeResult enc;
    std::string error;
    if (!encodeMessage(payload, host, modem, t, enc, error)) {
      setError(error.empty() ? "encode failed" : error);
      return nullptr;
    }

    jfloatArray out = env->NewFloatArray(static_cast<jsize>(enc.audio.size()));
    if (out == nullptr) {
      setError("out of memory allocating the output audio");
      return nullptr;
    }
    if (!enc.audio.empty()) {
      env->SetFloatArrayRegion(out, 0, static_cast<jsize>(enc.audio.size()), enc.audio.data());
    }
    return out;
  } catch (const std::exception& e) {
    // A sample rate too low for the carrier band throws from the modem
    // constructor, which is a normal outcome when the app passes 8 kHz.
    setError(e.what());
    return nullptr;
  }
}

/// Decode a capture. Returns the payload length on success and fills
/// `outPayload`; returns a negative status on failure, with the reason from
/// nativeLastError.
///
/// outStats, if non-null and at least 6 long, receives
/// [synced, framesRecovered, framesExpected, rsCorrected, rsErased, rsFailed].
/// framesRecovered is filled even on failure, because "3 of 7 fragments" is the
/// difference between a bad channel and a wrong configuration.
JNIEXPORT jint JNICALL
Java_dev_vacuum_android_Vacuum_nativeDecode(JNIEnv* env, jobject /*self*/, jfloatArray rxArray,
                                            jint sampleRate, jint parity, jint fragmentBytes,
                                            jbyteArray outPayload, jintArray outStats) {
  g_lastError.clear();

  Audio rx;
  if (!toAudio(env, rxArray, rx)) return -1;

  try {
    const SpreadModem modem(spreadFor(sampleRate));
    const auto dec = decodeMessage(rx, modem, transmitFor(parity, fragmentBytes));

    if (outStats != nullptr && env->GetArrayLength(outStats) >= 6) {
      const jint stats[6] = {
          dec.synced ? 1 : 0,
          static_cast<jint>(dec.framesRecovered),
          static_cast<jint>(dec.framesExpected),
          static_cast<jint>(dec.rsCorrectedSymbols),
          static_cast<jint>(dec.rsErasedSymbols),
          static_cast<jint>(dec.rsFailedBlocks),
      };
      env->SetIntArrayRegion(outStats, 0, 6, stats);
    }

    if (!dec.ok) {
      setError(dec.error.empty() ? "decode failed" : dec.error);
      return dec.synced ? -2 : -3;
    }

    if (outPayload == nullptr) {
      setError("outPayload is null");
      return -1;
    }
    const jsize capacity = env->GetArrayLength(outPayload);
    if (static_cast<jsize>(dec.payload.size()) > capacity) {
      setError("outPayload holds " + std::to_string(capacity) + " bytes but the message needs " +
               std::to_string(dec.payload.size()));
      return -4;
    }
    if (!dec.payload.empty()) {
      env->SetByteArrayRegion(outPayload, 0, static_cast<jsize>(dec.payload.size()),
                              reinterpret_cast<const jbyte*>(dec.payload.data()));
    }
    return static_cast<jint>(dec.payload.size());
  } catch (const std::exception& e) {
    setError(e.what());
    return -1;
  }
}

/// Audio seconds a payload of this size needs, so the app can size a recording
/// window before it starts one. Negative on failure.
JNIEXPORT jdouble JNICALL
Java_dev_vacuum_android_Vacuum_nativeEstimateSeconds(JNIEnv* /*env*/, jobject /*self*/,
                                                     jint payloadBytes, jint sampleRate,
                                                     jint parity, jint fragmentBytes) {
  g_lastError.clear();
  try {
    const SpreadModem modem(spreadFor(sampleRate));
    return estimateSeconds(static_cast<std::size_t>(payloadBytes), modem,
                           transmitFor(parity, fragmentBytes));
  } catch (const std::exception& e) {
    setError(e.what());
    return -1.0;
  }
}

/// The link parameters the other side must match, as a single human-readable
/// line. Exposed because a mismatch between transmitter and receiver is the
/// most common reason a decode fails, and the app should be able to show it.
JNIEXPORT jstring JNICALL
Java_dev_vacuum_android_Vacuum_nativeDescribeLink(JNIEnv* env, jobject /*self*/, jint sampleRate,
                                                  jint parity, jint fragmentBytes) {
  try {
    const SpreadConfig cfg = spreadFor(sampleRate);
    const SpreadModem modem(cfg);
    const TransmitConfig t = transmitFor(parity, fragmentBytes);
    // `netBitRate()` is the modem layer before framing and parity, which is not
    // what the user gets; derive the delivered rate from the same estimator the
    // encoder sizes the recording with.
    const double delivered = 8192.0 / std::max(1e-9, estimateSeconds(1024, modem, t));
    const std::string text = "band " + std::to_string(static_cast<int>(cfg.lowHz)) + "-" +
                             std::to_string(static_cast<int>(cfg.highHz)) + " Hz, " +
                             std::to_string(modem.sampleRate()) + " Hz, repeat " +
                             std::to_string(cfg.repeat) + ", parity " +
                             std::to_string(t.rsParitySymbols) + ", fragment<=" +
                             std::to_string(fragmentPayloadSize(t.maxFragmentSize)) + " B, " +
                             std::to_string(static_cast<int>(delivered)) + " bps delivered";
    return env->NewStringUTF(text.c_str());
  } catch (const std::exception& e) {
    return env->NewStringUTF(e.what());
  }
}

}  // extern "C"
