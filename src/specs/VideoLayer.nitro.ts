import type { HybridObject } from 'react-native-nitro-modules';

/** What a layer draws: the camera or a static image. */
export type LayerKind = 'camera' | 'image';

/**
 * Something the mixer can draw. `CameraLayer`
 * and `ImageLayer` implement it today; text, screen and bitmap layers come
 * later and implement the same interface. A layer belongs to at most one
 * mixer at a time.
 */
export interface VideoLayer extends HybridObject<{
  ios: 'swift';
  android: 'kotlin';
}> {
  readonly kind: LayerKind;
}
