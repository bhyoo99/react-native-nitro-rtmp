import type { VideoLayer } from './VideoLayer.nitro';

export type CameraPosition = 'front' | 'back';

/**
 * The device camera as a mixer layer. Frames go
 * to the mixer only; the encoder never sees the camera directly.
 */
export interface CameraSource extends VideoLayer {
  /** Changing it while running switches the camera; frames keep their clock. */
  position: CameraPosition;
  readonly isRunning: boolean;
  /**
   * Requests (iOS) or checks (Android) the camera permission, then starts the
   * capture. Call it for a preview too; the encoder starts with the session.
   * Rejects with a `CaptureError` ("<code>: <message>").
   */
  start(): Promise<void>;
  stop(): Promise<void>;
}
