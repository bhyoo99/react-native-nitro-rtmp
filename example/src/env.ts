import { Platform } from 'react-native';

// Scripted verification (values are inlined when Metro bundles):
// - EXPO_PUBLIC_SOURCE=camera|fixture: which screen opens (default camera)
// - EXPO_PUBLIC_RTMP_URL: initial URL (HOST is replaced by the platform's loopback address)
// - EXPO_PUBLIC_AUTOSTART=1: press Start automatically once ready
// - EXPO_PUBLIC_AUTOSTART_URLS="a;b;c": run these URLs one after another
// - EXPO_PUBLIC_DURATION_MS: camera mode, time until the automatic stop (default 10000)
// - EXPO_PUBLIC_WATERMARK=1: camera mode, watermark on from the start
// - EXPO_PUBLIC_ACTIONS="flip@3000;mute@4000;unmute@6000;watermark@7000;nowatermark@9000":
//   camera mode, actions performed at the given ms after publishing started
// - EXPO_PUBLIC_LOOP=1: fixture mode, loop toggle on

// The iOS simulator shares the Mac's loopback; the Android emulator reaches it at 10.0.2.2.
export const HOST = Platform.OS === 'android' ? '10.0.2.2' : '127.0.0.1';
export const DEFAULT_URL = `rtmp://${HOST}/live/test`;

export const withHost = (value: string) => value.replaceAll('HOST', HOST);

export type SourceMode = 'camera' | 'fixture';
export const INITIAL_SOURCE: SourceMode =
  process.env.EXPO_PUBLIC_SOURCE === 'fixture' ? 'fixture' : 'camera';
export const INITIAL_URL = withHost(
  process.env.EXPO_PUBLIC_RTMP_URL ?? DEFAULT_URL
);
export const INITIAL_LOOP = process.env.EXPO_PUBLIC_LOOP === '1';
export const INITIAL_WATERMARK = process.env.EXPO_PUBLIC_WATERMARK === '1';
export const DURATION_MS = Number(process.env.EXPO_PUBLIC_DURATION_MS ?? 10000);

const SCENARIO_SOURCE: string =
  process.env.EXPO_PUBLIC_AUTOSTART_URLS ??
  (process.env.EXPO_PUBLIC_AUTOSTART === '1' ? INITIAL_URL : '');
export const AUTOSTART_URLS: readonly string[] = SCENARIO_SOURCE.split(';')
  .map((value: string) => withHost(value.trim()))
  .filter((value: string) => value !== '');

export type ScriptedAction =
  'flip' | 'mute' | 'unmute' | 'watermark' | 'nowatermark';
const ACTION_NAMES: readonly ScriptedAction[] = [
  'flip',
  'mute',
  'unmute',
  'watermark',
  'nowatermark',
];
export const ACTIONS: readonly { action: ScriptedAction; atMs: number }[] = (
  process.env.EXPO_PUBLIC_ACTIONS ?? ''
)
  .split(';')
  .map((entry: string) => entry.trim())
  .filter((entry: string) => entry !== '')
  .flatMap((entry: string) => {
    const [name, at] = entry.split('@');
    const action = ACTION_NAMES.find((candidate) => candidate === name);
    const atMs = Number(at);
    return action !== undefined && Number.isFinite(atMs)
      ? [{ action, atMs }]
      : [];
  });

export const delay = (ms: number) =>
  new Promise<void>((resolve) => setTimeout(resolve, ms));

export function timestamp(): string {
  return new Date().toISOString().slice(11, 23);
}
