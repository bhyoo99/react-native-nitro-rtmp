import {
  NitroModules,
  getHostComponent,
  type HybridViewMethods,
} from 'react-native-nitro-modules';
import type { CameraLayer } from './specs/CameraLayer.nitro';
import type { ImageLayer } from './specs/ImageLayer.nitro';
import type { MicrophoneSource } from './specs/MicrophoneSource.nitro';
import type { Mixer } from './specs/Mixer.nitro';
import type { RtmpPreviewViewProps } from './specs/RtmpPreviewView.nitro';
import type { RtmpPublisher } from './specs/RtmpPublisher.nitro';
import { toCaptureError, toPublisherError, wrapRejections } from './errors';
import { PREVIEW_VIEW_CONFIG } from './previewViewConfig';

/**
 * Creates a new RTMP publish session. One object holds one socket and one
 * session queue; call `start()` to connect and `stop()` to tear it down.
 * The same object can be started again after `stop()` or a failure.
 * `start()` rejects with a `RtmpPublisherError` (code + message).
 */
export function createPublisher(): RtmpPublisher {
  const publisher =
    NitroModules.createHybridObject<RtmpPublisher>('RtmpPublisher');
  return wrapRejections(publisher, ['start'], toPublisherError);
}

/**
 * Creates the scene + encoder owner. Attach layers and a
 * microphone, then `publisher.setMixer(mixer)` before `publisher.start(url)`.
 */
export function createMixer(): Mixer {
  return NitroModules.createHybridObject<Mixer>('Mixer');
}

/**
 * A camera layer fed by VisionCamera: add it to a mixer and pass
 * `layer.output` to VisionCamera's `outputs`.
 */
export function createCameraLayer(): CameraLayer {
  return NitroModules.createHybridObject<CameraLayer>('CameraLayer');
}

/** A microphone. `start()` rejects with a `RtmpCaptureError`. */
export function createMicrophoneSource(): MicrophoneSource {
  const mic =
    NitroModules.createHybridObject<MicrophoneSource>('MicrophoneSource');
  return wrapRejections(mic, ['start', 'stop'], toCaptureError);
}

/** A static image layer. `load()` rejects with a `RtmpCaptureError`. */
export function createImageLayer(): ImageLayer {
  const layer = NitroModules.createHybridObject<ImageLayer>('ImageLayer');
  return wrapRejections(layer, ['load'], toCaptureError);
}

/**
 * Draws the mixer's composited scene: what is being sent, with a front
 * camera mirrored. Pass the mixer as the `mixer` prop. (The native view is
 * named `RtmpPreviewView`; VisionCamera registers its own `PreviewView`.)
 */
export const PreviewView = getHostComponent<
  RtmpPreviewViewProps,
  HybridViewMethods
>('RtmpPreviewView', () => PREVIEW_VIEW_CONFIG);
