package dev.vacuum.android

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Bundle
import android.widget.Button
import android.widget.EditText
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import java.io.File
import java.util.concurrent.Executors

/**
 * Minimal driver for the link: pick a host track, type a payload, transmit it
 * through the speaker or save it to a file, then recover it from the microphone.
 *
 * The interesting code is in [Vacuum], [AudioIO] and [AacWriter]; this activity
 * only wires them to buttons and a log, and keeps the audio work off the main
 * thread because a decode is a few seconds of DSP.
 */
class MainActivity : AppCompatActivity() {

    private val worker = Executors.newSingleThreadExecutor()

    private lateinit var log: TextView
    private lateinit var payloadField: EditText
    private lateinit var hostLabel: TextView

    private var hostUri: Uri? = null

    private val pickHost =
        registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
            hostUri = uri
            // Persist so the Uri survives a process restart; without this the
            // grant expires and the next encode fails to open the file.
            uri?.let {
                contentResolver.takePersistableUriPermission(
                    it, Intent.FLAG_GRANT_READ_URI_PERMISSION,
                )
            }
            hostLabel.text = uri?.lastPathSegment ?: getString(R.string.no_host)
            appendLog("host: ${uri ?: "none"}")
        }

    private val requestMic =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
            if (granted) startCapture() else appendLog("RECORD_AUDIO denied; cannot receive")
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        log = findViewById(R.id.log)
        payloadField = findViewById(R.id.payload)
        hostLabel = findViewById(R.id.host_label)

        findViewById<Button>(R.id.pick_host).setOnClickListener {
            pickHost.launch(arrayOf("audio/*"))
        }
        findViewById<Button>(R.id.play).setOnClickListener { transmit(save = false) }
        findViewById<Button>(R.id.save).setOnClickListener { transmit(save = true) }
        findViewById<Button>(R.id.receive).setOnClickListener {
            if (hasMicPermission()) startCapture() {
                requestMic.launch(Manifest.permission.RECORD_AUDIO)
            }
        }
        findViewById<Button>(R.id.decode_file).setOnClickListener {
            pickAndDecodeFile.launch(arrayOf("audio/*"))
        }

        appendLog(Vacuum.describeLink(sampleRate = RATE, parity = PARITY, fragmentBytes = FRAGMENT))
        appendLog("payload is hex; the default fits in one fragment")
    }

    private val pickAndDecodeFile =
        registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
            if (uri == null) return@registerForActivityResult
            appendLog("decoding $uri")
            worker.execute {
                runCatching {
                    // Decode at the file's own rate and run the modem at that
                    // rate. Resampling first would move the carriers.
                    val pcm = AudioIO.decodeFile(this, uri)
                    val result = Vacuum.decode(
                        pcm.samples, pcm.sampleRate, PARITY, FRAGMENT,
                    )
                    reportDecode(result, "file at ${pcm.sampleRate} Hz, %.2f s".format(pcm.seconds))
                }.onFailure { appendLog("failed: ${it.message}") }
            }
        }

    // --------------------------------------------------------------- transmit

    private fun transmit(save: Boolean) {
        val payload = parseHex(payloadField.text.toString())
        if (payload == null) {
            appendLog("payload must be an even number of hex digits")
            return
        }
        if (payload.isEmpty()) {
            appendLog("payload is empty")
            return
        }
        val uri = hostUri
        val seconds = Vacuum.estimateSeconds(payload.size, RATE, PARITY, FRAGMENT)
        appendLog("payload ${payload.size} B, about %.1f s of audio".format(seconds))

        worker.execute {
            runCatching {
                var host = FloatArray(0)
                if (uri != null) {
                    val decoded = AudioIO.decodeFile(this, uri)
                    // The host is masking material, so resampling it to the
                    // playback rate is harmless; see AudioIO for why the same
                    // is not true of a recording.
                    host = AudioIO.fitLength(
                        AudioIO.resample(decoded.samples, decoded.sampleRate, RATE),
                        seconds + 2.0, RATE,
                    )
                    appendLog("host %.2f s at %d Hz -> %d Hz".format(
                        decoded.seconds, decoded.sampleRate, RATE))
                } else {
                    appendLog("no host: the watermark will be audible on its own")
                }

                val audio = Vacuum.encode(payload, host, RATE, PARITY, FRAGMENT)
                val pcm = Pcm(audio, RATE)
                appendLog("encoded %.2f s, peak %.3f".format(pcm.seconds, AudioIO.peak(audio)))

                if (save) {
                    // App-private external storage: no permission prompt, and it
                    // is visible to a file manager so the result can be pulled
                    // off the device. Falls back to internal when unmounted.
                    val dir = getExternalFilesDir(null) ?: filesDir
                    val out = File(dir, "watermarked.m4a")
                    AacWriter.write(audio, RATE, out)
                    appendLog("saved ${out.absolutePath} (${out.length() / 1024} KiB)")
                } else {
                    appendLog("playing...")
                    AudioIO.play(pcm)
                    appendLog("playback finished")
                }
            }.onFailure { appendLog("transmit failed: ${it.message}") }
        }
    }

    // ---------------------------------------------------------------- receive

    private fun hasMicPermission(): Boolean =
        ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) ==
            PackageManager.PERMISSION_GRANTED

    private fun startCapture(onDenied: () -> Unit = {}) {
        if (!hasMicPermission()) {
            onDenied()
            return
        }
        // When receiving we do not know the sender's payload size, so the
        // recording window is estimated from whatever is typed in the payload
        // field. For a loopback against this device that is the right guess;
        // against a different transmitter it is only a starting point, and a
        // window that is too short truncates the message.
        val seconds = Vacuum.estimateSeconds(
            maxOf(payloadField.text.toString().length / 2, 16), RATE, PARITY, FRAGMENT,
        )
        // A second of slack at each end: the transmitter is never exactly
        // aligned with the recording, and the acquisition preamble needs room.
        val window = seconds + 2.0
        appendLog("recording %.1f s at %d Hz...".format(window, RATE))

        worker.execute {
            runCatching {
                val pcm = AudioIO.record(window, RATE)
                appendLog("captured %.2f s, peak %.3f".format(pcm.seconds, AudioIO.peak(pcm.samples)))
                val result = Vacuum.decode(pcm.samples, RATE, PARITY, FRAGMENT)
                reportDecode(result, "microphone at $RATE Hz")
            }.onFailure { appendLog("capture failed: ${it.message}") }
        }
    }

    private fun reportDecode(result: Vacuum.Decode, source: String) {
        appendLog("[$source] ${result.stats.summary}")
        if (!result.ok) {
            appendLog("decode failed: ${result.error}")
            return
        }
        val payload = result.payload ?: return
        appendLog("recovered ${payload.size} B")
        appendLog("hex: ${toHex(payload)}")
        payload.toString(Charsets.UTF_8).let { text ->
            if (text.isNotEmpty() && text.all { it.code in 32..126 }) {
                appendLog("text: $text")
            }
        }
    }

    // ------------------------------------------------------------------- misc

    private fun appendLog(line: String) {
        runOnUiThread {
            log.append(line + "\n")
        }
    }

    override fun onDestroy() {
        worker.shutdown()
        super.onDestroy()
    }

    private companion object {
        /** Must match on both ends of the link; 48 kHz is what AudioRecord guarantees. */
        const val RATE = AudioIO.RATE_48K
        const val PARITY = 64
        const val FRAGMENT = 64

        fun parseHex(text: String): ByteArray? {
            val clean = text.filter { it.isDigit() || it.lowercaseChar() in 'a'..'f' }
            if (clean.length % 2 != 0) return null
            return ByteArray(clean.length / 2) { i ->
                clean.substring(i * 2, i * 2 + 2).toInt(16).toByte()
            }
        }

        fun toHex(bytes: ByteArray): String =
            bytes.joinToString("") { "%02x".format(it) }
    }
}
