#include "Fixtures.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

extern "C" {
#include "flv-muxer.h"
#include "flv-proto.h"
}

namespace test {

std::string fixturePath(const char* name) {
  return std::string(NITRORTMP_FIXTURE_DIR) + "/" + name;
}

std::vector<uint8_t> readFile(const std::string& path) {
  std::FILE* fp = std::fopen(path.c_str(), "rb");
  if (fp == nullptr) {
    throw std::runtime_error("cannot open " + path);
  }
  std::vector<uint8_t> data;
  uint8_t buffer[64 * 1024];
  size_t n;
  while ((n = std::fread(buffer, 1, sizeof(buffer), fp)) > 0) {
    data.insert(data.end(), buffer, buffer + n);
  }
  std::fclose(fp);
  return data;
}

namespace {

struct Nalu {
  size_t begin;  // offset of the start code
  size_t payload;  // offset of the NALU header byte
  size_t end;    // one past the last byte (before the next start code)
};

std::vector<Nalu> findNalus(const std::vector<uint8_t>& s) {
  std::vector<Nalu> out;
  size_t i = 0;
  while (i + 3 <= s.size()) {
    if (s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1) {
      size_t begin = i;
      if (begin > 0 && s[begin - 1] == 0) --begin;  // 4-byte start code
      if (!out.empty()) out.back().end = begin;
      out.push_back({begin, i + 3, s.size()});
      i += 3;
    } else {
      ++i;
    }
  }
  return out;
}

}  // namespace

std::vector<AccessUnit> splitAnnexB(const std::vector<uint8_t>& stream) {
  std::vector<AccessUnit> units;
  AccessUnit current;
  bool currentHasVcl = false;
  for (const Nalu& n : findNalus(stream)) {
    if (n.payload >= n.end) continue;
    const uint8_t type = stream[n.payload] & 0x1F;
    const bool vcl = type >= 1 && type <= 5;
    bool startsNew = false;
    if (currentHasVcl) {
      if (!vcl && (type == 6 || type == 7 || type == 8 || type == 9)) {
        startsNew = true;
      } else if (vcl && n.payload + 1 < n.end && (stream[n.payload + 1] & 0x80) != 0) {
        startsNew = true;  // first_mb_in_slice == 0
      }
    }
    if (startsNew) {
      units.push_back(std::move(current));
      current = AccessUnit{};
      currentHasVcl = false;
    }
    current.data.insert(current.data.end(), stream.begin() + static_cast<long>(n.begin), stream.begin() + static_cast<long>(n.end));
    if (vcl) currentHasVcl = true;
    if (type == 5) current.keyframe = true;
  }
  if (!current.data.empty()) units.push_back(std::move(current));
  return units;
}

std::vector<std::vector<uint8_t>> splitAdts(const std::vector<uint8_t>& s) {
  std::vector<std::vector<uint8_t>> frames;
  size_t i = 0;
  while (i + 7 <= s.size()) {
    if (s[i] != 0xFF || (s[i + 1] & 0xF6) != 0xF0) {
      throw std::runtime_error("ADTS sync lost at offset " + std::to_string(i));
    }
    const size_t length = (static_cast<size_t>(s[i + 3] & 0x03) << 11) | (static_cast<size_t>(s[i + 4]) << 3) | (s[i + 5] >> 5);
    if (length < 7 || i + length > s.size()) {
      throw std::runtime_error("ADTS frame length out of range at offset " + std::to_string(i));
    }
    frames.emplace_back(s.begin() + static_cast<long>(i), s.begin() + static_cast<long>(i + length));
    i += length;
  }
  return frames;
}

uint32_t videoTimestampMs(size_t n, double fps) {
  return static_cast<uint32_t>(std::llround(static_cast<double>(n) * 1000.0 / fps));
}

uint32_t audioTimestampMs(size_t n, int sampleRate) {
  return static_cast<uint32_t>(std::llround(static_cast<double>(n) * 1024.0 * 1000.0 / sampleRate));
}

std::vector<MediaFrame> loadFixtureFrames() {
  const std::vector<AccessUnit> video = splitAnnexB(readFile(fixturePath("video.h264")));
  const std::vector<std::vector<uint8_t>> audio = splitAdts(readFile(fixturePath("audio.aac")));
  std::vector<MediaFrame> frames;
  size_t v = 0, a = 0;
  while (v < video.size() || a < audio.size()) {
    const uint32_t vts = v < video.size() ? videoTimestampMs(v) : UINT32_MAX;
    const uint32_t ats = a < audio.size() ? audioTimestampMs(a) : UINT32_MAX;
    if (vts <= ats) {
      frames.push_back({true, video[v].data, vts, vts});
      ++v;
    } else {
      frames.push_back({false, audio[a], ats, ats});
      ++a;
    }
  }
  return frames;
}

std::vector<FlvTag> parseFlv(const std::vector<uint8_t>& f) {
  if (f.size() < 13 || f[0] != 'F' || f[1] != 'L' || f[2] != 'V') {
    throw std::runtime_error("not an FLV file");
  }
  const size_t dataOffset = (static_cast<size_t>(f[5]) << 24) | (f[6] << 16) | (f[7] << 8) | f[8];
  std::vector<FlvTag> tags;
  size_t i = dataOffset + 4;  // PreviousTagSize0
  while (i + 11 <= f.size()) {
    FlvTag tag;
    tag.type = f[i] & 0x1F;
    const size_t size = (static_cast<size_t>(f[i + 1]) << 16) | (f[i + 2] << 8) | f[i + 3];
    tag.timestamp = (static_cast<uint32_t>(f[i + 4]) << 16) | (f[i + 5] << 8) | f[i + 6] | (static_cast<uint32_t>(f[i + 7]) << 24);
    if (i + 11 + size + 4 > f.size()) {
      throw std::runtime_error("truncated FLV tag at offset " + std::to_string(i));
    }
    tag.data.assign(f.begin() + static_cast<long>(i + 11), f.begin() + static_cast<long>(i + 11 + size));
    tags.push_back(std::move(tag));
    i += 11 + size + 4;
  }
  return tags;
}

namespace {
int collectTag(void* param, int type, const void* data, size_t bytes, uint32_t timestamp) {
  auto* tags = static_cast<std::vector<FlvTag>*>(param);
  FlvTag tag;
  tag.type = type;
  tag.timestamp = timestamp;
  tag.data.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + bytes);
  tags->push_back(std::move(tag));
  return 0;
}
}  // namespace

std::vector<FlvTag> muxWithVendor(const std::vector<MediaFrame>& frames) {
  std::vector<FlvTag> tags;
  flv_muxer_t* muxer = flv_muxer_create(&collectTag, &tags);
  for (const MediaFrame& frame : frames) {
    const int r = frame.video ? flv_muxer_avc(muxer, frame.data.data(), frame.data.size(), frame.pts, frame.dts)
                              : flv_muxer_aac(muxer, frame.data.data(), frame.data.size(), frame.pts, frame.dts);
    if (r != 0) {
      flv_muxer_destroy(muxer);
      throw std::runtime_error("flv_muxer rejected a fixture frame");
    }
  }
  flv_muxer_destroy(muxer);
  return tags;
}

// Baseline profile SPS/PPS (structurally valid ids: sps_id 0, pps_id 0).
std::vector<uint8_t> syntheticSpsPps() {
  return {0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xC0, 0x1E, 0xDA, 0x02, 0x80, 0xF6, 0x80, 0x6D, 0x0A, 0x13, 0x50,
          0x00, 0x00, 0x00, 0x01, 0x68, 0xCE, 0x38, 0x80};
}

std::vector<uint8_t> syntheticIdr(bool withSpsPps, uint8_t seed) {
  std::vector<uint8_t> out;
  if (withSpsPps) out = syntheticSpsPps();
  const uint8_t idr[] = {0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x00, 0x10, seed, 0x3F, 0xFF, 0xA0};
  out.insert(out.end(), idr, idr + sizeof(idr));
  return out;
}

std::vector<uint8_t> syntheticPFrame(uint8_t seed) {
  return {0x00, 0x00, 0x00, 0x01, 0x41, 0x9A, 0x02, 0x04, seed, 0x7F, 0xF0};
}

std::vector<uint8_t> syntheticAdts(size_t payloadSize, uint8_t seed) {
  const size_t length = 7 + payloadSize;
  std::vector<uint8_t> out = {
      0xFF, 0xF1,
      static_cast<uint8_t>((1 << 6) | (3 << 2)),  // AAC LC, 48 kHz
      static_cast<uint8_t>((2 << 6) | ((length >> 11) & 0x03)),  // stereo
      static_cast<uint8_t>((length >> 3) & 0xFF),
      static_cast<uint8_t>(((length & 0x07) << 5) | 0x1F),
      0xFC,
  };
  for (size_t i = 0; i < payloadSize; ++i) out.push_back(static_cast<uint8_t>(seed + i));
  return out;
}

bool contains(const std::vector<uint8_t>& haystack, const std::string& needle) {
  if (needle.empty() || haystack.size() < needle.size()) return false;
  for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
    if (std::memcmp(haystack.data() + i, needle.data(), needle.size()) == 0) return true;
  }
  return false;
}

}  // namespace test

namespace test {

std::vector<FlvTag> filterTags(const std::vector<FlvTag>& tags, int type) {
  std::vector<FlvTag> out;
  for (const FlvTag& t : tags) {
    if (t.type != type) continue;
    if (type == 9 && t.data.size() == 5 && (t.data[0] & 0x0F) == 7 && t.data[1] == 2) continue;  // AVC end of sequence
    // FFmpeg writes an AAC sequence header with an empty AudioSpecificConfig
    // before the aac_adtstoasc filter has produced one, then a real one. Not a
    // valid tag by the spec; the publisher never emits it.
    if (type == 8 && t.data.size() == 2 && (t.data[0] & 0xF0) == 0xA0 && t.data[1] == 0) continue;
    out.push_back(t);
  }
  return out;
}

std::string diffTags(const std::vector<FlvTag>& actual, const std::vector<FlvTag>& expected, const char* label) {
  const size_t n = std::min(actual.size(), expected.size());
  for (size_t i = 0; i < n; ++i) {
    const FlvTag& a = actual[i];
    const FlvTag& e = expected[i];
    if (a.type != e.type) {
      return std::string(label) + " tag " + std::to_string(i) + ": type " + std::to_string(a.type) + " vs " + std::to_string(e.type);
    }
    if (a.timestamp != e.timestamp) {
      return std::string(label) + " tag " + std::to_string(i) + ": timestamp " + std::to_string(a.timestamp) + " vs " + std::to_string(e.timestamp);
    }
    if (a.data != e.data) {
      size_t k = 0;
      while (k < a.data.size() && k < e.data.size() && a.data[k] == e.data[k]) ++k;
      return std::string(label) + " tag " + std::to_string(i) + ": body differs at byte " + std::to_string(k) + " (sizes " +
             std::to_string(a.data.size()) + " vs " + std::to_string(e.data.size()) + ")";
    }
  }
  if (actual.size() != expected.size()) {
    return std::string(label) + ": " + std::to_string(actual.size()) + " tags vs " + std::to_string(expected.size()) + " expected";
  }
  return "";
}

}  // namespace test
