import {
  RtmpCaptureError,
  RtmpPublisherError,
  toCaptureError,
  toPublisherError,
} from './errors';
import type { CameraPosition, CameraSource } from './specs/CameraSource.nitro';
import type { MicrophoneSource } from './specs/MicrophoneSource.nitro';
import type { AudioSettings, Mixer, VideoSettings } from './specs/Mixer.nitro';
import type { RtmpPublisher } from './specs/RtmpPublisher.nitro';
import type { RtmpStream } from './streamTypes';

export interface StreamConfiguration {
  audio: boolean;
  video: VideoSettings;
  audioSettings: AudioSettings;
  statsIntervalMs: number;
}

interface Dependencies {
  createPublisher(): RtmpPublisher;
  createMixer(): Mixer;
  createCameraSource(): CameraSource;
  createMicrophoneSource(): MicrophoneSource;
  requestPermissions(audio: boolean): Promise<void>;
}

interface Session {
  prepare: Promise<void>;
  publisher?: RtmpPublisher;
  mixer?: Mixer;
  camera?: CameraSource;
  mic?: MicrophoneSource;
  stopTask?: Promise<void>;
  releaseTask?: Promise<void>;
  timer?: ReturnType<typeof setInterval>;
  publishVersion: number;
}

type Snapshot = Pick<
  RtmpStream,
  | 'state'
  | 'ready'
  | 'isBusy'
  | 'error'
  | 'camera'
  | 'muted'
  | 'stats'
  | 'mixer'
>;
const noop = () => {};
// A different component may mount while the previous hook's async cleanup is pending.
// Capture ownership must pass only after that cleanup, even across controller instances.
let captureCleanup = Promise.resolve();
const cancelled = () =>
  new RtmpPublisherError(
    'notReady',
    'The stream is inactive or start() was cancelled.'
  );

function validate(config: StreamConfiguration) {
  const { video, audioSettings, statsIntervalMs } = config;
  const positive = [
    video.width,
    video.height,
    video.frameRate,
    video.bitrateKbps,
    video.keyframeIntervalSeconds ?? 2,
    audioSettings.bitrateKbps,
  ];
  if (
    positive.some((value) => !Number.isFinite(value) || value <= 0) ||
    !Number.isInteger(video.width / 2) ||
    !Number.isInteger(video.height / 2) ||
    audioSettings.sampleRate !== 48000 ||
    ![1, 2].includes(audioSettings.channels) ||
    !Number.isFinite(statsIntervalMs) ||
    statsIntervalMs < 0
  ) {
    throw new RtmpCaptureError(
      'configurationFailed',
      'Use positive video settings, even pixel dimensions, 48000 Hz audio with 1 or 2 channels, and a non-negative stats interval.'
    );
  }
}

/** Internal lifecycle owner. Construction is pure; native objects are created after activation. */
export class RtmpStreamController {
  private current?: Session;
  private cleanup = Promise.resolve();
  private listeners = new Set<() => void>();
  private snapshot: Snapshot;

  constructor(
    private dependencies: Dependencies,
    camera: CameraPosition
  ) {
    this.snapshot = {
      state: 'idle',
      ready: false,
      isBusy: false,
      error: null,
      camera,
      muted: false,
      stats: null,
      mixer: undefined,
    };
  }

  getSnapshot = (): Snapshot => this.snapshot;

  subscribe = (listener: () => void): (() => void) => {
    this.listeners.add(listener);
    return () => {
      this.listeners.delete(listener);
    };
  };

  private update(change: Partial<Snapshot>) {
    this.snapshot = { ...this.snapshot, ...change };
    this.listeners.forEach((listener) => listener());
  }

  activate(config: StreamConfiguration): Promise<void> {
    this.deactivate();
    const session: Session = { prepare: Promise.resolve(), publishVersion: 0 };
    this.current = session;
    this.update({
      state: 'idle',
      ready: false,
      isBusy: false,
      error: null,
      stats: null,
    });
    session.prepare = this.prepare(session, config).catch(
      async (error: unknown) => {
        const failure = toCaptureError(error);
        if (this.current === session) {
          this.update({ error: failure, ready: false, mixer: undefined });
        }
        await this.release(session);
        throw failure;
      }
    );
    // Initialization errors are exposed as state even when start() is never called.
    session.prepare.catch(noop);
    return session.prepare;
  }

  private async prepare(session: Session, config: StreamConfiguration) {
    // A permission prompt or native start may still be finishing for the previous mount.
    await Promise.all([this.cleanup, captureCleanup]);
    if (this.current !== session) return;
    validate(config);
    await this.dependencies.requestPermissions(config.audio);
    if (this.current !== session) return;

    const publisher = (session.publisher = this.dependencies.createPublisher());
    const mixer = (session.mixer = this.dependencies.createMixer());
    const camera = (session.camera = this.dependencies.createCameraSource());
    const mic = config.audio
      ? (session.mic = this.dependencies.createMicrophoneSource())
      : undefined;
    camera.position = this.snapshot.camera;
    if (mic) mic.muted = this.snapshot.muted;
    mixer.video = config.video;
    mixer.audio = config.audioSettings;
    mixer.addLayer(camera);
    mixer.setAudioSource(mic);
    publisher.setMixer(mixer);
    publisher.setMetadata({
      width: config.video.width,
      height: config.video.height,
      frameRate: config.video.frameRate,
      videoBitrateKbps: config.video.bitrateKbps,
      audioSampleRate: config.audio ? config.audioSettings.sampleRate : 0,
      audioChannels: config.audio ? config.audioSettings.channels : 0,
      audioBitrateKbps: config.audio ? config.audioSettings.bitrateKbps : 0,
    });
    publisher.onStateChange((state) => {
      if (this.current === session) this.update({ state });
    });
    publisher.onError((error) => {
      if (this.current === session)
        this.update({
          error: new RtmpPublisherError(error.code, error.message),
        });
    });
    mixer.onError((error) => {
      if (this.current === session)
        this.update({ error: new RtmpCaptureError(error.code, error.message) });
    });
    this.update({ mixer });

    await camera.start();
    if (this.current !== session) return;
    if (mic) await mic.start();
    if (this.current !== session) return;
    this.update({ ready: true });

    if (config.statsIntervalMs > 0) {
      session.timer = setInterval(() => {
        if (this.current === session) {
          this.update({
            stats: { publisher: publisher.stats, mixer: mixer.stats },
          });
        }
      }, config.statsIntervalMs);
    }
  }

  deactivate = (): Promise<void> => {
    const session = this.current;
    if (!session) return this.cleanup;
    this.current = undefined;
    session.publishVersion++;
    if (session.timer) clearInterval(session.timer);
    this.update({
      ready: false,
      isBusy: false,
      mixer: undefined,
      stats: null,
      state: 'stopped',
    });
    // Interrupt connecting immediately; don't wait for the start promise to resolve.
    const stop = this.stopPublisher(session);
    this.cleanup = Promise.allSettled([
      session.prepare,
      stop,
      Promise.resolve().then(() => session.camera?.stop()),
      Promise.resolve().then(() => session.mic?.stop()),
    ]).then(() => this.release(session));
    captureCleanup = Promise.allSettled([captureCleanup, this.cleanup]).then(
      noop
    );
    return this.cleanup;
  };

  private stopPublisher(session: Session): Promise<void> {
    if (session.stopTask) return session.stopTask;
    if (!session.publisher) return Promise.resolve();
    const publisher = session.publisher;
    session.stopTask = Promise.resolve()
      .then(() => publisher.stop())
      .finally(() => {
        session.stopTask = undefined;
      });
    return session.stopTask;
  }

  private release(session: Session): Promise<void> {
    if (session.releaseTask) return session.releaseTask;
    if (session.timer) clearInterval(session.timer);
    // Try every cleanup even if a native method throws or rejects. Native objects can
    // then be collected; never dispose a mixer while a native preview may still retain it.
    session.releaseTask = (async () => {
      await Promise.allSettled([
        Promise.resolve().then(() => session.publisher?.onStateChange(noop)),
        Promise.resolve().then(() => session.publisher?.onError(noop)),
        Promise.resolve().then(() => session.mixer?.onError(noop)),
        Promise.resolve().then(() => session.publisher?.setMixer(undefined)),
        this.stopPublisher(session),
        Promise.resolve().then(() => session.camera?.stop()),
        Promise.resolve().then(() => session.mic?.stop()),
      ]);
      await Promise.allSettled([
        Promise.resolve().then(() => session.mixer?.setAudioSource(undefined)),
        Promise.resolve().then(() => {
          if (session.camera) session.mixer?.removeLayer(session.camera);
        }),
      ]);
    })();
    return session.releaseTask;
  }

  start = async (url: string): Promise<void> => {
    const session = this.current;
    if (!session) throw cancelled();
    if (
      this.snapshot.isBusy ||
      ['connecting', 'connected', 'publishing'].includes(this.snapshot.state)
    ) {
      throw new RtmpPublisherError(
        'notReady',
        'A stream operation is already in progress. Call stop() first.'
      );
    }
    const version = ++session.publishVersion;
    this.update({ isBusy: true, error: null });
    try {
      await session.prepare;
      if (
        this.current !== session ||
        version !== session.publishVersion ||
        !session.publisher
      )
        throw cancelled();
      await session.publisher.start(url);
      if (this.current !== session || version !== session.publishVersion)
        throw cancelled();
      this.update({ state: 'publishing' });
    } catch (error: unknown) {
      const failure =
        error instanceof RtmpCaptureError ? error : toPublisherError(error);
      if (this.current === session && version === session.publishVersion) {
        this.update({ error: failure });
      }
      throw failure;
    } finally {
      if (this.current === session && version === session.publishVersion)
        this.update({ isBusy: false });
    }
  };

  stop = async (): Promise<void> => {
    const session = this.current;
    if (!session) return;
    const version = ++session.publishVersion;
    this.update({ isBusy: true });
    try {
      await this.stopPublisher(session);
      if (this.current === session && version === session.publishVersion)
        this.update({ state: 'stopped' });
    } catch (error: unknown) {
      const failure = toPublisherError(error);
      if (this.current === session) this.update({ error: failure });
      throw failure;
    } finally {
      if (this.current === session && version === session.publishVersion)
        this.update({ isBusy: false });
    }
  };

  setCameraPosition = (camera: CameraPosition): void => {
    if (this.current?.camera) this.current.camera.position = camera;
    if (camera !== this.snapshot.camera) this.update({ camera });
  };

  flipCamera = (): void => {
    this.setCameraPosition(this.snapshot.camera === 'back' ? 'front' : 'back');
  };

  setMuted = (muted: boolean): void => {
    if (this.current?.mic) this.current.mic.muted = muted;
    if (muted !== this.snapshot.muted) this.update({ muted });
  };
}
