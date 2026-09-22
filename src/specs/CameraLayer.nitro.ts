import type { CameraOutput } from 'react-native-vision-camera';
import type { VideoLayer } from './VideoLayer.nitro';

/**
 * The camera as a mixer layer, fed by VisionCamera. Add the layer to a
 * mixer and hand `output` to VisionCamera (`<Camera outputs={[output]}>` or
 * `useCamera({ outputs })`): frames then go from the camera session to the
 * mixer only. The encoder never sees the camera, and nothing crosses into
 * JavaScript.
 *
 * VisionCamera owns the camera: device, format, frame rate (`constraints`)
 * and permission. The output rotates its frames to `outputOrientation`
 * (set by VisionCamera from the device orientation) and reports a front
 * camera so the preview can mirror it; the encoded stream is never mirrored.
 */
export interface CameraLayer extends VideoLayer {
  /**
   * The VisionCamera output feeding this layer. Stable for the layer's
   * lifetime; one output belongs to one camera session at a time.
   */
  readonly output: CameraOutput;
  /** True while a front camera feeds the layer. */
  readonly isFrontCamera: boolean;
  /** True while the layer is attached to a running camera session. */
  readonly isReceivingFrames: boolean;
}
