#include "Loopback.hpp"

#include <cstring>

extern "C" {
#include "rtmp-control-message.h"
#include "rtmp-event.h"
#include "rtmp-netconnection.h"
#include "rtmp-netstream.h"
}

using nitrortmp::PublisherCallbacks;
using nitrortmp::PublisherError;
using nitrortmp::PublisherState;

namespace test {

// --- LoopbackServer --------------------------------------------------------------

namespace {

int serverSend(void* param, const void* header, size_t len, const void* payload, size_t bytes) {
  auto* self = static_cast<LoopbackServer*>(param);
  const auto* h = static_cast<const uint8_t*>(header);
  const auto* p = static_cast<const uint8_t*>(payload);
  if (len > 0) self->output.insert(self->output.end(), h, h + len);
  if (bytes > 0) self->output.insert(self->output.end(), p, p + bytes);
  return static_cast<int>(len + bytes);
}

int serverOnPlay(void*, const char*, const char*, double, double, uint8_t) { return -1; }
int serverOnPause(void*, int, uint32_t) { return -1; }
int serverOnSeek(void*, uint32_t) { return -1; }
int serverOnGetDuration(void*, const char*, const char*, double*) { return -1; }

int serverOnPublish(void* param, const char* app, const char* stream, const char* type) {
  auto* self = static_cast<LoopbackServer*>(param);
  ++self->publishCalls;
  self->app = app ? app : "";
  self->stream = stream ? stream : "";
  self->type = type ? type : "";
  return self->publishResult;
}

int serverCollect(LoopbackServer* self, int type, const void* data, size_t bytes, uint32_t timestamp) {
  FlvTag tag;
  tag.type = type;
  tag.timestamp = timestamp;
  tag.data.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + bytes);
  self->tags.push_back(std::move(tag));
  return 0;
}
int serverOnVideo(void* param, const void* data, size_t bytes, uint32_t timestamp) {
  return serverCollect(static_cast<LoopbackServer*>(param), 9, data, bytes, timestamp);
}
int serverOnAudio(void* param, const void* data, size_t bytes, uint32_t timestamp) {
  return serverCollect(static_cast<LoopbackServer*>(param), 8, data, bytes, timestamp);
}
int serverOnScript(void* param, const void* data, size_t bytes, uint32_t timestamp) {
  return serverCollect(static_cast<LoopbackServer*>(param), 18, data, bytes, timestamp);
}

const struct rtmp_server_handler_t kServerHandler = {
    &serverSend, &serverOnPlay, &serverOnPause, &serverOnSeek, &serverOnPublish,
    &serverOnVideo, &serverOnAudio, &serverOnScript, &serverOnGetDuration,
};

}  // namespace

LoopbackServer::LoopbackServer() : server(rtmp_server_create(this, &kServerHandler)) {}

LoopbackServer::~LoopbackServer() {
  if (server != nullptr) rtmp_server_destroy(server);
}

int LoopbackServer::input(const uint8_t* data, size_t size) {
  return rtmp_server_input(server, data, size);
}

// --- LoopbackPublisher ---------------------------------------------------------

namespace {
PublisherCallbacks loopbackCallbacks(LoopbackPublisher* self) {
  PublisherCallbacks cb;
  cb.onSend = [self](const uint8_t* data, size_t size) { self->toServer.insert(self->toServer.end(), data, data + size); };
  cb.onStateChange = [self](PublisherState state) { self->states.push_back(state); };
  cb.onError = [self](PublisherError error, const std::string& message) { self->errors.emplace_back(error, message); };
  return cb;
}
}  // namespace

LoopbackPublisher::LoopbackPublisher(size_t chunk)
    : deliverChunk(chunk), server(std::make_unique<LoopbackServer>()), pub(loopbackCallbacks(this)) {}

void LoopbackPublisher::pump() {
  for (;;) {
    bool moved = false;
    if (!toServer.empty()) {
      std::vector<uint8_t> bytes;
      bytes.swap(toServer);
      const size_t step = deliverChunk > 0 ? deliverChunk : bytes.size();
      for (size_t i = 0; i < bytes.size() && serverInputResult == 0; i += step) {
        const size_t n = std::min(step, bytes.size() - i);
        serverInputResult = server->input(bytes.data() + i, n);
      }
      moved = true;
    }
    if (!server->output.empty()) {
      std::vector<uint8_t> bytes;
      bytes.swap(server->output);
      const size_t step = deliverChunk > 0 ? deliverChunk : bytes.size();
      for (size_t i = 0; i < bytes.size(); i += step) {
        const size_t n = std::min(step, bytes.size() - i);
        pub.onReceive(bytes.data() + i, n);
      }
      moved = true;
    }
    if (!moved) return;
  }
}

bool LoopbackPublisher::connect(const char* url) {
  if (!pub.start(std::string_view(url))) return false;
  pump();
  return pub.state() == PublisherState::Publishing;
}

size_t LoopbackPublisher::push(const std::vector<MediaFrame>& frames) {
  size_t accepted = 0;
  for (const MediaFrame& f : frames) {
    const bool ok = f.video ? pub.pushVideo(f.data.data(), f.data.size(), f.pts, f.dts)
                            : pub.pushAudio(f.data.data(), f.data.size(), f.pts);
    if (ok) ++accepted;
    pump();
  }
  return accepted;
}

void LoopbackPublisher::newConnection() {
  toServer.clear();
  server = std::make_unique<LoopbackServer>();
  serverInputResult = 0;
}

// --- ScriptedServer ---------------------------------------------------------------

std::vector<uint8_t> ScriptedServer::handshakeResponse(const std::vector<uint8_t>& c0c1) {
  std::vector<uint8_t> out(1 + 1536 + 1536, 0);
  out[0] = 3;
  // S1: time(4) + zero(4) => version 0, the client treats it as a simple handshake
  for (size_t i = 8; i < 1536; ++i) out[1 + i] = static_cast<uint8_t>(i * 7);
  // S2: echo of C1
  if (c0c1.size() >= 1537) std::memcpy(out.data() + 1 + 1536, c0c1.data() + 1, 1536);
  return out;
}

std::vector<uint8_t> ScriptedServer::chunkMessage(uint32_t cid, uint8_t typeId, uint32_t streamId,
                                                  const std::vector<uint8_t>& payload, uint32_t chunkSize) {
  std::vector<uint8_t> out;
  out.push_back(static_cast<uint8_t>(cid & 0x3F));  // fmt 0
  out.push_back(0); out.push_back(0); out.push_back(0);  // timestamp
  out.push_back(static_cast<uint8_t>(payload.size() >> 16));
  out.push_back(static_cast<uint8_t>(payload.size() >> 8));
  out.push_back(static_cast<uint8_t>(payload.size()));
  out.push_back(typeId);
  out.push_back(static_cast<uint8_t>(streamId));  // little endian stream id
  out.push_back(static_cast<uint8_t>(streamId >> 8));
  out.push_back(static_cast<uint8_t>(streamId >> 16));
  out.push_back(static_cast<uint8_t>(streamId >> 24));
  for (size_t i = 0; i < payload.size(); i += chunkSize) {
    if (i > 0) out.push_back(static_cast<uint8_t>(0xC0 | (cid & 0x3F)));  // fmt 3
    const size_t n = std::min<size_t>(chunkSize, payload.size() - i);
    out.insert(out.end(), payload.begin() + static_cast<long>(i), payload.begin() + static_cast<long>(i + n));
  }
  return out;
}

namespace {
constexpr uint8_t kTypeInvoke = 20;
constexpr uint32_t kCidInvoke = 3;

std::vector<uint8_t> invoke(uint8_t* begin, uint8_t* end, uint32_t streamId) {
  return ScriptedServer::chunkMessage(kCidInvoke, kTypeInvoke, streamId, std::vector<uint8_t>(begin, end));
}
}  // namespace

std::vector<uint8_t> ScriptedServer::connectResult() {
  uint8_t buf[512];
  uint8_t* end = rtmp_netconnection_connect_reply(buf, sizeof(buf), 1 /* RTMP_TRANSACTION_CONNECT */, "FMS/3,0,1,123", 31,
                                                  "NetConnection.Connect.Success", "status", "Connection succeeded.", 0);
  return invoke(buf, end, 0);
}

std::vector<uint8_t> ScriptedServer::connectError(const char* code, const char* description) {
  uint8_t buf[512];
  uint8_t* end = rtmp_netconnection_error(buf, sizeof(buf), 1, code, "error", description);
  return invoke(buf, end, 0);
}

std::vector<uint8_t> ScriptedServer::createStreamResult(uint32_t streamId) {
  uint8_t buf[256];
  uint8_t* end = rtmp_netconnection_create_stream_reply(buf, sizeof(buf), 2 /* RTMP_TRANSACTION_CREATE_STREAM */, streamId);
  return invoke(buf, end, 0);
}

std::vector<uint8_t> ScriptedServer::onStatus(const char* level, const char* code, const char* description, uint32_t streamId) {
  uint8_t buf[512];
  uint8_t* end = rtmp_netstream_onstatus(buf, sizeof(buf), 0, level, code, description);
  return invoke(buf, end, streamId);
}

std::vector<uint8_t> ScriptedServer::streamEof(uint32_t streamId) {
  uint8_t buf[64];
  const int n = rtmp_event_stream_eof(buf, sizeof(buf), streamId);  // full chunk, header included
  return std::vector<uint8_t>(buf, buf + n);
}

std::vector<uint8_t> ScriptedServer::setChunkSize(uint32_t size) {
  uint8_t buf[64];
  const int n = rtmp_set_chunk_size(buf, sizeof(buf), size);
  return std::vector<uint8_t>(buf, buf + n);
}

// --- CapturedPublisher --------------------------------------------------------------

namespace {
PublisherCallbacks capturedCallbacks(CapturedPublisher* self) {
  PublisherCallbacks cb;
  cb.onSend = [self](const uint8_t* data, size_t size) {
    self->output.insert(self->output.end(), data, data + size);
    if (self->stopFromOnSend) {
      self->stopFromOnSend = false;
      self->pub.stop();
    }
  };
  cb.onStateChange = [self](PublisherState state) { self->states.push_back(state); };
  cb.onError = [self](PublisherError error, const std::string& message) { self->errors.emplace_back(error, message); };
  return cb;
}
}  // namespace

CapturedPublisher::CapturedPublisher() : pub(capturedCallbacks(this)) {}

std::vector<uint8_t> CapturedPublisher::takeOutput() {
  std::vector<uint8_t> out;
  out.swap(output);
  return out;
}

bool CapturedPublisher::driveToPublishing() {
  const std::vector<uint8_t> c0c1 = takeOutput();
  if (c0c1.size() != 1537) return false;
  const std::vector<uint8_t> s = ScriptedServer::handshakeResponse(c0c1);
  pub.onReceive(s.data(), s.size());
  if (!contains(takeOutput(), "connect")) return false;
  const std::vector<uint8_t> connectOk = ScriptedServer::connectResult();
  pub.onReceive(connectOk.data(), connectOk.size());
  if (pub.state() != PublisherState::Connected || !contains(takeOutput(), "createStream")) return false;
  const std::vector<uint8_t> created = ScriptedServer::createStreamResult();
  pub.onReceive(created.data(), created.size());
  if (!contains(takeOutput(), "publish")) return false;
  const std::vector<uint8_t> started = ScriptedServer::onStatus("status", "NetStream.Publish.Start", "Start publishing");
  pub.onReceive(started.data(), started.size());
  return pub.state() == PublisherState::Publishing;
}

}  // namespace test
