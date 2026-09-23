import type { ViewConfig } from 'react-native-nitro-modules';
import type { RtmpPreviewViewProps } from './specs/RtmpPreviewView.nitro';

/**
 * The Fabric view config of `RtmpPreviewView`. It mirrors
 * `nitrogen/generated/shared/json/RtmpPreviewViewConfig.json`; it is inlined
 * so the built package (lib/) does not depend on the generated JSON's path.
 * `src/__tests__/previewViewConfig.test.ts` checks the two stay identical.
 */
export const PREVIEW_VIEW_CONFIG: ViewConfig<RtmpPreviewViewProps> = {
  uiViewClassName: 'RtmpPreviewView',
  supportsRawText: false,
  bubblingEventTypes: {},
  directEventTypes: {},
  validAttributes: {
    mixer: true,
    resizeMode: true,
  },
};
