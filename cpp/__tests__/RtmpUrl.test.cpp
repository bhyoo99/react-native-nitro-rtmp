#include "RtmpUrl.hpp"
#include "TestHarness.hpp"

using nitrortmp::RtmpUrl;

TEST(parses_plain_rtmp_url) {
  auto u = RtmpUrl::parse("rtmp://a.rtmp.youtube.com/live2/abcd-1234");
  ASSERT_TRUE(u.has_value());
  EXPECT_EQ(u->scheme, "rtmp");
  EXPECT_EQ(u->host, "a.rtmp.youtube.com");
  EXPECT_EQ(u->port, 1935);
  EXPECT_EQ(u->app, "live2");
  EXPECT_EQ(u->stream, "abcd-1234");
  EXPECT_EQ(u->tcUrl, "rtmp://a.rtmp.youtube.com/live2");
  EXPECT_FALSE(u->useTls);
}

TEST(rtmps_defaults_to_443_and_keeps_scheme_in_tcurl) {
  auto u = RtmpUrl::parse("rtmps://live.twitch.tv/app/live_123?bandwidthtest=true");
  ASSERT_TRUE(u.has_value());
  EXPECT_EQ(u->scheme, "rtmps");
  EXPECT_EQ(u->port, 443);
  EXPECT_TRUE(u->useTls);
  EXPECT_EQ(u->app, "app");
  EXPECT_EQ(u->stream, "live_123?bandwidthtest=true");
  EXPECT_EQ(u->tcUrl, "rtmps://live.twitch.tv/app");
}

TEST(explicit_ports_are_kept_and_shown_only_when_non_default) {
  auto a = RtmpUrl::parse("rtmps://host.example:1935/app/key");
  ASSERT_TRUE(a.has_value());
  EXPECT_EQ(a->port, 1935);
  EXPECT_EQ(a->tcUrl, "rtmps://host.example:1935/app");

  auto b = RtmpUrl::parse("rtmp://host.example:1935/app/key");
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ(b->port, 1935);
  EXPECT_EQ(b->tcUrl, "rtmp://host.example/app");

  auto c = RtmpUrl::parse("rtmp://host.example:1936/app/key");
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(c->port, 1936);
  EXPECT_EQ(c->tcUrl, "rtmp://host.example:1936/app");

  auto d = RtmpUrl::parse("rtmps://host.example:443/app/key");
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->port, 443);
  EXPECT_EQ(d->tcUrl, "rtmps://host.example/app");
}

TEST(vhost_from_path_replaces_tcurl_host_but_not_connect_host) {
  auto u = RtmpUrl::parse("rtmp://192.168.1.100:1936/www.abc.com/app//stream");
  ASSERT_TRUE(u.has_value());
  EXPECT_EQ(u->host, "192.168.1.100");
  EXPECT_EQ(u->port, 1936);
  EXPECT_EQ(u->app, "app");
  EXPECT_EQ(u->stream, "stream");
  EXPECT_EQ(u->tcUrl, "rtmp://www.abc.com:1936/app");
}

TEST(vhost_from_query_replaces_tcurl_host_and_stays_in_stream) {
  auto u = RtmpUrl::parse("rtmp://192.168.1.100/app/stream?param1=value1&vhost=www.abc.com&param2=value2");
  ASSERT_TRUE(u.has_value());
  EXPECT_EQ(u->host, "192.168.1.100");
  EXPECT_EQ(u->tcUrl, "rtmp://www.abc.com/app");
  EXPECT_EQ(u->stream, "stream?param1=value1&vhost=www.abc.com&param2=value2");
}

TEST(scheme_is_case_insensitive) {
  auto u = RtmpUrl::parse("RTMPS://Host.Example/app/s");
  ASSERT_TRUE(u.has_value());
  EXPECT_EQ(u->scheme, "rtmps");
  EXPECT_TRUE(u->useTls);
  EXPECT_EQ(u->tcUrl, "rtmps://Host.Example/app");
}

TEST(double_slashes_are_tolerated_like_obs) {
  auto u = RtmpUrl::parse("rtmp://www.abc.com:1936//app//stream");
  ASSERT_TRUE(u.has_value());
  EXPECT_EQ(u->app, "app");
  EXPECT_EQ(u->stream, "stream");
  EXPECT_EQ(u->tcUrl, "rtmp://www.abc.com:1936/app");
}

TEST(rejects_urls_without_app_or_stream_or_with_other_schemes) {
  EXPECT_FALSE(RtmpUrl::parse("").has_value());
  EXPECT_FALSE(RtmpUrl::parse("rtmp://").has_value());
  EXPECT_FALSE(RtmpUrl::parse("rtmp://host").has_value());
  EXPECT_FALSE(RtmpUrl::parse("rtmp://host/").has_value());
  EXPECT_FALSE(RtmpUrl::parse("rtmp://host/app").has_value());
  EXPECT_FALSE(RtmpUrl::parse("rtmp://host/app/").has_value());
  EXPECT_FALSE(RtmpUrl::parse("rtmp://host///stream").has_value());
  EXPECT_FALSE(RtmpUrl::parse("rtmp://host/a/b/c/d").has_value());
  EXPECT_FALSE(RtmpUrl::parse("http://host/app/stream").has_value());
  EXPECT_FALSE(RtmpUrl::parse("rtmpt://host/app/stream").has_value());
  EXPECT_FALSE(RtmpUrl::parse("srt://host:9000").has_value());
  EXPECT_FALSE(RtmpUrl::parse("host/app/stream").has_value());
  EXPECT_FALSE(RtmpUrl::parse("rtmp://host:99999/app/stream").has_value());
}

TEST(rejects_fields_that_the_client_would_truncate) {
  EXPECT_TRUE(RtmpUrl::parse("rtmp://host/live/" + std::string(254, 's')).has_value());
  EXPECT_FALSE(RtmpUrl::parse("rtmp://host/live/" + std::string(255, 's')).has_value());
  EXPECT_FALSE(RtmpUrl::parse("rtmp://host/live/" + std::string(300, 's')).has_value());
  EXPECT_TRUE(RtmpUrl::parse("rtmp://host/" + std::string(126, 'a') + "/key").has_value());
  EXPECT_FALSE(RtmpUrl::parse("rtmp://host/" + std::string(127, 'a') + "/key").has_value());

  // tcUrl = "rtmp://" (7) + host (242) + "/live" (5).
  EXPECT_TRUE(RtmpUrl::parse("rtmp://" + std::string(242, 'h') + "/live/key").has_value());
  EXPECT_FALSE(RtmpUrl::parse("rtmp://" + std::string(243, 'h') + "/live/key").has_value());

  // Validate decoded bytes, with the query string included in the stream name.
  const std::string prefix = "rtmp://host/live/" + std::string(245, 's');
  EXPECT_TRUE(RtmpUrl::parse(prefix + "%73?token=x").has_value());
  EXPECT_FALSE(RtmpUrl::parse(prefix + "%73%73?token=x").has_value());
}
