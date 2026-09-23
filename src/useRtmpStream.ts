import { useEffect, useState, useSyncExternalStore } from 'react';
import {
  createCameraLayer,
  createMicrophoneSource,
  createMixer,
  createPublisher,
} from './native';
import { RtmpStreamController } from './RtmpStreamController';
import { requestStreamPermissions } from './streamPermissions';
import type { RtmpStream, RtmpStreamOptions } from './streamTypes';

/**
 * Owns the camera layer, microphone, mixer and publisher for a mounted React
 * component. The camera itself is VisionCamera's: pass `stream.cameraOutput`
 * to its `outputs`.
 */
export function useRtmpStream(options: RtmpStreamOptions = {}): RtmpStream {
  const { active = true, audio = true, statsIntervalMs = 0 } = options;
  const {
    width = 720,
    height = 1280,
    frameRate = 30,
    bitrateKbps = 2500,
    keyframeIntervalSeconds = 2,
  } = options.video ?? {};
  const {
    sampleRate = 48000,
    channels = 1,
    bitrateKbps: audioBitrateKbps = 128,
  } = options.audioSettings ?? {};
  // Only the JS controller is allocated during render, including Strict Mode renders.
  const [controller] = useState(
    () =>
      new RtmpStreamController({
        createCameraLayer,
        createMicrophoneSource,
        createMixer,
        createPublisher,
        requestPermissions: requestStreamPermissions,
      })
  );
  const snapshot = useSyncExternalStore(
    controller.subscribe,
    controller.getSnapshot
  );

  useEffect(() => {
    if (active)
      controller.activate({
        audio,
        video: {
          width,
          height,
          frameRate,
          bitrateKbps,
          keyframeIntervalSeconds,
        },
        audioSettings: { sampleRate, channels, bitrateKbps: audioBitrateKbps },
        statsIntervalMs,
      });
    return () => {
      controller.deactivate();
    };
  }, [
    controller,
    active,
    audio,
    width,
    height,
    frameRate,
    bitrateKbps,
    keyframeIntervalSeconds,
    sampleRate,
    channels,
    audioBitrateKbps,
    statsIntervalMs,
  ]);

  return {
    ...snapshot,
    start: controller.start,
    stop: controller.stop,
    setMuted: controller.setMuted,
  };
}
