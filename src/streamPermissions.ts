import { PermissionsAndroid, Platform } from 'react-native';
import { RtmpCaptureError } from './errors';

/**
 * The microphone permission, when audio is on. The camera is VisionCamera's:
 * request it there (`useCameraPermission`) before the session starts.
 */
export async function requestStreamPermissions(audio: boolean): Promise<void> {
  // The iOS microphone source requests access itself. Android sources only check it.
  if (!audio || Platform.OS !== 'android') return;
  const result = await PermissionsAndroid.request(
    PermissionsAndroid.PERMISSIONS.RECORD_AUDIO
  );
  if (result !== PermissionsAndroid.RESULTS.GRANTED) {
    throw new RtmpCaptureError(
      'permissionDenied',
      'Microphone permission was denied.'
    );
  }
}
