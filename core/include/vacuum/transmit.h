#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "vacuum/frame.h"
#include "vacuum/modem.h"
#include "vacuum/types.h"

namespace vacuum {

struct TransmitConfig {
  /// Reed-Solomon parity symbols per block. 64 corrects 32 bad symbols, or 64
  /// when the erasure retry below has located them.
  ///
  /// This is the single strongest robustness knob, and much stronger than
  /// shrinking the block: message success out of 8 trials at a 120-byte payload,
  /// `vacuum_bench --mode matrix --payload 120 --trials 8`:
  ///
  ///     scenario                   parity 32   parity 48   parity 64
  ///     air delay + lowpass 11 kHz       8/8       8/8       8/8
  ///     + noise 12 dB                    0/8       7/8       8/8
  ///     + lowpass 8 kHz                  1/8       8/8       8/8
  ///     + lowpass 6 kHz                  0/8       7/8       8/8
  ///     + lowpass 5 kHz                  0/8       3/8       8/8
  ///     + lowpass 4 kHz                  0/8       1/8       4/8
  ///     mp3 128k + lp 8k + noise 18 dB   2/8       8/8       8/8
  ///     audio seconds                    9.97     11.25     12.53
  ///
  /// At a 400-byte payload (6 trials) parity 32 scores 0/6 on both the 8 kHz
  /// and 6 kHz lowpass channels where parity 64 scores 5/6. The cost is 26% more
  /// airtime. Drop to 32 when the channel is a file rather than the air, where
  /// every parity measured 8/8 anyway.
  int rsParitySymbols = 64;
  /// Upper bound on the fragment size `planFrames` may choose. `planFrames`
  /// minimises total airtime, so without a cap it picks the largest fragment
  /// that fits; the cap trades that airtime for resilience, since a block
  /// absorbs a fixed number of bad symbols and a smaller block therefore
  /// tolerates a higher bit error rate.
  ///
  /// Shrinking the fragment is weaker than raising `rsParitySymbols` and costs
  /// far more airtime, because a message only survives if *every* fragment
  /// does: at parity 32 and a 120-byte payload, Bytes24 improves the raw bit
  /// error rate (8.9e-3 against 1.1e-2 on the clean loopback) yet still scores
  /// 7/8 on noise 18 dB where Bytes64 at parity 64 scores 8/8, for 15.65 s of
  /// audio against 12.53 s. Measured with `vacuum_bench --mode matrix
  /// --payload 120 --trials 8 --fragment-size 24`.
  ///
  /// Note that `blockBytes = fragment + 18 + parity` must stay within
  /// `ReedSolomon::kMaxBlock` (255), so a large parity silently rules out the
  /// large fragments; at parity 64 the most `planFrames` can use is Bytes128.
  FragmentSize maxFragmentSize = FragmentSize::Bytes64;
  /// The physical header is sent this many times and majority-voted, since it
  /// is the only part of the message the receiver cannot afford to lose.
  int physicalHeaderRepeat = 4;
  /// When a block fails plain error decoding, retry it with the least confident
  /// half of the parity budget's worth of symbols flagged as erasures. An
  /// erasure costs one parity symbol where an unlocated error costs two, so
  /// ranking by confidence and flagging half the budget is a trade, not "as
  /// many as possible": flagging all of it leaves no error capacity at all and
  /// any damaged symbol outside the ranking becomes a miscorrection.
  ///
  /// Correct blocks out of 16, `vacuum_bench --mode fec --payload 120
  /// --trials 8`, at the two parities worth comparing. "retry" is the shipped
  /// cascade, and no cell below is a miscorrection -- the command's own `miscorr`
  /// column reads zero for every one of these rows:
  ///
  ///     scenario                   parity 64          parity 32
  ///                              no retry  retry    no retry  retry
  ///     clean file loopback           16      16          16     16
  ///     air delay + lowpass 11 kHz    16      16          14     16
  ///     + noise 18 dB                 16      16          13   14 (2 failed)
  ///     + lowpass 8 kHz               15      16           3  4 (12 failed)
  ///     mp3 128 kbps                  16      16          16     16
  ///
  /// At the default parity 64 the retry is close to redundant -- it rescues one
  /// block in eighty -- because the parity alone already covers the damage. It
  /// earns its keep at parity 32: two blocks rescued on the delayed channel, one
  /// each on noise 18 dB and lowpass 8 kHz, four of eighty. The miscorrections
  /// come from a different setting, flagging *all* the parity -- there the `k=64`
  /// rows return 2 to 10 valid-but-wrong blocks out of 16, and at parity 32 the
  /// lowpass 8 kHz row returns sixteen. Half the budget is what keeps the failure
  /// mode a failure. A miscorrection is not a wrong payload: the frame's own
  /// CRC-32 trailer and the end-to-end CRC-32 on the reassembled message reject
  /// it. The retry only runs on blocks that already failed.
  bool softErasureRetry = true;
  /// 0 derives an id from the payload so retransmissions of the same data
  /// group together on the receive side.
  std::uint16_t streamId = 0;
  /// Where in the host track the watermark starts, in samples.
  std::size_t embedOffsetSamples = 0;
  std::size_t maxPayloadBytes = 8192;
};

struct EncodeResult {
  Audio audio;
  std::size_t watermarkSamples = 0;
  double seconds = 0;
  /// Modem-layer rate after repetition, before framing and error correction.
  /// Always higher than `payloadBitRate`; do not quote it as throughput.
  double netBitRate = 0;
  /// Payload bytes actually delivered per second of audio, overhead included:
  /// header repeats, CRC, Reed-Solomon parity and the acquisition preamble all
  /// come out of this. It rises with the message because the per-message
  /// overhead amortizes: 47.8 bps at 40 B, 76.6 at 120 B (12.53 s), 86.9 at a
  /// kilobyte -- against the flat 200 bps `netBitRate` reports.
  double payloadBitRate = 0;
  std::size_t codedBits = 0;
  std::size_t fragmentCount = 0;
  /// The exact bit stream handed to the modem. Exposed so benchmarks can
  /// measure raw bit error rate instead of only end-to-end success.
  std::vector<std::uint8_t> txBits;
};

/// Payload -> frames -> Reed-Solomon -> physical header -> modem -> audio,
/// mixed additively into `host`.
///
/// Returns false with `error` set when the payload is empty or too large.
bool encodeMessage(const Bytes& payload, const Audio& host, const Modem& modem,
                   const TransmitConfig& cfg, EncodeResult& out, std::string& error);

struct DecodeResult {
  bool ok = false;
  Bytes payload;
  std::string error;

  bool synced = false;
  std::ptrdiff_t syncSample = -1;
  double syncScore = 0;
  double meanBitConfidence = 0;

  std::size_t framesExpected = 0;
  std::size_t framesRecovered = 0;
  /// Fragment payload size the sender chose, read back from the physical
  /// header. Set even when the decode fails, as long as the header survived --
  /// together with `framesExpected` this is what tells a receiver that it was
  /// configured with the wrong parity for the transmission it is looking at.
  std::size_t fragmentBytes = 0;
  int rsCorrectedSymbols = 0;
  int rsErasedSymbols = 0;
  int rsFailedBlocks = 0;

  /// Partial payload salvaged from recovered fragments when full message CRC fails.
  Bytes salvagePayload;
};

/// Decode a recording. Fails cleanly (ok == false, error set) rather than
/// returning partial data: a payload is only reported when every fragment
/// passed Reed-Solomon and the reassembled bytes matched the end-to-end CRC.
DecodeResult decodeMessage(const Audio& rx, const Modem& modem, const TransmitConfig& cfg);

/// Audio seconds needed for a payload of this size, including preamble and
/// physical header. Useful for deciding a fragment size before committing.
double estimateSeconds(std::size_t payloadBytes, const Modem& modem, const TransmitConfig& cfg);

}  // namespace vacuum
