import type { VideoLayer } from './VideoLayer.nitro';

/**
 * A static PNG/JPEG drawn into a rectangle of the output:
 * the proof that overlays are just another layer.
 */
export interface ImageLayer extends VideoLayer {
  /** Loads a `file://` URI (PNG or JPEG). Rejects with a `CaptureError` when the file cannot be decoded. */
  load(fileUri: string): Promise<void>;
  /** True after a successful `load`. */
  readonly isLoaded: boolean;
}
