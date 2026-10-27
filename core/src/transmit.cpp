#include "vacuum/transmit.h"

#include <algorithm>
#include <array>
#include <numeric>

#include "vacuum/crc.h"
#include "vacuum/physical.h"
#include "vacuum/rs.h"

namespace vacuum {
namespace {

/// Vote on physical header across repeats, checking CRC-16 and bit-level consensus.
std::optional<PhysicalHeader> voteHeader(const Bytes& bytes, int repeat) {
  if (static_cast<int>(bytes.size()) < repeat * 4) return std::nullopt;

  // 1. Check if any repeat candidate directly passes CRC-16
  std::vector<std::pair<Bytes, int>> validCandidates;
  for (int r = 0; r < repeat; ++r) {
    Bytes cand(bytes.begin() + r * 4, bytes.begin() + (r + 1) * 4);
    if (decodePhysicalHeader(cand).has_value()) {
      auto it = std::find_if(validCandidates.begin(), validCandidates.end(),
                             [&](const auto& p) { return p.first == cand; });
      if (it != validCandidates.end()) {
        it->second++;
      } else {
        validCandidates.push_back({cand, 1});
      }
    }
  }
  if (!validCandidates.empty()) {
    auto best = std::max_element(validCandidates.begin(), validCandidates.end(),
                                 [](const auto& a, const auto& b) { return a.second < b.second; });
    return decodePhysicalHeader(best->first);
  }

  // 2. Bit-by-bit majority voting
  Bytes bitVoted(4, 0);
  for (int b = 0; b < 32; ++b) {
    const int byteIdx = b / 8;
    const int bitShift = 7 - (b % 8);
    int ones = 0;
    for (int r = 0; r < repeat; ++r) {
      if ((bytes[r * 4 + byteIdx] >> bitShift) & 1) ++ones;
    }
    if (ones * 2 >= repeat) {
      bitVoted[byteIdx] |= static_cast<std::uint8_t>(1 << bitShift);
    }
  }
  if (auto h = decodePhysicalHeader(bitVoted); h.has_value()) {
    return h;
  }

  // 3. Fallback to byte tally
  Bytes voted(4, 0);
  for (int i = 0; i < 4; ++i) {
    std::array<int, 256> tally{};
    for (int r = 0; r < repeat; ++r) ++tally[bytes[r * 4 + i]];
    voted[i] = static_cast<std::uint8_t>(
        std::max_element(tally.begin(), tally.end()) - tally.begin());
  }
  return decodePhysicalHeader(voted);
}

std::uint16_t deriveStreamId(const Bytes& payload) {
  return static_cast<std::uint16_t>(Crc32::compute(payload.data(), payload.size()) & 0xFFFFu);
}

/// Collapse per-bit confidence to per-byte: a byte is only as trustworthy as
/// its least confident bit, which is what an erasure decision needs.
std::vector<float> byteConfidence(const std::vector<float>& bitConfidence) {
  std::vector<float> out;
  out.reserve(bitConfidence.size() / 8);
  for (std::size_t i = 0; i + 8 <= bitConfidence.size(); i += 8) {
    float weakest = bitConfidence[i];
    for (std::size_t k = 1; k < 8; ++k) weakest = std::min(weakest, bitConfidence[i + k]);
    out.push_back(weakest);
  }
  return out;
}

}  // namespace

bool encodeMessage(const Bytes& payload, const Audio& host, const Modem& modem,
                   const TransmitConfig& cfg, EncodeResult& out, std::string& error) {
  if (payload.empty()) {
    error = "payload is empty";
    return false;
  }
  if (payload.size() > cfg.maxPayloadBytes) {
    error = "payload exceeds maxPayloadBytes";
    return false;
  }

  const auto plan = planFrames(payload.size(), cfg.rsParitySymbols, cfg.maxFragmentSize);
  if (!plan) {
    error = "payload cannot be framed with the current Reed-Solomon configuration";
    return false;
  }

  const std::uint16_t streamId = cfg.streamId != 0 ? cfg.streamId : deriveStreamId(payload);
  const std::vector<Bytes> blocks = packetize(payload, streamId, *plan, cfg.rsParitySymbols);

  Bytes coded;
  const Bytes headerBytes = encodePhysicalHeader(
      PhysicalHeader{static_cast<std::uint8_t>(plan->size),
                     static_cast<std::uint16_t>(plan->fragmentCount)});
  for (int r = 0; r < std::max(1, cfg.physicalHeaderRepeat); ++r) {
    coded.insert(coded.end(), headerBytes.begin(), headerBytes.end());
  }
  for (const Bytes& b : blocks) coded.insert(coded.end(), b.begin(), b.end());

  std::vector<std::uint8_t> bits;
  bytesToBits(coded, bits);
  out.txBits = bits;

  Audio shifted(cfg.embedOffsetSamples + host.size(), 0.0f);
  std::copy(host.begin(), host.end(), shifted.begin() + cfg.embedOffsetSamples);

  out.audio = modem.embed(bits, shifted, cfg.embedOffsetSamples);
  out.codedBits = bits.size();
  out.fragmentCount = plan->fragmentCount;
  out.netBitRate = modem.netBitRate();
  out.watermarkSamples = modem.samplesForBits(bits.size());
  out.seconds = static_cast<double>(out.watermarkSamples) / static_cast<double>(modem.sampleRate());
  out.payloadBitRate = static_cast<double>(payload.size()) * 8.0 / std::max(1e-9, out.seconds);
  error.clear();
  return true;
}

DecodeResult decodeMessage(const Audio& rx, const Modem& modem, const TransmitConfig& cfg) {
  DecodeResult result;

  const auto extraction = modem.extract(rx);
  result.synced = extraction.synced;
  result.syncSample = extraction.syncSample;
  result.syncScore = extraction.syncScore;
  if (!extraction.synced) {
    result.error = "acquisition preamble not found";
    return result;
  }
  if (!extraction.confidence.empty()) {
    double sum = 0.0;
    for (float c : extraction.confidence) sum += c;
    result.meanBitConfidence = sum / static_cast<double>(extraction.confidence.size());
  }

  const int repeat = std::max(1, cfg.physicalHeaderRepeat);
  const std::size_t headerBits = static_cast<std::size_t>(repeat) * 32;
  if (extraction.bits.size() < headerBits) {
    result.error = "recording too short to contain the physical header";
    return result;
  }

  Bytes headerBytes;
  bitsToBytes({extraction.bits.begin(),
               extraction.bits.begin() + static_cast<std::ptrdiff_t>(headerBits)},
              headerBytes);
  const auto header = voteHeader(headerBytes, repeat);
  if (!header) {
    result.error = "physical header failed CRC-16";
    return result;
  }
  const auto size = fragmentSizeFromCode(header->sizeCode);
  if (!size) {
    result.error = "physical header carries an unknown fragment size";
    return result;
  }

  const std::size_t frameBytes = kFrameHeaderSize + fragmentPayloadSize(*size) + kFrameCrcSize;
  const std::size_t blockBytes = frameBytes + static_cast<std::size_t>(cfg.rsParitySymbols);
  result.framesExpected = header->fragmentCount;
  result.fragmentBytes = fragmentPayloadSize(*size);

  Bytes coded;
  bitsToBytes({extraction.bits.begin() + static_cast<std::ptrdiff_t>(headerBits),
               extraction.bits.end()},
              coded);
  const std::size_t wanted = blockBytes * header->fragmentCount;
  if (coded.size() < wanted) {
    result.error = "recording ended before the whole message was recovered";
  }

  const ReedSolomon rs(cfg.rsParitySymbols);
  Reassembler reassembler;
  const std::size_t available =
      std::min(coded.size() / blockBytes, static_cast<std::size_t>(header->fragmentCount));

  const auto conf =
      cfg.softErasureRetry ? byteConfidence(extraction.confidence) : std::vector<float>{};
  const std::size_t confBase0 = static_cast<std::size_t>(repeat) * 4;

  for (std::size_t i = 0; i < available; ++i) {
    const Bytes block(coded.begin() + static_cast<std::ptrdiff_t>(i * blockBytes),
                      coded.begin() + static_cast<std::ptrdiff_t>((i + 1) * blockBytes));

    auto decoded = rs.decode(block);
    std::vector<int> erasures;
    if (!decoded.ok && !conf.empty()) {
      // Rank rather than threshold: flag the least confident symbols, up to
      // half the parity budget. Flagging more is a trap -- k erasures leave
      // room for only (parity - k) / 2 further errors, so k = parity has no
      // error capacity at all and any damaged symbol outside the ranking turns
      // into a miscorrection instead of a clean failure. Half the budget is the
      // measured optimum; see `vacuum_bench --mode fec`.
      const std::size_t base = confBase0 + i * blockBytes;
      const std::size_t budget = static_cast<std::size_t>(cfg.rsParitySymbols) / 2;
      erasures.resize(blockBytes);
      std::iota(erasures.begin(), erasures.end(), 0);
      std::stable_sort(erasures.begin(), erasures.end(), [&](int x, int y) {
        return conf[base + static_cast<std::size_t>(x)] < conf[base + static_cast<std::size_t>(y)];
      });
      erasures.resize(std::min(erasures.size(), budget));
      decoded = rs.decode(block, erasures);
    }

    if (!decoded.ok) {
      ++result.rsFailedBlocks;
      continue;
    }
    result.rsCorrectedSymbols += decoded.correctedSymbols;
    result.rsErasedSymbols += static_cast<int>(erasures.size());
    reassembler.add(decoded.data);
  }

  result.framesRecovered = reassembler.fragmentsReceived();
  if (reassembler.complete()) {
    result.ok = true;
    result.payload = reassembler.payload();
    result.error.clear();
  } else {
    result.salvagePayload = reassembler.salvage();
    if (result.error.empty()) {
      // The reassembler only learns the total from a decoded start frame, so when
      // every block failed it still says zero. The physical header was readable --
      // that is what `framesExpected` holds -- and "0 of 2" is a diagnosis where
      // "0 of 0" is not.
      const std::size_t expected =
          reassembler.fragmentsExpected() > 0 ? reassembler.fragmentsExpected()
                                              : result.framesExpected;
      result.error = "incomplete message: " + std::to_string(result.framesRecovered) + "/" +
                     std::to_string(expected) + " fragments recovered";
    }
  }
  return result;
}

double estimateSeconds(std::size_t payloadBytes, const Modem& modem, const TransmitConfig& cfg) {
  if (payloadBytes == 0) return 0.0;
  const auto plan = planFrames(payloadBytes, cfg.rsParitySymbols, cfg.maxFragmentSize);
  if (!plan) return -1.0;

  const std::size_t codedBytes =
      static_cast<std::size_t>(std::max(1, cfg.physicalHeaderRepeat)) * 4 +
      plan->fragmentCount * plan->blockBytes;
  const std::size_t totalBits = codedBytes * 8;
  return static_cast<double>(modem.samplesForBits(totalBits)) /
         static_cast<double>(modem.sampleRate());
}

}  // namespace vacuum
