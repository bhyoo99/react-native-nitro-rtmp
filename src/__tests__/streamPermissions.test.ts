import { afterEach, describe, expect, it, jest } from '@jest/globals';
import { PermissionsAndroid, Platform } from 'react-native';
import { requestStreamPermissions } from '../streamPermissions';

const originalPlatform = Platform.OS;
afterEach(() => {
  Platform.OS = originalPlatform;
  jest.restoreAllMocks();
});

describe('requestStreamPermissions', () => {
  it('requests only the camera for a video-only stream on Android', async () => {
    Platform.OS = 'android';
    const request = jest
      .spyOn(PermissionsAndroid, 'requestMultiple')
      .mockResolvedValue({
        [PermissionsAndroid.PERMISSIONS.CAMERA]: 'granted',
      } as Awaited<ReturnType<typeof PermissionsAndroid.requestMultiple>>);
    await requestStreamPermissions(false);
    expect(request).toHaveBeenCalledWith([
      PermissionsAndroid.PERMISSIONS.CAMERA,
    ]);
  });

  it('rejects when microphone access is permanently denied', async () => {
    Platform.OS = 'android';
    jest.spyOn(PermissionsAndroid, 'requestMultiple').mockResolvedValue({
      [PermissionsAndroid.PERMISSIONS.CAMERA]: 'granted',
      [PermissionsAndroid.PERMISSIONS.RECORD_AUDIO]: 'never_ask_again',
    } as Awaited<ReturnType<typeof PermissionsAndroid.requestMultiple>>);
    await expect(requestStreamPermissions(true)).rejects.toMatchObject({
      code: 'permissionDenied',
    });
  });

  it('leaves iOS permission handling to the native sources', async () => {
    Platform.OS = 'ios';
    const request = jest.spyOn(PermissionsAndroid, 'requestMultiple');
    await requestStreamPermissions(true);
    expect(request).not.toHaveBeenCalled();
  });
});
