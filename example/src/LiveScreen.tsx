import { Asset } from 'expo-asset';
import { useCallback, useEffect, useRef, useState } from 'react';
import {
  Button,
  StyleSheet,
  Switch,
  Text,
  TextInput,
  View,
} from 'react-native';
import {
  createImageLayer,
  RtmpPreview,
  useRtmpStream,
  type ImageLayer,
  type Mixer,
  type PublisherState,
} from 'react-native-nitro-rtmp';

import watermarkAsset from '../assets/watermark.png';
import {
  ACTIONS,
  AUTOSTART_URLS,
  DURATION_MS,
  INITIAL_URL,
  INITIAL_WATERMARK,
  delay,
  timestamp,
  type ScriptedAction,
} from './env';

const WATERMARK_FRAME = { x: 0.7, y: 0.05, width: 0.25, height: 0.1 };
const SCENARIO_GAP_MS = 3000;
const ACTIVE_STATES: readonly PublisherState[] = [
  'connecting',
  'connected',
  'publishing',
];

export default function LiveScreen({
  onSwitchToFixture,
}: {
  onSwitchToFixture: () => void;
}) {
  const stream = useRtmpStream({ statsIntervalMs: 500 });
  const {
    state,
    ready,
    isBusy: busy,
    camera: position,
    muted,
    error,
    stats,
    mixer,
    start: startPublishing,
    stop: stopPublishing,
    flipCamera: flip,
    setMuted,
  } = stream;
  const publisherStats = stats?.publisher;
  const mixerStats = stats?.mixer;
  const lastError = error ? `${error.code}: ${error.message}` : '';
  const [url, setUrl] = useState(INITIAL_URL);
  const [watermarkOn, setWatermarkOn] = useState(false);
  const [watermarkLoaded, setWatermarkLoaded] = useState(false);
  const [encodeFps, setEncodeFps] = useState(0);
  const [log, setLog] = useState<string[]>([]);
  const watermarkRef = useRef<{ mixer: Mixer; layer: ImageLayer } | null>(null);
  const lastEncodedRef = useRef({ frames: 0, at: Date.now() });

  const append = useCallback((line: string) => {
    console.log(`[nitrortmp-example] ${line}`);
    setLog((previous) => [...previous.slice(-7), `${timestamp()} ${line}`]);
  }, []);

  useEffect(() => {
    append(`state: ${state}`);
  }, [state, append]);
  useEffect(() => {
    if (error) append(`error ${error.code}: ${error.message}`);
  }, [error, append]);
  useEffect(() => {
    const now = Date.now();
    const previous = lastEncodedRef.current;
    const frames = mixerStats?.encodedVideoFrames ?? 0;
    const seconds = (now - previous.at) / 1000;
    setEncodeFps(
      seconds > 0 && frames >= previous.frames
        ? (frames - previous.frames) / seconds
        : 0
    );
    lastEncodedRef.current = { frames, at: now };
  }, [mixerStats]);

  // Advanced composition still uses the existing layer API on the hook-owned mixer.
  useEffect(() => {
    setWatermarkLoaded(false);
    setWatermarkOn(false);
    if (!mixer) return;
    let cancelled = false;
    const watermark = createImageLayer();
    const load = async () => {
      const asset = Asset.fromModule(watermarkAsset);
      await asset.downloadAsync();
      if (cancelled) return;
      await watermark.load(asset.localUri ?? asset.uri);
      if (cancelled) return;
      watermarkRef.current = { mixer, layer: watermark };
      if (INITIAL_WATERMARK) mixer.addLayer(watermark, WATERMARK_FRAME);
      setWatermarkOn(INITIAL_WATERMARK);
      setWatermarkLoaded(true);
      append('watermark loaded');
    };
    load().catch((failure: unknown) => {
      if (!cancelled) append(`watermark failed: ${String(failure)}`);
    });
    return () => {
      cancelled = true;
      watermarkRef.current = null;
      mixer.removeLayer(watermark);
    };
  }, [mixer, append]);

  const start = useCallback(
    async (target: string) => {
      try {
        await startPublishing(target);
        append('start() resolved');
        return true;
      } catch (failure: unknown) {
        append(`start() rejected: ${String(failure)}`);
        return false;
      }
    },
    [startPublishing, append]
  );

  const stop = useCallback(async () => {
    try {
      await stopPublishing();
      append('stop() resolved');
    } catch (failure: unknown) {
      append(`stop() rejected: ${String(failure)}`);
    }
  }, [stopPublishing, append]);

  const flipCamera = useCallback(() => {
    flip();
    append('camera flipped');
  }, [flip, append]);

  const toggleMute = useCallback(
    (value: boolean) => {
      setMuted(value);
      append(value ? 'muted' : 'unmuted');
    },
    [setMuted, append]
  );

  const toggleWatermark = useCallback(
    (value: boolean) => {
      const watermark = watermarkRef.current;
      if (!watermark) return;
      if (value) watermark.mixer.addLayer(watermark.layer, WATERMARK_FRAME);
      else watermark.mixer.removeLayer(watermark.layer);
      setWatermarkOn(value);
      append(value ? 'watermark on' : 'watermark off');
    },
    [append]
  );

  useEffect(() => {
    if (!ready || AUTOSTART_URLS.length === 0) return;
    let cancelled = false;
    const timers = new Set<ReturnType<typeof setTimeout>>();
    const perform = (action: ScriptedAction) => {
      if (cancelled) return;
      append(`action: ${action}`);
      switch (action) {
        case 'flip':
          flipCamera();
          break;
        case 'mute':
          toggleMute(true);
          break;
        case 'unmute':
          toggleMute(false);
          break;
        case 'watermark':
          toggleWatermark(true);
          break;
        case 'nowatermark':
          toggleWatermark(false);
          break;
      }
    };
    const run = async () => {
      for (const [index, target] of AUTOSTART_URLS.entries()) {
        if (index > 0) await delay(SCENARIO_GAP_MS);
        if (cancelled) return;
        setUrl(target);
        append(`scenario ${index + 1}/${AUTOSTART_URLS.length}: ${target}`);
        const publishing = await start(target);
        if (cancelled) return;
        if (publishing) {
          ACTIONS.forEach(({ action, atMs }) => {
            timers.add(setTimeout(() => perform(action), atMs));
          });
          await delay(DURATION_MS);
          timers.forEach(clearTimeout);
          timers.clear();
          if (cancelled) return;
          await stop();
        }
      }
      if (!cancelled) append('scenario finished');
    };
    run().catch((failure: unknown) => {
      if (!cancelled) append(`scenario failed: ${String(failure)}`);
    });
    return () => {
      cancelled = true;
      timers.forEach(clearTimeout);
    };
  }, [ready, start, stop, append, flipCamera, toggleMute, toggleWatermark]);

  const active = ACTIVE_STATES.includes(state);

  return (
    <View style={styles.container}>
      <RtmpPreview style={styles.preview} stream={stream} resizeMode="cover" />
      <View style={styles.top}>
        <View style={styles.row}>
          <TextInput
            style={styles.input}
            value={url}
            onChangeText={setUrl}
            autoCapitalize="none"
            autoCorrect={false}
            editable={!active}
            testID="url"
          />
          <Button title="Fixture" onPress={onSwitchToFixture} testID="mode" />
        </View>
        <View style={styles.row}>
          <Button
            title="Start"
            onPress={() => start(url)}
            disabled={busy || active || !ready}
            testID="start"
          />
          <Button
            title="Stop"
            onPress={() => stop()}
            disabled={!busy && !active}
            testID="stop"
          />
          <Button title="Flip" onPress={flipCamera} testID="flip" />
          <Text style={styles.label}>mute</Text>
          <Switch value={muted} onValueChange={toggleMute} testID="mute" />
          <Text style={styles.label}>wm</Text>
          <Switch
            value={watermarkOn}
            onValueChange={toggleWatermark}
            disabled={!watermarkLoaded}
            testID="watermark"
          />
        </View>
      </View>
      <View style={styles.bottom}>
        <Text style={styles.state} testID="state">
          {state} · {position} · {encodeFps.toFixed(1)} fps ·{' '}
          {mixerStats?.videoBitrateKbps.toFixed(0) ?? '-'} kbps
        </Text>
        <Text style={styles.stat} testID="mixerStats">
          captured {mixerStats?.capturedFrames ?? '-'} · rendered{' '}
          {mixerStats?.renderedFrames ?? '-'} · dropped{' '}
          {mixerStats?.droppedFrames ?? '-'} · video{' '}
          {mixerStats?.encodedVideoFrames ?? '-'} · audio{' '}
          {mixerStats?.encodedAudioFrames ?? '-'} · encFail{' '}
          {mixerStats?.encoderFailures ?? '-'}
        </Text>
        <Text style={styles.stat} testID="publisherStats">
          sent {publisherStats?.bytesSent ?? '-'} · queued{' '}
          {publisherStats?.queuedBytes ?? '-'} · tags v
          {publisherStats?.videoTags ?? '-'} a{publisherStats?.audioTags ?? '-'}{' '}
          · rejected {publisherStats?.rejectedFrames ?? '-'} · clamps{' '}
          {publisherStats?.timestampClamps ?? '-'}
        </Text>
        <Text style={styles.error} testID="lastError">
          {lastError === '' ? 'no error' : lastError}
        </Text>
        {log.map((line, index) => (
          <Text key={index} style={styles.logLine}>
            {line}
          </Text>
        ))}
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  container: {
    flex: 1,
    backgroundColor: '#000',
  },
  preview: {
    position: 'absolute',
    top: 0,
    left: 0,
    right: 0,
    bottom: 0,
  },
  top: {
    position: 'absolute',
    top: 56,
    left: 12,
    right: 12,
    gap: 6,
    padding: 8,
    borderRadius: 8,
    backgroundColor: 'rgba(255,255,255,0.85)',
  },
  bottom: {
    position: 'absolute',
    bottom: 24,
    left: 12,
    right: 12,
    padding: 8,
    borderRadius: 8,
    backgroundColor: 'rgba(255,255,255,0.85)',
  },
  row: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 8,
  },
  input: {
    flex: 1,
    borderWidth: 1,
    borderColor: '#999',
    borderRadius: 6,
    paddingHorizontal: 8,
    paddingVertical: 4,
    fontSize: 13,
    backgroundColor: '#fff',
  },
  label: {
    marginLeft: 'auto',
    fontSize: 12,
  },
  state: {
    fontSize: 14,
    fontWeight: '600',
  },
  stat: {
    fontSize: 11,
    fontVariant: ['tabular-nums'],
    color: '#333',
  },
  error: {
    color: '#b00020',
    fontSize: 12,
  },
  logLine: {
    fontSize: 10,
    fontVariant: ['tabular-nums'],
    color: '#555',
  },
});
