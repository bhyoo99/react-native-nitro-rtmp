import { describe, expect, it } from '@jest/globals';
import * as fs from 'node:fs';
import * as path from 'node:path';
import { PREVIEW_VIEW_CONFIG } from '../previewViewConfig';

const GENERATED = path.join(
  __dirname,
  '../../nitrogen/generated/shared/json/RtmpPreviewViewConfig.json'
);

describe('PREVIEW_VIEW_CONFIG', () => {
  it('matches the nitrogen output when it exists', () => {
    if (!fs.existsSync(GENERATED)) {
      // `yarn nitrogen` has not run; the inline copy is what ships either way.
      return;
    }
    const generated = JSON.parse(fs.readFileSync(GENERATED, 'utf8'));
    // hybridRef is added by getHostComponent's default props; it is not a spec prop.
    const { hybridRef, ...validAttributes } = generated.validAttributes;
    expect(hybridRef).toBe(true);
    expect({ ...generated, validAttributes }).toEqual(PREVIEW_VIEW_CONFIG);
  });
});
