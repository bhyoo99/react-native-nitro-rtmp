import type { ComponentProps } from 'react';
import { PreviewView } from './native';
import type { RtmpStream } from './streamTypes';

export type RtmpPreviewProps = Omit<
  ComponentProps<typeof PreviewView>,
  'mixer'
> & {
  stream: Pick<RtmpStream, 'mixer'>;
};

/** Renders the stream's composited preview, including any advanced mixer layers. */
export function RtmpPreview({ stream, ...props }: RtmpPreviewProps) {
  return <PreviewView {...props} mixer={stream.mixer} />;
}
