package dev.vacuum.android

import android.annotation.SuppressLint
import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.AudioTrack
import android.media.MediaCodec
import android.media.MediaExtractor
import android.media.MediaFormat
import android.media.MediaRecorder
import android.net.Uri
import java.io.ByteArrayOutputStream
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min

/** Decoded audio plus the rate it was decoded at. */
data class Pcm(val samples: FloatArray, val sampleRate: Int) {
    val seconds: Double get() = samples.size.toDouble() / sampleRate

    override fun equals(other: Any?): Boolean =
        other is Pcm && sampleRate == other.sampleRate && samples.contentEquals(other.samples)

    override fun hashCode(): Int = 31 * sampleRate + samples.contentHashCode()
}

/**
 * Microphone, speaker and file decoding.
 *
 * The core insists that the modem's sample rate equals the audio's, because its
 * carrier bins are absolute frequencies. That drives the two rules below:
 *
 *  - A file is decoded at its own rate and fed to the modem at that rate. Never
 *    resample a *recording* before decoding -- interpolation moves the carriers
 *    and smears the tones the detector is looking for.
 *  - A host track may be resampled freely, because it is only masking material.
 *    The watermark is generated fresh at the capture/playback rate; small
 *    artefacts in the music underneath it cost nothing.
 */
object AudioIO {

    /** AudioRecord guarantees these two rates on every device. */
    const val RATE_48K = 48_000
    const val RATE_44K = 44_100

    class AudioError(message: String, cause: Throwable? = null) : Exception(message, cause)

    // ---------------------------------------------------------------- capture

    /**
     * Record [seconds] of mono audio at [sampleRate].
     *
     * Uses the voice-recognition preset rather than the default mic preset: the
     * default route on many handsets applies automatic gain control and noise
     * suppression, both of which attack a low-level tone the way a noise gate
     * would. Voice recognition keeps the path closer to linear.
     */
    @SuppressLint("MissingPermission")
    fun record(seconds: Double, sampleRate: Int = RATE_48K): Pcm {
        val minBuffer = AudioRecord.getMinBufferSize(
            sampleRate,
            AudioFormat.CHANNEL_IN_MONO,
            AudioFormat.ENCODING_PCM_16BIT,
        )
        if (minBuffer <= 0) {
            throw AudioError("the device will not record mono 16-bit at $sampleRate Hz")
        }
        // A tenth of a second of slack absorbs the time between startRecording
        // and the first buffer being ready, which is otherwise dropped audio at
        // the head of the capture -- exactly where the acquisition preamble is.
        val totalSamples = (seconds * sampleRate).toInt() + sampleRate / 10
        val frameBytes = max(minBuffer, totalSamples * 2)

        val recorder = AudioRecord(
            MediaRecorder.AudioSource.VOICE_RECOGNITION,
            sampleRate,
            AudioFormat.CHANNEL_IN_MONO,
            AudioFormat.ENCODING_PCM_16BIT,
            frameBytes,
        )
        if (recorder.state != AudioRecord.STATE_INITIALIZED) {
            recorder.release()
            throw AudioError("AudioRecord failed to initialise; is RECORD_AUDIO granted?")
        }

        val out = FloatArray(totalSamples)
        val buffer = ByteArray(frameBytes)
        try {
            recorder.startRecording()
            if (recorder.recordingState != AudioRecord.RECORDSTATE_RECORDING) {
                throw AudioError("startRecording did not take effect")
            }
            var written = 0
            while (written < totalSamples) {
                val want = min(buffer.size, (totalSamples - written) * 2)
                val n = recorder.read(buffer, 0, want)
                if (n <= 0) {
                    if (n == AudioRecord.ERROR_INVALID_OPERATION ||
                        n == AudioRecord.ERROR_BAD_VALUE
                    ) {
                        throw AudioError("AudioRecord.read failed with $n")
                    }
                    continue
                }
                for (i in 0 until n / 2) {
                    val lo = buffer[2 * i].toInt() and 0xFF
                    val hi = buffer[2 * i + 1].toInt()
                    out[written + i] = ((hi shl 8) or lo).toShort() / 32768.0f
                }
                written += n / 2
            }
        } finally {
            try {
                recorder.stop()
            } catch (_: IllegalStateException) {
                // Never started, or already stopped; nothing to clean up.
            }
            recorder.release()
        }
        return Pcm(out, sampleRate)
    }

    // --------------------------------------------------------------- playback

    /** Play mono audio. Blocks until the last sample has been written. */
    fun play(pcm: Pcm) {
        val minBuffer = AudioTrack.getMinBufferSize(
            pcm.sampleRate,
            AudioFormat.CHANNEL_OUT_MONO,
            AudioFormat.ENCODING_PCM_16BIT,
        )
        if (minBuffer <= 0) {
            throw AudioError("the device will not play mono 16-bit at ${pcm.sampleRate} Hz")
        }
        val track = AudioTrack.Builder()
            .setAudioAttributes(
                AudioAttributes.Builder()
                    // MEDIA rather than SONIFICATION: some handsets route the
                    // notification stream through a band-limited speaker path.
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                    .build(),
            )
            .setAudioFormat(
                AudioFormat.Builder()
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setSampleRate(pcm.sampleRate)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                    .build(),
            )
            .setBufferSizeInBytes(max(minBuffer, pcm.samples.size * 2))
            .setTransferMode(AudioTrack.MODE_STATIC)
            .build()

        val buffer = ByteArray(pcm.samples.size * 2)
        for (i in pcm.samples.indices) {
            val v = (pcm.samples[i].coerceIn(-1.0f, 1.0f) * 32767.0f).toInt().toShort()
            buffer[2 * i] = (v.toInt() and 0xFF).toByte()
            buffer[2 * i + 1] = (v.toInt() shr 8).toByte()
        }
        try {
            track.write(buffer, 0, buffer.size)
            track.play()
            // MODE_STATIC plays the buffer once; wait it out rather than racing
            // the release, which would truncate the tail of the message.
            val micros = (pcm.seconds * 1_000_000).toLong() + 250_000
            var waited = 0L
            while (waited < micros && track.playState == AudioTrack.PLAYSTATE_PLAYING) {
                Thread.sleep(50)
                waited += 50_000
            }
        } finally {
            try {
                track.stop()
            } catch (_: IllegalStateException) {
                // Already stopped.
            }
            track.release()
        }
    }

    // ----------------------------------------------------------- file decode

    /**
     * Decode an audio file to mono float PCM at the file's own sample rate.
     * Accepts a content:// Uri from the system picker as well as file:// paths.
     */
    fun decodeFile(context: Context, uri: Uri): Pcm {
        val extractor = MediaExtractor()
        try {
            extractor.setDataSource(context, uri, null)
        } catch (e: Exception) {
            extractor.release()
            throw AudioError("cannot open this file as audio", e)
        }

        val trackIndex = (0 until extractor.trackCount).firstOrNull {
            extractor.getTrackFormat(it).getString(MediaFormat.KEY_MIME)?.startsWith("audio/") == true
        } ?: run {
            extractor.release()
            throw AudioError("no audio track in this file")
        }
        extractor.selectTrack(trackIndex)
        val format = extractor.getTrackFormat(trackIndex)
        val channels = if (format.containsKey(MediaFormat.KEY_CHANNEL_COUNT)) {
            max(1, format.getInteger(MediaFormat.KEY_CHANNEL_COUNT))
        } else {
            1
        }

        val mime = format.getString(MediaFormat.KEY_MIME)!!
        val codec = MediaCodec.createDecoderByType(mime)
        val pcm = ByteArrayOutputStream()
        val info = MediaCodec.BufferInfo()
        // The container's declared rate is a hint; the decoder's output format
        // is authoritative, and for some AAC streams they differ.
        var sampleRate = if (format.containsKey(MediaFormat.KEY_SAMPLE_RATE)) {
            format.getInteger(MediaFormat.KEY_SAMPLE_RATE)
        } else {
            RATE_48K
        }
        var outChannels = channels

        try {
            codec.configure(format, null, null, 0)
            codec.start()

            var inputDone = false
            var outputDone = false
            var idlePolls = 0
            while (!outputDone) {
                if (idlePolls > MAX_IDLE_POLLS) {
                    throw AudioError("the decoder stalled before reporting end of stream")
                }
                if (!inputDone) {
                    val index = codec.dequeueInputBuffer(TIMEOUT_MICROS)
                    if (index >= 0) {
                        val buffer = codec.getInputBuffer(index)!!
                        val n = extractor.readSampleData(buffer, 0)
                        if (n < 0) {
                            codec.queueInputBuffer(
                                index, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM,
                            )
                            inputDone = true
                        } else {
                            codec.queueInputBuffer(index, 0, n, extractor.sampleTime, 0)
                            extractor.advance()
                        }
                    }
                }

                when (val index = codec.dequeueOutputBuffer(info, TIMEOUT_MICROS)) {
                    MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                        idlePolls = 0
                        val out = codec.outputFormat
                        if (out.containsKey(MediaFormat.KEY_SAMPLE_RATE)) {
                            sampleRate = out.getInteger(MediaFormat.KEY_SAMPLE_RATE)
                        }
                        if (out.containsKey(MediaFormat.KEY_CHANNEL_COUNT)) {
                            outChannels = max(1, out.getInteger(MediaFormat.KEY_CHANNEL_COUNT))
                        }
                    }
                    MediaCodec.INFO_TRY_AGAIN_LATER -> {
                        // Nothing ready this pass. Once the input is finished
                        // that is only normal for a bounded number of polls.
                        ++idlePolls
                        if (inputDone) continue
                    }
                    else -> if (index >= 0) {
                        idlePolls = 0
                        val buffer = codec.getOutputBuffer(index)!!
                        if (info.size > 0) {
                            val bytes = ByteArray(info.size)
                            buffer.position(info.offset)
                            buffer.get(bytes, 0, info.size)
                            pcm.write(bytes, 0, info.size)
                        }
                        codec.releaseOutputBuffer(index, false)
                        if (info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) {
                            outputDone = true
                        }
                    }
            }
        } catch (e: Exception) {
            throw AudioError("decoding failed", e)
        } finally {
            try {
                codec.stop()
            } catch (_: IllegalStateException) {
                // Already stopped or never started.
            }
            codec.release()
            extractor.release()
        }

        return Pcm(interleaveToMono(pcm.toByteArray(), outChannels), sampleRate)
    }

    /** Downmix decoder output (16-bit little-endian interleaved) to mono. */
    private fun interleaveToMono(pcm16: ByteArray, channels: Int): FloatArray {
        val frames = pcm16.size / 2 / channels
        val out = FloatArray(frames)
        for (f in 0 until frames) {
            var sum = 0
            for (c in 0 until channels) {
                val i = (f * channels + c) * 2
                if (i + 1 >= pcm16.size) break
                val lo = pcm16[i].toInt() and 0xFF
                val hi = pcm16[i + 1].toInt()
                sum += ((hi shl 8) or lo).toShort().toInt()
            }
            out[f] = (sum.toFloat() / channels) / 32768.0f
        }
        return out
    }

    // ------------------------------------------------------------- resampling

    /**
     * Linear resample. Only ever applied to a *host* track, where it is
     * harmless; see the note at the top of this file for why a recording must
     * not go through it.
     */
    fun resample(samples: FloatArray, fromRate: Int, toRate: Int): FloatArray {
        if (fromRate == toRate || samples.isEmpty()) return samples
        val outLength = (samples.size.toLong() * toRate / fromRate).toInt()
        val out = FloatArray(outLength)
        val step = fromRate.toDouble() / toRate
        for (i in 0 until outLength) {
            val pos = i * step
            val lo = pos.toInt()
            val hi = min(lo + 1, samples.size - 1)
            val frac = (pos - lo).toFloat()
            out[i] = samples[lo] * (1.0f - frac) + samples[hi] * frac
        }
        return out
    }

    /** Trim or zero-pad a host so it covers [seconds]. */
    fun fitLength(samples: FloatArray, seconds: Double, sampleRate: Int): FloatArray =
        samples.copyOf((seconds * sampleRate).toInt())

    /** Peak absolute amplitude, for reporting whether a capture clipped. */
    fun peak(samples: FloatArray): Float =
        if (samples.isEmpty()) 0.0f else samples.maxOf { abs(it) }

    private const val TIMEOUT_MICROS = 10_000L

    /** 10 ms per poll, so this is a couple of seconds of the decoder producing nothing. */
    private const val MAX_IDLE_POLLS = 200
}
