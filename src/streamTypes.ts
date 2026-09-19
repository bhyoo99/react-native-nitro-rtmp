import type { RtmpCaptureError, RtmpPublisherError } from './errors';
import type { CameraPosition } from './specs/CameraSource.nitro';
import type {
  AudioSettings,
  Mixer,
  MixerStats,
  VideoSettings,
} from './specs/Mixer.nitro';
import type {
  PublisherState,
  PublisherStats,
} from './specs/RtmpPublisher.nitro';

export type RtmpStreamError = RtmpCaptureError | RtmpPublisherError;

export interface RtmpStreamOptions {
  /** Starts preview/capture, not publishing. False stops capture and publishing. Default true. */
  active?: boolean;
  /** Initial camera; subsequent prop changes switch it without restarting the stream. Default back. */
  camera?: CameraPosition;
  /** Enable microphone capture. Default true. */
  audio?: boolean;
  /** Defaults to 720 × 1280, 30 fps, 2500 kbps, a keyframe every 2 seconds. */
  video?: Partial<VideoSettings>;
  /** Defaults to 48000 Hz, mono, 128 kbps. */
  audioSettings?: Partial<AudioSettings>;
  /** Opt-in stats polling while ready. Zero disables polling (default). */
  statsIntervalMs?: number;
}

export interface RtmpStreamStats {
  publisher: PublisherStats;
  mixer: MixerStats;
}

export interface RtmpStream {
  readonly state: PublisherState;
  readonly ready: boolean;
  readonly isBusy: boolean;
  readonly error: RtmpStreamError | null;
  readonly camera: CameraPosition;
  readonly muted: boolean;
  readonly stats: RtmpStreamStats | null;
  /** Advanced composition only. Replaced after reactivation or capture configuration changes. */
  readonly mixer: Mixer | undefined;
  /** Waits for capture readiness; resolves when publishing. Rejects if inactive or cancelled. */
  start(url: string): Promise<void>;
  /** Cancels a pending start or stops publishing, keeping the preview running. */
  stop(): Promise<void>;
  setCameraPosition(position: CameraPosition): void;
  flipCamera(): void;
  setMuted(muted: boolean): void;
}
