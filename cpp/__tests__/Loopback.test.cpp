// The publisher against ireader's rtmp-server.c in the same process: the full
// handshake/connect/createStream/publish exchange, and every tag the server
// receives compared byte for byte with what the FLV muxer produced.
#include "Fixtures.hpp"
#include "Loopback.hpp"
#include "TestHarness.hpp"

using namespace nitrortmp;
using test::LoopbackPublisher;

TEST(publishes_maximum_length_app_and_stream_without_truncation) {
  LoopbackPublisher lb;
  const std::string app(126, 'a');
  const std::string stream = std::string(246, 's') + "?token=x";
  const std::string url = "rtmp://localhost/" + app + "/" + stream;
  ASSERT_TRUE(lb.connect(url.c_str()));
  EXPECT_EQ(lb.server->app, app);
  EXPECT_EQ(lb.server->stream, stream);
}

TEST(publishes_fixture_through_ireader_server) {
  LoopbackPublisher lb;
  StreamMetadata md;
  md.width = 320;
  md.height = 240;
  md.frameRate = 30;
  md.audioSampleRate = 48000;
  md.audioChannels = 2;
  lb.pub.setMetadata(md);

  ASSERT_TRUE(lb.connect("rtmp://127.0.0.1/live/test?token=abc"));
  const std::vector<PublisherState> expected = {PublisherState::Connecting, PublisherState::Connected, PublisherState::Publishing};
  EXPECT_TRUE(lb.states == expected);
  EXPECT_TRUE(lb.errors.empty());
  EXPECT_EQ(lb.server->publishCalls, 1);
  EXPECT_EQ(lb.server->app, "live");
  EXPECT_EQ(lb.server->stream, "test?token=abc");
  EXPECT_EQ(lb.server->type, "live");

  const std::vector<test::MediaFrame> frames = test::loadFixtureFrames();
  ASSERT_TRUE(frames.size() > 100);
  EXPECT_EQ(lb.push(frames), frames.size());
  EXPECT_EQ(lb.serverInputResult, 0);
  EXPECT_TRUE(lb.errors.empty());
  EXPECT_EQ(lb.pub.state(), PublisherState::Publishing);

  // onMetaData arrives first (the server strips @setDataFrame).
  ASSERT_TRUE(lb.server->tags.size() > 1);
  EXPECT_EQ(lb.server->tags[0].type, 18);
  EXPECT_TRUE(test::contains(lb.server->tags[0].data, "onMetaData"));
  EXPECT_FALSE(test::contains(lb.server->tags[0].data, "@setDataFrame"));

  // Everything else is exactly what flv_muxer emitted, in order.
  std::vector<test::FlvTag> media(lb.server->tags.begin() + 1, lb.server->tags.end());
  const std::vector<test::FlvTag> reference = test::muxWithVendor(frames);
  EXPECT_EQ(media.size(), reference.size());
  EXPECT_EQ(test::diffTags(media, reference, "server tags"), "");

  const PublisherStats stats = lb.pub.stats();
  EXPECT_EQ(stats.videoTags + stats.audioTags, reference.size());
  EXPECT_EQ(stats.scriptTags, 1u);
  EXPECT_EQ(stats.droppedBeforeKeyframe, 0u);
  EXPECT_EQ(stats.timestampClamps, 0u);
  EXPECT_EQ(stats.invalidFrames, 0u);

  lb.pub.stop();
  lb.pump();
  EXPECT_EQ(lb.pub.state(), PublisherState::Stopped);
  EXPECT_EQ(lb.serverInputResult, 0);
  EXPECT_TRUE(lb.errors.empty());
}

TEST(survives_byte_wise_delivery_in_both_directions) {
  LoopbackPublisher lb(3);
  ASSERT_TRUE(lb.connect());
  const std::vector<test::MediaFrame> frames = test::loadFixtureFrames();
  std::vector<test::MediaFrame> head(frames.begin(), frames.begin() + 40);
  EXPECT_EQ(lb.push(head), head.size());
  EXPECT_EQ(test::diffTags(std::vector<test::FlvTag>(lb.server->tags.begin(), lb.server->tags.end()),
                           test::muxWithVendor(head), "server tags"), "");
}

TEST(large_and_extended_timestamps_survive_chunking) {
  LoopbackPublisher lb;
  ASSERT_TRUE(lb.connect());
  const auto idr = test::syntheticIdr(true);
  const auto p = test::syntheticPFrame();
  const auto aac = test::syntheticAdts(32);

  EXPECT_TRUE(lb.pub.pushVideo(idr.data(), idr.size(), 0, 0));
  EXPECT_TRUE(lb.pub.pushVideo(p.data(), p.size(), 0x1000005, 0x1000005));  // delta needs an extended timestamp
  EXPECT_TRUE(lb.pub.pushAudio(aac.data(), aac.size(), 0x1000010));
  EXPECT_TRUE(lb.pub.pushVideo(p.data(), p.size(), 0x1000010, 0x1000010));
  EXPECT_TRUE(lb.pub.pushVideo(p.data(), p.size(), 0xFFFFFFF0, 0xFFFFFFF0));  // absolute, near 32-bit wrap
  EXPECT_TRUE(lb.pub.pushVideo(p.data(), p.size(), 0xFFFFFFF0, 0xFFFFFFF0));  // same timestamp => type-3 header
  lb.pump();
  EXPECT_EQ(lb.serverInputResult, 0);

  std::vector<uint32_t> video, audio;
  for (const test::FlvTag& t : lb.server->tags) {
    (t.type == 9 ? video : audio).push_back(t.timestamp);
  }
  const std::vector<uint32_t> expectedVideo = {0, 0, 0x1000005, 0x1000010, 0xFFFFFFF0, 0xFFFFFFF0};
  const std::vector<uint32_t> expectedAudio = {0x1000010, 0x1000010};
  EXPECT_TRUE(video == expectedVideo);
  EXPECT_TRUE(audio == expectedAudio);
}

TEST(server_rejecting_publish_fails_with_publish_bad_name) {
  LoopbackPublisher lb;
  lb.server->publishResult = -1;
  EXPECT_FALSE(lb.connect());
  EXPECT_EQ(lb.pub.state(), PublisherState::Failed);
  ASSERT_EQ(lb.errors.size(), 1u);
  EXPECT_EQ(lb.errors[0].first, PublisherError::PublishBadName);
  const std::vector<PublisherState> expected = {PublisherState::Connecting, PublisherState::Connected, PublisherState::Failed};
  EXPECT_TRUE(lb.states == expected);
}

TEST(reconnect_on_a_new_connection_resends_sequence_headers) {
  LoopbackPublisher lb;
  ASSERT_TRUE(lb.connect());
  const std::vector<test::MediaFrame> frames = test::loadFixtureFrames();
  std::vector<test::MediaFrame> head(frames.begin(), frames.begin() + 20);
  EXPECT_EQ(lb.push(head), head.size());
  lb.pub.stop();
  lb.pump();
  EXPECT_EQ(lb.pub.state(), PublisherState::Stopped);

  lb.newConnection();
  lb.states.clear();
  ASSERT_TRUE(lb.connect());
  EXPECT_EQ(lb.server->publishCalls, 1);
  // Continue with a later part of the stream: P-frames are dropped until the next IDR.
  std::vector<test::MediaFrame> tail(frames.begin() + 20, frames.end());
  const size_t accepted = lb.push(tail);
  EXPECT_TRUE(accepted < tail.size());
  EXPECT_TRUE(lb.pub.stats().droppedBeforeKeyframe > 0);

  // t = 0 is the first accepted push (an audio frame: video waits for the IDR),
  // and the video clock keeps its distance to audio.
  uint32_t offset = 0, idrDts = 0;
  bool haveOffset = false, haveIdr = false;
  for (const test::MediaFrame& f : tail) {
    const bool keyframe = f.video && test::splitAnnexB(f.data)[0].keyframe;
    if (!haveOffset && (!f.video || keyframe)) { offset = f.dts; haveOffset = true; }
    if (!haveIdr && keyframe) { idrDts = f.dts; haveIdr = true; }
  }
  ASSERT_TRUE(haveOffset && haveIdr && idrDts > offset);

  const std::vector<test::FlvTag> video = test::filterTags(lb.server->tags, 9);
  ASSERT_TRUE(video.size() > 1);
  EXPECT_EQ(video[0].data[0], 0x17);
  EXPECT_EQ(video[0].data[1], 0x00);  // fresh AVC sequence header
  EXPECT_EQ(video[0].timestamp, idrDts - offset);
  EXPECT_EQ(video[1].data[0], 0x17);  // the IDR that triggered it
  EXPECT_EQ(video[1].timestamp, idrDts - offset);
  const std::vector<test::FlvTag> audio = test::filterTags(lb.server->tags, 8);
  ASSERT_TRUE(audio.size() > 1);
  EXPECT_EQ(audio[0].data[1], 0x00);  // AAC sequence header again
  EXPECT_EQ(audio[0].timestamp, 0u);
  EXPECT_EQ(lb.pub.stats().timestampClamps, 0u);
}

TEST(sps_pps_change_emits_a_new_sequence_header) {
  LoopbackPublisher lb;
  ASSERT_TRUE(lb.connect());
  const auto idr = test::syntheticIdr(true);
  const auto p = test::syntheticPFrame();
  EXPECT_TRUE(lb.pub.pushVideo(idr.data(), idr.size(), 0, 0));
  EXPECT_TRUE(lb.pub.pushVideo(p.data(), p.size(), 33, 33));
  EXPECT_TRUE(lb.pub.pushVideo(idr.data(), idr.size(), 67, 67));  // same SPS/PPS: no new header
  lb.pump();
  EXPECT_EQ(test::filterTags(lb.server->tags, 9).size(), 4u);

  // A different SPS (same id, other level byte) => a new AVCDecoderConfigurationRecord.
  auto changed = test::syntheticIdr(true, 1);
  changed[7] = 0x28;  // level_idc 30 -> 40
  EXPECT_TRUE(lb.pub.pushVideo(changed.data(), changed.size(), 100, 100));
  lb.pump();
  const std::vector<test::FlvTag> video = test::filterTags(lb.server->tags, 9);
  ASSERT_EQ(video.size(), 6u);
  EXPECT_EQ(video[4].data[1], 0x00);  // sequence header
  EXPECT_EQ(video[4].timestamp, 100u);
  EXPECT_EQ(video[5].data[1], 0x01);  // the IDR
  EXPECT_TRUE(video[4].data != video[0].data);
}
