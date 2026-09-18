import { describe, expect, it } from '@jest/globals';
import {
  RtmpCaptureError,
  RtmpPublisherError,
  toCaptureError,
  toPublisherError,
  wrapRejections,
} from '../errors';

describe('toPublisherError', () => {
  it('parses an iOS rejection', () => {
    const e = toPublisherError(
      new Error('connectFailed: Connection refused (POSIX 61)')
    );
    expect(e).toBeInstanceOf(RtmpPublisherError);
    expect(e.code).toBe('connectFailed');
    expect(e.message).toBe('Connection refused (POSIX 61)');
  });

  it('parses an Android rejection with a class prefix and a stack trace', () => {
    const e = toPublisherError(
      new Error(
        'java.lang.Exception: publishBadName: rejected by server\n\tat foo(Bar.kt:1)'
      )
    );
    expect(e.code).toBe('publishBadName');
    expect(e.message).toBe('rejected by server');
  });

  it('keeps unknown text under protocolError', () => {
    const e = toPublisherError('something odd');
    expect(e.code).toBe('protocolError');
    expect(e.message).toBe('something odd');
  });

  it('returns an existing error as is', () => {
    const original = new RtmpPublisherError('timeout', 'x');
    expect(toPublisherError(original)).toBe(original);
  });
});

describe('toCaptureError', () => {
  it('parses capture codes and never publisher codes', () => {
    const e = toCaptureError(
      new Error('permissionDenied: camera access was denied')
    );
    expect(e).toBeInstanceOf(RtmpCaptureError);
    expect(e.code).toBe('permissionDenied');
    expect(e.message).toBe('camera access was denied');
    expect(toCaptureError(new Error('timeout: x')).code).toBe(
      'configurationFailed'
    );
  });
});

describe('wrapRejections', () => {
  it('converts rejections and keeps this', async () => {
    const object = {
      tag: 'native',
      start(): Promise<void> {
        return Promise.reject(
          new Error(`cameraUnavailable: no camera on ${this.tag}`)
        );
      },
      stop(): Promise<void> {
        return Promise.resolve();
      },
    };
    wrapRejections(object, ['start', 'stop'], toCaptureError);
    await expect(object.start()).rejects.toMatchObject({
      code: 'cameraUnavailable',
      message: 'no camera on native',
    });
    await expect(object.stop()).resolves.toBeUndefined();
    expect(Object.keys(object)).toEqual(['tag']);
  });
});
