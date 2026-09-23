import { afterEach, describe, expect, it, jest } from '@jest/globals';
import { PermissionsAndroid, Platform } from 'react-native';
import { requestStreamPermissions } from '../streamPermissions';

const originalPlatform = Platform.OS;
afterEach(() => {
  Platform.OS = originalPlatform;
  jest.restoreAllMocks();
});

describe('requestStreamPermissions', () => {
  it('requests only the microphone on Android; the camera is VisionCamera’s', async () => {
    Platform.OS = 'android';
    const request = jest
      .spyOn(PermissionsAndroid, 'request')
      .mockResolvedValue('granted');
    await requestStreamPermissions(true);
    expect(request).toHaveBeenCalledWith(
      PermissionsAndroid.PERMISSIONS.RECORD_AUDIO
    );
  });

  it('requests nothing for a video-only stream', async () => {
    Platform.OS = 'android';
    const request = jest.spyOn(PermissionsAndroid, 'request');
    await requestStreamPermissions(false);
    expect(request).not.toHaveBeenCalled();
  });

  it('rejects when microphone access is permanently denied', async () => {
    Platform.OS = 'android';
    jest
      .spyOn(PermissionsAndroid, 'request')
      .mockResolvedValue('never_ask_again');
    await expect(requestStreamPermissions(true)).rejects.toMatchObject({
      code: 'permissionDenied',
    });
  });

  it('leaves the iOS microphone permission to the native source', async () => {
    Platform.OS = 'ios';
    const request = jest.spyOn(PermissionsAndroid, 'request');
    await requestStreamPermissions(true);
    expect(request).not.toHaveBeenCalled();
  });
});
