// The publisher's output against FFmpeg's FLV muxer on the same input. The
// comparison is per stream (video tags in order, audio tags in order): bodies
// and timestamps must match tag for tag. onMetaData is FFmpeg specific and the
// AVC end-of-sequence tag FFmpeg appends on close are left out.
#include "Fixtures.hpp"
#include "Loopback.hpp"
#include "TestHarness.hpp"

using test::FlvTag;

namespace {
std::vector<FlvTag> publishFixture(std::vector<test::MediaFrame>& frames) {
  test::LoopbackPublisher lb;
  if (!lb.connect()) FAIL_TEST("loopback connect failed");
  frames = test::loadFixtureFrames();
  if (lb.push(frames) != frames.size()) FAIL_TEST("not every fixture frame was accepted");
  return lb.server->tags;
}
}  // namespace

TEST(reference_flv_has_the_expected_shape) {
  const std::vector<FlvTag> tags = test::parseFlv(test::readFile(test::fixturePath("reference.flv")));
  const std::vector<FlvTag> video = test::filterTags(tags, 9);
  const std::vector<FlvTag> audio = test::filterTags(tags, 8);
  ASSERT_TRUE(video.size() > 2);
  ASSERT_TRUE(audio.size() > 2);
  EXPECT_EQ(tags[0].type, 18);  // onMetaData
  EXPECT_EQ(video[0].data[0], 0x17);
  EXPECT_EQ(video[0].data[1], 0x00);  // AVCDecoderConfigurationRecord
  EXPECT_EQ(video[0].timestamp, 0u);
  EXPECT_EQ(video[1].data[1], 0x01);  // first IDR
  const std::vector<uint8_t> asc = {0xAF, 0x00, 0x11, 0x90};  // AAC LC 48 kHz stereo
  EXPECT_BYTES_EQ(audio[0].data, asc);
  EXPECT_EQ(audio[1].data[1], 0x01);
}

TEST(video_tags_match_ffmpeg) {
  std::vector<test::MediaFrame> frames;
  const std::vector<FlvTag> ours = test::filterTags(publishFixture(frames), 9);
  const std::vector<FlvTag> reference = test::filterTags(test::parseFlv(test::readFile(test::fixturePath("reference.flv"))), 9);
  EXPECT_EQ(test::diffTags(ours, reference, "video"), "");
  EXPECT_EQ(ours.size(), reference.size());
}

TEST(audio_tags_match_ffmpeg) {
  std::vector<test::MediaFrame> frames;
  const std::vector<FlvTag> ours = test::filterTags(publishFixture(frames), 8);
  const std::vector<FlvTag> reference = test::filterTags(test::parseFlv(test::readFile(test::fixturePath("reference.flv"))), 8);
  EXPECT_EQ(test::diffTags(ours, reference, "audio"), "");
  EXPECT_EQ(ours.size(), reference.size());
}

TEST(fixture_clock_matches_ffmpeg_rounding) {
  const std::vector<FlvTag> tags = test::parseFlv(test::readFile(test::fixturePath("reference.flv")));
  const std::vector<FlvTag> video = test::filterTags(tags, 9);
  const std::vector<FlvTag> audio = test::filterTags(tags, 8);
  for (size_t i = 1; i < video.size(); ++i) {
    if (video[i].timestamp != test::videoTimestampMs(i - 1)) {
      FAIL_TEST("video tag " + std::to_string(i) + " has timestamp " + std::to_string(video[i].timestamp));
    }
  }
  for (size_t i = 1; i < audio.size(); ++i) {
    if (audio[i].timestamp != test::audioTimestampMs(i - 1)) {
      FAIL_TEST("audio tag " + std::to_string(i) + " has timestamp " + std::to_string(audio[i].timestamp));
    }
  }
}
