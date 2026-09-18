#include "nitrortmp_c.h"

#include <cstring>
#include <optional>
#include <string>

#include "Adts.hpp"
#include "AnnexB.hpp"
#include "RtmpPublisher.hpp"
#include "RtmpUrl.hpp"

namespace {

// Order matches the enums in nitrortmp_c.h and the JS unions (checked by CApi.test.cpp).
const char* const kStateNames[NITRORTMP_STATE_COUNT] = {
    "idle", "connecting", "connected", "publishing", "stopped", "failed",
};

const char* const kErrorNames[NITRORTMP_ERROR_COUNT] = {
    "invalidUrl",  "handshakeFailed", "connectRejected", "invalidApp",   "publishBadName",
    "streamAlreadyExists", "serverClosed", "protocolError", "notReady",
    "connectFailed", "tlsFailed", "socketClosed", "timeout",
};

int toC(nitrortmp::PublisherState state) {
  using nitrortmp::PublisherState;
  switch (state) {
    case PublisherState::Idle: return NITRORTMP_STATE_IDLE;
    case PublisherState::Connecting: return NITRORTMP_STATE_CONNECTING;
    case PublisherState::Connected: return NITRORTMP_STATE_CONNECTED;
    case PublisherState::Publishing: return NITRORTMP_STATE_PUBLISHING;
    case PublisherState::Stopped: return NITRORTMP_STATE_STOPPED;
    case PublisherState::Failed: return NITRORTMP_STATE_FAILED;
  }
  return NITRORTMP_STATE_FAILED;
}

int toC(nitrortmp::PublisherError error) {
  using nitrortmp::PublisherError;
  switch (error) {
    case PublisherError::InvalidUrl: return NITRORTMP_ERROR_INVALID_URL;
    case PublisherError::HandshakeFailed: return NITRORTMP_ERROR_HANDSHAKE_FAILED;
    case PublisherError::ConnectRejected: return NITRORTMP_ERROR_CONNECT_REJECTED;
    case PublisherError::InvalidApp: return NITRORTMP_ERROR_INVALID_APP;
    case PublisherError::PublishBadName: return NITRORTMP_ERROR_PUBLISH_BAD_NAME;
    case PublisherError::StreamAlreadyExists: return NITRORTMP_ERROR_STREAM_ALREADY_EXISTS;
    case PublisherError::ServerClosed: return NITRORTMP_ERROR_SERVER_CLOSED;
    case PublisherError::ProtocolError: return NITRORTMP_ERROR_PROTOCOL_ERROR;
    case PublisherError::NotReady: return NITRORTMP_ERROR_NOT_READY;
  }
  return NITRORTMP_ERROR_PROTOCOL_ERROR;
}

}  // namespace

struct nitrortmp_publisher {
  nitrortmp_callbacks_t callbacks;
  void* ctx;
  std::optional<nitrortmp::RtmpPublisher> core;  // constructed after `callbacks`/`ctx` are set

  nitrortmp_publisher(const nitrortmp_callbacks_t& cb, void* context) : callbacks(cb), ctx(context) {
    nitrortmp::PublisherCallbacks c;
    c.onSend = [this](const uint8_t* data, size_t size) {
      if (callbacks.on_send != nullptr) callbacks.on_send(ctx, data, size);
    };
    c.onStateChange = [this](nitrortmp::PublisherState state) {
      if (callbacks.on_state != nullptr) callbacks.on_state(ctx, toC(state));
    };
    c.onError = [this](nitrortmp::PublisherError error, const std::string& message) {
      if (callbacks.on_error != nullptr) callbacks.on_error(ctx, toC(error), message.c_str());
    };
    core.emplace(std::move(c));
  }
};

extern "C" {

int nitrortmp_url_endpoint(const char* url, nitrortmp_endpoint_t* out) {
  if (url == nullptr || out == nullptr) return 0;
  const std::optional<nitrortmp::RtmpUrl> parsed = nitrortmp::RtmpUrl::parse(url);
  if (!parsed.has_value() || parsed->host.size() >= sizeof(out->host)) return 0;
  std::memset(out, 0, sizeof(*out));
  std::memcpy(out->host, parsed->host.c_str(), parsed->host.size() + 1);
  out->port = parsed->port;
  out->use_tls = parsed->useTls ? 1 : 0;
  return 1;
}

nitrortmp_publisher_t* nitrortmp_publisher_create(const nitrortmp_callbacks_t* cb, void* ctx) {
  nitrortmp_callbacks_t callbacks{};
  if (cb != nullptr) callbacks = *cb;
  return new nitrortmp_publisher(callbacks, ctx);
}

void nitrortmp_publisher_destroy(nitrortmp_publisher_t* publisher) {
  delete publisher;
}

int nitrortmp_publisher_start(nitrortmp_publisher_t* publisher, const char* url) {
  if (publisher == nullptr) return 0;
  return publisher->core->start(std::string_view(url != nullptr ? url : "")) ? 1 : 0;
}

void nitrortmp_publisher_on_receive(nitrortmp_publisher_t* publisher, const uint8_t* data, size_t size) {
  if (publisher == nullptr || data == nullptr) return;
  publisher->core->onReceive(data, size);
}

void nitrortmp_publisher_stop(nitrortmp_publisher_t* publisher) {
  if (publisher == nullptr) return;
  publisher->core->stop();
}

int nitrortmp_publisher_push_video(nitrortmp_publisher_t* publisher, const uint8_t* annexb, size_t size,
                                   uint32_t pts_ms, uint32_t dts_ms) {
  if (publisher == nullptr) return 0;
  return publisher->core->pushVideo(annexb, size, pts_ms, dts_ms) ? 1 : 0;
}

int nitrortmp_publisher_push_audio(nitrortmp_publisher_t* publisher, const uint8_t* adts, size_t size, uint32_t pts_ms) {
  if (publisher == nullptr) return 0;
  return publisher->core->pushAudio(adts, size, pts_ms) ? 1 : 0;
}

void nitrortmp_publisher_set_metadata(nitrortmp_publisher_t* publisher, const nitrortmp_metadata_t* metadata) {
  if (publisher == nullptr || metadata == nullptr) return;
  nitrortmp::StreamMetadata m;
  m.width = metadata->width;
  m.height = metadata->height;
  m.frameRate = metadata->frame_rate;
  m.videoBitrateKbps = metadata->video_bitrate_kbps;
  m.audioSampleRate = metadata->audio_sample_rate;
  m.audioChannels = metadata->audio_channels;
  m.audioBitrateKbps = metadata->audio_bitrate_kbps;
  if (metadata->encoder != nullptr) m.encoder = metadata->encoder;
  publisher->core->setMetadata(m);
}

int nitrortmp_publisher_state(const nitrortmp_publisher_t* publisher) {
  if (publisher == nullptr) return NITRORTMP_STATE_IDLE;
  return toC(publisher->core->state());
}

void nitrortmp_publisher_stats(const nitrortmp_publisher_t* publisher, nitrortmp_stats_t* out) {
  if (out == nullptr) return;
  std::memset(out, 0, sizeof(*out));
  if (publisher == nullptr) return;
  const nitrortmp::PublisherStats s = publisher->core->stats();
  out->bytes_sent = s.bytesSent;
  out->video_tags = s.videoTags;
  out->audio_tags = s.audioTags;
  out->script_tags = s.scriptTags;
  out->rejected_frames = s.rejectedFrames;
  out->dropped_before_keyframe = s.droppedBeforeKeyframe;
  out->invalid_frames = s.invalidFrames;
  out->timestamp_clamps = s.timestampClamps;
  out->last_video_timestamp = s.lastVideoTimestamp;
  out->last_audio_timestamp = s.lastAudioTimestamp;
}

const char* nitrortmp_state_name(int state) {
  if (state < 0 || state >= NITRORTMP_STATE_COUNT) return nullptr;
  return kStateNames[state];
}

const char* nitrortmp_error_name(int code) {
  if (code < 0 || code >= NITRORTMP_ERROR_COUNT) return nullptr;
  return kErrorNames[code];
}

static size_t copyOut(const std::vector<uint8_t>& result, uint8_t* out, size_t out_capacity) {
  if (out != nullptr && out_capacity >= result.size() && !result.empty()) {
    std::memcpy(out, result.data(), result.size());
  }
  return result.size();
}

size_t nitrortmp_avcc_to_annexb(const uint8_t* avcc, size_t size, int length_size, uint8_t* out,
                                size_t out_capacity) {
  return copyOut(nitrortmp::avccToAnnexB(avcc, size, length_size), out, out_capacity);
}

size_t nitrortmp_prepend_parameter_sets(const uint8_t* sps, size_t sps_size, const uint8_t* pps, size_t pps_size,
                                        const uint8_t* annexb, size_t size, uint8_t* out, size_t out_capacity) {
  const std::vector<uint8_t> s(sps != nullptr ? sps : nullptr, sps != nullptr ? sps + sps_size : nullptr);
  const std::vector<uint8_t> p(pps != nullptr ? pps : nullptr, pps != nullptr ? pps + pps_size : nullptr);
  return copyOut(nitrortmp::prependParameterSets(s, p, annexb, size), out, out_capacity);
}

int nitrortmp_annexb_is_keyframe(const uint8_t* annexb, size_t size) {
  return nitrortmp::isKeyframe(annexb, size) ? 1 : 0;
}

int nitrortmp_annexb_has_parameter_sets(const uint8_t* annexb, size_t size) {
  return nitrortmp::hasParameterSets(annexb, size) ? 1 : 0;
}

int nitrortmp_write_adts_header(uint8_t* out, int profile, int sample_rate, int channels, size_t payload_size) {
  return nitrortmp::writeAdtsHeader(out, profile, sample_rate, channels, payload_size) ? 1 : 0;
}

}  // extern "C"
