import {
  NitroModules,
  getHostComponent,
  type HybridViewMethods,
} from 'react-native-nitro-modules';
import type { CameraSource } from './specs/CameraSource.nitro';
import type { ImageLayer } from './specs/ImageLayer.nitro';
import type { MicrophoneSource } from './specs/MicrophoneSource.nitro';
import type { Mixer } from './specs/Mixer.nitro';
import type { PreviewViewProps } from './specs/PreviewView.nitro';
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

/** A camera layer. `start()` rejects with a `RtmpCaptureError`. */
export function createCameraSource(): CameraSource {
  const camera = NitroModules.createHybridObject<CameraSource>('CameraSource');
  return wrapRejections(camera, ['start', 'stop'], toCaptureError);
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
 * Draws the mixer's composited scene: what is being sent, with
 * the front camera mirrored. Pass the mixer as the `mixer` prop.
 */
export const PreviewView = getHostComponent<
  PreviewViewProps,
  HybridViewMethods
>('PreviewView', () => PREVIEW_VIEW_CONFIG);
