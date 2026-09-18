import type { HybridObject } from 'react-native-nitro-modules';

/**
 * Microphone PCM capture. Attach it to a mixer with
 * `mixer.setAudioSource(mic)`; the mixer owns the AAC encoder.
 */
export interface MicrophoneSource extends HybridObject<{
  ios: 'swift';
  android: 'kotlin';
}> {
  /** When true the capture keeps running and silent PCM is encoded, so the timeline never stops. */
  muted: boolean;
  readonly isRunning: boolean;
  /** Requests (iOS) or checks (Android) the microphone permission, then starts the capture. */
  start(): Promise<void>;
  stop(): Promise<void>;
}
