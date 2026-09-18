import { Asset } from 'expo-asset';
import { useCallback, useEffect, useRef, useState } from 'react';
import {
  Button,
  PermissionsAndroid,
  Platform,
  StyleSheet,
  Switch,
  Text,
  TextInput,
  View,
} from 'react-native';
import {
  createCameraSource,
  createImageLayer,
  createMicrophoneSource,
  createMixer,
  createPublisher,
  PreviewView,
  toCaptureError,
  toPublisherError,
  type CameraPosition,
  type MixerStats,
  type PublisherState,
  type PublisherStats,
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

/** Top-right corner, output-frame coordinates. */
const WATERMARK_FRAME = { x: 0.7, y: 0.05, width: 0.25, height: 0.1 };
const STATS_INTERVAL_MS = 500;
const SCENARIO_GAP_MS = 3000;

const ACTIVE_STATES: readonly PublisherState[] = [
  'connecting',
  'connected',
  'publishing',
];

/**
 * The live screen: camera + microphone through the mixer,
 * the preview shows the composited scene, the watermark is an `ImageLayer`
 * that is only added to and removed from the mixer.
 */
export default function LiveScreen({
  onSwitchToFixture,
}: {
  onSwitchToFixture: () => void;
}) {
  // Native objects live for the screen's lifetime.
  const nativeRef = useRef<{
    publisher: ReturnType<typeof createPublisher>;
    mixer: ReturnType<typeof createMixer>;
    camera: ReturnType<typeof createCameraSource>;
    mic: ReturnType<typeof createMicrophoneSource>;
    watermark: ReturnType<typeof createImageLayer>;
  } | null>(null);
  if (nativeRef.current === null) {
    nativeRef.current = {
      publisher: createPublisher(),
      mixer: createMixer(),
      camera: createCameraSource(),
      mic: createMicrophoneSource(),
      watermark: createImageLayer(),
    };
  }
  const { publisher, mixer, camera, mic, watermark } = nativeRef.current;

  const [url, setUrl] = useState(INITIAL_URL);
  const [state, setState] = useState<PublisherState>('idle');
  const [lastError, setLastError] = useState('');
  const [ready, setReady] = useState(false);
  const [busy, setBusy] = useState(false);
  const [position, setPosition] = useState<CameraPosition>('back');
  const [muted, setMuted] = useState(false);
  const [watermarkOn, setWatermarkOn] = useState(false);
  const [watermarkLoaded, setWatermarkLoaded] = useState(false);
  const [publisherStats, setPublisherStats] = useState<PublisherStats | null>(
    null
  );
  const [mixerStats, setMixerStats] = useState<MixerStats | null>(null);
  const [encodeFps, setEncodeFps] = useState(0);
  const [log, setLog] = useState<string[]>([]);
  const autoStartedRef = useRef(false);
  const lastEncodedRef = useRef({ frames: 0, at: Date.now() });

  const append = useCallback((line: string) => {
    console.log(`[nitrortmp-example] ${line}`);
    setLog((previous) => [...previous.slice(-7), `${timestamp()} ${line}`]);
  }, []);

  // Session callbacks and the stats poll.
  useEffect(() => {
    publisher.onStateChange((next) => {
      setState(next);
      append(`state: ${next}`);
    });
    publisher.onError((error) => {
      setLastError(`${error.code}: ${error.message}`);
      append(`onError ${error.code}: ${error.message}`);
    });
    mixer.onError((error) => {
      setLastError(`${error.code}: ${error.message}`);
      append(`mixer error ${error.code}: ${error.message}`);
    });
    const timer = setInterval(() => {
      setPublisherStats(publisher.stats);
      const stats = mixer.stats;
      setMixerStats(stats);
      const now = Date.now();
      const previous = lastEncodedRef.current;
      const seconds = (now - previous.at) / 1000;
      if (seconds > 0) {
        setEncodeFps((stats.encodedVideoFrames - previous.frames) / seconds);
      }
      lastEncodedRef.current = { frames: stats.encodedVideoFrames, at: now };
    }, STATS_INTERVAL_MS);
    return () => clearInterval(timer);
  }, [publisher, mixer, append]);

  // Scene setup: permissions, camera + microphone, layers, the session link.
  useEffect(() => {
    let cancelled = false;
    const setup = async () => {
      if (Platform.OS === 'android') {
        // The library only checks permissions on Android.
        const granted = await PermissionsAndroid.requestMultiple([
          PermissionsAndroid.PERMISSIONS.CAMERA,
          PermissionsAndroid.PERMISSIONS.RECORD_AUDIO,
        ]);
        append(`permissions: ${JSON.stringify(granted)}`);
      }
      mixer.video = {
        width: 720,
        height: 1280,
        frameRate: 30,
        bitrateKbps: 2500,
        keyframeIntervalSeconds: 2,
      };
      mixer.audio = { sampleRate: 48000, channels: 1, bitrateKbps: 128 };
      mixer.addLayer(camera);
      mixer.setAudioSource(mic);
      publisher.setMixer(mixer);
      publisher.setMetadata({
        width: 720,
        height: 1280,
        frameRate: 30,
        videoBitrateKbps: 2500,
        audioSampleRate: 48000,
        audioChannels: 1,
        audioBitrateKbps: 128,
      });
      try {
        await camera.start();
        append('camera started');
      } catch (error: unknown) {
        const failure = toCaptureError(error);
        setLastError(`${failure.code}: ${failure.message}`);
        append(`camera.start() rejected ${failure.code}: ${failure.message}`);
      }
      try {
        await mic.start();
        append('microphone started');
      } catch (error: unknown) {
        const failure = toCaptureError(error);
        setLastError(`${failure.code}: ${failure.message}`);
        append(`mic.start() rejected ${failure.code}: ${failure.message}`);
      }
      try {
        const asset = Asset.fromModule(watermarkAsset);
        await asset.downloadAsync();
        const uri = asset.localUri ?? asset.uri;
        await watermark.load(uri);
        if (cancelled) {
          return;
        }
        setWatermarkLoaded(true);
        append('watermark loaded');
        if (INITIAL_WATERMARK) {
          mixer.addLayer(watermark, WATERMARK_FRAME);
          setWatermarkOn(true);
        }
      } catch (error: unknown) {
        const failure = toCaptureError(error);
        append(`watermark failed ${failure.code}: ${failure.message}`);
      }
      if (!cancelled) {
        setReady(true);
      }
    };
    setup();
    return () => {
      cancelled = true;
      publisher.setMixer(undefined);
      publisher.stop();
      camera.stop();
      mic.stop();
    };
  }, [publisher, mixer, camera, mic, watermark, append]);

  const start = useCallback(
    async (target: string) => {
      setBusy(true);
      setLastError('');
      try {
        await publisher.start(target);
        append('start() resolved');
      } catch (error: unknown) {
        const failure = toPublisherError(error);
        setLastError(`${failure.code}: ${failure.message}`);
        append(`start() rejected ${failure.code}: ${failure.message}`);
      } finally {
        setBusy(false);
      }
    },
    [publisher, append]
  );

  const stop = useCallback(async () => {
    setBusy(true);
    try {
      await publisher.stop();
      append('stop() resolved');
    } catch (error: unknown) {
      append(`stop() rejected: ${String(error)}`);
    } finally {
      setBusy(false);
    }
  }, [publisher, append]);

  const flipCamera = useCallback(() => {
    const next: CameraPosition = position === 'back' ? 'front' : 'back';
    camera.position = next;
    setPosition(next);
    append(`camera: ${next}`);
  }, [camera, position, append]);

  const toggleMute = useCallback(
    (value: boolean) => {
      mic.muted = value;
      setMuted(value);
      append(value ? 'muted' : 'unmuted');
    },
    [mic, append]
  );

  // The overlay proof: the watermark is only added and removed.
  const toggleWatermark = useCallback(
    (value: boolean) => {
      if (value) {
        mixer.addLayer(watermark, WATERMARK_FRAME);
      } else {
        mixer.removeLayer(watermark);
      }
      setWatermarkOn(value);
      append(value ? 'watermark on' : 'watermark off');
    },
    [mixer, watermark, append]
  );

  // Scripted run: each URL for DURATION_MS with the ACTIONS in between, then stop.
  useEffect(() => {
    if (!ready || AUTOSTART_URLS.length === 0 || autoStartedRef.current) {
      return;
    }
    autoStartedRef.current = true;
    const perform = (action: ScriptedAction) => {
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
        if (index > 0) {
          await delay(SCENARIO_GAP_MS);
        }
        setUrl(target);
        append(`scenario ${index + 1}/${AUTOSTART_URLS.length}: ${target}`);
        await start(target);
        if (publisher.state === 'publishing') {
          const timers = ACTIONS.map(({ action, atMs }) =>
            setTimeout(() => perform(action), atMs)
          );
          await delay(DURATION_MS);
          timers.forEach(clearTimeout);
          await stop();
        }
      }
      append('scenario finished');
    };
    run();
  }, [
    ready,
    publisher,
    start,
    stop,
    append,
    flipCamera,
    toggleMute,
    toggleWatermark,
  ]);

  const active = ACTIVE_STATES.includes(state);

  return (
    <View style={styles.container}>
      <PreviewView style={styles.preview} mixer={mixer} resizeMode="cover" />
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
            disabled={busy || !active}
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
