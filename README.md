# vacuum

Binary payloads carried by audio, hidden inside an ordinary music file.

Play a watermarked MP3 through earphones, capture it with a microphone a metre
away, recover the bytes. The watermark sits roughly 10 dB under the music and
survives MP3 compression, an unknown air-path delay, band-limited speakers and
background noise.

```
vacuum watermark --host song.mp3 --data payload.bin --out tagged.mp3
#  … play tagged.mp3 through earphones into a microphone …
vacuum mic --seconds 20 --out payload.out
```

**50-90 bps of delivered payload** at 48 kHz -- 47.8 bps for a 40-byte message,
76.6 for 120 bytes, 86.9 for a kilobyte (`android/host-jni`, which encodes each
size and divides payload bits by audio seconds). That is deliberately small: the
point is a few hundred bytes -- a token, a pairing code, a URL -- not file
transfer. The modem layer puts out 200 bps, and framing, CRC, Reed-Solomon
parity and the acquisition preamble take 57-76% of it -- the figures above are
what survives.

## Every number in this file is a measurement

Nothing here is a back-of-envelope estimate. Each table was produced by
`tests/vacuum_bench` -- except the delivered-throughput figures, which come from
`android/host-jni` encoding each payload size and dividing payload bits by audio
seconds -- and the command that regenerates it is printed underneath.
Benchmark input is a deterministic pseudo-random payload and a synthesised host
track (seeded sum-of-tones with a shaped envelope), so re-running reproduces the
same numbers on the same machine.

Where a number is *not* measured, it says so.

## Build

Requires CMake 3.24, a C++20 compiler, and ffmpeg on the PATH to read anything
other than WAV. MP3 encoding additionally needs libmp3lame; without it the
build succeeds and `watermark` refuses `.mp3` output with a clear message.

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure     # 73 cases
```

macOS: `brew install cmake ffmpeg lame`. Debian/Ubuntu: `apt install cmake
ffmpeg libmp3lame-dev`.

Artifacts:

| path | what |
| --- | --- |
| `build/desktop/vacuum` | the command line tool |
| `build/tests/vacuum_tests` | unit tests |
| `build/tests/vacuum_bench` | the benchmark that produces every number below |

## How it works

```
payload bytes
  -> frames          (header + index + fragment + CRC-32 of the whole payload)
  -> Reed-Solomon    (one GF(256) block per frame)
  -> physical header (fragment size and count, CRC-16, repeated 4x, majority voted)
  -> OFDM/MFSK modem (masked by a psychoacoustic model of the host)
  -> added to the host track
```

**Modem.** CP-OFDM in shape, non-coherent MFSK in detection. Each logical
symbol picks one tone group in each of 8 independent sub-bands between 1.1 and
9.4 kHz, and the receiver decides by comparing band energies after an FFT -- no
phase tracking, which is what lets it survive an air path with an unknown and
drifting delay. Symbols repeat 3x and the energies sum. A 16-chip LFSR preamble
is found by FFT-based correlation to give the time and frequency reference.

**Hiding.** A Schroeder spreading function over a Bark-scale filter bank gives a
per-bin masking threshold; each carrier may go as loud as the music already is
at its bin, plus a 6 dB margin. There is a second, harder constraint: the
receiver finds a tone by subtracting its estimate of the host energy at the same
bin, leaving `|T|² + 2·Re(H·T*)`, which can go negative if the tone is not the
louder of the two. So each tone also has a floor of `10 × ` the host's own-bin
amplitude. Where a host partial sits exactly on a carrier that floor binds, not
the masking -- and spending the energy there is nearly free, because it lands on
top of a partial that is already audible.

**Band choice: 1.1–9.4 kHz.** Chosen for the stated use, and the trade is
measured. `--high-hz 12000` extends the band upward, and at 10 trials it is
*better* through a file -- clean loopback BER 2.84e-03 against 9.42e-03, with
more carriers to spread the message over. The five rows that only add MP3, alone
or with noise, still hold 10/10. Two rows the default passes drop to 8/10:
`+ noise 12 dB`, and `mp3 128k + lowpass 8k + noise 18 dB`, where the cutoff
lands inside the new top of the band. The roll-off rows fall hardest: lowpass
8 kHz 9/10 against 10/10, 6 kHz 5/10 against 10/10, 4 kHz 0/10 against 4/10. Not
a loudness effect; the wider band sits 11.3 dB below the host, slightly deeper
than 10.5. Earbud drivers and phone microphone capsules are what roll off up
there, so carriers past ~9.5 kHz are paid for in file performance and spent in
the air. The bottom of the band was not swept; below ~1 kHz the watermark would
land on the bass and vocals that people actually listen to.

**Host baseline removal.** The receiver estimates what the host alone was doing
at each carrier bin, two ways: spectrally from neighbouring probe bins, and
temporally from the same bin in nearby symbols where no tone was sent. The
median over dark symbols, scaled by each symbol's own broadband level, is what
gets subtracted. Without this, host partials sitting on carrier bins dominate
the energy comparison.

**Error correction.** Reed-Solomon over GF(256), 64 parity symbols per block:
Berlekamp-Massey for the locator, Chien to find positions, Forney for values.
Frames already know which symbols are suspect from the soft decision, so the
decoder runs errors *and* erasures: an erasure costs one parity symbol where an
unlocated error costs two, doubling the correction capacity for the same
overhead. A block that fails error decoding is retried with the least confident
`parity/2` symbols flagged.

**Verification, three layers.** CRC-16 on the physical header, CRC-32 over each
complete frame, CRC-32 over the reassembled payload. Reed-Solomon *will* return a
valid codeword that is not the one sent once damage passes its capacity, so `ok`
on a decode is not evidence of correct data -- this project measured that failure
(see the erasure-retry table) and it is the reason the CRC layers are not
redundant. A message is only reported
when every fragment decoded and the whole-payload CRC matched; otherwise the
decode fails cleanly with diagnostics.

## Measured results

### Message success

`vacuum_bench --mode matrix --payload 120 --trials 10`

A trial is one complete 120-byte payload. Success means the exact bytes came
back; a partial recovery counts as a failure. "air delay" is a 3333-sample
(69 ms) fractional delay, `lowpass N` models earbud/microphone band limits, and
noise is additive at the stated SNR.

| channel | success | raw BER | audio |
| --- | --- | --- | --- |
| clean file loopback | 10/10 | 9.42e-03 | 12.53 s |
| air delay + lowpass 11 kHz | 10/10 | 2.04e-02 | 12.53 s |
| + noise 25 dB | 10/10 | 2.13e-02 | 12.53 s |
| + noise 18 dB | 10/10 | 2.26e-02 | 12.53 s |
| + noise 12 dB | 10/10 | 2.80e-02 | 12.53 s |
| + lowpass 8 kHz | 10/10 | 2.83e-02 | 12.53 s |
| + lowpass 6 kHz | 10/10 | 3.46e-02 | 12.53 s |
| + lowpass 5 kHz | 9/10 | 4.16e-02 | 12.53 s |
| + lowpass 4 kHz | 4/10 | 5.11e-02 | 12.53 s |
| mp3 320 kbps | 10/10 | 9.37e-03 | 12.53 s |
| mp3 192 kbps | 10/10 | 9.78e-03 | 12.53 s |
| mp3 128 kbps | 10/10 | 8.97e-03 | 12.53 s |
| mp3 320k + noise 18 dB | 10/10 | 9.82e-03 | 12.53 s |
| mp3 128k + noise 18 dB | 10/10 | 1.02e-02 | 12.53 s |
| mp3 128k + lowpass 8k + noise 18 dB | 10/10 | 2.77e-02 | 12.53 s |

Over the span where the host exists, the whole watermark sits **10.5 dB below the
host RMS**. That is the aggregate of the 16 carriers active per symbol; the
masking model sets each one against its own host bin, subject to
`peakAmplitude`, so the aggregate is not a single knob.

MP3 is essentially free. 9.42e-03 clean against 8.97e-03 through 128 kbps --
encoding and decoding a lossy file did not measurably degrade the link, which is
the whole argument for the masking-model design. The air path is the fragile
part: BER rises to 2.8e-02 with a lowpass and noise, three times worse, and it
is the roll-off rather than the noise that does the damage. Every carrier above
the cutoff is gone, not merely noisy, so the lowpass rows degrade monotonically
and break at 4 kHz, where most of the 1.1-9.4 kHz band is removed.

### Raw bit error rate at the modem layer

`vacuum_bench --mode modem --payload 32 --trials 5` (6480 bits per channel)

| channel | errors | BER |
| --- | --- | --- |
| clean | 32 | 4.94e-03 |
| snr 30 dB | 35 | 5.40e-03 |
| snr 20 dB | 39 | 6.02e-03 |
| snr 10 dB | 70 | 1.08e-02 |
| snr 5 dB | 309 | 4.77e-02 |
| lowpass 10 kHz | 100 | 1.54e-02 |
| mp3 320 kbps | 31 | 4.78e-03 |
| mp3 128 kbps | 38 | 5.86e-03 |
| delay 4321.5 samples | 49 | 7.56e-03 |

A fractional delay is nearly free (7.6e-03 against 4.9e-03) -- that is the
non-coherent detector doing its job. Noise at 20 dB is nearly free too. Only
5 dB SNR and a lowpass that eats sub-bands bite.

### How much Reed-Solomon is worth

`vacuum_bench --mode matrix --payload 120 --trials 8 --fragment-size 64 --parity N`

Correct messages out of 8, at a fixed 64-byte fragment:

| channel | parity 32 | parity 48 | parity 64 |
| --- | --- | --- | --- |
| air delay + lowpass 11 kHz | 8/8 | 8/8 | 8/8 |
| + noise 12 dB | 0/8 | 7/8 | 8/8 |
| + lowpass 8 kHz | 1/8 | 8/8 | 8/8 |
| + lowpass 6 kHz | 0/8 | 7/8 | 8/8 |
| + lowpass 5 kHz | 0/8 | 3/8 | 8/8 |
| + lowpass 4 kHz | 0/8 | 1/8 | 4/8 |
| mp3 128k + lowpass 8k + noise 18 dB | 2/8 | 8/8 | 8/8 |
| audio seconds | 9.97 | 11.25 | 12.53 |

Parity is the strongest knob and 64 is the default: 26% more airtime buys back
every hard channel in the table except the 4 kHz cutoff, where it still only
gets to 4/8. The band-limited rows show it best -- at a 5 kHz cutoff parity 64 is
perfect where 32 never recovers a single message. Long messages are where the
gap widens: at 400 bytes (`--payload 400 --trials 6`, 41.73 s against 32.77 s)
parity 32 scores 0/6 on both the 8 kHz and 6 kHz rows where parity 64 gets 5/6
each. But 400 bytes also shows the ceiling -- at parity 64 the same run drops to
3/6 at a 5 kHz cutoff, 0/6 at 4 kHz, and 5/6 even on the delayed-but-wide row
that 120-byte messages pass 10 times in 10.

Shrinking the fragment block is a weaker lever than it looks. A message only
survives if *every* fragment does, so five small blocks are more all-or-nothing
draws than two large ones: `--fragment-size 24 --parity 32` improves the clean
BER, 8.9e-3 against 1.1e-2 with both measured at parity 32, yet still scores 7/8
on noise 18 dB -- worse than 64-byte fragments at parity 64 (8/8), for 15.65 s of
audio against 12.53 s.

### Does the erasure retry earn its keep?

`vacuum_bench --mode fec --payload 120 --trials 8` -- 16 RS blocks, two per trial
counted against ground truth rather than against the decoder's own claim of
success. `no retry` is error-only decoding; `csc` is the shipped cascade.

| channel | parity 64: no retry | parity 64: csc | parity 32: no retry | parity 32: csc |
| --- | --- | --- | --- | --- |
| clean file loopback | 16 | 16 | 16 | 16 |
| air delay + lowpass 11 kHz | 16 | 16 | 14 | 16 |
| + noise 18 dB | 16 | 16 | 13 | 14 (2 still failed) |
| + lowpass 8 kHz | 15 | 16 | 3 | 4 (12 still failed) |
| mp3 128 kbps | 16 | 16 | 16 | 16 |

Nothing in this table is a miscorrection: every block the two cascade columns
got wrong refused instead of returning a valid codeword, and the command's own
`miscorr` column reads zero in every `no retry` and `csc` row it printed, at
both parities.

At the default parity the retry rescues one block in eighty and is nearly
redundant, because 64 parity symbols already cover the damage. At parity 32 it
earns its keep: two blocks on the delayed channel, one each on noise 18 dB and
lowpass 8 kHz, four across the eighty. Even so it leaves 2 and 12 blocks still
failing on those last two channels -- which is why parity 64 is the default
rather than a cleverer decoder.

Flagging *all* `parity` symbols as erasures (the `k=32` and `k=64` rows the same
command prints) is a trap: the erasure budget consumes the whole parity, leaving
zero residual error capacity, and then the blocks do not fail -- they come back
*wrong and confident*. 2 to 10 of 16 miscorrect at parity 64, and on lowpass
8 kHz at parity 32 all sixteen do. Half the budget is the measured optimum.

### Detection floor: loudness against reliability

`vacuum_bench --mode matrix --payload 32 --trials 3` sweeping `--detect-floor`
over synthetic music; each row below is that command's clean file loopback line.

| detectFloorRatio | 2.0 | 4.0 | 6.0 | 8.0 | 10.0 | 14.0 |
| --- | --- | --- | --- | --- | --- | --- |
| clean bit error | 3.3e-2 | 2.9e-2 | 2.2e-2 | 1.5e-2 | 8.5e-3 | 5.9e-3 |
| payloads recovered | 1/3 | 3/3 | 3/3 | 3/3 | 3/3 | 3/3 |
| watermark vs host RMS | -20.6 dB | -15.2 | -12.4 | -10.9 | -10.3 | -9.6 |

5.6x fewer errors from the bottom of the sweep to the top, for 11 dB more level.
Most of the gain arrives between 6 and 10; 14.0 buys a further 1.4x for half a
dB, which is where `peakAmplitude` has taken over. The message row is the reason
not to shop for loudness here: at 2.0 -- "the smallest safe value" for the
detection inequality -- two of three payloads failed to come back at all *on a
clean file loopback*, because the Reed-Solomon block is a hard all-or-nothing
threshold on top of the bit error rate. 10.0 chosen on that curve.

## Command line

```
vacuum watermark --host SONG.mp3 --data PAYLOAD --out TAGGED.mp3
vacuum extract   --in TAGGED.mp3 [--out PAYLOAD]
vacuum mic       --seconds 20 [--out PAYLOAD] [--save-wav CAPTURE.WAV]
vacuum info      --in AUDIO
vacuum devices
```

`--data -` reads the payload from stdin; without `--out` an extracted payload
goes to stdout as hex. `--host` is optional -- omitting it produces the watermark
alone, which is audible.

`watermark` and `extract` run the modem at the sample rate the audio file
actually uses (most music is 44.1 kHz), so no resampling step is needed; `mic`
records at 48 kHz. Pass `--rate` to pin it. The two ends of a link must agree on
`--parity`, `--fragment` and the rate, and `vacuum info` prints what a file was
sent with.

| option | default | notes |
| --- | --- | --- |
| `--parity N` | 64 | RS parity per block; see the table above |
| `--fragment BYTES` | 64 | 8, 24, 64, 128 or 196 |
| `--low-hz` / `--high-hz` | 1100 / 9400 | carrier band |
| `--repeat N` | 3 | physical symbols per logical symbol |
| `--detect-floor R` | 10.0 | tone floor as a multiple of the host bin |
| `--kbps N` | 192 | MP3 bitrate when `--out` names an `.mp3` |
| `--offset SEC` | 0 | where in the host the watermark starts |

`vacuum_bench --mode sweep` re-derives the effect of each of these on two hard
channels, and is the way to check a change of default rather than trusting the
table above.

## Android

`android/` holds a Kotlin app plus a JNI bridge over the same `core/` sources --
`add_subdirectory`, not a copy, so the two cannot drift apart. `AudioRecord`
with the `VOICE_RECOGNITION` preset (the default route applies noise suppression
that attacks a low-level tone), `AudioTrack` for playback, `MediaExtractor` +
`MediaCodec` to decode files, and the platform AAC encoder for a watermarked
`.m4a`, since Android has no MP3 encoder.

**Verification status, stated plainly.** The native half is build- *and*
run-verified on this machine. `android/app/src/main/cpp/CMakeLists.txt`
configures and compiles `jni_bridge.cpp` plus the whole core with a host clang,
`-Wall -Wextra -Wpedantic`, no warnings. `android/host-jni` then mirrors the
five `external fun` declarations from `Vacuum.kt` in a Java class of the same
name, so a desktop JVM binds and executes them -- nineteen checks pass: 120
bytes encoded, decoded back byte-identical, sync 1.000, 2 of 2 fragments; the
same audio handed to the wrong parity fails cleanly with `-2` and "0/2 fragments
recovered" instead of returning data; a bare second of silence does not decode.

```
cmake -S android/app/src/main/cpp -B /tmp/vandroid \
  -DCMAKE_CXX_FLAGS="-I$JAVA_HOME/include -I$JAVA_HOME/include/darwin -Wall -Wextra -Wpedantic"
cmake --build /tmp/vandroid
javac -d /tmp/vjni_out android/host-jni/dev/vacuum/android/Vacuum.java
java -Djava.library.path=/tmp/vandroid -cp /tmp/vjni_out dev.vacuum.android.Vacuum
```

That is a JDK, not ART, and the harness is a hand-written mirror of the Kotlin
declarations rather than the compiled class -- so it evidences the bridge and its
symbol names, not the app. The Kotlin half, the Gradle files and the resource
files are **not** built or run; there is no Android SDK, NDK or Gradle on this
machine. Treat them as reviewed source, not tested code. Two consequences: the
version pins in `android/build.gradle.kts` are plausible rather than resolved,
and there is no Gradle wrapper, so a first build needs `gradle wrapper` or
Android Studio.

**No Android measurement exists.** Every table above was produced on desktop
against a synthesised channel. The AAC path in particular is unquantified --
AAC and MP3 are both perceptual coders, but the 128 kbps result above is a
libmp3lame measurement, not an argument that transfers.

## Layout and limits

```
core/     DSP and codec, no platform dependencies: crc, gf256, rs, fft,
          masking, physical layer, modem, framing, transmit, correlate
io/       desktop audio I/O: WAV, libmp3lame, ffmpeg
desktop/  the CLI
android/  Kotlin app + JNI bridge; host-jni/ runs the bridge on a desktop JVM
tests/    unit tests, and bench.cpp which produces every number above
```

Known limits. The first four are measurements; the last three are things this
machine could not verify, and each states what it did observe instead.

- **Delivered throughput is 76.6 bps for a 120-byte message, and repetition is
  not a robustness knob you can turn up.** Doubling `--repeat` to 6 halves it to
  38.6 bps -- 24.85 s of audio for the same payload. Over
  `vacuum_bench --mode matrix --payload 120 --trials 10 --repeat 6` every row's
  raw BER falls (0.4% to 19.5% per row; 7.5% as the mean of the fifteen relative
  changes) yet message success gets *worse* on four of them: lowpass 6 kHz 7/10
  against 10/10 at the default repeat 3, 5 kHz 6/10 against 9/10, 4 kHz 2/10
  against 4/10, and `+ noise 12 dB` 9/10 against 10/10. The other eleven rows
  hold 10/10 at both settings. Why the four regress is not measured here; repeat
  3 is the default because the matrix says so, not because of a theory about
  diversity.
- **Sample rate must match within a few percent.** It is the symbol clock as
  well as a frequency axis. A transmitter at 44.1 kHz captured at 48 kHz does
  not decode, which is why the CLI and the app both follow the file.
- **Silence is the easiest channel and the quietest watermark.** With nothing to
  mask, tones sit at `floorAmplitude` (0.004 per carrier) and the whole watermark
  measures -39.6 dBFS; `--host silence` scores 6/6 on all fifteen rows with BER
  0.00e+00 and zero symbols corrected. Buried in music the watermark is *louder*
  in absolute terms -- 10.5 dB below the host RMS -- because the masking model
  raises it toward `peakAmplitude`. Hidden, not quiet.
- **The band limit is what kills the link, and it breaks near 4 kHz.** The
  matrix's `+ lowpass` rows degrade monotonically: 8 kHz 10/10, 6 kHz 10/10, 5
  kHz 9/10, 4 kHz 4/10. That is a *channel* lowpass. A carrier band narrower than
  the modem needs to seat `subBands x groups x carriersPerGroup` carriers is not
  a degraded configuration, it is a rejected one: `--low-hz 900 --high-hz 4500`
  throws `carrier band too narrow` before any audio is made.
- **`vacuum mic` has never completed a capture on this machine.** The file paths
  have: a 200-byte payload watermarked into a 44.1 kHz MP3 host and extracted
  back byte-identical. But `mic --seconds 2` blocked for two minutes with no
  audio and no error, and bare `ffmpeg -f avfoundation -i ":0" -t 1` did the
  same from this shell while `-list_devices` listed the headset fine. The likely
  cause is the microphone consent prompt macOS shows to the calling terminal,
  which a headless process can neither see nor answer -- that is an inference,
  not a measured cause. Run it from a terminal already granted the microphone
  (System Settings -> Privacy & Security -> Microphone).
- **Clock drift over long transmissions is unmeasured.** The cyclic-prefix
  tracker absorbs drift within `timingSearch` = 192 samples per symbol; whether
  two independent sound cards stay inside that over, say, five minutes of audio
  has not been tested here.
- **No live over-the-air test.** Everything above is a synthesised channel --
  delay, one-pole lowpass, additive noise, real MP3 and AAC encoding. A
  loudspeaker-into-microphone loop in a real room, with a real device's
  automatic gain control in the path, is a different and harder channel that this
  machine cannot measure.
