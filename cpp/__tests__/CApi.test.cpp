// The C facade (nitrortmp_c.h) end to end: a round trip against the loopback
// server through the C functions only, the error path, and the name tables
// checked against the JS unions in src/specs/RtmpPublisher.nitro.ts.
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "Fixtures.hpp"
#include "Loopback.hpp"
#include "TestHarness.hpp"
#include "nitrortmp_c.h"

namespace {

struct Session {
  nitrortmp_publisher_t* pub = nullptr;
  std::vector<uint8_t> sent;
  std::vector<int> states;
  std::vector<std::pair<int, std::string>> errors;

  Session() {
    nitrortmp_callbacks_t cb{};
    cb.on_send = [](void* ctx, const uint8_t* data, size_t size) {
      auto* self = static_cast<Session*>(ctx);
      self->sent.insert(self->sent.end(), data, data + size);
    };
    cb.on_state = [](void* ctx, int state) { static_cast<Session*>(ctx)->states.push_back(state); };
    cb.on_error = [](void* ctx, int code, const char* message) {
      static_cast<Session*>(ctx)->errors.emplace_back(code, message != nullptr ? message : "");
    };
    pub = nitrortmp_publisher_create(&cb, this);
  }
  ~Session() { nitrortmp_publisher_destroy(pub); }

  /// Moves bytes between the facade and the loopback server until both are quiet.
  void pump(test::LoopbackServer& server) {
    for (;;) {
      bool moved = false;
      if (!sent.empty()) {
        std::vector<uint8_t> bytes;
        bytes.swap(sent);
        server.input(bytes.data(), bytes.size());
        moved = true;
      }
      if (!server.output.empty()) {
        std::vector<uint8_t> bytes;
        bytes.swap(server.output);
        nitrortmp_publisher_on_receive(pub, bytes.data(), bytes.size());
        moved = true;
      }
      if (!moved) return;
    }
  }
};

/// Extracts the string literals of `export type <name> = 'a' | 'b' ...;` in order.
std::vector<std::string> unionMembers(const std::string& source, const std::string& name) {
  const std::string marker = "export type " + name + " =";
  const size_t begin = source.find(marker);
  if (begin == std::string::npos) return {};
  const size_t end = source.find(';', begin);
  std::vector<std::string> out;
  size_t i = begin + marker.size();
  while (i < end) {
    if (source.compare(i, 2, "//") == 0) {  // skip comments
      i = source.find('\n', i);
      if (i == std::string::npos) break;
      continue;
    }
    if (source[i] == '\'') {
      const size_t close = source.find('\'', i + 1);
      if (close == std::string::npos || close > end) break;
      out.push_back(source.substr(i + 1, close - i - 1));
      i = close + 1;
      continue;
    }
    ++i;
  }
  return out;
}

std::string readSpec() {
  std::ifstream in(NITRORTMP_SPEC_PATH);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

TEST(state_names_match_the_js_union) {
  const std::vector<std::string> js = unionMembers(readSpec(), "PublisherState");
  ASSERT_EQ(js.size(), static_cast<size_t>(NITRORTMP_STATE_COUNT));
  for (int i = 0; i < NITRORTMP_STATE_COUNT; ++i) {
    const char* name = nitrortmp_state_name(i);
    ASSERT_TRUE(name != nullptr);
    EXPECT_EQ(std::string(name), js[static_cast<size_t>(i)]);
  }
  EXPECT_TRUE(nitrortmp_state_name(-1) == nullptr);
  EXPECT_TRUE(nitrortmp_state_name(NITRORTMP_STATE_COUNT) == nullptr);
}

TEST(error_names_match_the_js_union) {
  const std::vector<std::string> js = unionMembers(readSpec(), "PublisherErrorCode");
  ASSERT_EQ(js.size(), static_cast<size_t>(NITRORTMP_ERROR_COUNT));
  for (int i = 0; i < NITRORTMP_ERROR_COUNT; ++i) {
    const char* name = nitrortmp_error_name(i);
    ASSERT_TRUE(name != nullptr);
    EXPECT_EQ(std::string(name), js[static_cast<size_t>(i)]);
  }
  EXPECT_TRUE(nitrortmp_error_name(-1) == nullptr);
  EXPECT_TRUE(nitrortmp_error_name(NITRORTMP_ERROR_COUNT) == nullptr);
  // The core's enum and the C codes agree (the facade maps them by switch).
  EXPECT_EQ(std::string(nitrortmp_error_name(NITRORTMP_ERROR_NOT_READY)), "notReady");
  EXPECT_EQ(std::string(nitrortmp_error_name(NITRORTMP_ERROR_TIMEOUT)), "timeout");
}

TEST(url_endpoint_gives_host_port_and_tls) {
  nitrortmp_endpoint_t ep{};
  ASSERT_EQ(nitrortmp_url_endpoint("rtmp://a.rtmp.youtube.com/live2/key", &ep), 1);
  EXPECT_EQ(std::string(ep.host), "a.rtmp.youtube.com");
  EXPECT_EQ(ep.port, 1935);
  EXPECT_EQ(ep.use_tls, 0);

  ASSERT_EQ(nitrortmp_url_endpoint("rtmps://live.twitch.tv/app/live_123?bandwidthtest=true", &ep), 1);
  EXPECT_EQ(std::string(ep.host), "live.twitch.tv");
  EXPECT_EQ(ep.port, 443);
  EXPECT_EQ(ep.use_tls, 1);

  ASSERT_EQ(nitrortmp_url_endpoint("rtmp://host:1936/vhost.example/app/stream", &ep), 1);
  EXPECT_EQ(std::string(ep.host), "host");  // the connect target, never the vhost
  EXPECT_EQ(ep.port, 1936);

  EXPECT_EQ(nitrortmp_url_endpoint("http://example.com/live/key", &ep), 0);
  EXPECT_EQ(nitrortmp_url_endpoint("rtmp://example.com/live", &ep), 0);  // no stream
  EXPECT_EQ(nitrortmp_url_endpoint(nullptr, &ep), 0);
  EXPECT_EQ(nitrortmp_url_endpoint("rtmp://example.com/live/key", nullptr), 0);
}

TEST(facade_rejects_long_stream_keys_before_connecting) {
  const std::string url = "rtmp://host/live/" + std::string(300, 's');
  nitrortmp_endpoint_t endpoint{};
  EXPECT_EQ(nitrortmp_url_endpoint(url.c_str(), &endpoint), 0);
  Session s;
  EXPECT_EQ(nitrortmp_publisher_start(s.pub, url.c_str()), 0);
  EXPECT_TRUE(s.sent.empty());
  ASSERT_EQ(s.errors.size(), 1u);
  EXPECT_EQ(s.errors[0].first, NITRORTMP_ERROR_INVALID_URL);
}

TEST(facade_round_trip_publishes_the_fixture) {
  Session s;
  test::LoopbackServer server;
  EXPECT_EQ(nitrortmp_publisher_state(s.pub), NITRORTMP_STATE_IDLE);

  nitrortmp_metadata_t md{};
  md.width = 320;
  md.height = 240;
  md.frame_rate = 30;
  md.audio_sample_rate = 48000;
  md.audio_channels = 2;
  nitrortmp_publisher_set_metadata(s.pub, &md);

  ASSERT_EQ(nitrortmp_publisher_start(s.pub, "rtmp://127.0.0.1/live/test"), 1);
  EXPECT_EQ(nitrortmp_publisher_state(s.pub), NITRORTMP_STATE_CONNECTING);
  s.pump(server);
  ASSERT_EQ(nitrortmp_publisher_state(s.pub), NITRORTMP_STATE_PUBLISHING);
  const std::vector<int> expectedStates = {NITRORTMP_STATE_CONNECTING, NITRORTMP_STATE_CONNECTED, NITRORTMP_STATE_PUBLISHING};
  EXPECT_TRUE(s.states == expectedStates);
  EXPECT_EQ(server.app, "live");
  EXPECT_EQ(server.stream, "test");

  const std::vector<test::MediaFrame> frames = test::loadFixtureFrames();
  size_t accepted = 0;
  for (const test::MediaFrame& f : frames) {
    const int ok = f.video ? nitrortmp_publisher_push_video(s.pub, f.data.data(), f.data.size(), f.pts, f.dts)
                           : nitrortmp_publisher_push_audio(s.pub, f.data.data(), f.data.size(), f.pts);
    if (ok == 1) ++accepted;
    s.pump(server);
  }
  EXPECT_EQ(accepted, frames.size());

  nitrortmp_stats_t st{};
  nitrortmp_publisher_stats(s.pub, &st);
  EXPECT_EQ(st.script_tags, 1u);  // onMetaData from set_metadata
  EXPECT_TRUE(st.video_tags > 60);
  EXPECT_TRUE(st.audio_tags > 94);
  EXPECT_EQ(st.rejected_frames, 0u);
  EXPECT_EQ(st.invalid_frames, 0u);
  EXPECT_TRUE(st.bytes_sent > 100000u);

  const std::vector<test::FlvTag> expected = test::muxWithVendor(frames);
  EXPECT_EQ(test::diffTags(test::filterTags(server.tags, 9), test::filterTags(expected, 9), "video"), "");
  EXPECT_EQ(test::diffTags(test::filterTags(server.tags, 8), test::filterTags(expected, 8), "audio"), "");
  EXPECT_EQ(test::filterTags(server.tags, 18).size(), 1u);

  const size_t sentBefore = st.bytes_sent;
  nitrortmp_publisher_stop(s.pub);
  EXPECT_EQ(nitrortmp_publisher_state(s.pub), NITRORTMP_STATE_STOPPED);
  EXPECT_EQ(s.states.back(), NITRORTMP_STATE_STOPPED);
  EXPECT_TRUE(test::contains(s.sent, "FCUnpublish"));
  nitrortmp_publisher_stats(s.pub, &st);
  EXPECT_TRUE(st.bytes_sent > sentBefore);
  EXPECT_TRUE(s.errors.empty());

  // Frames after stop are rejected, not crashes.
  EXPECT_EQ(nitrortmp_publisher_push_video(s.pub, frames[0].data.data(), frames[0].data.size(), 0, 0), 0);
  nitrortmp_publisher_stats(s.pub, &st);
  EXPECT_EQ(st.rejected_frames, 1u);
}

TEST(facade_reports_errors_with_c_codes) {
  Session s;
  EXPECT_EQ(nitrortmp_publisher_start(s.pub, "http://example.com/live/key"), 0);
  ASSERT_EQ(s.errors.size(), 1u);
  EXPECT_EQ(s.errors[0].first, NITRORTMP_ERROR_INVALID_URL);
  EXPECT_FALSE(s.errors[0].second.empty());
  EXPECT_EQ(nitrortmp_publisher_state(s.pub), NITRORTMP_STATE_IDLE);

  ASSERT_EQ(nitrortmp_publisher_start(s.pub, "rtmp://127.0.0.1/live/key"), 1);
  EXPECT_EQ(nitrortmp_publisher_start(s.pub, "rtmp://127.0.0.1/live/other"), 0);
  ASSERT_EQ(s.errors.size(), 2u);
  EXPECT_EQ(s.errors[1].first, NITRORTMP_ERROR_NOT_READY);

  const uint8_t http[] = {'H', 'T', 'T', 'P', '/', '1', '.', '1'};
  nitrortmp_publisher_on_receive(s.pub, http, sizeof(http));
  ASSERT_EQ(s.errors.size(), 3u);
  EXPECT_EQ(s.errors[2].first, NITRORTMP_ERROR_HANDSHAKE_FAILED);
  EXPECT_EQ(nitrortmp_publisher_state(s.pub), NITRORTMP_STATE_FAILED);
  // on_error came before on_state(FAILED)
  EXPECT_EQ(s.states.back(), NITRORTMP_STATE_FAILED);

  // Restart after a failure works on the same object.
  s.sent.clear();
  test::LoopbackServer server;
  ASSERT_EQ(nitrortmp_publisher_start(s.pub, "rtmp://127.0.0.1/live/key"), 1);
  s.pump(server);
  EXPECT_EQ(nitrortmp_publisher_state(s.pub), NITRORTMP_STATE_PUBLISHING);
}

TEST(null_publisher_is_tolerated) {
  nitrortmp_stats_t st;
  std::memset(&st, 0xFF, sizeof(st));
  nitrortmp_publisher_stats(nullptr, &st);
  EXPECT_EQ(st.bytes_sent, 0u);
  EXPECT_EQ(nitrortmp_publisher_state(nullptr), NITRORTMP_STATE_IDLE);
  EXPECT_EQ(nitrortmp_publisher_start(nullptr, "rtmp://h/a/s"), 0);
  EXPECT_EQ(nitrortmp_publisher_push_audio(nullptr, nullptr, 0, 0), 0);
  nitrortmp_publisher_on_receive(nullptr, nullptr, 0);
  nitrortmp_publisher_stop(nullptr);
  nitrortmp_publisher_destroy(nullptr);

  // A publisher without callbacks still runs.
  nitrortmp_publisher_t* p = nitrortmp_publisher_create(nullptr, nullptr);
  ASSERT_TRUE(p != nullptr);
  EXPECT_EQ(nitrortmp_publisher_start(p, "rtmp://127.0.0.1/live/key"), 1);
  EXPECT_EQ(nitrortmp_publisher_state(p), NITRORTMP_STATE_CONNECTING);
  nitrortmp_publisher_stop(p);
  nitrortmp_publisher_destroy(p);
}
