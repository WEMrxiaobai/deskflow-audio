/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "audio/transport/SrtpTransport.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {
using namespace deskflow::audio::transport;
using Bytes = std::vector<std::uint8_t>;

// Public, synthetic, deterministic TEST material ONLY. Never generate production keys this way.
std::array<std::uint8_t, kMasterKeySaltBytes> testMaterial(std::uint8_t salt = 0)
{
  std::array<std::uint8_t, kMasterKeySaltBytes> bytes{};
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<std::uint8_t>(i * 7 + 13 + salt);
  }
  return bytes;
}
SrtpKey testKey(std::uint8_t salt = 0)
{
  return SrtpKey(testMaterial(salt));
}
StreamConfig config(std::uint32_t ssrc = 0x10203040, std::uint64_t generation = 1, std::uint16_t samples = 480)
{
  return {ssrc, generation, 48000, 2, samples};
}

void require(bool condition, const char *message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}
template <class Exception, class Function> void expectThrows(Function function, const char *message)
{
  try {
    function();
  } catch (const Exception &) {
    return;
  }
  throw std::runtime_error(message);
}

Bytes copyPacket(ProtectResult result)
{
  require(bool(result), "Protection unexpectedly failed");
  return {result.packet.begin(), result.packet.end()};
}
void rejected(ReceiveResult result, const char *message)
{
  require(!result, message);
  require(result.payload.empty(), "Rejected datagram exposed payload");
  require(
      result.metadata.sequence == 0 && result.metadata.timestamp == 0 && result.metadata.ssrc == 0 &&
          result.metadata.generation == 0 && result.metadata.frameSamples == 0,
      "Rejected datagram exposed unauthenticated metadata"
  );
}
void payloadEquals(ReceiveResult result, std::span<const std::uint8_t> payload)
{
  require(bool(result), "Valid datagram rejected");
  require(
      std::equal(result.payload.begin(), result.payload.end(), payload.begin(), payload.end()),
      "Decrypted payload differs"
  );
}

// The transport handles opaque encoded payloads; Opus decoding/duration validation belongs
// to the codec layer. These synthetic bytes deliberately require no Opus test dependency.
constexpr std::array<std::uint8_t, 8> kPayload{0xF4, 0x71, 0x82, 0x19, 0x55, 0x28, 0x93, 0xA5};

void roundTripAndHeader()
{
  const auto stream = config();
  SrtpSender sender(stream, testKey());
  SrtpReceiver receiver(stream, testKey());
  const auto wire = copyPacket(sender.protect(kPayload, 0x1234, 0x89abcdef));
  const std::array<std::uint8_t, 12> header{0x80, 111, 0x12, 0x34, 0x89, 0xab, 0xcd, 0xef, 0x10, 0x20, 0x30, 0x40};
  require(wire.size() == kRtpHeaderBytes + kPayload.size() + kAuthTagBytes, "Wrong SRTP packet length");
  require(std::equal(header.begin(), header.end(), wire.begin()), "RTP network byte order/base header differs");
  require(
      !std::equal(kPayload.begin(), kPayload.end(), wire.begin() + kRtpHeaderBytes),
      "Payload was transmitted in plaintext"
  );
  const auto result = receiver.unprotect(wire);
  payloadEquals(result, kPayload);
  require(
      result.metadata.sequence == 0x1234 && result.metadata.timestamp == 0x89abcdef &&
          result.metadata.ssrc == stream.ssrc && result.metadata.generation == stream.generation &&
          result.metadata.frameSamples == 480,
      "Authenticated metadata differs"
  );
}

void tamperEveryBit()
{
  const auto stream = config(0x10203041, 2);
  SrtpSender sender(stream, testKey(1));
  SrtpReceiver receiver(stream, testKey(1));
  const auto wire = copyPacket(sender.protect(kPayload, 73, 48240));
  // Includes every header bit (sequence, timestamp, SSRC), ciphertext and all 80 tag bits.
  for (std::size_t byte = 0; byte < wire.size(); ++byte) {
    for (unsigned bit = 0; bit < 8; ++bit) {
      auto tampered = wire;
      tampered[byte] ^= static_cast<std::uint8_t>(1U << bit);
      rejected(receiver.unprotect(tampered), "A tampered bit was accepted");
    }
  }
  // Failed authentication MUST NOT consume a replay index or poison the context.
  payloadEquals(receiver.unprotect(wire), kPayload);
  rejected(receiver.unprotect(wire), "Replay after tampering test was accepted");
}

void replayAndReorder()
{
  const auto stream = config(0x10203042, 3);
  SrtpSender sender(stream, testKey(2));
  SrtpReceiver receiver(stream, testKey(2));
  const auto first = copyPacket(sender.protect(kPayload, 100, 0));
  const auto second = copyPacket(sender.protect(kPayload, 101, 480));
  const auto third = copyPacket(sender.protect(kPayload, 102, 960));
  payloadEquals(receiver.unprotect(first), kPayload);
  const auto duplicate = receiver.unprotect(first);
  rejected(duplicate, "Duplicate datagram accepted");
  require(duplicate.error == TransportError::ReplayRejected, "Replay did not report replay rejection");
  payloadEquals(receiver.unprotect(third), kPayload);
  payloadEquals(receiver.unprotect(second), kPayload); // Unseen, in-window reordered packet.
  rejected(receiver.unprotect(second), "Reordered replay accepted");

  const auto delayed = copyPacket(sender.protect(kPayload, 103, 1440));
  const auto current = copyPacket(sender.protect(kPayload, 300, 96000));
  payloadEquals(receiver.unprotect(current), kPayload);
  const auto tooOld = receiver.unprotect(delayed);
  rejected(tooOld, "Packet outside replay window accepted");
  require(tooOld.error == TransportError::ReplayRejected, "Old replay did not report replay rejection");
  const auto repeatedTx = sender.protect(kPayload, 300, 96000);
  require(!repeatedTx && repeatedTx.packet.empty(), "Sender allowed repeated packet index");
}

void wrongSsrcAndKey()
{
  const auto stream = config(0x10203043, 4);
  SrtpSender sender(stream, testKey(3));
  SrtpReceiver wrongSsrc(config(stream.ssrc + 1, 4), testKey(3));
  SrtpReceiver wrongKey(stream, testKey(99));
  SrtpReceiver good(stream, testKey(3));
  const auto wire = copyPacket(sender.protect(kPayload, 501, 0));
  const auto mismatch = wrongSsrc.unprotect(wire);
  rejected(mismatch, "Unbound SSRC accepted");
  require(mismatch.error == TransportError::SsrcMismatch, "Wrong SSRC did not report mismatch");
  auto rewritten = wire;
  rewritten[11] = static_cast<std::uint8_t>(stream.ssrc + 1); // Same prefix; exactly the other bound SSRC.
  rejected(wrongSsrc.unprotect(rewritten), "Rewritten SSRC accepted");
  const auto keyMismatch = wrongKey.unprotect(wire);
  rejected(keyMismatch, "Wrong key accepted");
  require(keyMismatch.error == TransportError::AuthenticationFailed, "Wrong key did not fail authentication");
  // A syntactically valid plaintext RTP packet (large enough to appear to have a tag)
  // must never become a PCM/unencrypted fallback or consume the real packet's index.
  Bytes plaintext(kRtpHeaderBytes + 64, 0x5a);
  std::copy_n(wire.begin(), kRtpHeaderBytes, plaintext.begin());
  rejected(good.unprotect(plaintext), "Plaintext RTP fallback accepted");
  payloadEquals(good.unprotect(wire), kPayload);
}

void truncationAndStrictParsing()
{
  const auto stream = config(0x10203044, 5);
  SrtpSender sender(stream, testKey(4));
  SrtpReceiver receiver(stream, testKey(4));
  const auto wire = copyPacket(sender.protect(kPayload, 200, 0));
  for (std::size_t length = 0; length < wire.size(); ++length) {
    rejected(receiver.unprotect(std::span(wire).first(length)), "Truncated datagram accepted");
  }
  // Unnegotiated CSRC, extension, padding, version, marker, and payload type.
  constexpr std::array<std::uint8_t, 8> firstBytes{0x00, 0x40, 0xc0, 0x81, 0x8f, 0x90, 0xa0, 0xb0};
  for (const auto first : firstBytes) {
    auto malformed = wire;
    malformed[0] = first;
    const auto result = receiver.unprotect(malformed);
    rejected(result, "Non-base RTP header accepted");
    require(result.error == TransportError::InvalidHeader, "Non-base header did not fail strict parsing");
  }
  for (const std::uint8_t second : {std::uint8_t(0), std::uint8_t(110), std::uint8_t(112), std::uint8_t(0xef)}) {
    auto malformed = wire;
    malformed[1] = second;
    rejected(receiver.unprotect(malformed), "Unnegotiated marker/payload type accepted");
  }
  auto appended = wire;
  appended.push_back(0);
  rejected(receiver.unprotect(appended), "Trailing unauthenticated byte accepted");
  payloadEquals(receiver.unprotect(wire), kPayload);
}

void mtuAndBorrowedStorage()
{
  const auto stream = config(0x10203045, 6);
  SrtpSender sender(stream, testKey(5));
  SrtpReceiver receiver(stream, testKey(5));
  Bytes largest(kMaxPayloadBytes, 0x5a);
  const auto maximum = sender.protect(largest, 1, 0);
  require(bool(maximum) && maximum.packet.size() == 1200, "1200-byte boundary rejected");
  const auto *const senderStorage = maximum.packet.data();
  const Bytes wire(maximum.packet.begin(), maximum.packet.end());
  const auto decoded = receiver.unprotect(wire);
  payloadEquals(decoded, largest);
  const auto *const receiverStorage = decoded.payload.data();

  Bytes excessive(kMaxPayloadBytes + 1, 0x5a);
  const auto oversized = sender.protect(excessive, 2, 480);
  require(
      !oversized && oversized.error == TransportError::PacketTooLarge && oversized.packet.empty(), "Sender exceeded MTU"
  );
  const auto empty = sender.protect({}, 2, 480);
  require(!empty && empty.error == TransportError::InvalidPayload && empty.packet.empty(), "Empty payload accepted");
  const auto next = sender.protect(kPayload, 2, 480); // Validation failures must not consume seq 2.
  require(bool(next) && next.packet.data() == senderStorage, "Sender packet storage was not reused");
  const auto nextWire = Bytes(next.packet.begin(), next.packet.end());
  const auto nextDecoded = receiver.unprotect(nextWire);
  payloadEquals(nextDecoded, kPayload);
  require(nextDecoded.payload.data() == receiverStorage, "Receiver payload storage was not reused");
  Bytes tooLarge(1201, 0);
  const auto rejectedLarge = receiver.unprotect(tooLarge);
  rejected(rejectedLarge, "Receiver accepted oversized UDP");
  require(rejectedLarge.error == TransportError::PacketTooLarge, "Receiver MTU did not report size error");
  const std::array<std::uint8_t, 1> minimum{0x80};
  payloadEquals(receiver.unprotect(copyPacket(sender.protect(minimum, 3, 960))), minimum);
}

void sequenceAndTimestampWrap()
{
  const auto stream = config(0x10203046, 7, 240);
  SrtpSender sender(stream, testKey(6));
  SrtpReceiver receiver(stream, testKey(6));
  constexpr std::array<std::uint16_t, 5> sequences{65533, 65534, 65535, 0, 1};
  std::array<Bytes, 5> packets;
  std::uint32_t timestamp = 0xffffff00;
  for (std::size_t i = 0; i < sequences.size(); ++i) {
    packets[i] = copyPacket(sender.protect(kPayload, sequences[i], timestamp));
    timestamp += 240;
  }
  // Reordering across ROC transition must work without sharing sender/receiver state.
  constexpr std::array<std::size_t, 5> order{0, 2, 3, 1, 4};
  for (const auto i : order) {
    const auto result = receiver.unprotect(packets[i]);
    payloadEquals(result, kPayload);
    require(
        result.metadata.sequence == sequences[i] &&
            result.metadata.timestamp == static_cast<std::uint32_t>(0xffffff00U + 240U * i) &&
            result.metadata.frameSamples == 240,
        "Sequence/timestamp wrap metadata differs"
    );
  }
  for (const auto &packet : packets) {
    rejected(receiver.unprotect(packet), "Replay at sequence wrap accepted");
  }
}

void trustedConfigAndGeneration()
{
  for (const auto bad :
       {StreamConfig{0, 1, 48000, 2, 480}, StreamConfig{1, 0, 48000, 2, 480}, StreamConfig{1, 1, 44100, 2, 480},
        StreamConfig{1, 1, 48000, 1, 480}, StreamConfig{1, 1, 48000, 2, 120}, StreamConfig{1, 1, 48000, 2, 960}}) {
    expectThrows<std::invalid_argument>([&] { SrtpSender sender(bad, testKey()); }, "Invalid sender config accepted");
    expectThrows<std::invalid_argument>(
        [&] { SrtpReceiver receiver(bad, testKey()); }, "Invalid receiver config accepted"
    );
  }
  const auto material = testMaterial();
  expectThrows<std::invalid_argument>([&] { SrtpKey key(std::span(material).first(29)); }, "Short key accepted");
  std::array<std::uint8_t, 31> tooLong{};
  expectThrows<std::invalid_argument>([&] { SrtpKey key(tooLong); }, "Long key accepted");
  auto rejectedKey = testKey(11);
  expectThrows<std::invalid_argument>(
      [&] { SrtpSender invalid(StreamConfig{}, std::move(rejectedKey)); }, "Invalid config accepted"
  );
  expectThrows<std::invalid_argument>(
      [&] { SrtpSender retry(config(), std::move(rejectedKey)); }, "Failure path did not consume key"
  );
  auto movedFrom = testKey(12);
  SrtpKey moved(std::move(movedFrom));
  expectThrows<std::invalid_argument>(
      [&] { SrtpReceiver invalid(config(), std::move(movedFrom)); }, "Moved-from key reused"
  );
  SrtpReceiver ownsMoved(config(0x10203051, 12), std::move(moved));
  auto key = testKey(7);
  SrtpSender oldSender(config(0x10203047, 8), std::move(key));
  expectThrows<std::invalid_argument>(
      [&] { SrtpSender duplicate(config(0x10203048, 9), std::move(key)); }, "Consumed key reused"
  );
  SrtpReceiver oldReceiver(config(0x10203047, 8), testKey(7));
  SrtpSender newSender(config(0x10203048, 9, 240), testKey(8));
  SrtpReceiver newReceiver(config(0x10203048, 9, 240), testKey(8));
  const auto oldPacket = copyPacket(oldSender.protect(kPayload, 1, 0));
  const auto newPacket = copyPacket(newSender.protect(kPayload, 1, 0));
  rejected(newReceiver.unprotect(oldPacket), "Old generation switched new receiver");
  rejected(oldReceiver.unprotect(newPacket), "New packet switched old receiver");
  const auto newResult = newReceiver.unprotect(newPacket);
  payloadEquals(newResult, kPayload);
  require(
      newResult.metadata.generation == 9 && newResult.metadata.frameSamples == 240,
      "New generation did not use trusted configuration"
  );
  const auto oldResult = oldReceiver.unprotect(oldPacket);
  payloadEquals(oldResult, kPayload);
  require(
      oldResult.metadata.generation == 8 && oldResult.metadata.frameSamples == 480,
      "Rejected packets changed old configuration"
  );
}

void contextLifetimeAndIndependentThreads()
{
  // Repeated construction/deallocation must not shut down a still-live context.
  const auto stream = config(0x10203049, 10);
  SrtpSender sender(stream, testKey(9));
  SrtpReceiver receiver(stream, testKey(9));
  for (std::uint16_t i = 0; i < 16; ++i) {
    {
      SrtpSender temporary(config(0x20203040U + i, 100U + i), testKey(static_cast<std::uint8_t>(20 + i)));
      require(bool(temporary.protect(kPayload, 0, 0)), "Temporary context failed");
    }
    payloadEquals(receiver.unprotect(copyPacket(sender.protect(kPayload, i, i * 480U))), kPayload);
  }

  Bytes wire;
  std::exception_ptr error;
  // No instance is shared/concurrently touched: sender and receiver each own their context
  // on a different thread. join is the only synchronization; no sleeps or timing assertions.
  std::thread tx([&] {
    try {
      SrtpSender threaded(config(0x10203050, 11), testKey(10));
      wire = copyPacket(threaded.protect(kPayload, 77, 48000));
    } catch (...) {
      error = std::current_exception();
    }
  });
  tx.join();
  if (error) {
    std::rethrow_exception(error);
  }
  std::thread rx([&] {
    try {
      SrtpReceiver threaded(config(0x10203050, 11), testKey(10));
      payloadEquals(threaded.unprotect(wire), kPayload);
    } catch (...) {
      error = std::current_exception();
    }
  });
  rx.join();
  if (error) {
    std::rethrow_exception(error);
  }
}

static_assert(!std::is_copy_constructible_v<SrtpKey> && !std::is_copy_assignable_v<SrtpKey>);
static_assert(std::is_nothrow_move_constructible_v<SrtpKey>);
static_assert(!std::is_copy_constructible_v<SrtpSender> && !std::is_move_constructible_v<SrtpSender>);
static_assert(!std::is_copy_constructible_v<SrtpReceiver> && !std::is_move_constructible_v<SrtpReceiver>);

} // namespace

// Standalone standard-library runner: no gtest, Qt, sockets, device or wall-clock dependency.
int main()
{
  struct Test
  {
    const char *name;
    void (*run)();
  };
  constexpr std::array tests{
      Test{"round trip and RFC3550 base header", roundTripAndHeader},
      Test{"every header/ciphertext/tag bit tamper", tamperEveryBit},
      Test{"replay, old-window rejection and reordering", replayAndReorder},
      Test{"SSRC binding and key mismatch", wrongSsrcAndKey},
      Test{"every truncation and strict parsing", truncationAndStrictParsing},
      Test{"MTU boundaries and fixed borrowed storage", mtuAndBorrowedStorage},
      Test{"sequence, ROC and timestamp wrap", sequenceAndTimestampWrap},
      Test{"trusted config, one-shot key and generation", trustedConfigAndGeneration},
      Test{"runtime lifetime and independent thread contexts", contextLifetimeAndIndependentThreads}
  };
  unsigned failures = 0;
  for (const auto &test : tests) {
    try {
      test.run();
      std::cout << "[PASS] " << test.name << '\n';
    } catch (const std::exception &error) {
      ++failures;
      std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
    } catch (...) {
      ++failures;
      std::cerr << "[FAIL] " << test.name << ": unknown exception\n";
    }
  }
  std::cout << tests.size() - failures << '/' << tests.size() << " transport tests passed\n";
  return failures == 0 ? 0 : 1;
}
