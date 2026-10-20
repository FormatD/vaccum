#include "vacuum/physical.h"

#include <cmath>

#include "vacuum/crc.h"
#include "vacuum/frame.h"

namespace vacuum {
namespace {

constexpr double kTwoPi = 6.283185307179586;

}  // namespace

Bytes encodePhysicalHeader(const PhysicalHeader& header) {
  const std::uint16_t field =
      static_cast<std::uint16_t>(((header.sizeCode & 0x0Fu) << 12) | (header.fragmentCount & 0x0FFFu));
  const std::uint8_t raw[2] = {static_cast<std::uint8_t>(field >> 8),
                               static_cast<std::uint8_t>(field & 0xFF)};
  Bytes out{raw[0], raw[1]};
  const std::uint16_t crc = crc16(raw, 2);
  out.push_back(static_cast<std::uint8_t>(crc >> 8));
  out.push_back(static_cast<std::uint8_t>(crc & 0xFF));
  return out;
}

std::optional<PhysicalHeader> decodePhysicalHeader(const Bytes& fourBytes) {
  if (fourBytes.size() < 4) return std::nullopt;
  const std::uint8_t raw[2] = {fourBytes[0], fourBytes[1]};
  const std::uint16_t expected =
      static_cast<std::uint16_t>((fourBytes[2] << 8) | fourBytes[3]);
  if (crc16(raw, 2) != expected) return std::nullopt;

  const std::uint16_t field = static_cast<std::uint16_t>((raw[0] << 8) | raw[1]);
  PhysicalHeader header;
  header.sizeCode = static_cast<std::uint8_t>(field >> 12);
  header.fragmentCount = static_cast<std::uint16_t>(field & 0x0FFFu);
  if (!fragmentSizeFromCode(header.sizeCode).has_value()) return std::nullopt;
  if (header.fragmentCount == 0) return std::nullopt;
  return header;
}

void bytesToBits(const Bytes& in, std::vector<std::uint8_t>& bits) {
  bits.reserve(bits.size() + in.size() * 8);
  for (std::uint8_t byte : in) {
    for (int b = 7; b >= 0; --b) bits.push_back(static_cast<std::uint8_t>((byte >> b) & 1));
  }
}

void bitsToBytes(const std::vector<std::uint8_t>& bits, Bytes& out) {
  out.clear();
  out.reserve(bits.size() / 8);
  for (std::size_t i = 0; i + 8 <= bits.size(); i += 8) {
    std::uint8_t byte = 0;
    for (int b = 0; b < 8; ++b) byte = static_cast<std::uint8_t>((byte << 1) | (bits[i + b] & 1));
    out.push_back(byte);
  }
}

int Lfsr::next() noexcept {
  // Taps at bits 16, 14, 13, 11 -> mask 0xB400 in Galois form.
  const std::uint32_t lsb = state_ & 1u;
  state_ >>= 1;
  if (lsb) state_ ^= 0xB400u;
  return static_cast<int>(lsb);
}

std::vector<std::uint8_t> Lfsr::bits(std::size_t count) {
  std::vector<std::uint8_t> out;
  out.reserve(count);
  for (std::size_t i = 0; i < count; ++i) out.push_back(static_cast<std::uint8_t>(next()));
  return out;
}

float carrierPhase(int carrierIndex, int symbolIndex, int carrierCount) noexcept {
  // A fixed irrational rotation per carrier: tones decorrelate across carriers
  // and across symbols without needing to signal any phase information.
  const double golden = 0.6180339887498949;
  const double a = std::fmod(golden * static_cast<double>(carrierIndex + 1), 1.0);
  const double b = std::fmod(golden * static_cast<double>(carrierIndex + 1) *
                                 static_cast<double>(carrierCount + 1),
                             1.0);
  const double turns = std::fmod(a + b * static_cast<double>(symbolIndex), 1.0);
  return static_cast<float>(kTwoPi * turns);
}

}  // namespace vacuum
