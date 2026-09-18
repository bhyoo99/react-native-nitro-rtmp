#include "RtmpPublisher.hpp"

#include <cctype>
#include <cerrno>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include "amf0.h"
#include "flv-muxer.h"
#include "flv-proto.h"
#include "rtmp-client.h"
#include "rtmp-internal.h"  // enum rtmp_state_t
}

namespace nitrortmp {

const char* toString(PublisherState state) {
  switch (state) {
    case PublisherState::Idle: return "Idle";
    case PublisherState::Connecting: return "Connecting";
    case PublisherState::Connected: return "Connected";
    case PublisherState::Publishing: return "Publishing";
    case PublisherState::Stopped: return "Stopped";
    case PublisherState::Failed: return "Failed";
  }
  return "?";
}

const char* toString(PublisherError error) {
  switch (error) {
    case PublisherError::InvalidUrl: return "InvalidUrl";
    case PublisherError::HandshakeFailed: return "HandshakeFailed";
    case PublisherError::ConnectRejected: return "ConnectRejected";
    case PublisherError::InvalidApp: return "InvalidApp";
    case PublisherError::PublishBadName: return "PublishBadName";
    case PublisherError::StreamAlreadyExists: return "StreamAlreadyExists";
    case PublisherError::ServerClosed: return "ServerClosed";
    case PublisherError::ProtocolError: return "ProtocolError";
    case PublisherError::NotReady: return "NotReady";
  }
  return "?";
}

namespace {

constexpr uint8_t kRtmpVersion = 3;
constexpr uint8_t kNaluIdr = 5;

/// Scans an Annex-B buffer for NALU headers. Returns true if any NALU is an
/// IDR slice. Frames without a start code are reported as "no IDR" and are
/// then rejected by the muxer.
bool containsIdr(const uint8_t* data, size_t size) {
  size_t zeros = 0;
  for (size_t i = 0; i < size; ++i) {
    const uint8_t b = data[i];
    if (b == 0x00) {
      ++zeros;
      continue;
    }
    if (b == 0x01 && zeros >= 2 && i + 1 < size) {
      if ((data[i + 1] & 0x1F) == kNaluIdr) {
        return true;
      }
    }
    zeros = 0;
  }
  return false;
}

/// One ADTS frame, no CRC, frame_length equal to the buffer.
bool isSingleAdtsFrame(const uint8_t* data, size_t size) {
  if (size < 7 || data[0] != 0xFF || (data[1] & 0xF6) != 0xF0) {
    return false;
  }
  const bool protectionAbsent = (data[1] & 0x01) != 0;
  if (!protectionAbsent) {
    return false;  // ireader strips 7 bytes only; a CRC would leak into the payload
  }
  const size_t frameLength = (static_cast<size_t>(data[3] & 0x03) << 11) |
                             (static_cast<size_t>(data[4]) << 3) |
                             (static_cast<size_t>(data[5]) >> 5);
  return frameLength == size;
}

int rank(PublisherState state) {
  switch (state) {
    case PublisherState::Connecting: return 1;
    case PublisherState::Connected: return 2;
    case PublisherState::Publishing: return 3;
    default: return 0;
  }
}

bool isActive(PublisherState state) {
  return rank(state) > 0;
}

std::string lowercase(const char* text) {
  std::string out(text != nullptr ? text : "");
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

bool endsWith(const std::string& text, const char* suffix) {
  const size_t n = std::strlen(suffix);
  return text.size() >= n && text.compare(text.size() - n, n, suffix) == 0;
}

/// Status codes that end the session even when the level is not "error"
/// (ksyun sends level "finish"; Adobe closes with level "status").
bool isFatalStatusCode(const std::string& code) {
  return endsWith(code, ".connect.rejected") || endsWith(code, ".connect.invalidapp") ||
         endsWith(code, ".connect.closed") || endsWith(code, ".connect.appshutdown") ||
         endsWith(code, ".connect.illegalapplication") || endsWith(code, ".publish.badname") ||
         endsWith(code, ".publish.alreadyexiststream") || endsWith(code, ".publish.rejected") ||
         code == "netstream.failed";
}

std::vector<uint8_t> buildOnMetaData(const StreamMetadata& m) {
  std::vector<uint8_t> buf(1024);
  uint8_t* p = buf.data();
  const uint8_t* end = p + buf.size();
  uint32_t count = 0;

  p = AMFWriteString(p, end, "@setDataFrame", 13);
  p = AMFWriteString(p, end, "onMetaData", 10);
  uint8_t* array = p;
  p = AMFWriteECMAArarry(p, end);  // associative count patched below

  auto number = [&](const char* name, double value) {
    p = AMFWriteNamedDouble(p, end, name, std::strlen(name), value);
    ++count;
  };
  auto boolean = [&](const char* name, bool value) {
    p = AMFWriteNamedBoolean(p, end, name, std::strlen(name), value ? 1 : 0);
    ++count;
  };
  auto string = [&](const char* name, const std::string& value) {
    p = AMFWriteNamedString(p, end, name, std::strlen(name), value.c_str(), value.size());
    ++count;
  };

  number("duration", 0);
  const bool hasVideo = m.width > 0 || m.height > 0 || m.frameRate > 0 || m.videoBitrateKbps > 0;
  const bool hasAudio = m.audioSampleRate > 0 || m.audioChannels > 0 || m.audioBitrateKbps > 0;
  if (hasVideo) {
    if (m.width > 0) number("width", m.width);
    if (m.height > 0) number("height", m.height);
    if (m.videoBitrateKbps > 0) number("videodatarate", m.videoBitrateKbps);
    if (m.frameRate > 0) number("framerate", m.frameRate);
    number("videocodecid", FLV_VIDEO_H264);
  }
  if (hasAudio) {
    if (m.audioBitrateKbps > 0) number("audiodatarate", m.audioBitrateKbps);
    if (m.audioSampleRate > 0) number("audiosamplerate", m.audioSampleRate);
    number("audiosamplesize", 16);
    if (m.audioChannels > 0) {
      number("audiochannels", m.audioChannels);
      boolean("stereo", m.audioChannels >= 2);
    }
    number("audiocodecid", FLV_AUDIO_AAC >> 4);
  }
  if (!m.encoder.empty()) {
    string("encoder", m.encoder);
  }
  p = AMFWriteObjectEnd(p, end);
  if (p == nullptr) {
    return {};
  }
  array[1] = static_cast<uint8_t>(count >> 24);
  array[2] = static_cast<uint8_t>(count >> 16);
  array[3] = static_cast<uint8_t>(count >> 8);
  array[4] = static_cast<uint8_t>(count);
  buf.resize(static_cast<size_t>(p - buf.data()));
  return buf;
}

}  // namespace

struct RtmpPublisher::Impl {
  explicit Impl(PublisherCallbacks callbacks) : cb(std::move(callbacks)) {}
  ~Impl() {
    destroyClient();
    if (muxer != nullptr) {
      flv_muxer_destroy(muxer);
    }
  }

  PublisherCallbacks cb;
  RtmpUrl url;
  PublisherState state = PublisherState::Idle;
  PublisherStats stats;

  rtmp_client_t* client = nullptr;
  flv_muxer_t* muxer = nullptr;
  bool enhancedRtmp = false;
  std::optional<StreamMetadata> metadata;
  std::vector<uint8_t> sendBuffer;

  // Timestamp normalization
  bool haveOffset = false;
  uint32_t offset = 0;
  bool haveVideoDts = false;
  uint32_t lastVideoDts = 0;
  bool haveAudioDts = false;
  uint32_t lastAudioDts = 0;

  bool waitingForKeyframe = true;
  bool sawFirstByte = false;

  // Last onStatus/_error seen during the current rtmp_client_input call
  // (vendor patch 0001 exposes it), used for the error mapping.
  struct Status {
    bool fatal = false;
    std::string level;
    std::string code;  // lowercase
    std::string original;
    std::string description;
  } status;

  // Re-entrancy: vendor code invokes onSend synchronously; a stop() issued from
  // inside that callback would re-enter the chunk writer, so it is deferred.
  bool inVendorCall = false;
  bool stopRequested = false;

  // --- callbacks from the vendor code -------------------------------------

  static int onVendorSend(void* param, const void* header, size_t len, const void* payload, size_t bytes) {
    auto* self = static_cast<Impl*>(param);
    self->send(static_cast<const uint8_t*>(header), len, static_cast<const uint8_t*>(payload), bytes);
    return static_cast<int>(len + bytes);
  }

  static int onVendorIgnore(void*, const void*, size_t, uint32_t) {
    return 0;  // a publisher never receives media
  }

  static int onVendorStatus(void* param, const char* level, const char* code, const char* description) {
    auto* self = static_cast<Impl*>(param);
    Status& st = self->status;
    st.level = lowercase(level);
    st.code = lowercase(code);
    st.original = code != nullptr ? code : "";
    st.description = description != nullptr ? description : "";
    st.fatal = st.level == "error" || isFatalStatusCode(st.code);
    return st.fatal ? -1 : 0;
  }

  static int onFlvTag(void* param, int type, const void* data, size_t bytes, uint32_t timestamp) {
    auto* self = static_cast<Impl*>(param);
    int r;
    switch (type) {
      case FLV_TYPE_VIDEO:
        r = rtmp_client_push_video(self->client, data, bytes, timestamp);
        if (r == 0) ++self->stats.videoTags;
        return r;
      case FLV_TYPE_AUDIO:
        r = rtmp_client_push_audio(self->client, data, bytes, timestamp);
        if (r == 0) ++self->stats.audioTags;
        return r;
      case FLV_TYPE_SCRIPT:
        r = rtmp_client_push_script(self->client, data, bytes, timestamp);
        if (r == 0) ++self->stats.scriptTags;
        return r;
      default:
        return -1;
    }
  }

  // --- helpers --------------------------------------------------------------

  void send(const uint8_t* header, size_t len, const uint8_t* payload, size_t bytes) {
    const uint8_t* data = header;
    size_t size = len;
    if (bytes > 0) {
      sendBuffer.resize(len + bytes);
      if (len > 0) std::memcpy(sendBuffer.data(), header, len);
      std::memcpy(sendBuffer.data() + len, payload, bytes);
      data = sendBuffer.data();
      size = len + bytes;
    }
    if (size == 0) {
      return;
    }
    stats.bytesSent += size;
    if (cb.onSend) {
      cb.onSend(data, size);
    }
  }

  template <typename F>
  auto withVendor(F&& f) -> decltype(f()) {
    inVendorCall = true;
    auto r = f();
    inVendorCall = false;
    return r;
  }

  void flushDeferredStop(RtmpPublisher& owner) {
    if (stopRequested) {
      stopRequested = false;
      owner.stop();
    }
  }

  void setState(PublisherState next) {
    if (state == next) {
      return;
    }
    state = next;
    if (cb.onStateChange) {
      cb.onStateChange(next);
    }
  }

  void reportError(PublisherError error, const std::string& message) {
    if (cb.onError) {
      cb.onError(error, message);
    }
  }

  void fail(PublisherError error, const std::string& message) {
    reportError(error, message);
    setState(PublisherState::Failed);
  }

  void destroyClient() {
    if (client != nullptr) {
      rtmp_client_destroy(client);
      client = nullptr;
    }
  }

  void resetSession() {
    destroyClient();
    stats = PublisherStats{};
    haveOffset = false;
    offset = 0;
    haveVideoDts = false;
    lastVideoDts = 0;
    haveAudioDts = false;
    lastAudioDts = 0;
    waitingForKeyframe = true;
    sawFirstByte = false;
    stopRequested = false;
  }

  int vendorState() const {
    return client != nullptr ? rtmp_client_getstate(client) : RTMP_STATE_UNINIT;
  }

  void advanceTo(PublisherState target) {
    if (rank(state) >= rank(target)) {
      return;
    }
    if (target == PublisherState::Publishing) {
      sendMetadata();
    }
    setState(target);
  }

  /// Maps the vendor state to ours after each input, emitting
  /// intermediate transitions so callers always see Connected before Publishing.
  void syncState() {
    switch (vendorState()) {
      case RTMP_STATE_UNINIT:
      case RTMP_STATE_HANDSHAKE:
        break;
      case RTMP_STATE_CONNECTED:
      case RTMP_STATE_CREATE_STREAM:
        advanceTo(PublisherState::Connected);
        break;
      case RTMP_STATE_START:
        advanceTo(PublisherState::Connected);
        advanceTo(PublisherState::Publishing);
        break;
      case RTMP_STATE_STOP:
      case RTMP_STATE_DELETE_STREAM:
      default:
        reportError(PublisherError::ServerClosed, "server ended the stream (StreamEOF or NetStream.*.Stop)");
        setState(PublisherState::Stopped);
        break;
    }
  }

  /// Error mapping. With a server status code (vendor patch 0001)
  /// the code decides; otherwise the phase we were in does.
  PublisherError mapInputError(int r, std::string& message) const {
    if (status.fatal) {
      message = status.original + (status.description.empty() ? "" : ": " + status.description);
      const std::string& c = status.code;
      if (endsWith(c, ".connect.invalidapp") || endsWith(c, ".connect.illegalapplication")) return PublisherError::InvalidApp;
      if (endsWith(c, ".connect.rejected")) return PublisherError::ConnectRejected;
      if (endsWith(c, ".connect.closed") || endsWith(c, ".connect.appshutdown")) return PublisherError::ServerClosed;
      if (endsWith(c, ".publish.alreadyexiststream")) return PublisherError::StreamAlreadyExists;
      if (endsWith(c, ".publish.badname") || endsWith(c, ".publish.rejected")) return PublisherError::PublishBadName;
      switch (vendorState()) {
        case RTMP_STATE_UNINIT:
        case RTMP_STATE_HANDSHAKE: return PublisherError::ConnectRejected;
        case RTMP_STATE_CONNECTED:
        case RTMP_STATE_CREATE_STREAM: return PublisherError::PublishBadName;
        default: return PublisherError::ServerClosed;
      }
    }

    const bool parserError = (r == -EINVAL || r == -ENOMEM || r == -E2BIG);
    const std::string detail = " (rtmp_client_input returned " + std::to_string(r) + ")";
    switch (vendorState()) {
      case RTMP_STATE_UNINIT:
        message = "handshake failed before S0/S1/S2 completed" + detail;
        return PublisherError::HandshakeFailed;
      case RTMP_STATE_HANDSHAKE:
        if (parserError) break;
        message = "connect rejected by server (NetConnection.Connect.Rejected/InvalidApp)" + detail;
        return PublisherError::ConnectRejected;
      case RTMP_STATE_CONNECTED:
      case RTMP_STATE_CREATE_STREAM:
        if (parserError) break;
        message = "publish rejected by server (NetStream.Publish.BadName or stream already published)" + detail;
        return PublisherError::PublishBadName;
      case RTMP_STATE_START:
        if (parserError) break;
        message = "server reported an error while publishing" + detail;
        return PublisherError::ServerClosed;
      default:
        message = "server closed the stream" + detail;
        return PublisherError::ServerClosed;
    }
    message = "malformed RTMP data from server" + detail;
    return PublisherError::ProtocolError;
  }

  void sendMetadata() {
    if (!metadata.has_value() || client == nullptr) {
      return;
    }
    const std::vector<uint8_t> amf = buildOnMetaData(*metadata);
    if (amf.empty()) {
      return;
    }
    withVendor([&] { return rtmp_client_push_script(client, amf.data(), amf.size(), 0); });
    ++stats.scriptTags;
  }

  struct Normalized {
    uint32_t dts;
    uint32_t pts;
    bool clamped;
  };

  Normalized normalize(uint32_t pts, uint32_t dts, bool haveLast, uint32_t last) const {
    Normalized n{0, 0, false};
    const int64_t base = haveOffset ? static_cast<int64_t>(offset) : static_cast<int64_t>(dts);
    int64_t ndts = static_cast<int64_t>(dts) - base;
    int64_t npts = static_cast<int64_t>(pts) - base;
    if (ndts < 0) {
      ndts = 0;
      n.clamped = true;
    }
    if (haveLast && ndts < static_cast<int64_t>(last)) {
      ndts = last;
      n.clamped = true;
    }
    if (npts < ndts) {
      npts = ndts;
      n.clamped = true;
    }
    n.dts = static_cast<uint32_t>(ndts);
    n.pts = static_cast<uint32_t>(npts);
    return n;
  }

  void commitOffset(uint32_t dts) {
    if (!haveOffset) {
      haveOffset = true;
      offset = dts;
    }
  }
};

// --- public API ---------------------------------------------------------------

RtmpPublisher::RtmpPublisher(PublisherCallbacks callbacks)
    : impl_(std::make_unique<Impl>(std::move(callbacks))) {}

RtmpPublisher::~RtmpPublisher() = default;

bool RtmpPublisher::start(std::string_view url) {
  std::optional<RtmpUrl> parsed = RtmpUrl::parse(url);
  if (!parsed.has_value()) {
    impl_->reportError(PublisherError::InvalidUrl, "not an rtmp:// or rtmps:// URL with app and stream: " + std::string(url));
    return false;
  }
  return start(*parsed);
}

bool RtmpPublisher::start(const RtmpUrl& url) {
  Impl& im = *impl_;
  if (im.inVendorCall) {
    im.reportError(PublisherError::NotReady, "start() called from inside a callback");
    return false;
  }
  if (isActive(im.state)) {
    im.reportError(PublisherError::NotReady, "start() while " + std::string(toString(im.state)) + "; call stop() first");
    return false;
  }
  if (url.app.empty() || url.stream.empty() || url.tcUrl.empty() || url.host.empty()) {
    im.reportError(PublisherError::InvalidUrl, "RtmpUrl needs host, app, stream and tcUrl");
    return false;
  }
  if (!url.fitsClientBuffers()) {
    im.reportError(PublisherError::InvalidUrl, "app, stream or tcUrl would be truncated by the RTMP client");
    return false;
  }

  im.resetSession();
  im.url = url;

  static const struct rtmp_client_handler_t kHandler = {
      &Impl::onVendorSend,
      &Impl::onVendorIgnore,  // onvideo
      &Impl::onVendorIgnore,  // onaudio
      &Impl::onVendorIgnore,  // onscript
      &Impl::onVendorStatus,  // onstatus (vendor patch 0001)
  };
  im.client = rtmp_client_create(url.app.c_str(), url.stream.c_str(), url.tcUrl.c_str(), &im, &kHandler);
  if (im.client == nullptr) {
    im.fail(PublisherError::ProtocolError, "rtmp_client_create failed");
    return false;
  }

  if (im.muxer == nullptr) {
    im.muxer = flv_muxer_create(&Impl::onFlvTag, &im);
    if (im.muxer == nullptr) {
      im.destroyClient();
      im.fail(PublisherError::ProtocolError, "flv_muxer_create failed");
      return false;
    }
  } else {
    flv_muxer_reset(im.muxer);  // fresh AVC/AAC sequence headers on reconnect
  }
  flv_muxer_set_enhanced_rtmp(im.muxer, im.enhancedRtmp ? 1 : 0);

  im.setState(PublisherState::Connecting);
  const int r = im.withVendor([&] { return rtmp_client_start(im.client, 0 /* publish */); });
  im.flushDeferredStop(*this);
  if (r != 0) {
    im.fail(PublisherError::HandshakeFailed, "rtmp_client_start failed (" + std::to_string(r) + ")");
    return false;
  }
  return true;
}

void RtmpPublisher::onReceive(const uint8_t* data, size_t size) {
  Impl& im = *impl_;
  if (!isActive(im.state) || im.client == nullptr || size == 0 || im.inVendorCall) {
    return;
  }
  if (!im.sawFirstByte) {
    im.sawFirstByte = true;
    if (data[0] != kRtmpVersion) {
      im.fail(PublisherError::HandshakeFailed,
              "unexpected S0 version " + std::to_string(data[0]) + " (not an RTMP server, or TLS expected)");
      return;
    }
  }
  im.status = Impl::Status{};
  const int r = im.withVendor([&] { return rtmp_client_input(im.client, data, size); });
  im.flushDeferredStop(*this);
  if (!isActive(im.state)) {
    return;  // stop() ran from a callback
  }
  if (r != 0) {
    std::string message;
    const PublisherError error = im.mapInputError(r, message);
    im.fail(error, message);
    return;
  }
  im.syncState();
}

void RtmpPublisher::stop() {
  Impl& im = *impl_;
  if (im.inVendorCall) {
    im.stopRequested = true;
    return;
  }
  switch (im.state) {
    case PublisherState::Idle:
    case PublisherState::Stopped:
    case PublisherState::Failed:
      return;
    case PublisherState::Connecting:
      im.setState(PublisherState::Stopped);
      return;
    case PublisherState::Connected:
    case PublisherState::Publishing:
      if (im.vendorState() >= RTMP_STATE_CREATE_STREAM) {
        im.withVendor([&] { return rtmp_client_stop(im.client); });  // FCUnpublish + deleteStream
      }
      im.setState(PublisherState::Stopped);
      return;
  }
}

void RtmpPublisher::setEnhancedRtmp(bool enabled) {
  impl_->enhancedRtmp = enabled;
  if (impl_->muxer != nullptr) {
    flv_muxer_set_enhanced_rtmp(impl_->muxer, enabled ? 1 : 0);
  }
}

void RtmpPublisher::setMetadata(const StreamMetadata& metadata) {
  Impl& im = *impl_;
  im.metadata = metadata;
  if (im.state == PublisherState::Publishing && !im.inVendorCall) {
    im.sendMetadata();
    im.flushDeferredStop(*this);
  }
}

bool RtmpPublisher::pushVideo(const uint8_t* annexb, size_t size, uint32_t ptsMs, uint32_t dtsMs) {
  Impl& im = *impl_;
  if (im.state != PublisherState::Publishing || im.inVendorCall) {
    ++im.stats.rejectedFrames;
    return false;
  }
  if (annexb == nullptr || size == 0) {
    ++im.stats.invalidFrames;
    return false;
  }
  if (im.waitingForKeyframe && !containsIdr(annexb, size)) {
    ++im.stats.droppedBeforeKeyframe;
    return false;
  }

  const Impl::Normalized ts = im.normalize(ptsMs, dtsMs, im.haveVideoDts, im.lastVideoDts);
  const uint64_t tagsBefore = im.stats.videoTags;
  const int r = im.withVendor([&] { return flv_muxer_avc(im.muxer, annexb, size, ts.pts, ts.dts); });
  im.flushDeferredStop(*this);
  if (r != 0) {
    ++im.stats.invalidFrames;
    return false;
  }
  if (im.stats.videoTags == tagsBefore) {
    // The muxer had no SPS/PPS to build a sequence header from and dropped the frame.
    ++im.stats.droppedBeforeKeyframe;
    return false;
  }

  im.waitingForKeyframe = false;
  im.commitOffset(dtsMs);
  im.haveVideoDts = true;
  im.lastVideoDts = ts.dts;
  im.stats.lastVideoTimestamp = ts.dts;
  if (ts.clamped) ++im.stats.timestampClamps;
  return true;
}

bool RtmpPublisher::pushAudio(const uint8_t* adts, size_t size, uint32_t ptsMs) {
  Impl& im = *impl_;
  if (im.state != PublisherState::Publishing || im.inVendorCall) {
    ++im.stats.rejectedFrames;
    return false;
  }
  if (adts == nullptr || !isSingleAdtsFrame(adts, size)) {
    ++im.stats.invalidFrames;
    return false;
  }

  const Impl::Normalized ts = im.normalize(ptsMs, ptsMs, im.haveAudioDts, im.lastAudioDts);
  const uint64_t tagsBefore = im.stats.audioTags;
  const int r = im.withVendor([&] { return flv_muxer_aac(im.muxer, adts, size, ts.pts, ts.dts); });
  im.flushDeferredStop(*this);
  if (r != 0 || im.stats.audioTags == tagsBefore) {
    ++im.stats.invalidFrames;
    return false;
  }

  im.commitOffset(ptsMs);
  im.haveAudioDts = true;
  im.lastAudioDts = ts.dts;
  im.stats.lastAudioTimestamp = ts.dts;
  if (ts.clamped) ++im.stats.timestampClamps;
  return true;
}

PublisherState RtmpPublisher::state() const {
  return impl_->state;
}

PublisherStats RtmpPublisher::stats() const {
  return impl_->stats;
}

const RtmpUrl& RtmpPublisher::url() const {
  return impl_->url;
}

}  // namespace nitrortmp
