/* eslint-disable no-bitwise */
// The C++ test fixture (cpp/__tests__/fixtures) as an encoder would hand
// it to the publisher: Annex-B access units and ADTS frames, merged by dts and
// pushed in real time. The splitting rules are those of
// cpp/__tests__/support/Fixtures.cpp so the received FLV can be compared with
// reference.flv tag for tag (scripts/verify-flv.sh).
//
// This code is example-only: the library never splits frames.
import { Asset } from 'expo-asset';
import { File } from 'expo-file-system';
import type { RtmpPublisher } from 'react-native-nitro-rtmp';

import audioAsset from '../../cpp/__tests__/fixtures/audio.aac';
import videoAsset from '../../cpp/__tests__/fixtures/video.h264';

export const VIDEO_FPS = 30;
export const AUDIO_SAMPLE_RATE = 48000;
export const AUDIO_SAMPLES_PER_FRAME = 1024;
/** One pass of the fixture: 2 s of video and audio. */
export const FIXTURE_DURATION_MS = 2000;

export interface MediaFrame {
  video: boolean;
  /** Exactly one access unit (Annex-B, start codes kept) or one ADTS frame. */
  data: ArrayBuffer;
  pts: number;
  dts: number;
}

export async function loadFixtureFrames(): Promise<MediaFrame[]> {
  const [video, audio] = await Promise.all([
    readAsset(videoAsset),
    readAsset(audioAsset),
  ]);
  return mergeFrames(splitAnnexB(video), splitAdts(audio));
}

async function readAsset(module: number): Promise<Uint8Array> {
  const asset = Asset.fromModule(module);
  await asset.downloadAsync();
  const uri = asset.localUri ?? asset.uri;
  return new File(uri).bytes();
}

interface Nalu {
  begin: number; // offset of the start code
  payload: number; // offset of the NALU header byte
  end: number; // one past the last byte
}

function findNalus(s: Uint8Array): Nalu[] {
  const out: Nalu[] = [];
  let i = 0;
  while (i + 3 <= s.length) {
    if (s[i] === 0 && s[i + 1] === 0 && s[i + 2] === 1) {
      let begin = i;
      if (begin > 0 && s[begin - 1] === 0) {
        begin -= 1; // 4-byte start code
      }
      const previous = out[out.length - 1];
      if (previous !== undefined) {
        previous.end = begin;
      }
      out.push({ begin, payload: i + 3, end: s.length });
      i += 3;
    } else {
      i += 1;
    }
  }
  return out;
}

function concat(parts: Uint8Array[]): ArrayBuffer {
  const size = parts.reduce((total, part) => total + part.length, 0);
  const out = new Uint8Array(size);
  let offset = 0;
  for (const part of parts) {
    out.set(part, offset);
    offset += part.length;
  }
  return out.buffer;
}

/**
 * Splits a raw H.264 Annex-B stream into access units: a new unit starts at
 * SPS/PPS/SEI/AUD after a VCL NALU, or at a VCL NALU with
 * first_mb_in_slice == 0 after another VCL NALU.
 */
export function splitAnnexB(stream: Uint8Array): ArrayBuffer[] {
  const units: ArrayBuffer[] = [];
  let current: Uint8Array[] = [];
  let currentHasVcl = false;
  for (const nalu of findNalus(stream)) {
    if (nalu.payload >= nalu.end) {
      continue;
    }
    const type = (stream[nalu.payload] ?? 0) & 0x1f;
    const vcl = type >= 1 && type <= 5;
    let startsNew = false;
    if (currentHasVcl) {
      if (!vcl && (type === 6 || type === 7 || type === 8 || type === 9)) {
        startsNew = true;
      } else if (
        vcl &&
        nalu.payload + 1 < nalu.end &&
        ((stream[nalu.payload + 1] ?? 0) & 0x80) !== 0
      ) {
        startsNew = true; // first_mb_in_slice == 0
      }
    }
    if (startsNew) {
      units.push(concat(current));
      current = [];
      currentHasVcl = false;
    }
    current.push(stream.subarray(nalu.begin, nalu.end));
    if (vcl) {
      currentHasVcl = true;
    }
  }
  if (current.length > 0) {
    units.push(concat(current));
  }
  return units;
}

/** Splits an ADTS stream into frames (7-byte header + payload). */
export function splitAdts(s: Uint8Array): ArrayBuffer[] {
  const frames: ArrayBuffer[] = [];
  let i = 0;
  while (i + 7 <= s.length) {
    const b0 = s[i] ?? 0;
    const b1 = s[i + 1] ?? 0;
    if (b0 !== 0xff || (b1 & 0xf6) !== 0xf0) {
      throw new Error(`ADTS sync lost at offset ${i}`);
    }
    const length =
      (((s[i + 3] ?? 0) & 0x03) << 11) |
      ((s[i + 4] ?? 0) << 3) |
      ((s[i + 5] ?? 0) >> 5);
    if (length < 7 || i + length > s.length) {
      throw new Error(`ADTS frame length out of range at offset ${i}`);
    }
    frames.push(s.slice(i, i + length).buffer);
    i += length;
  }
  return frames;
}

/** FFmpeg's rounding for the fixture clock: round(n * 1000 / fps). */
export function videoTimestampMs(n: number): number {
  return Math.round((n * 1000) / VIDEO_FPS);
}

/** round(n * 1024 * 1000 / sampleRate) */
export function audioTimestampMs(n: number): number {
  return Math.round((n * AUDIO_SAMPLES_PER_FRAME * 1000) / AUDIO_SAMPLE_RATE);
}

/** Video and audio merged by dts, video first on ties. */
export function mergeFrames(
  video: ArrayBuffer[],
  audio: ArrayBuffer[]
): MediaFrame[] {
  const frames: MediaFrame[] = [];
  let v = 0;
  let a = 0;
  while (v < video.length || a < audio.length) {
    const vts =
      v < video.length ? videoTimestampMs(v) : Number.POSITIVE_INFINITY;
    const ats =
      a < audio.length ? audioTimestampMs(a) : Number.POSITIVE_INFINITY;
    if (vts <= ats) {
      const data = video[v];
      if (data === undefined) {
        break;
      }
      frames.push({ video: true, data, pts: vts, dts: vts });
      v += 1;
    } else {
      const data = audio[a];
      if (data === undefined) {
        break;
      }
      frames.push({ video: false, data, pts: ats, dts: ats });
      a += 1;
    }
  }
  return frames;
}

export interface PlaybackOptions {
  /** Read at the end of every pass; when true the fixture repeats, shifted by FIXTURE_DURATION_MS. */
  loop: () => boolean;
  onPass?: (pass: number) => void;
  /** Called after the last frame when not looping. */
  onDone?: () => void;
}

/**
 * Pushes the frames in real time, paced by dts with setTimeout. Frames are
 * only pushed after start() resolved, so the core never rejects them.
 * Returns a function that stops the playback.
 */
export function playFixture(
  publisher: RtmpPublisher,
  frames: MediaFrame[],
  options: PlaybackOptions
): () => void {
  let cancelled = false;
  let timer: ReturnType<typeof setTimeout> | null = null;
  let index = 0;
  let pass = 0;
  let offset = 0;
  const startedAt = Date.now();

  const tick = () => {
    if (cancelled) {
      return;
    }
    const elapsed = Date.now() - startedAt;
    while (!cancelled) {
      const frame = frames[index];
      if (frame === undefined) {
        if (options.loop()) {
          // The first frame is an IDR with SPS/PPS, so the seam needs no new
          // sequence header.
          pass += 1;
          offset += FIXTURE_DURATION_MS;
          index = 0;
          options.onPass?.(pass);
          continue;
        }
        options.onDone?.();
        return;
      }
      const dts = frame.dts + offset;
      if (dts > elapsed) {
        timer = setTimeout(tick, dts - elapsed);
        return;
      }
      if (frame.video) {
        publisher.pushVideo(frame.data, frame.pts + offset, dts);
      } else {
        publisher.pushAudio(frame.data, frame.pts + offset);
      }
      index += 1;
    }
  };
  tick();

  return () => {
    cancelled = true;
    if (timer !== null) {
      clearTimeout(timer);
    }
  };
}
