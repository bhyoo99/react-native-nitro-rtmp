// ADTS header for raw AAC frames. AudioToolbox and
// MediaCodec both deliver raw AAC access units; the core's pushAudio wants
// one ADTS frame per call with protection_absent = 1 (no CRC).
#pragma once

#include <cstddef>
#include <cstdint>

namespace nitrortmp {

constexpr size_t kAdtsHeaderSize = 7;

/// MPEG-4 sampling frequency index for `sampleRate`, or -1 when the rate is
/// not one of the 13 standard values.
int adtsSampleRateIndex(int sampleRate);

/// Writes the 7-byte header for one AAC frame. `profile` is the MPEG-4 audio
/// object type minus one (AAC-LC = 1). `channels` is 1..7 (the channel
/// configuration). `payloadSize` is the raw AAC frame size; the header's
/// frame_length field is payloadSize + 7 and must fit in 13 bits.
/// @return false (and leaves `out` untouched) when a value is out of range.
bool writeAdtsHeader(uint8_t out[kAdtsHeaderSize], int profile, int sampleRate, int channels, size_t payloadSize);

}  // namespace nitrortmp
