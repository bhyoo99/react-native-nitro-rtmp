package com.margelo.nitro.nitrortmp.encoder

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.view.Surface
import com.margelo.nitro.nitrortmp.NativeCodec
import java.nio.ByteBuffer

/**
 * MediaCodec H.264 encoder with a Surface input: no
 * B-frames, real time, CBR when the device supports it, keyframe every
 * `keyframeIntervalSeconds`. Output is normalized to what the core wants:
 * Annex-B access units with SPS/PPS in front of every keyframe (the codec
 * config buffer is kept and prepended).
 *
 * The callback thread is the codec's own handler thread; the listener copies
 * nothing (the byte array handed over is already a private copy) and only
 * posts to the session queue.
 */
internal class H264Encoder(
  private val width: Int,
  private val height: Int,
  private val frameRate: Int,
  private val bitrateKbps: Int,
  private val keyframeIntervalSeconds: Double,
  private val listener: Listener,
) {
  interface Listener {
    /** One Annex-B access unit; keyframes carry SPS/PPS. Called on the encoder thread. */
    fun onEncodedVideo(annexb: ByteArray, presentationTimeUs: Long, keyframe: Boolean)
    fun onEncoderError(message: String)
  }

  private val thread = HandlerThread("nitrortmp.encoder").apply { start() }
  private val handler = Handler(thread.looper)
  private var codec: MediaCodec? = null
  private var sps: ByteArray = ByteArray(0)
  private var pps: ByteArray = ByteArray(0)
  @Volatile private var released = false

  /** The mixer's render thread makes this an EGL window surface. */
  val inputSurface: Surface

  private val callback = object : MediaCodec.Callback() {
    override fun onInputBufferAvailable(codec: MediaCodec, index: Int) {
      // Surface input: nothing to queue.
    }

    override fun onOutputBufferAvailable(codec: MediaCodec, index: Int, info: MediaCodec.BufferInfo) {
      if (released) return
      val buffer: ByteBuffer? = try {
        codec.getOutputBuffer(index)
      } catch (e: IllegalStateException) {
        null
      }
      if (buffer != null) {
        buffer.position(info.offset)
        buffer.limit(info.offset + info.size)
        if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) {
          rememberParameterSets(buffer, info.size)
        } else if (info.size > 0) {
          val frame = ByteArray(info.size)
          buffer.get(frame)
          val keyframe = (info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME != 0) ||
            NativeCodec.isKeyframe(frame, frame.size)
          val normalized = if (keyframe && !NativeCodec.hasParameterSets(frame, frame.size)) {
            NativeCodec.prependParameterSets(sps, pps, frame, frame.size)
          } else {
            frame
          }
          listener.onEncodedVideo(normalized, info.presentationTimeUs, keyframe)
        }
      }
      try {
        codec.releaseOutputBuffer(index, false)
      } catch (e: IllegalStateException) {
        // stopped meanwhile
      }
    }

    override fun onError(codec: MediaCodec, e: MediaCodec.CodecException) {
      if (released) return
      listener.onEncoderError("MediaCodec error ${e.errorCode}: ${e.diagnosticInfo}")
    }

    override fun onOutputFormatChanged(codec: MediaCodec, format: MediaFormat) {
      // Some codecs deliver SPS/PPS here only (csd-0/csd-1), not as a config buffer.
      if (sps.isEmpty()) {
        format.getByteBuffer("csd-0")?.let { rememberParameterSets(it, it.remaining()) }
      }
      if (pps.isEmpty()) {
        format.getByteBuffer("csd-1")?.let { rememberParameterSets(it, it.remaining()) }
      }
    }
  }

  init {
    val mime = MediaFormat.MIMETYPE_VIDEO_AVC
    val encoder = MediaCodec.createEncoderByType(mime)
    val capabilities = try {
      encoder.codecInfo.getCapabilitiesForType(mime)
    } catch (e: IllegalArgumentException) {
      null
    }
    val format = MediaFormat.createVideoFormat(mime, width, height)
    format.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
    format.setInteger(MediaFormat.KEY_BIT_RATE, bitrateKbps * 1000)
    format.setInteger(MediaFormat.KEY_FRAME_RATE, frameRate)
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N_MR1) {
      format.setFloat(MediaFormat.KEY_I_FRAME_INTERVAL, keyframeIntervalSeconds.toFloat())
    } else {
      format.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, Math.round(keyframeIntervalSeconds).toInt().coerceAtLeast(1))
    }
    // CBR when supported, else the codec's default VBR.
    val cbr = capabilities?.encoderCapabilities
      ?.isBitrateModeSupported(MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR) == true
    format.setInteger(
      MediaFormat.KEY_BITRATE_MODE,
      if (cbr) MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR else MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR,
    )
    // Main profile only where KEY_MAX_B_FRAMES = 0 can be pinned (API 29+);
    // older devices get Baseline, which never reorders frames.
    val main = capabilities?.profileLevels?.filter { it.profile == MediaCodecInfo.CodecProfileLevel.AVCProfileMain }
      ?.maxByOrNull { it.level }
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q && main != null) {
      format.setInteger(MediaFormat.KEY_PROFILE, MediaCodecInfo.CodecProfileLevel.AVCProfileMain)
      format.setInteger(MediaFormat.KEY_LEVEL, main.level)
      format.setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0)
    } else {
      format.setInteger(MediaFormat.KEY_PROFILE, MediaCodecInfo.CodecProfileLevel.AVCProfileBaseline)
    }
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
      format.setInteger(MediaFormat.KEY_LATENCY, 1)
    }
    encoder.setCallback(callback, handler)
    try {
      encoder.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
    } catch (e: Exception) {
      // KEY_LATENCY / profile are hints some devices reject: retry with the plain format.
      Log.w(TAG, "configure rejected (${e.message}); retrying without hints")
      format.setInteger(MediaFormat.KEY_PROFILE, MediaCodecInfo.CodecProfileLevel.AVCProfileBaseline)
      format.removeKey(MediaFormat.KEY_LEVEL)
      format.removeKey(MediaFormat.KEY_LATENCY)
      if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) format.removeKey(MediaFormat.KEY_MAX_B_FRAMES)
      encoder.reset()
      encoder.setCallback(callback, handler)
      encoder.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
    }
    inputSurface = encoder.createInputSurface()
    codec = encoder
    encoder.start()
  }

  /** The first frame must be a keyframe. */
  fun requestKeyframe() {
    val c = codec ?: return
    try {
      val params = android.os.Bundle()
      params.putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0)
      c.setParameters(params)
    } catch (e: IllegalStateException) {
      Log.w(TAG, "requestKeyframe: ${e.message}")
    }
  }

  fun release() {
    released = true
    val c = codec
    codec = null
    handler.post {
      try {
        c?.stop()
      } catch (e: IllegalStateException) {
        // never started or already in error
      }
      c?.release()
      inputSurface.release()
      thread.quitSafely()
    }
  }

  /** Splits an Annex-B config buffer into its SPS and PPS NALUs. */
  private fun rememberParameterSets(buffer: ByteBuffer, size: Int) {
    val bytes = ByteArray(size)
    buffer.get(bytes)
    var i = 0
    var begin = -1
    fun flush(end: Int) {
      if (begin < 0 || end <= begin) return
      val nalu = bytes.copyOfRange(begin, end)
      when (nalu[0].toInt() and 0x1F) {
        7 -> sps = nalu
        8 -> pps = nalu
      }
    }
    while (i + 3 <= bytes.size) {
      if (bytes[i].toInt() == 0 && bytes[i + 1].toInt() == 0 && bytes[i + 2].toInt() == 1) {
        var end = i
        if (end > 0 && bytes[end - 1].toInt() == 0) end -= 1
        flush(end)
        begin = i + 3
        i += 3
      } else {
        i += 1
      }
    }
    flush(bytes.size)
  }

  companion object {
    private const val TAG = "nitrortmp.h264"
  }
}
