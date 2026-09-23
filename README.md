![Nitro RTMP — The live-streaming output for VisionCamera](.github/assets/readme-banner.png)

# react-native-nitro-rtmp

Live video streaming over RTMP and RTMPS for React Native, built on [Nitro Modules](https://nitro.margelo.com/) as an output for [VisionCamera](https://github.com/mrousavy/react-native-vision-camera) 5.

VisionCamera owns the camera (device, format, frame rate, zoom, focus, permission); the library plugs a `CameraOutput` into its session, captures the microphone, composites camera and overlay layers on the GPU, encodes with the platform's hardware H.264 and AAC encoders, and publishes through a sans-IO RTMP core shared by iOS and Android. JavaScript configures the scene and the session; frames never cross into JavaScript.

> Early release. The API is small on purpose and may still change before 1.0.

## Features

- A VisionCamera output: the camera goes straight from VisionCamera's session into the mixer (an `AVCaptureVideoDataOutput` on iOS, a CameraX `Preview` use case into the mixer's `SurfaceTexture` on Android), so everything VisionCamera can do with a camera works while streaming
- Microphone capture (mute keeps sending silence so the timeline never breaks)
- A mixer with ordered layers placed in normalized rectangles: the camera and PNG/JPEG image overlays today, one interface for future layer types
- `PreviewView` renders exactly what is sent (a front camera is mirrored in the preview only)
- H.264 Main profile without B-frames with configurable bitrate, frame rate and keyframe interval; AAC-LC at 48 kHz
- `rtmp://` and `rtmps://` (TLS through the system trust store, SNI and host name verification)
- Typed session states, error codes and counters for both the session and the encoders
- `pushVideo` / `pushAudio` to feed your own encoder output through the same session
- A portable C++ protocol core with host tests, including a loopback against a real RTMP server

## Requirements

- React Native with the New Architecture (developed against 0.86), `react-native-nitro-modules` ^0.37 and `react-native-vision-camera` ^5.2 (with its peer `react-native-nitro-image`)
- iOS: the minimum version of your React Native release. Android: API 24+
- The example app is an Expo project with a development client; Expo Go cannot load native modules

## Installation

```sh
npm install react-native-nitro-rtmp react-native-vision-camera react-native-nitro-image react-native-nitro-modules
cd ios && pod install
```

Set VisionCamera up as usual (permissions, `Camera` view). The library's native code depends on VisionCamera's pod and Gradle module, so the two are always built together.

### Permissions

**Camera**: VisionCamera's. Request it with `useCameraPermission()` (or `VisionCamera.requestCameraPermission()`) before the camera session starts; add `NSCameraUsageDescription` on iOS and the `CAMERA` permission on Android as VisionCamera documents.

**Microphone**: add `NSMicrophoneUsageDescription` to `Info.plist` (or `expo.ios.infoPlist` in `app.json`); `mic.start()` requests access on iOS and rejects with `permissionDenied` when it is refused. On Android the library's manifest declares `RECORD_AUDIO`, and `useRtmpStream` requests it when activated with `audio: true`. When using `createMicrophoneSource()` directly on Android, request the permission yourself before `start()`.

## Usage

`useRtmpStream` owns the camera layer, microphone, mixer and publisher, and exposes `cameraOutput`: a VisionCamera `CameraOutput` you pass to your `<Camera>` (or `useCamera`) `outputs`. VisionCamera then feeds the mixer directly. The hook exposes reactive state, and releases everything on unmount. Call `start(url)` when the user is ready to go live.

```tsx
import { useMemo } from 'react';
import { Button, StyleSheet, Text, View } from 'react-native';
import { RtmpPreview, useRtmpStream } from 'react-native-nitro-rtmp';
import {
  Camera,
  useCameraDevice,
  useCameraPermission,
} from 'react-native-vision-camera';

export function Broadcast({ url }: { url: string }) {
  const { hasPermission } = useCameraPermission();
  const device = useCameraDevice('back');
  const stream = useRtmpStream({ audio: true });
  const outputs = useMemo(
    () => (stream.cameraOutput ? [stream.cameraOutput] : []),
    [stream.cameraOutput]
  );

  return (
    <View style={{ flex: 1 }}>
      {device && hasPermission ? (
        <Camera
          style={StyleSheet.absoluteFill}
          device={device}
          isActive
          outputs={outputs}
        />
      ) : null}
      <Button
        title="Go live"
        disabled={
          !stream.ready || stream.isBusy || stream.state === 'publishing'
        }
        onPress={() => void stream.start(url).catch(console.warn)}
      />
      <Button
        title="Stop"
        onPress={() => void stream.stop().catch(console.warn)}
      />
      <Text>{stream.error?.message}</Text>
    </View>
  );
}
```

The `<Camera>` view shows VisionCamera's own preview of the raw camera. To show what is actually sent (overlays included), render `RtmpPreview` instead and hide the `<Camera>` (or run the session without a view through VisionCamera's `useCamera` hook). Switching cameras, zoom, focus, torch, formats and frame rate are VisionCamera's: change its `device` prop or `constraints` (for example `[{ fps: 30 }]`) while the stream stays live.

`stop()` stops publishing while keeping the camera output and preview alive, and cancels an in-flight `start()`. Pass `active: false` to release the output, mixer and microphone and stop publishing; `cameraOutput` becomes `undefined`, which drops it from your `outputs`. For navigation, pass your screen's focus state as `active`; also include app foreground state if the microphone should stop in the background. Activation requests microphone access when `audio` is on, so use `active: false` until you want to ask for permission. React Strict Mode and rapid deactivation/reactivation are handled by waiting for the previous session to finish shutting down.

### Controls and state

```tsx
stream.setMuted(true); // Keeps the audio timeline, sends silence

// These values update React automatically:
stream.ready;
stream.state;
stream.isBusy;
stream.error;
stream.muted;
stream.cameraOutput; // Pass to VisionCamera's `outputs`
```

`start(url)` waits for readiness (the microphone, when enabled) and resolves when publishing. `start()` and `stop()` return promises; handle their rejections in event handlers. Capture and transport failures also appear in `stream.error` as `RtmpCaptureError` or `RtmpPublisherError`. A cancelled or inactive start rejects with `notReady`. A startup failure releases any partially started resources; fix the cause and toggle `active` off/on to retry. Stopping a stream does not revoke its permissions. Camera errors are reported by VisionCamera (`onError` on `<Camera>`).

### Configuration

```tsx
const stream = useRtmpStream({
  active: isFocused,
  audio: true,
  video: { width: 720, height: 1280, frameRate: 30, bitrateKbps: 2500 },
  statsIntervalMs: 1000,
});

// Populated only when stats polling is enabled and capture is ready.
stream.stats?.publisher.bytesSent;
stream.stats?.mixer.droppedFrames;
```

| Option            | Default                                           | Behavior                                                                                                                   |
| ----------------- | ------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------- |
| `active`          | `true`                                            | Creates the camera output, mixer and microphone; `false` releases them and stops publishing                                |
| `audio`           | `true`                                            | Enables the microphone; `false` needs no microphone permission                                                             |
| `video`           | 720 × 1280, 30 fps, 2500 kbps, 2-second keyframes | Partial `VideoSettings`; dimensions must be positive even integers. The camera format follows the size; the frame rate is the encoder's, set the camera's with VisionCamera constraints |
| `audioSettings`   | 48000 Hz, 1 channel, 128 kbps                     | Partial `AudioSettings`; capture supports 48000 Hz and 1 or 2 channels                                                     |
| `statsIntervalMs` | `0`                                               | Opt-in stats polling; `0` disables it                                                                                      |

Changing video/audio configuration or the stats interval recreates the mixer, camera output and microphone (a new `cameraOutput`, so VisionCamera reconfigures its session) and stops any current broadcast; call `start()` again to resume publishing. Passing a new options object with unchanged values does not restart it. Mute applies without recreating anything. Only one hook should own the microphone at a time.

### Image overlays and advanced composition

The hook exposes `stream.mixer` for the existing layer API. It is `undefined` before initialization and while inactive, and is replaced when the capture session is recreated. Attach custom layers in an effect that depends on this mixer, and remove them in cleanup:

```tsx
import { useEffect } from 'react';
import { createImageLayer } from 'react-native-nitro-rtmp';

const { mixer } = stream;
useEffect(() => {
  if (!mixer) return;
  let cancelled = false;
  const image = createImageLayer();
  void image
    .load(imageFileUri)
    .then(() => {
      if (!cancelled) {
        mixer.addLayer(image, { x: 0.7, y: 0.05, width: 0.25, height: 0.1 });
      }
    })
    .catch(console.warn);
  return () => {
    cancelled = true;
    mixer.removeLayer(image);
  };
}, [mixer, imageFileUri]);
```

The mixer is owned by the hook: use it to manage your additional layers, and configure encoding through hook options. For custom encoder input or full control over resource ownership, the factories and `PreviewView` remain available. See the API below and the [fixture example](example/src/FixtureScreen.tsx).

## API

### `useRtmpStream(options?): RtmpStream`

Returns the reactive state and controls described above. Native objects are created after mounting, never during render. Activation and publishing are separate; activation does not automatically connect to an RTMP server.

### `RtmpPreview`

Props: `stream`, `resizeMode` (`'cover'` or `'contain'`) and the same view props as `PreviewView`. Renders the hook-owned mixer's composition. Multiple previews may display the same stream.

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

- `createCameraLayer()`: `output` (the VisionCamera `CameraOutput` to pass to `outputs`; stable for the layer's lifetime), `isFrontCamera`, `isReceivingFrames`. Add the layer to a mixer before VisionCamera configures the session so the camera format follows the mixer's output size
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
VisionCamera session ──CameraLayer.output──┐
ImageLayer ────────────────────────────────┤→ Mixer (GPU compositor) ──→ H264Encoder ──Annex-B──┐
                                           └───────────────────────────→ PreviewView             ├→ session queue → RTMP core → socket
MicrophoneSource ──PCM──→ AacEncoder ──ADTS──────────────────────────────────────────────────────┘
```

- The camera output is a VisionCamera `CameraOutput` (its Swift/Kotlin base class plus the Nitro spec), so VisionCamera treats it like its own outputs: it negotiates the format, rotates it to the device orientation and starts and stops it with the session. On iOS it is an `AVCaptureVideoDataOutput` delivering BGRA buffers to the compositor; on Android a CameraX `Preview` use case whose surface is the mixer's `SurfaceTexture`.
- The encoder never sees the camera. Every frame takes one GPU pass, so overlays are just more layers and the preview shows the frame that is sent.
- Frames never enter JavaScript. Encoder output is copied once and posted to the session queue natively.
- One drop policy: capture callbacks are never blocked. When the compositor or the encoder is busy, the newest camera frame replaces the waiting one and `droppedFrames` counts it.
- Both streams use the same monotonic capture clock. The first encoded video frame defines t = 0, audio captured before it is dropped, and that frame is forced to be a keyframe.
- The RTMP core in `cpp/nitrortmp` is sans-IO: it consumes and emits bytes and the platforms own the socket. It is tested on the host against a loopback RTMP server and golden FLV fixtures.

iOS uses AVFoundation, Metal, VideoToolbox and AudioToolbox. Android uses CameraX, OpenGL ES 2.0, MediaCodec and AudioRecord.

## Limitations

- No rotation while publishing, adaptive bitrate, automatic reconnect or background mode yet
- H.264 and AAC only; no local recording
- The stream is never mirrored; a front camera is mirrored in the preview only. VisionCamera's `mirrorMode` does not apply to this output
- The iOS simulator has no camera. The example app's fixture screen streams a pre-encoded clip through `pushVideo` / `pushAudio` for simulator and regression checks

## Verifying a stream

```sh
scripts/verify-stream.sh --listen --width 720 --height 1280 --fps 30 --duration 10
```

starts `ffmpeg -listen`, waits for one publish and checks codecs, size, frame rate, keyframe interval, timestamp monotonicity, A/V alignment and a clean decode. The example app reads `EXPO_PUBLIC_*` variables (see `example/src/env.ts`) to start automatically and to switch cameras, mute and toggle the watermark on a schedule.

## Migrating from 0.3

0.3 captured the camera itself; this version plugs into VisionCamera instead.

1. Install `react-native-vision-camera` and `react-native-nitro-image`, add VisionCamera's camera permission setup, and rebuild the native apps (`pod install`, Gradle sync).
2. Render a VisionCamera `<Camera>` (or call `useCamera`) and pass `stream.cameraOutput` in its `outputs`. Nothing streams until the camera session runs with that output.
3. Replace the removed hook API:

| 0.3 | Now |
| --- | --- |
| `useRtmpStream({ camera: 'front' })` | `useCameraDevice('front')` and the `device` prop of `<Camera>` |
| `stream.camera`, `stream.flipCamera()`, `stream.setCameraPosition()` | Change `device`; the stream keeps running |
| Camera permission requested by the hook | `useCameraPermission()` from VisionCamera; the hook requests the microphone only |
| `video.frameRate` sets the camera frame rate | It sets the encoder's; give `<Camera>` `constraints={[{ fps: 30 }]}` |
| `createCameraSource()`, `camera.start()` / `stop()` | `createCameraLayer()`; VisionCamera starts and stops the output |

`RtmpPreview`, `PreviewView`, the mixer, image layers, the microphone and the publisher keep their APIs. The native view is registered as `RtmpPreviewView` now, which only matters if you referenced the Fabric component name directly.

## Third-party code

`cpp/third_party/media-server` vendors [ireader/media-server](https://github.com/ireader/media-server) and [ireader/sdk](https://github.com/ireader/sdk) (MIT) with one patch that adds an `onStatus` callback to the RTMP client. See `cpp/third_party/media-server/UPSTREAM.md` and `scripts/sync-media-server.sh`.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for the development workflow, the C++ host tests, stream verification and the vendored sources.

## License

MIT

---

Made with [create-react-native-library](https://github.com/callstack/react-native-builder-bob)
