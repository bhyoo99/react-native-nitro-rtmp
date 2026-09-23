import type { HybridView, HybridViewProps } from 'react-native-nitro-modules';
import type { Mixer } from './Mixer.nitro';

export type PreviewResizeMode = 'cover' | 'contain';

/**
 * Draws what the mixer sends: the composited scene,
 * mirrored only for a front camera. Several previews can share one mixer.
 * (Named `RtmpPreviewView` because VisionCamera registers its own `PreviewView`.)
 */
export interface RtmpPreviewViewProps extends HybridViewProps {
  mixer?: Mixer;
  /** How the output frame fits the view. Default `cover`. */
  resizeMode?: PreviewResizeMode;
}

export type RtmpPreviewView = HybridView<RtmpPreviewViewProps>;
