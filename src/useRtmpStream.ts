import { useEffect, useState, useSyncExternalStore } from 'react';
import {
  createCameraSource,
  createMicrophoneSource,
  createMixer,
  createPublisher,
} from './native';
import { RtmpStreamController } from './RtmpStreamController';
import { requestStreamPermissions } from './streamPermissions';
import type { RtmpStream, RtmpStreamOptions } from './streamTypes';

/** Owns capture, publishing and subscriptions for a mounted React component. */
export function useRtmpStream(options: RtmpStreamOptions = {}): RtmpStream {
  const {
    active = true,
    camera = 'back',
    audio = true,
    statsIntervalMs = 0,
  } = options;
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
      new RtmpStreamController(
        {
          createCameraSource,
          createMicrophoneSource,
          createMixer,
          createPublisher,
          requestPermissions: requestStreamPermissions,
        },
        camera
      )
  );
  const snapshot = useSyncExternalStore(
    controller.subscribe,
    controller.getSnapshot
  );

  useEffect(() => {
    controller.setCameraPosition(camera);
  }, [controller, camera]);

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
    setCameraPosition: controller.setCameraPosition,
    flipCamera: controller.flipCamera,
    setMuted: controller.setMuted,
  };
}
