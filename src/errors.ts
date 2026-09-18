import type { CaptureError, CaptureErrorCode } from './specs/Mixer.nitro';
import type {
  PublisherError,
  PublisherErrorCode,
} from './specs/RtmpPublisher.nitro';

// Native rejections read "<code>: <message>" (iOS) or
// "java.lang.Exception: <code>: <message>" (Android, fbjni uses toString()).
// Only the first line carries the code and message; a Java stack trace may follow.
function parseNativeError<Code extends string>(
  error: unknown,
  codes: readonly Code[]
): { code: Code; message: string } | null {
  const text = error instanceof Error ? error.message : String(error);
  const firstLine = text.split('\n')[0] ?? text;
  const pattern = new RegExp(
    `(?:^|[^A-Za-z])(${codes.join('|')}): ([\\s\\S]*)$`
  );
  const match = pattern.exec(firstLine);
  if (match === null) {
    return null;
  }
  return { code: match[1] as Code, message: match[2] ?? '' };
}

const PUBLISHER_ERROR_CODES: readonly PublisherErrorCode[] = [
  'invalidUrl',
  'handshakeFailed',
  'connectRejected',
  'invalidApp',
  'publishBadName',
  'streamAlreadyExists',
  'serverClosed',
  'protocolError',
  'notReady',
  'connectFailed',
  'tlsFailed',
  'socketClosed',
  'timeout',
];

const CAPTURE_ERROR_CODES: readonly CaptureErrorCode[] = [
  'permissionDenied',
  'cameraUnavailable',
  'configurationFailed',
  'encoderFailed',
];

/**
 * What `publisher.start()` rejects with: an `Error` that is also a
 * `PublisherError`, so `catch (e) { e.code }` works the same way as the
 * `onError` callback.
 */
export class RtmpPublisherError extends Error implements PublisherError {
  readonly code: PublisherErrorCode;

  constructor(code: PublisherErrorCode, message: string) {
    super(message);
    this.name = 'RtmpPublisherError';
    this.code = code;
  }
}

/**
 * Converts a rejection reason from the native `start()` into a
 * `RtmpPublisherError`. Unknown errors keep their text under `protocolError`.
 */
export function toPublisherError(error: unknown): RtmpPublisherError {
  if (error instanceof RtmpPublisherError) {
    return error;
  }
  const parsed = parseNativeError(error, PUBLISHER_ERROR_CODES);
  if (parsed !== null) {
    return new RtmpPublisherError(parsed.code, parsed.message);
  }
  const text = error instanceof Error ? error.message : String(error);
  return new RtmpPublisherError('protocolError', text);
}

/**
 * What `camera.start()`, `mic.start()` and `image.load()` reject with
 * a `CaptureError` that is also an `Error`. Capture errors are
 * never mixed into `PublisherError`; they have nothing to do with the session.
 */
export class RtmpCaptureError extends Error implements CaptureError {
  readonly code: CaptureErrorCode;

  constructor(code: CaptureErrorCode, message: string) {
    super(message);
    this.name = 'RtmpCaptureError';
    this.code = code;
  }
}

/** Unknown errors keep their text under `configurationFailed`. */
export function toCaptureError(error: unknown): RtmpCaptureError {
  if (error instanceof RtmpCaptureError) {
    return error;
  }
  const parsed = parseNativeError(error, CAPTURE_ERROR_CODES);
  if (parsed !== null) {
    return new RtmpCaptureError(parsed.code, parsed.message);
  }
  const text = error instanceof Error ? error.message : String(error);
  return new RtmpCaptureError('configurationFailed', text);
}

/**
 * Replaces promise-returning methods of a native object so their rejections
 * are converted with `convert`. The replacement is non-enumerable and keeps
 * the native method's `this`.
 */
export function wrapRejections<T extends object>(
  object: T,
  methods: readonly (keyof T)[],
  convert: (error: unknown) => Error
): T {
  for (const name of methods) {
    const native = object[name] as unknown as (
      ...args: unknown[]
    ) => Promise<unknown>;
    const wrapped = (...args: unknown[]): Promise<unknown> =>
      native.apply(object, args).catch((error: unknown) => {
        throw convert(error);
      });
    Object.defineProperty(object, name, {
      configurable: true,
      enumerable: false,
      writable: false,
      value: wrapped,
    });
  }
  return object;
}
