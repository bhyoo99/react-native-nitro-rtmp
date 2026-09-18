#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace nitrortmp {

/// A publish URL split into the pieces the RTMP `connect` and `publish`
/// commands need. Only `rtmp://` and `rtmps://` are accepted.
///
/// Examples:
///   rtmp://a.rtmp.youtube.com/live2/abcd-1234
///     host=a.rtmp.youtube.com port=1935 app=live2 stream=abcd-1234
///     tcUrl=rtmp://a.rtmp.youtube.com/live2 useTls=false
///   rtmps://live.twitch.tv/app/live_123?bandwidthtest=true
///     host=live.twitch.tv port=443 app=app stream=live_123?bandwidthtest=true
///     tcUrl=rtmps://live.twitch.tv/app useTls=true
///   rtmp://host:1936/vhost.example/app/stream
///     host=host port=1936 app=app stream=stream tcUrl=rtmp://vhost.example:1936/app
struct RtmpUrl {
  std::string scheme;  // "rtmp" | "rtmps"
  std::string host;    // TCP connect target (never the vhost)
  uint16_t port = 0;   // explicit port, else 1935 for rtmp / 443 for rtmps
  std::string app;     // "live2"
  std::string stream;  // "abcd-1234?backup=1" (query string stays in the stream key)
  std::string tcUrl;   // "<scheme>://<vhost>[:port]/<app>", port only when non-default
  bool useTls = false; // true for rtmps

  /// The vendored client's snprintf calls reserve two bytes in each buffer.
  /// Reject values that would be truncated, including embedded NUL bytes.
  /// Limits are UTF-8 byte counts: app 126, stream 254, tcUrl 254.
  bool fitsClientBuffers() const;

  /// @return std::nullopt when the URL is not an rtmp/rtmps URL with a host,
  ///         an app and a stream name, or a field exceeds the client limits.
  static std::optional<RtmpUrl> parse(std::string_view url);
};

}  // namespace nitrortmp
