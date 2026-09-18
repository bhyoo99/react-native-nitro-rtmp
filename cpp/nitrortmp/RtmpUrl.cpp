#include "RtmpUrl.hpp"

// rtmp-url.h defines static functions that use assert() without including
// <assert.h>; it must be included first.
#include <cassert>

#include <cctype>
#include <cstring>

extern "C" {
#include "uri-parse.h"
}
#include "rtmp-url.h"

namespace nitrortmp {

namespace {

constexpr uint16_t kDefaultRtmpPort = 1935;
constexpr uint16_t kDefaultRtmpsPort = 443;

bool equalsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

/// Reads the explicit port from the authority, 0 when absent or malformed.
/// rtmp_url_parse() substitutes 1935 for a missing port, so it cannot tell us
/// whether the caller wrote one; uri_parse() keeps 0 for "absent".
bool explicitPort(const std::string& url, int& port) {
  struct uri_t* uri = uri_parse(url.c_str(), static_cast<int>(url.size()));
  if (uri == nullptr) {
    return false;
  }
  port = uri->port;
  uri_free(uri);
  return port >= 0 && port <= 65535;
}

}  // namespace

bool RtmpUrl::fitsClientBuffers() const {
  // rtmp_client_create passes sizeof(buffer) - 1 to snprintf, which reserves
  // another byte for the terminator. Keep these limits in sync with the vendor.
  const auto fits = [](const std::string& value, size_t maxBytes) {
    return value.size() <= maxBytes && value.find('\0') == std::string::npos;
  };
  return fits(app, 126) && fits(stream, 254) && fits(tcUrl, 254);
}

std::optional<RtmpUrl> RtmpUrl::parse(std::string_view input) {
  const size_t schemeEnd = input.find("://");
  if (schemeEnd == std::string_view::npos || schemeEnd == 0) {
    return std::nullopt;
  }

  RtmpUrl out;
  const std::string_view scheme = input.substr(0, schemeEnd);
  if (equalsIgnoreCase(scheme, "rtmp")) {
    out.scheme = "rtmp";
    out.useTls = false;
  } else if (equalsIgnoreCase(scheme, "rtmps")) {
    out.scheme = "rtmps";
    out.useTls = true;
  } else {
    return std::nullopt;
  }

  // ireader silently drops path segments beyond vhost/app/stream; refuse them.
  const size_t pathStart = input.find('/', schemeEnd + 3);
  if (pathStart == std::string_view::npos) {
    return std::nullopt;
  }
  std::string_view path = input.substr(pathStart);
  path = path.substr(0, path.find('?'));
  int segments = 0;
  for (size_t i = 0; i < path.size();) {
    if (path[i] == '/') {
      ++i;
      continue;
    }
    ++segments;
    while (i < path.size() && path[i] != '/') ++i;
  }
  if (segments > 3) {
    return std::nullopt;
  }

  // rtmp-url.h expects a NUL-terminated string and keeps its own copies.
  const std::string url(input);
  if (url.find('\0') != std::string::npos || url.size() >= 1024) {
    return std::nullopt;
  }

  int port = 0;
  if (!explicitPort(url, port)) {
    return std::nullopt;
  }

  struct rtmp_url_t parsed;
  if (0 != rtmp_url_parse(url.c_str(), &parsed)) {
    return std::nullopt;
  }
  if (parsed.host == nullptr || *parsed.host == '\0' || parsed.app == nullptr ||
      *parsed.app == '\0' || parsed.stream == nullptr || *parsed.stream == '\0') {
    return std::nullopt;
  }

  const uint16_t defaultPort = out.useTls ? kDefaultRtmpsPort : kDefaultRtmpPort;
  out.port = port == 0 ? defaultPort : static_cast<uint16_t>(port);
  out.host = parsed.host;
  out.app = parsed.app;
  out.stream = parsed.stream;

  // ireader always writes "rtmp://" into tcurl; rebuild it with the original
  // scheme. The vhost (path prefix or ?vhost=) replaces the host, as upstream does.
  const char* vhost = (parsed.vhost != nullptr && *parsed.vhost != '\0') ? parsed.vhost : parsed.host;
  out.tcUrl = out.scheme + "://" + vhost;
  if (out.port != defaultPort) {
    out.tcUrl += ":" + std::to_string(out.port);
  }
  out.tcUrl += "/" + out.app;
  if (!out.fitsClientBuffers()) {
    return std::nullopt;
  }
  return out;
}

}  // namespace nitrortmp
