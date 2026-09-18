package com.margelo.nitro.nitrortmp

import androidx.annotation.Keep

/**
 * JNI bridge to the encoder output helpers of the C facade
 * (cpp/nitrortmp/nitrortmp_c.h):
 * android/src/main/cpp/NativeCodec.cpp is the other half.
 *
 * Every function is stateless and may be called from any thread. Byte
 * arrays are read only up to `length` so callers can hand in reusable buffers.
 */
@Keep
internal object NativeCodec {
  /**
   * 7-byte ADTS header (protection_absent = 1) for one raw AAC frame of
   * [payloadSize] bytes; [profile] is the audio object type minus one (AAC-LC = 1).
   * Null when a value is out of range.
   */
  @JvmStatic
  external fun writeAdtsHeader(profile: Int, sampleRate: Int, channels: Int, payloadSize: Int): ByteArray?

  /** True when the Annex-B buffer contains an IDR slice (what the core's keyframe gate accepts). */
  @JvmStatic
  external fun isKeyframe(annexb: ByteArray, length: Int): Boolean

  /** True when an SPS NALU comes before the first slice. */
  @JvmStatic
  external fun hasParameterSets(annexb: ByteArray, length: Int): Boolean

  /** start code + sps + start code + pps + annexb[0, length). Empty parameter sets are skipped. */
  @JvmStatic
  external fun prependParameterSets(sps: ByteArray, pps: ByteArray, annexb: ByteArray, length: Int): ByteArray
}
