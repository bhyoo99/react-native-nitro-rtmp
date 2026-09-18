package com.margelo.nitro.nitrortmp

/**
 * Where a mixer's encoders deliver their output: the
 * session implements it and posts to its queue. Not part of the Nitro spec.
 * The arrays handed in are private copies; timestamps are milliseconds on
 * the mixer's clock (the core normalizes the first frame to zero).
 */
internal interface EncodedFrameSink {
  /** One H.264 Annex-B access unit; keyframes carry SPS/PPS. Any thread. */
  fun pushEncodedVideo(annexb: ByteArray, ptsMs: Int, dtsMs: Int)

  /** One ADTS AAC frame (protection_absent = 1). Any thread. */
  fun pushEncodedAudio(adts: ByteArray, ptsMs: Int)
}
