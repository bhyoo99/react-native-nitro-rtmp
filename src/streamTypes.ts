import type { CameraOutput } from 'react-native-vision-camera';
import type { RtmpCaptureError, RtmpPublisherError } from './errors';
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
  /** Creates the mixer, camera output and microphone; false releases them and stops publishing. Default true. */
  active?: boolean;
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
  readonly muted: boolean;
  readonly stats: RtmpStreamStats | null;
  /**
   * The VisionCamera output that feeds the stream: pass it to
   * `<Camera outputs={[stream.cameraOutput]}>` (or `useCamera`). `undefined`
   * while inactive; replaced after reactivation or configuration changes.
   */
  readonly cameraOutput: CameraOutput | undefined;
  /** Advanced composition only. Replaced after reactivation or capture configuration changes. */
  readonly mixer: Mixer | undefined;
  /** Waits for readiness; resolves when publishing. Rejects if inactive or cancelled. */
  start(url: string): Promise<void>;
  /** Cancels a pending start or stops publishing, keeping the camera output and preview alive. */
  stop(): Promise<void>;
  setMuted(muted: boolean): void;
}
