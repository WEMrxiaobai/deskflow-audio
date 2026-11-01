/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include "Pcm.h"

namespace deskflow::audio::dsp {

struct PacketInfo
{
  std::uint16_t sequence = 0;
  std::uint64_t timestampFrames = 0;
  std::uint64_t generation = 0;
  bool resumed = false;
};
struct JitterStats
{
  std::uint64_t accepted = 0;
  std::uint64_t delivered = 0;
  std::uint64_t lost = 0;
  std::uint64_t late = 0;
  std::uint64_t duplicates = 0;
  std::uint64_t outOfWindow = 0;
  std::uint64_t wrongGeneration = 0;
  std::uint64_t invalid = 0;
};
enum class PacketInsert
{
  Accepted,
  Late,
  Duplicate,
  OutOfWindow,
  WrongGeneration,
  Invalid
};
enum class PacketPoll
{
  Waiting,
  Packet,
  Loss,
  DestinationTooSmall,
  NotStarted
};
struct PacketResult
{
  PacketPoll status = PacketPoll::NotStarted;
  PacketInfo info{};
  std::size_t bytes = 0;
  std::uint64_t deadlineNs = 0;
};

// Independent compressed/network packet reorder buffer, NOT decoded PCM storage.
// Single network/decoder worker owns all calls (external SPSC ingress if needed).
// Fixed preallocated slots and maximum payload; no allocations after construction.
// Fixed packet duration in frames/ns; slots are capacity, not startup delay. start()
// specifies an independent first playout deadline on the caller's monotonic clock.
// No authentication/replay security: only insert packets authenticated by the transport.
// 16-bit sequence wrap is supported with a strictly <32768-packet forward window.
// Long silence/resumption, SSRC replacement, or new format requires explicit start(),
// with a new generation; old-generation packets cannot reset/poison the stream.
class JitterBuffer
{
public:
  JitterBuffer(
      std::size_t capacityPackets, std::size_t maxPayloadBytes, std::uint32_t packetFrames,
      std::uint64_t packetDurationNs
  )
      : m_slots(validCapacity(capacityPackets)),
        m_payload(detail::checkedProduct(capacityPackets, maxPayloadBytes)),
        m_stride(maxPayloadBytes),
        m_packetFrames(packetFrames),
        m_nominalDuration(packetDurationNs),
        m_duration(packetDurationNs)
  {
    if (packetFrames == 0 || packetDurationNs == 0) {
      throw std::invalid_argument("Packet duration must be positive");
    }
  }
  // Explicit application-authorized stream initialization. Clears pending packets but
  // leaves lifetime counters intact. Timestamp arithmetic is modulo uint64_t; ns deadlines
  // saturate at UINT64_MAX (caller must replace stream before exhausting the clock).
  void start(
      std::uint64_t generation, std::uint16_t firstSequence, std::uint64_t firstTimestampFrames,
      std::uint64_t firstDeadlineNs
  ) noexcept
  {
    for (auto &slot : m_slots) {
      slot.occupied = false;
    }
    m_duration = m_nominalDuration;
    m_generation = generation;
    m_expected = firstSequence;
    m_timestamp = firstTimestampFrames;
    m_deadline = firstDeadlineNs;
    m_queued = 0;
    m_started = true;
  }
  void stop() noexcept
  {
    m_started = false;
    m_queued = 0;
    for (auto &slot : m_slots) {
      slot.occupied = false;
    }
  }
  [[nodiscard]] PacketInsert
  insert(PacketInfo info, std::span<const std::uint8_t> payload, std::uint64_t nowNs) noexcept
  {
    if (!m_started || payload.empty() || payload.size() > m_stride) {
      ++m_stats.invalid;
      return PacketInsert::Invalid;
    }
    if (info.generation != m_generation) {
      ++m_stats.wrongGeneration;
      return PacketInsert::WrongGeneration;
    }
    const auto delta = static_cast<std::uint16_t>(info.sequence - static_cast<std::uint16_t>(m_expected));
    if (delta >= 32768) {
      ++m_stats.late;
      return PacketInsert::Late;
    }
    if (delta >= m_slots.size()) {
      ++m_stats.outOfWindow;
      return PacketInsert::OutOfWindow;
    }
    const auto deadline = addSaturated(m_deadline, multiplySaturated(delta, m_duration));
    if (nowNs >= deadline) {
      ++m_stats.late;
      return PacketInsert::Late;
    }
    const auto extended = m_expected + delta;
    auto &slot = m_slots[extended % m_slots.size()];
    if (slot.occupied) {
      ++m_stats.duplicates;
      return PacketInsert::Duplicate;
    }
    // Constant-duration media timeline must agree with its sequence, including wrap.
    if (info.timestampFrames != m_timestamp + std::uint64_t(delta) * m_packetFrames) {
      ++m_stats.invalid;
      return PacketInsert::Invalid;
    }
    std::copy(payload.begin(), payload.end(), m_payload.begin() + (extended % m_slots.size()) * m_stride);
    slot = {info, payload.size(), true};
    ++m_queued;
    ++m_stats.accepted;
    return PacketInsert::Accepted;
  }
  // One decision per call. A packet is held until its playout deadline; an absent packet
  // becomes Loss exactly at that deadline, without waiting indefinitely for FEC/reorder.
  // Loss provides sequence/timestamp to the decoder for PLC. Too-small destination leaves
  // state unchanged, allowing retry. Caller bounds catch-up work per worker iteration.
  [[nodiscard]] PacketResult poll(std::uint64_t nowNs, std::span<std::uint8_t> destination) noexcept
  {
    PacketResult result;
    if (!m_started) {
      return result;
    }
    result.deadlineNs = m_deadline;
    result.info = {static_cast<std::uint16_t>(m_expected), m_timestamp, m_generation, false};
    if (nowNs < m_deadline) {
      result.status = PacketPoll::Waiting;
      return result;
    }
    const auto index = m_expected % m_slots.size();
    auto &slot = m_slots[index];
    if (slot.occupied) {
      result.bytes = slot.bytes;
      result.info = slot.info;
      if (destination.size() < slot.bytes) {
        result.status = PacketPoll::DestinationTooSmall;
        return result;
      }
      std::copy_n(m_payload.begin() + index * m_stride, slot.bytes, destination.begin());
      slot.occupied = false;
      --m_queued;
      ++m_stats.delivered;
      result.status = PacketPoll::Packet;
    } else {
      ++m_stats.lost;
      result.status = PacketPoll::Loss;
    }
    ++m_expected;
    m_timestamp += m_packetFrames;
    m_deadline = addSaturated(m_deadline, m_duration);
    return result;
  }
  // Worker-only clock mapping. Changes FUTURE steps, never moves an already promised
  // deadline. The trusted authenticated-arrival PLL is bounded to +/-2000ppm.
  [[nodiscard]] bool setPacketDurationNs(std::uint64_t durationNs) noexcept
  {
    const auto minimum = m_nominalDuration - m_nominalDuration / 500;
    const auto maximum = addSaturated(m_nominalDuration, m_nominalDuration / 500);
    if (durationNs < minimum || durationNs > maximum)
      return false;
    m_duration = durationNs;
    return true;
  }
  [[nodiscard]] std::uint64_t deadlineFor(std::uint16_t sequence) const noexcept
  {
    if (!m_started)
      return 0;
    const auto unsignedDelta = static_cast<std::uint16_t>(sequence - static_cast<std::uint16_t>(m_expected));
    const auto delta = unsignedDelta < 32768 ? std::int32_t(unsignedDelta) : std::int32_t(unsignedDelta) - 65536;
    if (delta >= 0)
      return addSaturated(m_deadline, multiplySaturated(static_cast<std::uint64_t>(delta), m_duration));
    const auto behind = multiplySaturated(static_cast<std::uint64_t>(-delta), m_duration);
    return behind > m_deadline ? 0 : m_deadline - behind;
  }
  [[nodiscard]] std::size_t queuedPackets() const noexcept
  {
    return m_queued;
  }
  [[nodiscard]] std::size_t capacityPackets() const noexcept
  {
    return m_slots.size();
  }
  [[nodiscard]] JitterStats stats() const noexcept
  {
    return m_stats;
  }

private:
  struct Slot
  {
    PacketInfo info{};
    std::size_t bytes = 0;
    bool occupied = false;
  };
  static std::size_t validCapacity(std::size_t capacity)
  {
    if (capacity == 0 || capacity >= 32768) {
      throw std::invalid_argument("Jitter capacity must be 1..32767 packets");
    }
    return capacity;
  }
  static std::uint64_t addSaturated(std::uint64_t a, std::uint64_t b) noexcept
  {
    return a > std::numeric_limits<std::uint64_t>::max() - b ? std::numeric_limits<std::uint64_t>::max() : a + b;
  }
  static std::uint64_t multiplySaturated(std::uint64_t a, std::uint64_t b) noexcept
  {
    return b != 0 && a > std::numeric_limits<std::uint64_t>::max() / b ? std::numeric_limits<std::uint64_t>::max()
                                                                       : a * b;
  }
  std::vector<Slot> m_slots;
  std::vector<std::uint8_t> m_payload;
  std::size_t m_stride, m_queued = 0;
  std::uint32_t m_packetFrames;
  const std::uint64_t m_nominalDuration;
  std::uint64_t m_duration, m_expected = 0, m_generation = 0, m_timestamp = 0, m_deadline = 0;
  bool m_started = false;
  JitterStats m_stats{};
};
} // namespace deskflow::audio::dsp
