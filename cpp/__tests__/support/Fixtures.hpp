#pragma once

// Fixture loading and the small media parsers the tests need: an H.264
// Annex-B access-unit splitter, an ADTS frame splitter, an FLV file parser and
// a reference muxer that runs the vendored flv_muxer directly.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace test {

std::string fixturePath(const char* name);
std::vector<uint8_t> readFile(const std::string& path);  // throws std::runtime_error

struct AccessUnit {
  std::vector<uint8_t> data;  // Annex-B, start codes kept
  bool keyframe = false;      // contains an IDR slice
};

/// Splits a raw H.264 Annex-B stream into access units using the same rules
/// as FFmpeg's parser: a new AU starts at SPS/PPS/SEI/AUD after a VCL NALU, or
/// at a VCL NALU with first_mb_in_slice == 0 after another VCL NALU.
std::vector<AccessUnit> splitAnnexB(const std::vector<uint8_t>& stream);

/// Splits an ADTS stream into frames (header + payload).
std::vector<std::vector<uint8_t>> splitAdts(const std::vector<uint8_t>& stream);

/// FFmpeg rounding for the fixture clock: round(n * 1000 / fps).
uint32_t videoTimestampMs(size_t n, double fps = 30.0);
/// round(n * 1024 * 1000 / sampleRate)
uint32_t audioTimestampMs(size_t n, int sampleRate = 48000);

struct MediaFrame {
  bool video = false;
  std::vector<uint8_t> data;
  uint32_t pts = 0;
  uint32_t dts = 0;
};

/// The fixture as an encoder would deliver it: video and audio frames merged
/// by dts, video first on ties.
std::vector<MediaFrame> loadFixtureFrames();

struct FlvTag {
  int type = 0;  // 8 audio, 9 video, 18 script
  uint32_t timestamp = 0;
  std::vector<uint8_t> data;
};

/// Parses an FLV file (header + tags). Throws std::runtime_error on damage.
std::vector<FlvTag> parseFlv(const std::vector<uint8_t>& file);

/// Runs the vendored flv_muxer over the frames, in order. This is what the
/// publisher is expected to emit when the first frame has dts 0.
std::vector<FlvTag> muxWithVendor(const std::vector<MediaFrame>& frames);

/// Synthetic frames for unit tests (not decodable, but structurally valid).
std::vector<uint8_t> syntheticSpsPps();
std::vector<uint8_t> syntheticIdr(bool withSpsPps, uint8_t seed = 0);
std::vector<uint8_t> syntheticPFrame(uint8_t seed = 0);
std::vector<uint8_t> syntheticAdts(size_t payloadSize, uint8_t seed = 0);

bool contains(const std::vector<uint8_t>& haystack, const std::string& needle);

}  // namespace test

namespace test {

/// Keeps tags of one type, dropping the AVC end-of-sequence tag FFmpeg writes on
/// close and the empty AAC sequence header FFmpeg writes for ADTS input.
std::vector<FlvTag> filterTags(const std::vector<FlvTag>& tags, int type);

/// "" when both sequences match tag for tag (type, timestamp, bytes), else a
/// description of the first difference.
std::string diffTags(const std::vector<FlvTag>& actual, const std::vector<FlvTag>& expected, const char* label);

}  // namespace test
