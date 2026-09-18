import type { ViewConfig } from 'react-native-nitro-modules';
import type { PreviewViewProps } from './specs/PreviewView.nitro';

/**
 * The Fabric view config of `PreviewView`. It mirrors
 * `nitrogen/generated/shared/json/PreviewViewConfig.json`; it is inlined so
 * the built package (lib/) does not depend on the generated JSON's path.
 * `src/__tests__/previewViewConfig.test.ts` checks the two stay identical.
 */
export const PREVIEW_VIEW_CONFIG: ViewConfig<PreviewViewProps> = {
  uiViewClassName: 'PreviewView',
  supportsRawText: false,
  bubblingEventTypes: {},
  directEventTypes: {},
  validAttributes: {
    mixer: true,
    resizeMode: true,
  },
};
