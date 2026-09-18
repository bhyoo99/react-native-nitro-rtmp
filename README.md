# react-native-nitro-rtmp

Live video streaming over RTMP and RTMPS for React Native, built on [Nitro Modules](https://nitro.margelo.com/).

The library captures the camera and the microphone, composites them with overlay layers on the GPU, encodes with the platform's hardware H.264 and AAC encoders, and publishes through a sans-IO RTMP core shared by iOS and Android. JavaScript configures the scene and the session; frames never cross into JavaScript.

> Early release. The API is small on purpose and may still change before 1.0.

## Features

- Camera (front/back, switchable while live) and microphone (mute keeps sending silence so the timeline never breaks)
- A mixer with ordered layers placed in normalized rectangles: the camera and PNG/JPEG image overlays today, one interface for future layer types
- `PreviewView` renders exactly what is sent (the front camera is mirrored in the preview only)
- H.264 Main profile without B-frames with configurable bitrate, frame rate and keyframe interval; AAC-LC at 48 kHz
- `rtmp://` and `rtmps://` (TLS through the system trust store, SNI and host name verification)
- Typed session states, error codes and counters for both the session and the encoders
- `pushVideo` / `pushAudio` to feed your own encoder output through the same session
- A portable C++ protocol core with host tests, including a loopback against a real RTMP server

## Requirements

- React Native with the New Architecture (developed against 0.86) and `react-native-nitro-modules` ^0.37
- iOS: the minimum version of your React Native release. Android: API 24+
- The example app is an Expo project with a development client; Expo Go cannot load native modules

## Installation

```sh
npm install react-native-nitro-rtmp react-native-nitro-modules
cd ios && pod install
```

### Permissions

**iOS**: add `NSCameraUsageDescription` and `NSMicrophoneUsageDescription` to `Info.plist` (or `expo.ios.infoPlist` in `app.json`). `camera.start()` and `mic.start()` request access and reject with `permissionDenied` when it is refused.

**Android**: the library's manifest declares `CAMERA` and `RECORD_AUDIO`. Request them at runtime before calling `start()`, for example with `PermissionsAndroid.requestMultiple`; the library only checks that they were granted.

## Usage

```tsx
import { useEffect, useRef } from 'react';
import { StyleSheet } from 'react-native';
import {
  createCameraSource,
  createImageLayer,
  createMicrophoneSource,
  createMixer,
  createPublisher,
  PreviewView,
} from 'react-native-nitro-rtmp';

function createSession() {
  return {
    publisher: createPublisher(),
    mixer: createMixer(),
    camera: createCameraSource(),
    mic: createMicrophoneSource(),
    watermark: createImageLayer(),
  };
}

export function Broadcast({ url }: { url: string }) {
  // Native objects live for the component's lifetime.
  const sessionRef = useRef<ReturnType<typeof createSession> | null>(null);
  if (sessionRef.current === null) {
    sessionRef.current = createSession();
  }
  const { publisher, mixer, camera, mic, watermark } = sessionRef.current;

  useEffect(() => {
    mixer.video = { width: 720, height: 1280, frameRate: 30, bitrateKbps: 2500 };
    mixer.audio = { sampleRate: 48000, channels: 1, bitrateKbps: 128 };
    mixer.addLayer(camera);
    mixer.setAudioSource(mic);
    publisher.setMixer(mixer);
    publisher.onStateChange((state) => console.log('state', state));
    publisher.onError((error) => console.warn(error.code, error.message));

    (async () => {
      await camera.start(); // the preview runs from here; the encoders wait for the session
      await mic.start();
      await watermark.load('file:///path/to/logo.png');
      mixer.addLayer(watermark, { x: 0.7, y: 0.05, width: 0.25, height: 0.1 });
      await publisher.start(url); // resolves once the server accepted the publish
    })().catch(console.warn);

    return () => {
      publisher.setMixer(undefined);
      publisher.stop();
      camera.stop();
      mic.stop();
    };
  }, [publisher, mixer, camera, mic, watermark, url]);

  return (
    <PreviewView mixer={mixer} resizeMode="cover" style={StyleSheet.absoluteFill} />
  );
}
```

While live: `camera.position = 'front'` switches the camera, `mic.muted = true` sends silence, `mixer.removeLayer(watermark)` takes the overlay off, and `publisher.stats` / `mixer.stats` report counters.

## API

### `createPublisher(): RtmpPublisher`

| Member | Description |
| --- | --- |
| `state` | `'idle' \| 'connecting' \| 'connected' \| 'publishing' \| 'stopped' \| 'failed'` |
| `stats` | `bytesSent`, `queuedBytes`, `videoTags`, `audioTags`, `rejectedFrames`, `droppedBeforeKeyframe`, `invalidFrames`, `timestampClamps`, `lastVideoTimestamp`, `lastAudioTimestamp` |
| `start(url)` | Opens the socket (TLS for `rtmps://`), runs the handshake and the connect/createStream/publish exchange. Resolves at `publishing`; rejects with an `RtmpPublisherError` before that. Later failures go to `onError` |
| `stop()` | Sends FCUnpublish/deleteStream, drains the send queue and closes the socket |
| `setMixer(mixer?)` | Attaches a mixer: its encoders start when the session reaches `publishing` and stop when it ends. `undefined` detaches |
| `setMetadata(metadata)` | Values for the `onMetaData` tag: `width`, `height`, `frameRate`, `videoBitrateKbps`, `audioSampleRate`, `audioChannels`, `audioBitrateKbps`, `encoder` |
| `onStateChange(cb)`, `onError(cb)` | Session callbacks |
| `pushVideo(annexB, ptsMs, dtsMs)`, `pushAudio(adts, ptsMs)` | External sources: one H.264 Annex-B access unit (the first one an IDR with SPS/PPS) or one ADTS AAC frame |

### `createMixer(): Mixer`

| Member | Description |
| --- | --- |
| `video` | `{ width, height, frameRate, bitrateKbps, keyframeIntervalSeconds? }`, changeable while not encoding |
| `audio` | `{ sampleRate, channels, bitrateKbps }` |
| `stats` | `capturedFrames`, `renderedFrames`, `droppedFrames`, `encodedVideoFrames`, `encodedAudioFrames`, `videoBitrateKbps`, `encoderFailures` |
| `isEncoding` | True while attached to a publishing session |
| `addLayer(layer, frame?)` | Z order is the order of addition. `frame` is in normalized output coordinates (0..1); omitted means the whole frame |
| `removeLayer(layer)`, `setLayerFrame(layer, frame)` | Scene changes, allowed while live |
| `setAudioSource(mic?)` | Attaches or detaches the microphone |
| `onError(cb)` | Encoder failures as `CaptureError` |

### Layers and sources

- `createCameraSource()`: `position` (`'front' | 'back'`, switchable while running), `isRunning`, `start()`, `stop()`
- `createMicrophoneSource()`: `muted`, `isRunning`, `start()`, `stop()`
- `createImageLayer()`: `load(fileUri)` for a PNG or JPEG `file://` URI, `isLoaded`

### `PreviewView`

Props: `mixer`, `resizeMode` (`'cover'` by default, or `'contain'`) and the usual view props. Several previews can share one mixer.

### Errors

- `RtmpPublisherError` codes: `invalidUrl`, `handshakeFailed`, `connectRejected`, `invalidApp`, `publishBadName`, `streamAlreadyExists`, `serverClosed`, `protocolError`, `notReady`, `connectFailed`, `tlsFailed`, `socketClosed`, `timeout`
- `RtmpCaptureError` codes: `permissionDenied`, `cameraUnavailable`, `configurationFailed`, `encoderFailed`
- `toPublisherError(e)` and `toCaptureError(e)` turn any rejection into the typed error

## How it works

```
CameraSource ──frames──┐
ImageLayer ────────────┤→ Mixer (GPU compositor) ──→ H264Encoder ──Annex-B──┐
                       └───────────────────────────→ PreviewView             ├→ session queue → RTMP core → socket
MicrophoneSource ──PCM──→ AacEncoder ──ADTS──────────────────────────────────┘
```

- The encoder never sees the camera. Every frame takes one GPU pass, so overlays are just more layers and the preview shows the frame that is sent.
- Frames never enter JavaScript. Encoder output is copied once and posted to the session queue natively.
- One drop policy: capture callbacks are never blocked. When the compositor or the encoder is busy, the newest camera frame replaces the waiting one and `droppedFrames` counts it.
- Both streams use the same monotonic capture clock. The first encoded video frame defines t = 0, audio captured before it is dropped, and that frame is forced to be a keyframe.
- The RTMP core in `cpp/nitrortmp` is sans-IO: it consumes and emits bytes and the platforms own the socket. It is tested on the host against a loopback RTMP server and golden FLV fixtures.

iOS uses AVCaptureSession, Metal, VideoToolbox and AudioToolbox. Android uses Camera2, OpenGL ES 2.0, MediaCodec and AudioRecord.

## Limitations

- No rotation while publishing, adaptive bitrate, automatic reconnect or background mode yet
- H.264 and AAC only; no local recording
- The iOS simulator has no camera. The example app's fixture screen streams a pre-encoded clip through `pushVideo` / `pushAudio` for simulator and regression checks

## Verifying a stream

```sh
scripts/verify-stream.sh --listen --width 720 --height 1280 --fps 30 --duration 10
```

starts `ffmpeg -listen`, waits for one publish and checks codecs, size, frame rate, keyframe interval, timestamp monotonicity, A/V alignment and a clean decode. The example app reads `EXPO_PUBLIC_*` variables (see `example/src/env.ts`) to start automatically and to switch cameras, mute and toggle the watermark on a schedule.

## Third-party code

`cpp/third_party/media-server` vendors [ireader/media-server](https://github.com/ireader/media-server) and [ireader/sdk](https://github.com/ireader/sdk) (MIT) with one patch that adds an `onStatus` callback to the RTMP client. See `cpp/third_party/media-server/UPSTREAM.md` and `scripts/sync-media-server.sh`.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for the development workflow, the C++ host tests, stream verification and the vendored sources.

## License

MIT

---

Made with [create-react-native-library](https://github.com/callstack/react-native-builder-bob)
