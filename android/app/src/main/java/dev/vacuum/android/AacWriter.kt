package dev.vacuum.android

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.media.MediaMuxer
import java.io.File
import java.nio.ByteBuffer

/**
 * Encode PCM to an AAC file in an MP4 container, using the platform encoder.
 *
 * This is the Android counterpart of the desktop tool's MP3 output. Android has
 * no MP3 encoder -- only a decoder -- so the native lossy format here is AAC.
 * The two are equivalent for this purpose: both are perceptual coders driven by
 * a masking model, and the watermark is designed to sit under that model rather
 * than to survive a specific codec. The desktop measurements were taken against
 * libmp3lame at 128 kbps (see `vacuum_bench --mode matrix`); the AAC path has
 * not been measured, so treat its margin as unverified until it has been.
 */
object AacWriter {

    private const val MIME = "audio/mp4a-latm"
    private const val TIMEOUT_MICROS = 10_000L

    /** 10 ms per poll, so this is a couple of seconds of the encoder producing nothing. */
    private const val MAX_IDLE_POLLS = 200

    class EncodeError(message: String, cause: Throwable? = null) : Exception(message, cause)

    /**
     * Write [samples] to [output] as mono AAC at [bitRate].
     *
     * [bitRate] matters more than it looks: a watermark sitting roughly 10 dB
     * under the music is close enough to the coding threshold that a very low
     * rate will start removing it. 128 kbps is what was measured on desktop.
     */
    fun write(
        samples: FloatArray,
        sampleRate: Int,
        output: File,
        bitRate: Int = 128_000,
    ) {
        if (samples.isEmpty()) throw EncodeError("nothing to encode")

        val format = MediaFormat.createAudioFormat(MIME, sampleRate, 1).apply {
            setInteger(MediaFormat.KEY_BIT_RATE, bitRate)
            setInteger(MediaFormat.KEY_AAC_PROFILE, MediaCodecInfo.CodecProfileLevel.AACObjectLC)
            setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 1 shl 16)
        }

        val codec = MediaCodec.createEncoderByType(MIME)
        val muxer = MediaMuxer(output.absolutePath, MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4)
        var muxerTrack = -1
        var muxerStarted = false

        // The encoder wants 16-bit PCM. Converting the whole array up front
        // would double peak memory for a long track, so it is done per buffer.
        val info = MediaCodec.BufferInfo()
        var inputPosition = 0
        var inputDone = false
        var outputDone = false
        var idlePolls = 0

        try {
            codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            codec.start()

            while (!outputDone) {
                if (idlePolls > MAX_IDLE_POLLS) {
                    throw EncodeError("the encoder stalled before reporting end of stream")
                }
                if (!inputDone) {
                    val index = codec.dequeueInputBuffer(TIMEOUT_MICROS)
                    if (index >= 0) {
                        val buffer = codec.getInputBuffer(index)!!
                        buffer.clear()
                        val remaining = samples.size - inputPosition
                        val frames = minOf(remaining, buffer.capacity() / 2)
                        for (i in 0 until frames) {
                            val v = (samples[inputPosition + i].coerceIn(-1.0f, 1.0f) * 32767.0f)
                                .toInt().toShort()
                            buffer.put((v.toInt() and 0xFF).toByte())
                            buffer.put((v.toInt() shr 8).toByte())
                        }
                        inputPosition += frames
                        val flags = if (frames == 0) {
                            inputDone = true
                            MediaCodec.BUFFER_FLAG_END_OF_STREAM
                        } else {
                            0
                        }
                        // presentationTimeUs must increase monotonically or the
                        // muxer rejects the sample.
                        val pts = inputPosition.toLong() * 1_000_000L / sampleRate
                        codec.queueInputBuffer(index, 0, frames * 2, pts, flags)
                    }
                }

                when (val index = codec.dequeueOutputBuffer(info, TIMEOUT_MICROS)) {
                    MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                        idlePolls = 0
                        if (muxerStarted) throw EncodeError("the encoder changed format twice")
                        muxerTrack = muxer.addTrack(codec.outputFormat)
                        muxer.start()
                        muxerStarted = true
                    }
                    MediaCodec.INFO_TRY_AGAIN_LATER -> {
                        ++idlePolls
                        if (inputDone) continue
                    }
                    else -> if (index >= 0) {
                        idlePolls = 0
                        val buffer: ByteBuffer = codec.getOutputBuffer(index)!!
                        // CODEC_CONFIG is the AudioSpecificConfig; it belongs in
                        // the track format, which addTrack already carried, so
                        // writing it as a sample would corrupt the stream.
                        val isConfig = info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0
                        if (!isConfig && info.size > 0 && muxerStarted) {
                            buffer.position(info.offset)
                            buffer.limit(info.offset + info.size)
                            muxer.writeSampleData(muxerTrack, buffer, info)
                        }
                        codec.releaseOutputBuffer(index, false)
                        if (info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) {
                            outputDone = true
                        }
                    }
                }
            }
        } catch (e: EncodeError) {
            output.delete()
            throw e
        } catch (e: Exception) {
            output.delete()
            throw EncodeError("AAC encoding failed", e)
        } finally {
            try {
                codec.stop()
            } catch (_: IllegalStateException) {
                // Already stopped.
            }
            codec.release()
            if (muxerStarted) {
                try {
                    muxer.stop()
                } catch (_: Exception) {
                    // Nothing more to do with a partially written file.
                }
            }
            muxer.release()
        }

        if (!muxerStarted) {
            output.delete()
            throw EncodeError("the encoder produced no output format")
        }
    }
}
