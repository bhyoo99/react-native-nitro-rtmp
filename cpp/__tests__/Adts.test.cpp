// ADTS header writer (cpp/nitrortmp/Adts.hpp): byte-identical to the headers
// FFmpeg wrote for the fixture, accepted by the fixture splitter and by the
// core's pushAudio, and range-checked.
#include <cstring>
#include <vector>

#include "Adts.hpp"
#include "Fixtures.hpp"
#include "Loopback.hpp"
#include "RtmpPublisher.hpp"
#include "TestHarness.hpp"
#include "nitrortmp_c.h"

TEST(header_matches_the_fixture) {
  // audio.aac is AAC-LC, 48 kHz, stereo, no CRC (see fixtures/README.md).
  const std::vector<std::vector<uint8_t>> frames = test::splitAdts(test::readFile(test::fixturePath("audio.aac")));
  ASSERT_TRUE(frames.size() > 90);
  for (const std::vector<uint8_t>& frame : frames) {
    uint8_t header[nitrortmp::kAdtsHeaderSize];
    ASSERT_TRUE(nitrortmp::writeAdtsHeader(header, 1, 48000, 2, frame.size() - nitrortmp::kAdtsHeaderSize));
    ASSERT_TRUE(test::checkBytesEq(header, sizeof(header), frame.data(), nitrortmp::kAdtsHeaderSize, "adts header",
                                   __FILE__, __LINE__));
  }
}

TEST(header_plus_payload_is_one_adts_frame) {
  for (const int rate : {48000, 44100, 16000}) {
    for (const int channels : {1, 2}) {
      const size_t payload = 371;
      std::vector<uint8_t> frame(nitrortmp::kAdtsHeaderSize + payload, 0xAB);
      ASSERT_TRUE(nitrortmp::writeAdtsHeader(frame.data(), 1, rate, channels, payload));
      const std::vector<std::vector<uint8_t>> split = test::splitAdts(frame);
      ASSERT_EQ(split.size(), 1u);
      EXPECT_EQ(split[0].size(), frame.size());
      EXPECT_EQ(frame[1] & 0x01, 1);  // protection_absent
      EXPECT_EQ((frame[2] >> 6) & 0x03, 1);  // AAC-LC
      EXPECT_EQ((frame[2] >> 2) & 0x0F, nitrortmp::adtsSampleRateIndex(rate));
      EXPECT_EQ(((frame[2] & 0x01) << 2) | (frame[3] >> 6), channels);
    }
  }
}

TEST(core_accepts_headered_frames) {
  std::vector<uint8_t> sent;
  nitrortmp::PublisherCallbacks cb;
  cb.onSend = [&](const uint8_t* d, size_t n) { sent.insert(sent.end(), d, d + n); };
  nitrortmp::RtmpPublisher pub(std::move(cb));
  test::LoopbackServer server;
  ASSERT_TRUE(pub.start("rtmp://127.0.0.1/live/test"));
  for (;;) {
    bool moved = false;
    if (!sent.empty()) {
      std::vector<uint8_t> b;
      b.swap(sent);
      server.input(b.data(), b.size());
      moved = true;
    }
    if (!server.output.empty()) {
      std::vector<uint8_t> b;
      b.swap(server.output);
      pub.onReceive(b.data(), b.size());
      moved = true;
    }
    if (!moved) break;
  }
  ASSERT_EQ(pub.state(), nitrortmp::PublisherState::Publishing);

  // The raw payload of a fixture frame, re-headered by the helper.
  const std::vector<std::vector<uint8_t>> frames = test::splitAdts(test::readFile(test::fixturePath("audio.aac")));
  std::vector<uint8_t> frame = frames[3];
  std::memset(frame.data(), 0, nitrortmp::kAdtsHeaderSize);
  ASSERT_TRUE(nitrortmp::writeAdtsHeader(frame.data(), 1, 48000, 2, frame.size() - nitrortmp::kAdtsHeaderSize));
  EXPECT_TRUE(pub.pushAudio(frame.data(), frame.size(), 0));
  EXPECT_EQ(pub.stats().audioTags, 2u);  // sequence header + frame
  EXPECT_EQ(pub.stats().invalidFrames, 0u);
}

TEST(header_rejects_out_of_range_values) {
  uint8_t header[nitrortmp::kAdtsHeaderSize];
  std::memset(header, 0xEE, sizeof(header));
  EXPECT_FALSE(nitrortmp::writeAdtsHeader(header, 1, 48001, 2, 100));
  EXPECT_FALSE(nitrortmp::writeAdtsHeader(header, 1, 48000, 0, 100));
  EXPECT_FALSE(nitrortmp::writeAdtsHeader(header, 1, 48000, 8, 100));
  EXPECT_FALSE(nitrortmp::writeAdtsHeader(header, 4, 48000, 2, 100));
  EXPECT_FALSE(nitrortmp::writeAdtsHeader(header, -1, 48000, 2, 100));
  EXPECT_FALSE(nitrortmp::writeAdtsHeader(header, 1, 48000, 2, 0x1FFF - 6));  // frame_length overflows 13 bits
  EXPECT_FALSE(nitrortmp::writeAdtsHeader(nullptr, 1, 48000, 2, 100));
  EXPECT_EQ(header[0], static_cast<uint8_t>(0xEE));  // untouched
  EXPECT_TRUE(nitrortmp::writeAdtsHeader(header, 1, 48000, 2, 0x1FFF - 7));
  EXPECT_EQ(nitrortmp::adtsSampleRateIndex(96000), 0);
  EXPECT_EQ(nitrortmp::adtsSampleRateIndex(7350), 12);
  EXPECT_EQ(nitrortmp::adtsSampleRateIndex(0), -1);
}

TEST(c_facade_writes_the_same_header) {
  uint8_t a[nitrortmp::kAdtsHeaderSize];
  uint8_t b[nitrortmp::kAdtsHeaderSize];
  ASSERT_TRUE(nitrortmp::writeAdtsHeader(a, 1, 44100, 1, 250));
  ASSERT_EQ(nitrortmp_write_adts_header(b, 1, 44100, 1, 250), 1);
  EXPECT_TRUE(test::checkBytesEq(a, sizeof(a), b, sizeof(b), "c facade header", __FILE__, __LINE__));
  EXPECT_EQ(nitrortmp_write_adts_header(b, 1, 12345, 1, 250), 0);
  EXPECT_EQ(nitrortmp_write_adts_header(nullptr, 1, 44100, 1, 250), 0);
}
