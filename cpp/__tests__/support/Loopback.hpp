#pragma once

// Two in-process counterparts for the publisher:
//  - LoopbackServer wraps ireader's rtmp-server.c, the reference implementation
//    of the other side of the protocol. LoopbackPublisher wires a publisher to
//    it through byte queues and pumps until both sides go quiet.
//  - ScriptedServer only does the handshake and lets a test inject hand-built
//    server messages (rejections, StreamEOF) that rtmp-server.c cannot produce.

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "Fixtures.hpp"
#include "RtmpPublisher.hpp"

extern "C" {
#include "rtmp-server.h"
}

namespace test {

struct LoopbackServer {
  LoopbackServer();
  ~LoopbackServer();
  LoopbackServer(const LoopbackServer&) = delete;
  LoopbackServer& operator=(const LoopbackServer&) = delete;

  int input(const uint8_t* data, size_t size);

  rtmp_server_t* server = nullptr;
  std::vector<uint8_t> output;  // bytes for the client, drained by the pump
  std::vector<FlvTag> tags;     // what onaudio/onvideo/onscript delivered
  std::string app, stream, type;
  int publishCalls = 0;
  int publishResult = 0;  // returned from onpublish: non-zero => NetStream.Publish.BadName
};

struct LoopbackPublisher {
  /// deliverChunk > 0 feeds both directions in pieces of that many bytes.
  explicit LoopbackPublisher(size_t deliverChunk = 0);

  void pump();
  /// start() + pump(); true when Publishing was reached.
  bool connect(const char* url = "rtmp://127.0.0.1/live/test");
  /// Pushes frames in order (pump after each), returns how many were accepted.
  size_t push(const std::vector<MediaFrame>& frames);
  /// Simulates a new TCP connection for a reconnect.
  void newConnection();

  size_t deliverChunk;
  std::vector<uint8_t> toServer;
  std::vector<nitrortmp::PublisherState> states;
  std::vector<std::pair<nitrortmp::PublisherError, std::string>> errors;
  std::unique_ptr<LoopbackServer> server;
  int serverInputResult = 0;
  nitrortmp::RtmpPublisher pub;
};

struct ScriptedServer {
  /// S0+S1+S2 for the given C0+C1 (simple handshake, no digest).
  static std::vector<uint8_t> handshakeResponse(const std::vector<uint8_t>& c0c1);
  /// One RTMP message as type-0 chunk + type-3 continuations.
  static std::vector<uint8_t> chunkMessage(uint32_t cid, uint8_t typeId, uint32_t streamId,
                                           const std::vector<uint8_t>& payload, uint32_t chunkSize = 128);
  static std::vector<uint8_t> connectResult();
  static std::vector<uint8_t> connectError(const char* code, const char* description);
  static std::vector<uint8_t> createStreamResult(uint32_t streamId = 1);
  static std::vector<uint8_t> onStatus(const char* level, const char* code, const char* description, uint32_t streamId = 1);
  static std::vector<uint8_t> streamEof(uint32_t streamId = 1);
  static std::vector<uint8_t> setChunkSize(uint32_t size);
};

/// A publisher whose onSend output is captured, for ScriptedServer tests.
struct CapturedPublisher {
  CapturedPublisher();
  std::vector<uint8_t> takeOutput();
  /// Feeds the handshake response and then walks the connect/createStream/publish
  /// exchange with canned replies. Returns true when Publishing was reached.
  bool driveToPublishing();

  std::vector<uint8_t> output;
  std::vector<nitrortmp::PublisherState> states;
  std::vector<std::pair<nitrortmp::PublisherError, std::string>> errors;
  bool stopFromOnSend = false;  // when true, calls stop() from inside the first onSend
  nitrortmp::RtmpPublisher pub;
};

}  // namespace test
