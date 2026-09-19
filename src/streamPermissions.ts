import { PermissionsAndroid, Platform } from 'react-native';
import { RtmpCaptureError } from './errors';

export async function requestStreamPermissions(audio: boolean): Promise<void> {
  // iOS sources request access themselves. Android sources only check it.
  if (Platform.OS !== 'android') return;
  const permissions = audio
    ? [
        PermissionsAndroid.PERMISSIONS.CAMERA,
        PermissionsAndroid.PERMISSIONS.RECORD_AUDIO,
      ]
    : [PermissionsAndroid.PERMISSIONS.CAMERA];
  const results = await PermissionsAndroid.requestMultiple(permissions);
  if (
    permissions.some(
      (permission) => results[permission] !== PermissionsAndroid.RESULTS.GRANTED
    )
  ) {
    throw new RtmpCaptureError(
      'permissionDenied',
      'Camera or microphone permission was denied.'
    );
  }
}
