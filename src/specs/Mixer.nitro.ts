import type { HybridObject } from 'react-native-nitro-modules';
import type { MicrophoneSource } from './MicrophoneSource.nitro';
import type { VideoLayer } from './VideoLayer.nitro';

export interface VideoSettings {
  width: number;
  height: number;
  frameRate: number;
  bitrateKbps: number;
  /** Seconds between keyframes. Default 2. */
  keyframeIntervalSeconds?: number;
}

export interface AudioSettings {
  /** 48000 by default. */
  sampleRate: number;
  /** 1 or 2. */
  channels: number;
  bitrateKbps: number;
}

/** A rectangle in output-frame coordinates, normalized to 0..1. Omitted: the whole frame. */
export interface LayerFrame {
  x: number;
  y: number;
  width: number;
  height: number;
}

export interface MixerStats {
  /** Frames the camera delivered. */
  capturedFrames: number;
  /** Frames the compositor drew. */
  renderedFrames: number;
  /** Camera frames discarded because the compositor or the encoder was busy. */
  droppedFrames: number;
  encodedVideoFrames: number;
  encodedAudioFrames: number;
  /** Encoder output over the last second. */
  videoBitrateKbps: number;
  encoderFailures: number;
}

export type CaptureErrorCode =
  | 'permissionDenied'
  | 'cameraUnavailable'
  | 'configurationFailed'
  | 'encoderFailed';

/** Capture side failures. They are never mixed into `PublisherError`. */
export interface CaptureError {
  code: CaptureErrorCode;
  message: string;
}

/**
 * The scene (layers, z order, rectangles), the output format and the encoders
 *. The GPU compositor sits between the layers
 * and the H.264 encoder; `PreviewView` draws the same scene.
 *
 * Attach it to a session with `publisher.setMixer(mixer)`: the encoders start
 * when the session reaches `publishing` and stop with it.
 */
export interface Mixer extends HybridObject<{
  ios: 'swift';
  android: 'kotlin';
}> {
  /** Output size, frame rate and bitrate. Can only be changed while not encoding. */
  video: VideoSettings;
  audio: AudioSettings;
  readonly stats: MixerStats;
  readonly isEncoding: boolean;

  /** Z order is the order of addition. Adding a layer twice is ignored. */
  addLayer(layer: VideoLayer, frame?: LayerFrame): void;
  removeLayer(layer: VideoLayer): void;
  setLayerFrame(layer: VideoLayer, frame: LayerFrame): void;
  /** `undefined` detaches the microphone. */
  setAudioSource(source?: MicrophoneSource): void;
  onError(callback: (error: CaptureError) => void): void;
}
