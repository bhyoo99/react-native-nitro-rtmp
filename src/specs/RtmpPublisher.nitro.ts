import type { HybridObject } from 'react-native-nitro-modules';
import type { Mixer } from './Mixer.nitro';

/**
 * Session state. The transport and the core share one state machine:
 * `connecting` covers DNS/TCP/TLS and the RTMP handshake, `connected` the
 * connect/createStream/publish exchange, `publishing` accepts frames.
 */
export type PublisherState =
  'idle' | 'connecting' | 'connected' | 'publishing' | 'stopped' | 'failed';

/**
 * One code set for core (protocol) and transport (socket) failures.
 * The names of the core codes are checked against the C facade by the host
 * test `cpp/__tests__/CApi.test.cpp`.
 */
export type PublisherErrorCode =
  // core (nitrortmp::PublisherError)
  | 'invalidUrl'
  | 'handshakeFailed'
  | 'connectRejected'
  | 'invalidApp'
  | 'publishBadName'
  | 'streamAlreadyExists'
  | 'serverClosed'
  | 'protocolError'
  | 'notReady'
  // transport
  | 'connectFailed' // DNS or TCP failure
  | 'tlsFailed' // TLS handshake or certificate validation failure
  | 'socketClosed' // the peer closed the socket while publishing (EOF/RST)
  | 'timeout'; // connect or publish timeout

export interface PublisherError {
  code: PublisherErrorCode;
  message: string;
}

export interface PublisherStats {
  bytesSent: number;
  videoTags: number;
  audioTags: number;
  rejectedFrames: number;
  droppedBeforeKeyframe: number;
  invalidFrames: number;
  timestampClamps: number;
  /** Bytes handed to the transport that the socket has not written yet. */
  queuedBytes: number;
  lastVideoTimestamp: number;
  lastAudioTimestamp: number;
}

export interface StreamMetadata {
  width?: number;
  height?: number;
  frameRate?: number;
  videoBitrateKbps?: number;
  audioSampleRate?: number;
  audioChannels?: number;
  audioBitrateKbps?: number;
  encoder?: string;
}

export interface RtmpPublisher extends HybridObject<{
  ios: 'swift';
  android: 'kotlin';
}> {
  /** Last state reported to JS. Read without touching the session queue. */
  readonly state: PublisherState;
  /** Core counters plus `queuedBytes` from the transport. */
  readonly stats: PublisherStats;

  /**
   * Opens the socket (TLS for rtmps://) and drives the RTMP handshake.
   * Resolves when `publishing` is reached, rejects with a `PublisherError`
   * when anything fails before that. Errors after `publishing` go to
   * `onError` only.
   */
  start(url: string): Promise<void>;
  /**
   * Sends FCUnpublish/deleteStream, drains the send queue and closes the
   * socket. Resolves once the socket is closed; also resolves when there is
   * nothing to stop.
   */
  stop(): Promise<void>;

  setMetadata(metadata: StreamMetadata): void;
  /**
   * Attaches a mixer: its encoders start when the
   * session reaches `publishing` and stop when it ends. Encoder output goes
   * to the session queue natively. `undefined` detaches (stopping the
   * encoders first). Encoder-side counters live in `MixerStats`.
   */
  setMixer(mixer?: Mixer): void;
  onStateChange(callback: (state: PublisherState) => void): void;
  onError(callback: (error: PublisherError) => void): void;

  /**
   * External source: one H.264 Annex-B access unit. The bytes are copied and
   * processed on the session queue; rejections show up in `stats`.
   */
  pushVideo(frame: ArrayBuffer, ptsMs: number, dtsMs: number): void;
  /** External source: one ADTS AAC frame (protection_absent = 1). */
  pushAudio(frame: ArrayBuffer, ptsMs: number): void;
}
