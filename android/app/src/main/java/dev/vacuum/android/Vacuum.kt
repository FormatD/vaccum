package dev.vacuum.android

/**
 * Kotlin facade over the shared C++ core.
 *
 * The core works on mono float PCM in [-1, 1]. Everything Android-specific --
 * microphone capture, playback, MP3 decoding, storage permissions -- lives in
 * this layer, so the same `libvacuum_core.a` the desktop CLI links is what runs
 * on the phone.
 *
 * `parity` and `fragmentBytes` are link parameters: the receiver must be given
 * the same values the transmitter used, or it will slice the bit stream into
 * blocks at the wrong boundaries. `describeLink` renders them for display.
 */
object Vacuum {

    /** Payload larger than this is rejected by the core's framing layer. */
    const val MAX_PAYLOAD_BYTES = 8192

    init {
        System.loadLibrary("vacuum_jni")
    }

    /** Diagnostics from a decode, mirroring the core's `DecodeResult`. */
    data class Stats(
        val synced: Boolean,
        val framesRecovered: Int,
        val framesExpected: Int,
        val rsCorrected: Int,
        val rsErased: Int,
        val rsFailedBlocks: Int,
    ) {
        val summary: String
            get() = "sync=${if (synced) "yes" else "no"} frames=$framesRecovered/$framesExpected" +
                " rs corrected=$rsCorrected erased=$rsErased failed=$rsFailedBlocks"
    }

    /** Outcome of a decode attempt. `payload` is null unless [ok]. */
    data class Decode(
        val ok: Boolean,
        val payload: ByteArray?,
        val error: String?,
        val stats: Stats,
    )

    /** Thrown when the core refuses to encode, or when the band will not fit. */
    class LinkException(message: String) : Exception(message)

    /**
     * Mix [payload] into [host] and return the watermarked audio.
     *
     * An empty [host] produces the watermark on its own, which is audible: with
     * no music to mask it the tones run at the core's floor amplitude. Pass real
     * audio whenever the point is to hide the transmission.
     */
    @JvmOverloads
    fun encode(
        payload: ByteArray,
        host: FloatArray = FloatArray(0),
        sampleRate: Int = 48_000,
        parity: Int = 64,
        fragmentBytes: Int = 64,
    ): FloatArray {
        val audio = nativeEncode(payload, host, sampleRate, parity, fragmentBytes)
            ?: throw LinkException(nativeLastError().ifEmpty { "encode failed" })
        return audio
    }

    /**
     * Recover a payload from a capture. Never throws: a failed decode is the
     * normal outcome on a bad channel and the caller needs the diagnostics to
     * tell "wrong settings" from "too noisy".
     *
     * [maxPayloadBytes] sizes the buffer the core writes into. The physical
     * header carries the true length, so this only has to be an upper bound.
     */
    @JvmOverloads
    fun decode(
        rx: FloatArray,
        sampleRate: Int = 48_000,
        parity: Int = 64,
        fragmentBytes: Int = 64,
        maxPayloadBytes: Int = MAX_PAYLOAD_BYTES,
    ): Decode {
        val out = ByteArray(maxPayloadBytes)
        val stats = IntArray(6)
        val n = nativeDecode(rx, sampleRate, parity, fragmentBytes, out, stats)
        val s = Stats(
            synced = stats[0] != 0,
            framesRecovered = stats[1],
            framesExpected = stats[2],
            rsCorrected = stats[3],
            rsErased = stats[4],
            rsFailedBlocks = stats[5],
        )
        return if (n >= 0) {
            Decode(ok = true, payload = out.copyOf(n), error = null, stats = s)
        } else {
            Decode(ok = false, payload = null, error = nativeLastError(), stats = s)
        }
    }

    /** Audio seconds a payload of this size occupies, preamble included. */
    @JvmOverloads
    fun estimateSeconds(
        payloadBytes: Int,
        sampleRate: Int = 48_000,
        parity: Int = 64,
        fragmentBytes: Int = 64,
    ): Double = nativeEstimateSeconds(payloadBytes, sampleRate, parity, fragmentBytes)

    /** The link parameters as one line, for showing the user what must match. */
    @JvmOverloads
    fun describeLink(
        sampleRate: Int = 48_000,
        parity: Int = 64,
        fragmentBytes: Int = 64,
    ): String = nativeDescribeLink(sampleRate, parity, fragmentBytes)

    private external fun nativeEncode(
        payload: ByteArray,
        host: FloatArray,
        sampleRate: Int,
        parity: Int,
        fragmentBytes: Int,
    ): FloatArray?

    private external fun nativeDecode(
        rx: FloatArray,
        sampleRate: Int,
        parity: Int,
        fragmentBytes: Int,
        outPayload: ByteArray,
        outStats: IntArray,
    ): Int

    private external fun nativeEstimateSeconds(
        payloadBytes: Int,
        sampleRate: Int,
        parity: Int,
        fragmentBytes: Int,
    ): Double

    private external fun nativeDescribeLink(
        sampleRate: Int,
        parity: Int,
        fragmentBytes: Int,
    ): String

    private external fun nativeLastError(): String
}
