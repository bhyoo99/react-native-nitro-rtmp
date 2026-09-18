// C facade over nitrortmp::RtmpPublisher.
//
// Swift (through the pod's module map) and the Android JNI bridge call the
// same functions, so the two transports stay symmetric. The facade maps one
// C function to one core method and holds no logic of its own; the state and
// error name tables below are the only data it adds, and CApi.test.cpp checks
// them against the JS unions in src/specs/RtmpPublisher.nitro.ts.
//
// Threading: every function taking a nitrortmp_publisher_t must be called from
// the session's serial queue. Callbacks run synchronously inside those calls;
// do not call back into the publisher from a callback.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nitrortmp_publisher nitrortmp_publisher_t;

/// Session states, in the order of the JS `PublisherState` union
/// (`nitrortmp_state_name` maps them to the JS strings).
enum {
  NITRORTMP_STATE_IDLE = 0,
  NITRORTMP_STATE_CONNECTING = 1,
  NITRORTMP_STATE_CONNECTED = 2,
  NITRORTMP_STATE_PUBLISHING = 3,
  NITRORTMP_STATE_STOPPED = 4,
  NITRORTMP_STATE_FAILED = 5,
  NITRORTMP_STATE_COUNT = 6
};

/// Error codes, in the order of the JS `PublisherErrorCode` union. 0..8 are
/// the core's nitrortmp::PublisherError values and come through `on_error`;
/// 9..12 belong to the transports, which report them the same way.
enum {
  NITRORTMP_ERROR_INVALID_URL = 0,
  NITRORTMP_ERROR_HANDSHAKE_FAILED = 1,
  NITRORTMP_ERROR_CONNECT_REJECTED = 2,
  NITRORTMP_ERROR_INVALID_APP = 3,
  NITRORTMP_ERROR_PUBLISH_BAD_NAME = 4,
  NITRORTMP_ERROR_STREAM_ALREADY_EXISTS = 5,
  NITRORTMP_ERROR_SERVER_CLOSED = 6,
  NITRORTMP_ERROR_PROTOCOL_ERROR = 7,
  NITRORTMP_ERROR_NOT_READY = 8,
  NITRORTMP_ERROR_CONNECT_FAILED = 9,
  NITRORTMP_ERROR_TLS_FAILED = 10,
  NITRORTMP_ERROR_SOCKET_CLOSED = 11,
  NITRORTMP_ERROR_TIMEOUT = 12,
  NITRORTMP_ERROR_COUNT = 13
};

typedef struct {
  /// Bytes to write to the socket, in order. `data` is only valid during the call.
  void (*on_send)(void* ctx, const uint8_t* data, size_t size);
  /// A NITRORTMP_STATE_* value.
  void (*on_state)(void* ctx, int state);
  /// A NITRORTMP_ERROR_* value (0..8 here). Always precedes the matching
  /// on_state(FAILED/STOPPED). `message` is only valid during the call.
  void (*on_error)(void* ctx, int code, const char* message);
} nitrortmp_callbacks_t;

/// What a transport needs to open the socket. The URL itself goes to
/// nitrortmp_publisher_start unchanged.
typedef struct {
  char host[256];  // NUL terminated
  uint16_t port;   // explicit port, else 1935 for rtmp / 443 for rtmps
  int use_tls;     // 1 for rtmps://
} nitrortmp_endpoint_t;

/// Mirrors nitrortmp::StreamMetadata. Zero means "unknown, leave it out";
/// a NULL encoder keeps the core's default.
typedef struct {
  int width;
  int height;
  double frame_rate;
  double video_bitrate_kbps;
  int audio_sample_rate;
  int audio_channels;
  double audio_bitrate_kbps;
  const char* encoder;
} nitrortmp_metadata_t;

/// Mirrors nitrortmp::PublisherStats.
typedef struct {
  uint64_t bytes_sent;
  uint64_t video_tags;
  uint64_t audio_tags;
  uint64_t script_tags;
  uint64_t rejected_frames;
  uint64_t dropped_before_keyframe;
  uint64_t invalid_frames;
  uint64_t timestamp_clamps;
  uint32_t last_video_timestamp;
  uint32_t last_audio_timestamp;
} nitrortmp_stats_t;

/// @return 1 and fills `out` for a valid rtmp:// or rtmps:// publish URL, else 0.
int nitrortmp_url_endpoint(const char* url, nitrortmp_endpoint_t* out);

/// `cb` is copied; `ctx` is passed back to every callback.
nitrortmp_publisher_t* nitrortmp_publisher_create(const nitrortmp_callbacks_t* cb, void* ctx);
void nitrortmp_publisher_destroy(nitrortmp_publisher_t* publisher);

/// Emits C0+C1 through on_send and enters CONNECTING. The socket must be
/// connected. @return 1 on success, 0 when on_error was called instead.
int nitrortmp_publisher_start(nitrortmp_publisher_t* publisher, const char* url);
/// Bytes read from the socket.
void nitrortmp_publisher_on_receive(nitrortmp_publisher_t* publisher, const uint8_t* data, size_t size);
/// Sends FCUnpublish + deleteStream (through on_send) and enters STOPPED.
void nitrortmp_publisher_stop(nitrortmp_publisher_t* publisher);

/// One H.264 Annex-B access unit. @return 1 when accepted, 0 when rejected (see stats).
int nitrortmp_publisher_push_video(nitrortmp_publisher_t* publisher, const uint8_t* annexb, size_t size,
                                   uint32_t pts_ms, uint32_t dts_ms);
/// One ADTS AAC frame (protection_absent = 1). @return 1 when accepted, 0 when rejected.
int nitrortmp_publisher_push_audio(nitrortmp_publisher_t* publisher, const uint8_t* adts, size_t size, uint32_t pts_ms);
void nitrortmp_publisher_set_metadata(nitrortmp_publisher_t* publisher, const nitrortmp_metadata_t* metadata);

int nitrortmp_publisher_state(const nitrortmp_publisher_t* publisher);
void nitrortmp_publisher_stats(const nitrortmp_publisher_t* publisher, nitrortmp_stats_t* out);

/// JS union strings ("publishing", "publishBadName"); NULL when out of range.
const char* nitrortmp_state_name(int state);
const char* nitrortmp_error_name(int code);

// --- encoder output helpers ------------------------------
//
// The platform encoders normalize their output with these before handing it
// to the session: VideoToolbox gives AVCC and needs Annex-B, both give raw
// AAC and need ADTS. The `out`/`out_capacity` functions return the number of
// bytes the result needs and write it only when `out` is non-NULL and
// `out_capacity` is large enough; call them with out = NULL to size a buffer.

/// AVCC (length-prefixed NALUs, `length_size` 1..4) to Annex-B (4-byte start codes).
size_t nitrortmp_avcc_to_annexb(const uint8_t* avcc, size_t size, int length_size, uint8_t* out,
                                size_t out_capacity);
/// start code + sps + start code + pps + annexb. Empty parameter sets are skipped.
size_t nitrortmp_prepend_parameter_sets(const uint8_t* sps, size_t sps_size, const uint8_t* pps, size_t pps_size,
                                        const uint8_t* annexb, size_t size, uint8_t* out, size_t out_capacity);
/// 1 when the Annex-B buffer contains an IDR slice (what the core's keyframe gate accepts).
int nitrortmp_annexb_is_keyframe(const uint8_t* annexb, size_t size);
/// 1 when an SPS NALU comes before the first slice.
int nitrortmp_annexb_has_parameter_sets(const uint8_t* annexb, size_t size);
/// 7-byte ADTS header (protection_absent = 1) for one raw AAC frame of
/// `payload_size` bytes. `profile` is the audio object type minus one (AAC-LC
/// = 1), `channels` 1..7. @return 1 on success, 0 when a value is out of range.
int nitrortmp_write_adts_header(uint8_t* out, int profile, int sample_rate, int channels, size_t payload_size);

#ifdef __cplusplus
}  // extern "C"
#endif
