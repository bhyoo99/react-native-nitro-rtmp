// Unit tests: state machine, push gating, timestamps and error mapping, driven
// by hand-built server messages (ScriptedServer) so every branch is reachable.
#include "Fixtures.hpp"
#include "Loopback.hpp"
#include "RtmpPublisher.hpp"
#include "TestHarness.hpp"

using namespace nitrortmp;
using test::CapturedPublisher;
using test::ScriptedServer;

namespace {
void feed(RtmpPublisher& pub, const std::vector<uint8_t>& bytes) { pub.onReceive(bytes.data(), bytes.size()); }
}  // namespace

TEST(push_before_start_is_rejected_and_counted) {
  CapturedPublisher c;
  const auto idr = test::syntheticIdr(true);
  const auto aac = test::syntheticAdts(10);
  EXPECT_FALSE(c.pub.pushVideo(idr.data(), idr.size(), 0, 0));
  EXPECT_FALSE(c.pub.pushAudio(aac.data(), aac.size(), 0));
  EXPECT_EQ(c.pub.stats().rejectedFrames, 2u);
  EXPECT_EQ(c.pub.state(), PublisherState::Idle);
  EXPECT_TRUE(c.output.empty());
}

TEST(start_with_invalid_url_reports_error_and_stays_idle) {
  CapturedPublisher c;
  EXPECT_FALSE(c.pub.start(std::string_view("http://example.com/live/key")));
  ASSERT_EQ(c.errors.size(), 1u);
  EXPECT_EQ(c.errors[0].first, PublisherError::InvalidUrl);
  EXPECT_EQ(c.pub.state(), PublisherState::Idle);
  EXPECT_TRUE(c.states.empty());
}

TEST(start_emits_c0_c1_and_enters_connecting) {
  CapturedPublisher c;
  ASSERT_TRUE(c.pub.start(std::string_view("rtmp://127.0.0.1/live/key")));
  EXPECT_EQ(c.pub.state(), PublisherState::Connecting);
  ASSERT_EQ(c.states.size(), 1u);
  EXPECT_EQ(c.states[0], PublisherState::Connecting);
  ASSERT_EQ(c.output.size(), 1537u);
  EXPECT_EQ(c.output[0], 3);
  EXPECT_EQ(c.pub.stats().bytesSent, 1537u);
  EXPECT_EQ(c.pub.url().tcUrl, "rtmp://127.0.0.1/live");

  // A second start while active is refused.
  EXPECT_FALSE(c.pub.start(std::string_view("rtmp://127.0.0.1/live/other")));
  ASSERT_EQ(c.errors.size(), 1u);
  EXPECT_EQ(c.errors[0].first, PublisherError::NotReady);
  EXPECT_EQ(c.pub.state(), PublisherState::Connecting);
}

TEST(direct_url_start_rejects_truncation_before_sending_any_bytes) {
  const auto parsed = RtmpUrl::parse("rtmp://host/live/key");
  ASSERT_TRUE(parsed.has_value());
  for (int field = 0; field < 4; ++field) {
    CapturedPublisher c;
    auto url = *parsed;
    if (field == 0) url.app.assign(127, 'a');
    if (field == 1) url.stream.assign(255, 's');
    if (field == 2) url.tcUrl.assign(255, 't');
    if (field == 3) url.stream = std::string("key\0suffix", 10);
    EXPECT_FALSE(c.pub.start(url));
    EXPECT_EQ(c.pub.state(), PublisherState::Idle);
    EXPECT_TRUE(c.output.empty());
    ASSERT_EQ(c.errors.size(), 1u);
    EXPECT_EQ(c.errors[0].first, PublisherError::InvalidUrl);
  }
}

TEST(bad_s0_version_fails_handshake) {
  CapturedPublisher c;
  ASSERT_TRUE(c.pub.start(std::string_view("rtmp://127.0.0.1/live/key")));
  const std::vector<uint8_t> http = {'H', 'T', 'T', 'P', '/', '1', '.', '1'};
  feed(c.pub, http);
  ASSERT_EQ(c.errors.size(), 1u);
  EXPECT_EQ(c.errors[0].first, PublisherError::HandshakeFailed);
  EXPECT_EQ(c.pub.state(), PublisherState::Failed);
  // Error is reported before the state change.
  ASSERT_EQ(c.states.size(), 2u);
  EXPECT_EQ(c.states[1], PublisherState::Failed);
}

TEST(connect_rejection_maps_to_connect_rejected) {
  CapturedPublisher c;
  ASSERT_TRUE(c.pub.start(std::string_view("rtmp://127.0.0.1/live/key")));
  feed(c.pub, ScriptedServer::handshakeResponse(c.takeOutput()));
  EXPECT_EQ(c.pub.state(), PublisherState::Connecting);
  const auto out = c.takeOutput();
  EXPECT_TRUE(test::contains(out, "connect"));
  EXPECT_TRUE(test::contains(out, "rtmp://127.0.0.1/live"));  // tcUrl
  EXPECT_TRUE(test::contains(out, "live"));                   // app

  feed(c.pub, ScriptedServer::connectError("NetConnection.Connect.Rejected", "bad app"));
  ASSERT_EQ(c.errors.size(), 1u);
  EXPECT_EQ(c.errors[0].first, PublisherError::ConnectRejected);
  EXPECT_EQ(c.pub.state(), PublisherState::Failed);
}

TEST(happy_path_reaches_publishing_and_sends_metadata_first) {
  CapturedPublisher c;
  StreamMetadata md;
  md.width = 320;
  md.height = 240;
  md.frameRate = 30;
  md.videoBitrateKbps = 250;
  md.audioSampleRate = 48000;
  md.audioChannels = 2;
  md.audioBitrateKbps = 96;
  c.pub.setMetadata(md);
  ASSERT_TRUE(c.pub.start(std::string_view("rtmp://127.0.0.1/live/key")));
  ASSERT_TRUE(c.driveToPublishing());

  const std::vector<PublisherState> expected = {PublisherState::Connecting, PublisherState::Connected, PublisherState::Publishing};
  EXPECT_TRUE(c.states == expected);
  EXPECT_TRUE(c.errors.empty());

  // onMetaData went out as part of entering Publishing.
  const auto out = c.takeOutput();
  EXPECT_TRUE(test::contains(out, "@setDataFrame"));
  EXPECT_TRUE(test::contains(out, "onMetaData"));
  EXPECT_TRUE(test::contains(out, "videocodecid"));
  EXPECT_TRUE(test::contains(out, "audiocodecid"));
  EXPECT_TRUE(test::contains(out, "react-native-nitro-rtmp"));
  EXPECT_EQ(c.pub.stats().scriptTags, 1u);
}

TEST(first_video_must_be_idr_with_sps_pps) {
  CapturedPublisher c;
  ASSERT_TRUE(c.pub.start(std::string_view("rtmp://127.0.0.1/live/key")));
  ASSERT_TRUE(c.driveToPublishing());
  c.takeOutput();

  const auto p = test::syntheticPFrame();
  EXPECT_FALSE(c.pub.pushVideo(p.data(), p.size(), 0, 0));
  const auto idrOnly = test::syntheticIdr(false);
  EXPECT_FALSE(c.pub.pushVideo(idrOnly.data(), idrOnly.size(), 33, 33));
  EXPECT_EQ(c.pub.stats().droppedBeforeKeyframe, 2u);
  EXPECT_EQ(c.pub.stats().videoTags, 0u);
  EXPECT_TRUE(c.output.empty());

  const auto idr = test::syntheticIdr(true);
  EXPECT_TRUE(c.pub.pushVideo(idr.data(), idr.size(), 67, 67));
  EXPECT_EQ(c.pub.stats().videoTags, 2u);  // sequence header + IDR
  EXPECT_FALSE(c.output.empty());

  EXPECT_TRUE(c.pub.pushVideo(p.data(), p.size(), 100, 100));
  EXPECT_EQ(c.pub.stats().videoTags, 3u);
  EXPECT_EQ(c.pub.stats().droppedBeforeKeyframe, 2u);
  EXPECT_EQ(c.pub.stats().rejectedFrames, 0u);
}

TEST(timestamps_start_at_zero_and_never_go_backwards) {
  CapturedPublisher c;
  ASSERT_TRUE(c.pub.start(std::string_view("rtmp://127.0.0.1/live/key")));
  ASSERT_TRUE(c.driveToPublishing());

  const auto idr = test::syntheticIdr(true);
  const auto p = test::syntheticPFrame();
  const auto aac = test::syntheticAdts(16);

  // First accepted push defines t = 0 for both streams.
  EXPECT_TRUE(c.pub.pushVideo(idr.data(), idr.size(), 5000, 5000));
  EXPECT_EQ(c.pub.stats().lastVideoTimestamp, 0u);
  EXPECT_TRUE(c.pub.pushAudio(aac.data(), aac.size(), 5021));
  EXPECT_EQ(c.pub.stats().lastAudioTimestamp, 21u);
  EXPECT_TRUE(c.pub.pushVideo(p.data(), p.size(), 5033, 5033));
  EXPECT_EQ(c.pub.stats().lastVideoTimestamp, 33u);
  EXPECT_EQ(c.pub.stats().timestampClamps, 0u);

  // Backwards dts is clamped to the previous value, per stream.
  EXPECT_TRUE(c.pub.pushVideo(p.data(), p.size(), 5010, 5010));
  EXPECT_EQ(c.pub.stats().lastVideoTimestamp, 33u);
  EXPECT_EQ(c.pub.stats().timestampClamps, 1u);
  EXPECT_TRUE(c.pub.pushAudio(aac.data(), aac.size(), 4000));  // before the offset
  EXPECT_EQ(c.pub.stats().lastAudioTimestamp, 21u);
  EXPECT_EQ(c.pub.stats().timestampClamps, 2u);

  // pts < dts is clamped too.
  EXPECT_TRUE(c.pub.pushVideo(p.data(), p.size(), 5060, 5067));
  EXPECT_EQ(c.pub.stats().lastVideoTimestamp, 67u);
  EXPECT_EQ(c.pub.stats().timestampClamps, 3u);
}

TEST(invalid_payloads_are_counted_not_sent) {
  CapturedPublisher c;
  ASSERT_TRUE(c.pub.start(std::string_view("rtmp://127.0.0.1/live/key")));
  ASSERT_TRUE(c.driveToPublishing());
  c.takeOutput();

  const std::vector<uint8_t> garbage = {0x12, 0x34, 0x56};
  EXPECT_FALSE(c.pub.pushAudio(garbage.data(), garbage.size(), 0));
  auto twoFrames = test::syntheticAdts(8);
  const auto second = test::syntheticAdts(8);
  twoFrames.insert(twoFrames.end(), second.begin(), second.end());
  EXPECT_FALSE(c.pub.pushAudio(twoFrames.data(), twoFrames.size(), 0));
  auto withCrc = test::syntheticAdts(8);
  withCrc[1] &= 0xFE;  // protection_absent = 0
  EXPECT_FALSE(c.pub.pushAudio(withCrc.data(), withCrc.size(), 0));
  EXPECT_FALSE(c.pub.pushVideo(nullptr, 0, 0, 0));
  EXPECT_EQ(c.pub.stats().invalidFrames, 4u);
  EXPECT_EQ(c.pub.stats().audioTags, 0u);
  EXPECT_TRUE(c.output.empty());
}

TEST(stop_sends_fcunpublish_and_delete_stream) {
  CapturedPublisher c;
  ASSERT_TRUE(c.pub.start(std::string_view("rtmp://127.0.0.1/live/key")));
  ASSERT_TRUE(c.driveToPublishing());
  c.takeOutput();

  c.pub.stop();
  EXPECT_EQ(c.pub.state(), PublisherState::Stopped);
  const auto out = c.takeOutput();
  EXPECT_TRUE(test::contains(out, "FCUnpublish"));
  EXPECT_TRUE(test::contains(out, "deleteStream"));

  // Nothing is accepted afterwards, and stop() is idempotent.
  const auto idr = test::syntheticIdr(true);
  EXPECT_FALSE(c.pub.pushVideo(idr.data(), idr.size(), 0, 0));
  c.pub.stop();
  feed(c.pub, ScriptedServer::onStatus("status", "NetStream.Unpublish.Success", ""));
  EXPECT_EQ(c.pub.state(), PublisherState::Stopped);
  EXPECT_TRUE(c.errors.empty());
}

TEST(stop_while_connecting_does_not_send_delete_stream) {
  CapturedPublisher c;
  ASSERT_TRUE(c.pub.start(std::string_view("rtmp://127.0.0.1/live/key")));
  c.takeOutput();
  c.pub.stop();
  EXPECT_EQ(c.pub.state(), PublisherState::Stopped);
  EXPECT_TRUE(c.output.empty());
}

TEST(stop_from_inside_on_send_is_deferred) {
  CapturedPublisher c;
  c.stopFromOnSend = true;
  ASSERT_TRUE(c.pub.start(std::string_view("rtmp://127.0.0.1/live/key")));
  EXPECT_EQ(c.pub.state(), PublisherState::Stopped);
  const std::vector<PublisherState> expected = {PublisherState::Connecting, PublisherState::Stopped};
  EXPECT_TRUE(c.states == expected);
}

TEST(publish_rejection_maps_to_publish_bad_name) {
  CapturedPublisher c;
  ASSERT_TRUE(c.pub.start(std::string_view("rtmp://127.0.0.1/live/key")));
  feed(c.pub, ScriptedServer::handshakeResponse(c.takeOutput()));
  feed(c.pub, ScriptedServer::connectResult());
  ASSERT_EQ(c.pub.state(), PublisherState::Connected);
  feed(c.pub, ScriptedServer::createStreamResult());
  EXPECT_TRUE(test::contains(c.takeOutput(), "publish"));
  feed(c.pub, ScriptedServer::onStatus("error", "NetStream.Publish.BadName", "Already publishing"));
  ASSERT_EQ(c.errors.size(), 1u);
  EXPECT_EQ(c.errors[0].first, PublisherError::PublishBadName);
  EXPECT_EQ(c.pub.state(), PublisherState::Failed);
}

TEST(publish_rejection_message_carries_the_server_description) {
  CapturedPublisher c;
  ASSERT_TRUE(c.pub.start(std::string_view("rtmp://127.0.0.1/live/key")));
  feed(c.pub, ScriptedServer::handshakeResponse(c.takeOutput()));
  feed(c.pub, ScriptedServer::connectResult());
  feed(c.pub, ScriptedServer::createStreamResult());
  feed(c.pub, ScriptedServer::onStatus("error", "NetStream.Publish.BadName", "Stream key invalid"));
  ASSERT_EQ(c.errors.size(), 1u);
  EXPECT_EQ(c.errors[0].first, PublisherError::PublishBadName);
  EXPECT_TRUE(c.errors[0].second.find("NetStream.Publish.BadName") != std::string::npos);
  EXPECT_TRUE(c.errors[0].second.find("Stream key invalid") != std::string::npos);
}

TEST(invalid_app_and_already_exist_stream_have_their_own_codes) {
  {
    CapturedPublisher c;
    ASSERT_TRUE(c.pub.start(std::string_view("rtmp://127.0.0.1/live/key")));
    feed(c.pub, ScriptedServer::handshakeResponse(c.takeOutput()));
    feed(c.pub, ScriptedServer::connectError("NetConnection.Connect.InvalidApp", "no such app"));
    ASSERT_EQ(c.errors.size(), 1u);
    EXPECT_EQ(c.errors[0].first, PublisherError::InvalidApp);
    EXPECT_EQ(c.pub.state(), PublisherState::Failed);
  }
  {
    CapturedPublisher c;
    ASSERT_TRUE(c.pub.start(std::string_view("rtmp://127.0.0.1/live/key")));
    feed(c.pub, ScriptedServer::handshakeResponse(c.takeOutput()));
    feed(c.pub, ScriptedServer::connectResult());
    feed(c.pub, ScriptedServer::createStreamResult());
    // ksyun style: level "finish" instead of "error"
    feed(c.pub, ScriptedServer::onStatus("finish", "NetStream.Publish.AlreadyExistStream", "Already exist stream!"));
    ASSERT_EQ(c.errors.size(), 1u);
    EXPECT_EQ(c.errors[0].first, PublisherError::StreamAlreadyExists);
    EXPECT_EQ(c.pub.state(), PublisherState::Failed);
  }
}

TEST(connect_closed_status_while_publishing_is_server_closed) {
  CapturedPublisher c;
  ASSERT_TRUE(c.pub.start(std::string_view("rtmp://127.0.0.1/live/key")));
  ASSERT_TRUE(c.driveToPublishing());
  feed(c.pub, ScriptedServer::onStatus("status", "NetConnection.Connect.Closed", "bye", 0));
  ASSERT_EQ(c.errors.size(), 1u);
  EXPECT_EQ(c.errors[0].first, PublisherError::ServerClosed);
  EXPECT_EQ(c.pub.state(), PublisherState::Failed);
}

TEST(informational_status_codes_are_ignored) {
  CapturedPublisher c;
  ASSERT_TRUE(c.pub.start(std::string_view("rtmp://127.0.0.1/live/key")));
  ASSERT_TRUE(c.driveToPublishing());
  feed(c.pub, ScriptedServer::onStatus("status", "NetStream.Publish.SomethingNew", "info"));
  feed(c.pub, ScriptedServer::onStatus("warning", "NetStream.Publish.Warning", "slow"));
  EXPECT_TRUE(c.errors.empty());
  EXPECT_EQ(c.pub.state(), PublisherState::Publishing);
}

TEST(stream_eof_while_publishing_is_server_closed_and_stopped) {
  CapturedPublisher c;
  ASSERT_TRUE(c.pub.start(std::string_view("rtmp://127.0.0.1/live/key")));
  ASSERT_TRUE(c.driveToPublishing());
  feed(c.pub, ScriptedServer::streamEof());
  ASSERT_EQ(c.errors.size(), 1u);
  EXPECT_EQ(c.errors[0].first, PublisherError::ServerClosed);
  EXPECT_EQ(c.pub.state(), PublisherState::Stopped);
}

TEST(error_status_while_publishing_is_server_closed_and_failed) {
  CapturedPublisher c;
  ASSERT_TRUE(c.pub.start(std::string_view("rtmp://127.0.0.1/live/key")));
  ASSERT_TRUE(c.driveToPublishing());
  feed(c.pub, ScriptedServer::onStatus("error", "NetStream.Failed", "internal error"));
  ASSERT_EQ(c.errors.size(), 1u);
  EXPECT_EQ(c.errors[0].first, PublisherError::ServerClosed);
  EXPECT_EQ(c.pub.state(), PublisherState::Failed);
}

TEST(malformed_chunk_data_is_protocol_error) {
  CapturedPublisher c;
  ASSERT_TRUE(c.pub.start(std::string_view("rtmp://127.0.0.1/live/key")));
  feed(c.pub, ScriptedServer::handshakeResponse(c.takeOutput()));
  // An AMF0 invoke whose body is not AMF at all.
  const std::vector<uint8_t> junk(40, 0xEE);
  feed(c.pub, ScriptedServer::chunkMessage(3, 20, 0, junk));
  ASSERT_EQ(c.errors.size(), 1u);
  EXPECT_EQ(c.errors[0].first, PublisherError::ProtocolError);
  EXPECT_EQ(c.pub.state(), PublisherState::Failed);
}

TEST(reconnect_after_stop_resends_sequence_headers) {
  CapturedPublisher c;
  ASSERT_TRUE(c.pub.start(std::string_view("rtmp://127.0.0.1/live/key")));
  ASSERT_TRUE(c.driveToPublishing());
  const auto idr = test::syntheticIdr(true);
  const auto p = test::syntheticPFrame();
  EXPECT_TRUE(c.pub.pushVideo(idr.data(), idr.size(), 0, 0));
  EXPECT_TRUE(c.pub.pushVideo(p.data(), p.size(), 33, 33));
  EXPECT_EQ(c.pub.stats().videoTags, 3u);
  c.pub.stop();
  c.takeOutput();
  c.states.clear();

  ASSERT_TRUE(c.pub.start(std::string_view("rtmp://127.0.0.1/live/key")));
  EXPECT_EQ(c.pub.stats().videoTags, 0u);  // stats are per session
  ASSERT_TRUE(c.driveToPublishing());
  EXPECT_FALSE(c.pub.pushVideo(p.data(), p.size(), 1000, 1000));  // keyframe wait again
  EXPECT_TRUE(c.pub.pushVideo(idr.data(), idr.size(), 1033, 1033));
  EXPECT_EQ(c.pub.stats().videoTags, 2u);  // sequence header re-sent
  EXPECT_EQ(c.pub.stats().lastVideoTimestamp, 0u);  // offset restarts
}

TEST(start_is_allowed_again_after_failure) {
  CapturedPublisher c;
  ASSERT_TRUE(c.pub.start(std::string_view("rtmp://127.0.0.1/live/key")));
  const std::vector<uint8_t> bad = {0x00};
  feed(c.pub, bad);
  EXPECT_EQ(c.pub.state(), PublisherState::Failed);
  c.takeOutput();
  ASSERT_TRUE(c.pub.start(std::string_view("rtmp://127.0.0.1/live/key")));
  EXPECT_EQ(c.pub.state(), PublisherState::Connecting);
  EXPECT_EQ(c.output.size(), 1537u);
}
