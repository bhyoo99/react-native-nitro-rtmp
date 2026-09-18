// H.264 byte-stream helpers for the platform encoders.
//
// VideoToolbox hands out AVCC samples (length-prefixed NALUs) plus the
// parameter sets in the format description; MediaCodec hands out Annex-B and
// the parameter sets once, as a codec-config buffer. The core (pushVideo)
// wants Annex-B access units whose keyframes carry SPS and PPS, so both
// encoders normalize their output with these three functions.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace nitrortmp {

/// Rewrites an AVCC sample (every NALU prefixed by a `lengthSize` byte
/// big-endian length, 1..4) as Annex-B (4-byte start codes). A truncated
/// NALU ends the output: the bytes before it are kept, the rest is dropped.
std::vector<uint8_t> avccToAnnexB(const uint8_t* avcc, size_t size, int lengthSize);

/// `sps` and `pps` are raw NALUs (no start code, no length). Returns
/// start code + SPS + start code + PPS + `annexb`. Empty parameter sets are
/// skipped, so the result is always a valid Annex-B stream.
std::vector<uint8_t> prependParameterSets(const std::vector<uint8_t>& sps, const std::vector<uint8_t>& pps,
                                          const uint8_t* annexb, size_t size);

/// True when the Annex-B buffer contains an IDR slice (NALU type 5). Same
/// rule as the core's keyframe gate, so what this accepts, pushVideo accepts.
bool isKeyframe(const uint8_t* annexb, size_t size);

/// True when the Annex-B buffer contains an SPS NALU (type 7) before the
/// first slice; used to avoid doubling parameter sets the encoder already emitted.
bool hasParameterSets(const uint8_t* annexb, size_t size);

}  // namespace nitrortmp
