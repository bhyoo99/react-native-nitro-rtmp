package com.margelo.nitro.nitrortmp

import androidx.annotation.Keep

/**
 * JNI bridge to the core C facade (cpp/nitrortmp/nitrortmp_c.h), one instance
 * per session (android/src/main/cpp/NativePublisher.cpp is the other half).
 *
 * Every method must be called on the session thread. The three `dispatch*`
 * methods are invoked by C++ synchronously from inside those calls, on that
 * same thread, and must not call back into this object.
 */
@Keep
internal class NativePublisher(private val listener: Listener) {
  interface Listener {
    fun onNativeSend(data: ByteArray)
    fun onNativeState(state: Int)
    fun onNativeError(code: Int, message: String)
  }

  private var handle: Long = nativeCreate()

  fun destroy() {
    if (handle != 0L) {
      nativeDestroy(handle)
      handle = 0L
    }
  }

  /** Emits C0+C1 through [Listener.onNativeSend]. False when onNativeError was called instead. */
  fun start(url: String): Boolean = nativeStart(handle, url)
  fun onReceive(data: ByteArray, length: Int) = nativeOnReceive(handle, data, length)
  fun stop() = nativeStop(handle)
  fun pushVideo(frame: ByteArray, ptsMs: Int, dtsMs: Int): Boolean = nativePushVideo(handle, frame, ptsMs, dtsMs)
  fun pushAudio(frame: ByteArray, ptsMs: Int): Boolean = nativePushAudio(handle, frame, ptsMs)
  fun setMetadata(
    width: Int, height: Int, frameRate: Double, videoBitrateKbps: Double,
    audioSampleRate: Int, audioChannels: Int, audioBitrateKbps: Double, encoder: String?,
  ) = nativeSetMetadata(handle, width, height, frameRate, videoBitrateKbps, audioSampleRate, audioChannels, audioBitrateKbps, encoder)

  /** A NITRORTMP_STATE_* value. */
  fun state(): Int = nativeState(handle)

  /**
   * [bytesSent, videoTags, audioTags, scriptTags, rejectedFrames, droppedBeforeKeyframe,
   * invalidFrames, timestampClamps, lastVideoTimestamp, lastAudioTimestamp]
   */
  fun stats(): LongArray = nativeStats(handle)

  // Called from C++. Names and signatures are part of the JNI contract.
  @Keep
  private fun dispatchSend(data: ByteArray) = listener.onNativeSend(data)

  @Keep
  private fun dispatchState(state: Int) = listener.onNativeState(state)

  @Keep
  private fun dispatchError(code: Int, message: String) = listener.onNativeError(code, message)

  private external fun nativeCreate(): Long
  private external fun nativeDestroy(handle: Long)
  private external fun nativeStart(handle: Long, url: String): Boolean
  private external fun nativeOnReceive(handle: Long, data: ByteArray, length: Int)
  private external fun nativeStop(handle: Long)
  private external fun nativePushVideo(handle: Long, frame: ByteArray, ptsMs: Int, dtsMs: Int): Boolean
  private external fun nativePushAudio(handle: Long, frame: ByteArray, ptsMs: Int): Boolean
  private external fun nativeSetMetadata(
    handle: Long, width: Int, height: Int, frameRate: Double, videoBitrateKbps: Double,
    audioSampleRate: Int, audioChannels: Int, audioBitrateKbps: Double, encoder: String?,
  )
  private external fun nativeState(handle: Long): Int
  private external fun nativeStats(handle: Long): LongArray

  companion object {
    const val STATE_IDLE = 0
    const val STATE_CONNECTING = 1
    const val STATE_CONNECTED = 2
    const val STATE_PUBLISHING = 3
    const val STATE_STOPPED = 4
    const val STATE_FAILED = 5

    const val ERROR_INVALID_URL = 0
    const val ERROR_NOT_READY = 8
    const val ERROR_CONNECT_FAILED = 9
    const val ERROR_TLS_FAILED = 10
    const val ERROR_SOCKET_CLOSED = 11
    const val ERROR_TIMEOUT = 12

    /** `[host, port, "1" | "0"]` for a valid rtmp:// or rtmps:// URL, else null. */
    @JvmStatic
    external fun urlEndpoint(url: String): Array<String>?

    /** JS union string ("publishing"), null when out of range. */
    @JvmStatic
    external fun stateName(state: Int): String?

    /** JS union string ("publishBadName"), null when out of range. */
    @JvmStatic
    external fun errorName(code: Int): String?
  }
}
