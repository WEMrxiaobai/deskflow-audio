// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once

// Backend-private, allocation-free conversion helpers. Not an audio DSP API.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <objbase.h> // Must precede ks.h, which defines a GUID_NULL macro.

#include <mmreg.h>

#include <ks.h>
#include <ksmedia.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace deskflow::audio::windows::detail {

enum class SampleType
{
  Float32,
  Int16,
  Int32
};

struct PcmLayout
{
  SampleType type = SampleType::Float32;
  std::uint16_t validBits = 32;
  std::uint16_t sampleBytes = 4;
  std::uint32_t channelMask = 0;
};

inline bool parseFormat(const WAVEFORMATEX &format, PcmLayout &out) noexcept
{
  if (format.nChannels == 0 || format.nChannels > 32 || format.nSamplesPerSec == 0 || format.nSamplesPerSec > 768000)
    return false;

  WORD tag = format.wFormatTag;
  WORD validBits = format.wBitsPerSample;
  DWORD mask = 0;
  if (tag == WAVE_FORMAT_EXTENSIBLE) {
    if (format.cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
      return false;
    const auto &ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE &>(format);
    validBits = ext.Samples.wValidBitsPerSample;
    mask = ext.dwChannelMask;
    if (IsEqualGUID(ext.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))
      tag = WAVE_FORMAT_IEEE_FLOAT;
    else if (IsEqualGUID(ext.SubFormat, KSDATAFORMAT_SUBTYPE_PCM))
      tag = WAVE_FORMAT_PCM;
    else
      return false;
  }

  PcmLayout result;
  result.validBits = validBits;
  result.channelMask = mask;
  if (tag == WAVE_FORMAT_IEEE_FLOAT && format.wBitsPerSample == 32 && validBits == 32) {
    result.type = SampleType::Float32;
    result.sampleBytes = 4;
  } else if (tag == WAVE_FORMAT_PCM && format.wBitsPerSample == 16 && validBits > 0 && validBits <= 16) {
    result.type = SampleType::Int16;
    result.sampleBytes = 2;
  } else if (tag == WAVE_FORMAT_PCM && format.wBitsPerSample == 32 && validBits > 0 && validBits <= 32) {
    result.type = SampleType::Int32;
    result.sampleBytes = 4;
  } else {
    return false; // Packed 24-bit and float64 are deliberately not reinterpreted.
  }

  const auto align = static_cast<DWORD>(format.nChannels) * result.sampleBytes;
  if (format.nBlockAlign != align ||
      static_cast<std::uint64_t>(format.nSamplesPerSec) * align != format.nAvgBytesPerSec)
    return false;
  if (mask != 0) {
    unsigned bits = 0;
    for (DWORD m = mask; m; m &= m - 1)
      ++bits;
    if (bits != format.nChannels)
      return false;
  }
  out = result;
  return true;
}

inline float finiteSample(float value) noexcept
{
  return std::isfinite(value) ? value : 0.0f;
}

inline void decodePcm(const BYTE *source, std::span<float> destination, const PcmLayout &layout) noexcept
{
  // A SILENT WASAPI buffer is permitted to have a null source. The caller must
  // select this path from its flags, not try to inspect the borrowed pointer.
  if (!source) {
    std::fill(destination.begin(), destination.end(), 0.0f);
    return;
  }
  for (std::size_t i = 0; i < destination.size(); ++i) {
    switch (layout.type) {
    case SampleType::Float32: {
      float sample;
      std::memcpy(&sample, source + i * 4, 4);
      destination[i] = finiteSample(sample);
      break;
    }
    case SampleType::Int16: {
      std::int16_t sample;
      std::memcpy(&sample, source + i * 2, 2);
      destination[i] = static_cast<float>(sample) / 32768.0f;
      break;
    }
    case SampleType::Int32: {
      std::int32_t sample;
      std::memcpy(&sample, source + i * 4, 4);
      destination[i] = static_cast<float>(static_cast<double>(sample) / 2147483648.0);
      break;
    }
    }
  }
}

inline void encodePcm(std::span<const float> source, BYTE *destination, const PcmLayout &layout) noexcept
{
  for (std::size_t i = 0; i < source.size(); ++i) {
    const float sample = std::clamp(finiteSample(source[i]), -1.0f, 1.0f);
    switch (layout.type) {
    case SampleType::Float32:
      std::memcpy(destination + i * 4, &sample, 4);
      break;
    case SampleType::Int16: {
      const auto integer =
          static_cast<std::int32_t>(std::clamp(static_cast<double>(sample) * 32768.0, -32768.0, 32767.0));
      auto bits = static_cast<std::uint16_t>(integer);
      bits &= static_cast<std::uint16_t>(0xffffu << (16 - layout.validBits));
      std::memcpy(destination + i * 2, &bits, 2);
      break;
    }
    case SampleType::Int32: {
      // Double is essential: float(INT32_MAX) rounds up to 2^31. Converting
      // that float directly to int32 would be undefined at positive full scale.
      const auto integer =
          static_cast<std::int64_t>(std::clamp(static_cast<double>(sample) * 2147483648.0, -2147483648.0, 2147483647.0)
          );
      auto bits = static_cast<std::uint32_t>(integer);
      bits &= 0xffffffffu << (32 - layout.validBits);
      std::memcpy(destination + i * 4, &bits, 4);
      break;
    }
    }
  }
}

inline UINT32 shortestPeriod(UINT32 fundamental, UINT32 minimum, UINT32 maximum) noexcept
{
  if (!fundamental || !minimum || minimum > maximum)
    return 0;
  const auto rounded = ((static_cast<std::uint64_t>(minimum) + fundamental - 1) / fundamental) * fundamental;
  return rounded <= maximum ? static_cast<UINT32>(rounded) : 0;
}

} // namespace deskflow::audio::windows::detail
