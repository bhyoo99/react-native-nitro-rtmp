#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "RtmpUrl.hpp"
#include "StreamMetadata.hpp"

namespace nitrortmp {

enum class PublisherState {
  Idle,        // nothing started, or start() failed before any byte went out
  Connecting,  // C0+C1 sent, waiting for the handshake and the connect result
  Connected,   // connect accepted, createStream/publish in flight
  Publishing,  // NetStream.Publish.Start received; pushVideo/pushAudio allowed
  Stopped,     // stop() called, or the server ended the stream
  Failed,      // a protocol error or a rejection; onError was called first
};

enum class PublisherError {
  InvalidUrl,           // start(string) could not parse the URL
  HandshakeFailed,      // bad S0 version, or the vendor state machine failed before connect
  ConnectRejected,      // the connect command was answered with an error
  InvalidApp,           // connect was answered with NetConnection.Connect.InvalidApp (onStatus code)
  PublishBadName,       // publish was answered with an error (BadName, already publishing, auth)
  StreamAlreadyExists,  // publish was answered with NetStream.Publish.AlreadyExistStream (onStatus code)
  ServerClosed,         // the server stopped the stream (StreamEOF, NetStream.*.Stop, error while publishing)
  ProtocolError,        // malformed input, out of memory, or an internal vendor failure
  NotReady,             // start() while already active, or a method called from inside a callback
};

const char* toString(PublisherState state);
const char* toString(PublisherError error);

struct PublisherStats {
  uint64_t bytesSent = 0;              // bytes handed to onSend, handshake included
  uint64_t videoTags = 0;              // FLV video tags sent, sequence headers included
  uint64_t audioTags = 0;              // FLV audio tags sent, sequence header included
  uint64_t scriptTags = 0;             // onMetaData tags sent
  uint64_t rejectedFrames = 0;         // pushVideo/pushAudio before Publishing
  uint64_t droppedBeforeKeyframe = 0;  // video pushed before the first IDR with SPS/PPS
  uint64_t invalidFrames = 0;          // payload the muxer could not parse
  uint64_t timestampClamps = 0;        // dts went backwards or pts < dts and was clamped
  uint32_t lastVideoTimestamp = 0;     // normalized dts of the last video tag, ms
  uint32_t lastAudioTimestamp = 0;     // normalized dts of the last audio tag, ms
};

struct PublisherCallbacks {
  /// Bytes to write to the socket, in order. Called synchronously from
  /// start/onReceive/push/stop. Copy the bytes if you keep them.
  std::function<void(const uint8_t* data, size_t size)> onSend;
  std::function<void(PublisherState state)> onStateChange;
  /// Always called before the matching onStateChange(Failed/Stopped).
  std::function<void(PublisherError error, const std::string& message)> onError;
};

/// Sans-IO RTMP publish client. It never touches sockets, threads or timers:
/// feed it socket bytes with onReceive(), write what onSend() hands you, and
/// push encoder output. All methods must be called from one thread (or one
/// serialized queue). Do not call methods from inside the callbacks; stop()
/// from a callback is deferred, push/start from a callback are rejected.
class RtmpPublisher {
 public:
  explicit RtmpPublisher(PublisherCallbacks callbacks);
  ~RtmpPublisher();

  RtmpPublisher(const RtmpPublisher&) = delete;
  RtmpPublisher& operator=(const RtmpPublisher&) = delete;

  /// Emits C0+C1 through onSend and moves to Connecting. The socket must
  /// already be connected (or the transport must buffer). Allowed from Idle,
  /// Stopped and Failed; a reconnect gets fresh sequence headers.
  bool start(const RtmpUrl& url);
  /// Parses the URL first; reports InvalidUrl on failure.
  bool start(std::string_view url);

  /// Bytes read from the socket. Ignored unless Connecting/Connected/Publishing.
  void onReceive(const uint8_t* data, size_t size);

  /// Sends FCUnpublish + deleteStream when a stream exists and moves to
  /// Stopped. The transport closes the socket afterwards.
  void stop();

  /// Enhanced RTMP (FourCC) tag headers. No effect for H.264, which always
  /// uses the legacy header; reserved for HEVC/AV1 input. Default false.
  void setEnhancedRtmp(bool enabled);

  /// onMetaData sent when Publishing is entered (before onStateChange), or
  /// immediately when already Publishing.
  void setMetadata(const StreamMetadata& metadata);

  /// H.264 Annex-B access unit (start codes + NALUs), one frame per call.
  /// The first accepted frame must be an IDR that carries SPS and PPS.
  /// Timestamps are milliseconds; the first accepted push defines t=0.
  /// @return false when rejected (not Publishing, waiting for a keyframe, or
  ///         unparsable); see stats() for the reason.
  bool pushVideo(const uint8_t* annexb, size_t size, uint32_t ptsMs, uint32_t dtsMs);

  /// One AAC frame with a 7-byte ADTS header (protection_absent = 1).
  bool pushAudio(const uint8_t* adts, size_t size, uint32_t ptsMs);

  PublisherState state() const;
  PublisherStats stats() const;
  const RtmpUrl& url() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace nitrortmp
