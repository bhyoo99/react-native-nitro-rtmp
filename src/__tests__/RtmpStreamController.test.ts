import { afterEach, describe, expect, it, jest } from '@jest/globals';
import { RtmpCaptureError } from '../errors';
import {
  RtmpStreamController,
  type StreamConfiguration,
} from '../RtmpStreamController';
import type { CameraLayer } from '../specs/CameraLayer.nitro';
import type { MicrophoneSource } from '../specs/MicrophoneSource.nitro';
import type { CaptureError, Mixer } from '../specs/Mixer.nitro';
import type {
  PublisherError,
  PublisherState,
  RtmpPublisher,
} from '../specs/RtmpPublisher.nitro';

const CONFIG: StreamConfiguration = {
  audio: true,
  video: { width: 720, height: 1280, frameRate: 30, bitrateKbps: 2500 },
  audioSettings: { sampleRate: 48000, channels: 1, bitrateKbps: 128 },
  statsIntervalMs: 0,
};

function deferred() {
  let resolve!: () => void;
  let reject!: (reason: unknown) => void;
  const promise = new Promise<void>((res, rej) => {
    resolve = res;
    reject = rej;
  });
  return { promise, resolve, reject };
}

function fixture() {
  let stateCallback: (state: PublisherState) => void = () => {};
  let publisherError: (error: PublisherError) => void = () => {};
  let captureError: (error: CaptureError) => void = () => {};
  const camera = {
    kind: 'camera',
    output: { mediaType: 'video' },
    isFrontCamera: false,
    dispose: jest.fn(),
  };
  const mic = {
    muted: false,
    start: jest.fn<() => Promise<void>>().mockResolvedValue(),
    stop: jest.fn<() => Promise<void>>().mockResolvedValue(),
  };
  const mixer = {
    video: CONFIG.video,
    audio: CONFIG.audioSettings,
    stats: { encodedVideoFrames: 30 },
    addLayer: jest.fn(),
    removeLayer: jest.fn(),
    setAudioSource: jest.fn(),
    onError: jest.fn((callback: typeof captureError) => {
      captureError = callback;
    }),
  };
  const publisher = {
    stats: { bytesSent: 123 },
    start: jest.fn<() => Promise<void>>().mockResolvedValue(),
    stop: jest.fn<() => Promise<void>>().mockResolvedValue(),
    setMixer: jest.fn(),
    setMetadata: jest.fn(),
    onStateChange: jest.fn((callback: typeof stateCallback) => {
      stateCallback = callback;
    }),
    onError: jest.fn((callback: typeof publisherError) => {
      publisherError = callback;
    }),
  };
  const dependencies = {
    createCameraLayer: jest.fn(() => camera as unknown as CameraLayer),
    createMicrophoneSource: jest.fn(() => mic as unknown as MicrophoneSource),
    createMixer: jest.fn(() => mixer as unknown as Mixer),
    createPublisher: jest.fn(() => publisher as unknown as RtmpPublisher),
    requestPermissions: jest.fn<() => Promise<void>>().mockResolvedValue(),
  };
  const controller = new RtmpStreamController(dependencies);
  return {
    controller,
    dependencies,
    camera,
    mic,
    mixer,
    publisher,
    emitState: (state: PublisherState) => stateCallback(state),
    emitPublisherError: (error: PublisherError) => publisherError(error),
    emitCaptureError: (error: CaptureError) => captureError(error),
  };
}

afterEach(() => {
  jest.useRealTimers();
});

describe('RtmpStreamController', () => {
  it('does not allocate native resources until activated, then exposes the camera output without publishing', async () => {
    const { controller, dependencies, publisher, camera, mixer, mic } =
      fixture();
    expect(dependencies.createMixer).not.toHaveBeenCalled();
    await controller.activate(CONFIG);
    expect(mixer.addLayer).toHaveBeenCalledWith(camera);
    expect(mic.start).toHaveBeenCalledTimes(1);
    expect(publisher.start).not.toHaveBeenCalled();
    expect(controller.getSnapshot()).toMatchObject({
      ready: true,
      state: 'idle',
      cameraOutput: camera.output,
      mixer,
    });
    await controller.deactivate();
  });

  it('exposes the camera output before the microphone has started', async () => {
    const { controller, camera, mic } = fixture();
    const pending = deferred();
    const micStarted = deferred();
    mic.start.mockImplementationOnce(() => {
      micStarted.resolve();
      return pending.promise;
    });
    const prepare = controller.activate(CONFIG);
    await micStarted.promise;
    expect(controller.getSnapshot()).toMatchObject({
      ready: false,
      cameraOutput: camera.output,
    });
    pending.resolve();
    await prepare;
    expect(controller.getSnapshot().ready).toBe(true);
    await controller.deactivate();
  });

  it('waits for capture readiness before publishing', async () => {
    const { controller, mic, publisher } = fixture();
    const capture = deferred();
    mic.start.mockReturnValueOnce(capture.promise);
    controller.activate(CONFIG);
    const start = controller.start('rtmp://localhost/live/test');
    expect(publisher.start).not.toHaveBeenCalled();
    capture.resolve();
    await start;
    expect(publisher.start).toHaveBeenCalledWith('rtmp://localhost/live/test');
    expect(controller.getSnapshot()).toMatchObject({
      ready: true,
      state: 'publishing',
      isBusy: false,
    });
    await controller.deactivate();
  });

  it('stops only publishing and supports another start with the same camera output', async () => {
    const { controller, camera, mic, mixer, publisher } = fixture();
    await controller.activate(CONFIG);
    const output = controller.getSnapshot().cameraOutput;
    await controller.start('rtmp://localhost/live/first');
    await controller.stop();
    expect(controller.getSnapshot()).toMatchObject({
      ready: true,
      state: 'stopped',
      cameraOutput: output,
    });
    expect(camera.dispose).not.toHaveBeenCalled();
    expect(mixer.removeLayer).not.toHaveBeenCalled();
    expect(mic.stop).not.toHaveBeenCalled();
    await controller.start('rtmp://localhost/live/second');
    expect(publisher.start).toHaveBeenCalledTimes(2);
    await controller.deactivate();
  });

  it('does not create anything after unmounting during a permission prompt', async () => {
    const { controller, dependencies } = fixture();
    const permissions = deferred();
    const requested = deferred();
    dependencies.requestPermissions.mockImplementationOnce(() => {
      requested.resolve();
      return permissions.promise;
    });
    controller.activate(CONFIG);
    await requested.promise;
    const cleanup = controller.deactivate();
    permissions.resolve();
    await cleanup;
    expect(dependencies.createCameraLayer).not.toHaveBeenCalled();
    expect(controller.getSnapshot()).toMatchObject({
      ready: false,
      mixer: undefined,
      cameraOutput: undefined,
    });
  });

  it('drops the camera output immediately and releases the layer once a pending microphone start settles', async () => {
    const { controller, camera, mic, mixer } = fixture();
    const pending = deferred();
    const micStarted = deferred();
    mic.start.mockImplementationOnce(() => {
      micStarted.resolve();
      return pending.promise;
    });
    controller.activate(CONFIG);
    await micStarted.promise;
    const cleanup = controller.deactivate();
    expect(controller.getSnapshot()).toMatchObject({
      ready: false,
      cameraOutput: undefined,
    });
    expect(camera.dispose).not.toHaveBeenCalled();
    pending.resolve();
    await cleanup;
    expect(mic.stop).toHaveBeenCalled();
    expect(mixer.removeLayer).toHaveBeenCalledWith(camera);
    expect(camera.dispose).toHaveBeenCalledTimes(1);
  });

  it('serializes cleanup and reactivation like a Strict Mode effect replay', async () => {
    const first = fixture();
    const second = fixture();
    const { controller, dependencies, mic } = first;
    await controller.activate(CONFIG);
    const stopped = deferred();
    mic.stop.mockReturnValue(stopped.promise);
    dependencies.createCameraLayer.mockReturnValueOnce(
      second.camera as unknown as CameraLayer
    );
    dependencies.createMicrophoneSource.mockReturnValueOnce(
      second.mic as unknown as MicrophoneSource
    );
    dependencies.createMixer.mockReturnValueOnce(
      second.mixer as unknown as Mixer
    );
    dependencies.createPublisher.mockReturnValueOnce(
      second.publisher as unknown as RtmpPublisher
    );
    const cleanup = controller.deactivate();
    const next = controller.activate(CONFIG);
    expect(second.mic.start).not.toHaveBeenCalled();
    stopped.resolve();
    await cleanup;
    await next;
    expect(first.camera.dispose).toHaveBeenCalledTimes(1);
    expect(second.mic.start).toHaveBeenCalledTimes(1);
    expect(controller.getSnapshot()).toMatchObject({
      ready: true,
      mixer: second.mixer,
      cameraOutput: second.camera.output,
    });
    await controller.deactivate();
  });

  it('survives immediate setup/cleanup/setup without creating the abandoned session', async () => {
    const { controller, dependencies } = fixture();
    controller.activate(CONFIG);
    controller.deactivate();
    await controller.activate(CONFIG);
    expect(dependencies.createCameraLayer).toHaveBeenCalledTimes(1);
    expect(controller.getSnapshot().ready).toBe(true);
    await controller.deactivate();
  });

  it('waits for an unmounted hook to release the microphone before a new hook opens it', async () => {
    const first = fixture();
    const second = fixture();
    await first.controller.activate(CONFIG);
    const stopped = deferred();
    first.mic.stop.mockReturnValue(stopped.promise);
    const cleanup = first.controller.deactivate();
    const next = second.controller.activate(CONFIG);
    expect(second.mic.start).not.toHaveBeenCalled();
    stopped.resolve();
    await cleanup;
    await next;
    expect(second.mic.start).toHaveBeenCalledTimes(1);
    await second.controller.deactivate();
  });

  it('reports capture failure and rolls back partially started resources', async () => {
    const { controller, camera, mic, mixer, publisher } = fixture();
    mic.start.mockRejectedValueOnce(
      new Error('permissionDenied: microphone access was denied')
    );
    controller.activate(CONFIG);
    await expect(
      controller.start('rtmp://localhost/live/test')
    ).rejects.toMatchObject({ code: 'permissionDenied' });
    expect(controller.getSnapshot()).toMatchObject({
      ready: false,
      isBusy: false,
      mixer: undefined,
      cameraOutput: undefined,
      error: { code: 'permissionDenied' },
    });
    expect(publisher.start).not.toHaveBeenCalled();
    expect(mixer.removeLayer).toHaveBeenCalledWith(camera);
    expect(camera.dispose).toHaveBeenCalled();
    expect(mixer.setAudioSource).toHaveBeenLastCalledWith(undefined);
    await controller.deactivate();
  });

  it('reports permission denial without allocating native objects', async () => {
    const { controller, dependencies } = fixture();
    dependencies.requestPermissions.mockRejectedValueOnce(
      new RtmpCaptureError('permissionDenied', 'Denied')
    );
    await expect(controller.activate(CONFIG)).rejects.toMatchObject({
      code: 'permissionDenied',
    });
    expect(controller.getSnapshot().error?.code).toBe('permissionDenied');
    expect(dependencies.createPublisher).not.toHaveBeenCalled();
    await controller.deactivate();
  });

  it('cancels a pending publish while preserving initialization', async () => {
    const { controller, dependencies, publisher } = fixture();
    const permissions = deferred();
    dependencies.requestPermissions.mockReturnValueOnce(permissions.promise);
    const prepare = controller.activate(CONFIG);
    const pending = controller.start('rtmp://localhost/live/test');
    const rejected = pending.catch((error: unknown) => error);
    await controller.stop();
    permissions.resolve();
    await prepare;
    expect(await rejected).toMatchObject({ code: 'notReady' });
    expect(publisher.start).not.toHaveBeenCalled();
    expect(controller.getSnapshot()).toMatchObject({
      ready: true,
      error: null,
      isBusy: false,
    });
    await controller.deactivate();
  });

  it('interrupts a connecting publisher without waiting for start to resolve', async () => {
    const { controller, publisher } = fixture();
    await controller.activate(CONFIG);
    const connected = deferred();
    const connecting = deferred();
    publisher.start.mockImplementationOnce(() => {
      connecting.resolve();
      return connected.promise;
    });
    publisher.stop.mockImplementation(() => {
      connected.reject(new Error('notReady: stopped before publishing'));
      return Promise.resolve();
    });
    const pending = controller.start('rtmp://localhost/live/test');
    const rejected = pending.catch((error: unknown) => error);
    await connecting.promise;
    await controller.stop();
    expect(await rejected).toMatchObject({ code: 'notReady' });
    expect(controller.getSnapshot()).toMatchObject({
      state: 'stopped',
      ready: true,
      error: null,
      isBusy: false,
    });
    await controller.deactivate();
  });

  it('rejects duplicate starts and shares a pending native stop', async () => {
    const { controller, publisher } = fixture();
    await controller.activate(CONFIG);
    const first = controller.start('rtmp://localhost/live/test');
    await expect(
      controller.start('rtmp://localhost/live/test')
    ).rejects.toMatchObject({ code: 'notReady' });
    await first;
    const stopped = deferred();
    publisher.stop.mockReturnValueOnce(stopped.promise);
    const one = controller.stop();
    const two = controller.stop();
    stopped.resolve();
    await Promise.all([one, two]);
    expect(publisher.stop).toHaveBeenCalledTimes(1);
    expect(controller.getSnapshot().isBusy).toBe(false);
    await controller.deactivate();
  });

  it('rejects start while inactive and makes stop a no-op', async () => {
    const { controller, publisher } = fixture();
    await expect(
      controller.start('rtmp://localhost/live/test')
    ).rejects.toMatchObject({ code: 'notReady' });
    await controller.stop();
    expect(publisher.stop).not.toHaveBeenCalled();
  });

  it('applies mute state before and after activation without reopening capture', async () => {
    const { controller, mic } = fixture();
    controller.setMuted(true);
    await controller.activate(CONFIG);
    expect(mic.muted).toBe(true);
    controller.setMuted(false);
    expect(controller.getSnapshot()).toMatchObject({ muted: false });
    expect(mic.muted).toBe(false);
    expect(mic.start).toHaveBeenCalledTimes(1);
    await controller.deactivate();
  });

  it('does not request or create a microphone when audio is disabled', async () => {
    const { controller, dependencies, mixer } = fixture();
    await controller.activate({ ...CONFIG, audio: false });
    expect(dependencies.requestPermissions).toHaveBeenCalledWith(false);
    expect(dependencies.createMicrophoneSource).not.toHaveBeenCalled();
    expect(mixer.setAudioSource).toHaveBeenCalledWith(undefined);
    expect(controller.getSnapshot().ready).toBe(true);
    await controller.deactivate();
  });

  it('updates subscribers, supports unsubscription and ignores stale native callbacks', async () => {
    const {
      controller,
      publisher,
      emitState,
      emitPublisherError,
      emitCaptureError,
    } = fixture();
    await controller.activate(CONFIG);
    const listener = jest.fn();
    const unsubscribe = controller.subscribe(listener);
    const previous = controller.getSnapshot();
    expect(controller.getSnapshot()).toBe(previous);
    emitState('publishing');
    expect(controller.getSnapshot()).not.toBe(previous);
    expect(listener).toHaveBeenCalledTimes(1);
    unsubscribe();
    emitPublisherError({ code: 'socketClosed', message: 'Disconnected' });
    expect(controller.getSnapshot().error).toMatchObject({
      code: 'socketClosed',
    });
    emitCaptureError({ code: 'encoderFailed', message: 'Encoder failed' });
    expect(controller.getSnapshot().error).toBeInstanceOf(RtmpCaptureError);
    expect(listener).toHaveBeenCalledTimes(1);
    const lateCallback = publisher.onStateChange.mock.calls[0]![0];
    await controller.deactivate();
    const stopped = controller.getSnapshot();
    lateCallback('publishing');
    expect(controller.getSnapshot()).toBe(stopped);
  });

  it('cleans every resource even if stopping one fails', async () => {
    const { controller, camera, mic, mixer } = fixture();
    await controller.activate(CONFIG);
    mic.stop.mockRejectedValue(new Error('microphone stop failed'));
    await controller.deactivate();
    expect(mixer.removeLayer).toHaveBeenCalledWith(camera);
    expect(camera.dispose).toHaveBeenCalled();
    expect(controller.getSnapshot().ready).toBe(false);
  });

  it('polls stats only when requested and clears the timer on deactivation', async () => {
    jest.useFakeTimers();
    const { controller } = fixture();
    await controller.activate({ ...CONFIG, statsIntervalMs: 500 });
    expect(controller.getSnapshot().stats).toBeNull();
    jest.advanceTimersByTime(500);
    expect(controller.getSnapshot().stats).toMatchObject({
      publisher: { bytesSent: 123 },
    });
    await controller.deactivate();
    expect(jest.getTimerCount()).toBe(0);
  });

  it('validates configuration before touching permissions or native objects', async () => {
    const { controller, dependencies } = fixture();
    await expect(
      controller.activate({ ...CONFIG, video: { ...CONFIG.video, width: 0 } })
    ).rejects.toMatchObject({ code: 'configurationFailed' });
    expect(dependencies.requestPermissions).not.toHaveBeenCalled();
    await controller.deactivate();
  });
});
