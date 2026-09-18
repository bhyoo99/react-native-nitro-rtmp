import { useCallback, useEffect, useRef, useState } from 'react';
import {
  Button,
  ScrollView,
  StyleSheet,
  Switch,
  Text,
  TextInput,
  View,
} from 'react-native';
import {
  createPublisher,
  toPublisherError,
  type PublisherState,
  type PublisherStats,
  type RtmpPublisher,
} from 'react-native-nitro-rtmp';

import { AUTOSTART_URLS, INITIAL_LOOP, INITIAL_URL } from './env';
import { loadFixtureFrames, playFixture, type MediaFrame } from './fixtures';

const SCENARIO_GAP_MS = 3000;

const delay = (ms: number) =>
  new Promise<void>((resolve) => setTimeout(resolve, ms));

const ACTIVE_STATES: readonly PublisherState[] = [
  'connecting',
  'connected',
  'publishing',
];

const STAT_KEYS: readonly (keyof PublisherStats)[] = [
  'bytesSent',
  'queuedBytes',
  'videoTags',
  'audioTags',
  'rejectedFrames',
  'droppedBeforeKeyframe',
  'invalidFrames',
  'timestampClamps',
  'lastVideoTimestamp',
  'lastAudioTimestamp',
];

function timestamp(): string {
  return new Date().toISOString().slice(11, 23);
}

/**
 * The fixture screen: pushes the C++ test fixture through `pushVideo` /
 * `pushAudio` (simulator and regression verification).
 */
export default function FixtureScreen({
  onSwitchToCamera,
}: {
  onSwitchToCamera: () => void;
}) {
  const publisherRef = useRef<RtmpPublisher | null>(null);
  if (publisherRef.current === null) {
    publisherRef.current = createPublisher();
  }
  const publisher = publisherRef.current;

  const [url, setUrl] = useState(INITIAL_URL);
  const [state, setState] = useState<PublisherState>('idle');
  const [lastError, setLastError] = useState('');
  const [stats, setStats] = useState<PublisherStats | null>(null);
  const [loop, setLoop] = useState(INITIAL_LOOP);
  const [busy, setBusy] = useState(false);
  const [frames, setFrames] = useState<MediaFrame[] | null>(null);
  const [log, setLog] = useState<string[]>([]);
  const loopRef = useRef(loop);
  const stopPlaybackRef = useRef<(() => void) | null>(null);
  const sessionDoneRef = useRef<(() => void) | null>(null);
  const autoStartedRef = useRef(false);

  const append = useCallback((line: string) => {
    console.log(`[nitrortmp-example] ${line}`);
    setLog((previous) => [...previous.slice(-39), `${timestamp()} ${line}`]);
  }, []);

  useEffect(() => {
    loopRef.current = loop;
  }, [loop]);

  useEffect(() => {
    publisher.onStateChange((next) => {
      setState(next);
      append(`state: ${next}`);
    });
    publisher.onError((error) => {
      setLastError(`${error.code}: ${error.message}`);
      append(`onError ${error.code}: ${error.message}`);
    });
    const timer = setInterval(() => setStats(publisher.stats), 500);
    return () => clearInterval(timer);
  }, [publisher, append]);

  useEffect(() => {
    loadFixtureFrames()
      .then((loaded) => {
        setFrames(loaded);
        append(`fixture loaded: ${loaded.length} frames`);
      })
      .catch((error: unknown) => {
        append(`fixture failed: ${String(error)}`);
      });
  }, [append]);

  const stop = useCallback(async () => {
    stopPlaybackRef.current?.();
    stopPlaybackRef.current = null;
    setBusy(true);
    try {
      await publisher.stop();
      append('stop() resolved');
    } catch (error: unknown) {
      append(`stop() rejected: ${String(error)}`);
    } finally {
      setBusy(false);
      sessionDoneRef.current?.();
      sessionDoneRef.current = null;
    }
  }, [publisher, append]);

  /** start(), one fixture pass (or more with loop), stop(). Resolves when the session is over. */
  const runSession = useCallback(
    async (target: string) => {
      if (frames === null) {
        append('fixture not loaded yet');
        return;
      }
      setBusy(true);
      setLastError('');
      publisher.setMetadata({
        width: 320,
        height: 240,
        frameRate: 30,
        audioSampleRate: 48000,
        audioChannels: 2,
      });
      try {
        await publisher.start(target);
        append('start() resolved');
      } catch (error: unknown) {
        const failure = toPublisherError(error);
        setLastError(`${failure.code}: ${failure.message}`);
        append(`start() rejected ${failure.code}: ${failure.message}`);
        return;
      } finally {
        setBusy(false);
      }
      await new Promise<void>((resolve) => {
        sessionDoneRef.current = resolve;
        stopPlaybackRef.current = playFixture(publisher, frames, {
          loop: () => loopRef.current,
          onPass: (pass) => append(`fixture pass ${pass + 1}`),
          onDone: () => {
            append('fixture finished, stopping');
            stop();
          },
        });
      });
    },
    [publisher, frames, append, stop]
  );

  useEffect(() => {
    if (
      frames === null ||
      AUTOSTART_URLS.length === 0 ||
      autoStartedRef.current
    ) {
      return;
    }
    autoStartedRef.current = true;
    const run = async () => {
      for (const [index, target] of AUTOSTART_URLS.entries()) {
        if (index > 0) {
          await delay(SCENARIO_GAP_MS);
        }
        setUrl(target);
        append(`scenario ${index + 1}/${AUTOSTART_URLS.length}: ${target}`);
        await runSession(target);
      }
      append('scenario finished');
    };
    run();
  }, [frames, runSession, append]);

  const active = ACTIVE_STATES.includes(state);

  return (
    <View style={styles.container}>
      <View style={styles.row}>
        <Text style={styles.title}>NitroRtmp fixture</Text>
        <View style={styles.label}>
          <Button title="Camera" onPress={onSwitchToCamera} testID="mode" />
        </View>
      </View>
      <TextInput
        style={styles.input}
        value={url}
        onChangeText={setUrl}
        autoCapitalize="none"
        autoCorrect={false}
        editable={!active}
        testID="url"
      />
      <View style={styles.row}>
        <Button
          title="Start"
          onPress={() => runSession(url)}
          disabled={busy || active || frames === null}
          testID="start"
        />
        <Button
          title="Stop"
          onPress={() => stop()}
          disabled={busy || !active}
          testID="stop"
        />
        <Text style={styles.label}>loop</Text>
        <Switch value={loop} onValueChange={setLoop} testID="loop" />
      </View>
      <Text style={styles.state} testID="state">
        state: {state}
      </Text>
      <Text style={styles.error} testID="lastError">
        {lastError === '' ? 'no error' : lastError}
      </Text>
      <View style={styles.stats}>
        {STAT_KEYS.map((key) => (
          <View key={key} style={styles.statRow}>
            <Text style={styles.statKey}>{key}</Text>
            <Text style={styles.statValue}>
              {stats === null ? '-' : String(stats[key])}
            </Text>
          </View>
        ))}
      </View>
      <ScrollView style={styles.log}>
        {log.map((line, index) => (
          <Text key={index} style={styles.logLine}>
            {line}
          </Text>
        ))}
      </ScrollView>
    </View>
  );
}

const styles = StyleSheet.create({
  container: {
    flex: 1,
    paddingTop: 60,
    paddingHorizontal: 16,
    gap: 8,
  },
  title: {
    fontSize: 20,
    fontWeight: '600',
  },
  input: {
    borderWidth: 1,
    borderColor: '#999',
    borderRadius: 6,
    paddingHorizontal: 8,
    paddingVertical: 6,
    fontSize: 14,
  },
  row: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 12,
  },
  label: {
    marginLeft: 'auto',
  },
  state: {
    fontSize: 16,
    fontWeight: '500',
  },
  error: {
    color: '#b00020',
  },
  stats: {
    borderWidth: 1,
    borderColor: '#ddd',
    borderRadius: 6,
    padding: 8,
  },
  statRow: {
    flexDirection: 'row',
    justifyContent: 'space-between',
  },
  statKey: {
    fontVariant: ['tabular-nums'],
    color: '#555',
  },
  statValue: {
    fontVariant: ['tabular-nums'],
  },
  log: {
    flex: 1,
    backgroundColor: '#f4f4f4',
    borderRadius: 6,
    padding: 8,
  },
  logLine: {
    fontSize: 11,
    fontVariant: ['tabular-nums'],
  },
});
