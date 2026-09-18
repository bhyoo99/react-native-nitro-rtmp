package com.margelo.nitro.nitrortmp.encoder

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.util.Log
import com.margelo.nitro.nitrortmp.NativeCodec
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * MediaCodec AAC-LC encoder, driven synchronously on
 * the microphone thread: one PCM chunk in, zero or more ADTS frames
 * out. The 7-byte ADTS header comes from the C helper (`NativeCodec`),
 * protection_absent = 1 as the core requires.
 */
internal class AacEncoder(
  private val sampleRate: Int,
  private val channels: Int,
  bitrateKbps: Int,
  private val listener: Listener,
) {
  interface Listener {
    /** One ADTS frame. Called on the microphone thread. */
    fun onEncodedAudio(adts: ByteArray, presentationTimeUs: Long)
    fun onEncoderError(message: String)
  }

  private val codec: MediaCodec
  private val info = MediaCodec.BufferInfo()
  private var scratch: ByteBuffer = ByteBuffer.allocateDirect(16384).order(ByteOrder.nativeOrder())
  private var released = false
  /** Chunks the codec had no input buffer for (never blocks the capture thread). */
  var droppedChunks = 0L
    private set

  init {
    val mime = MediaFormat.MIMETYPE_AUDIO_AAC
    val format = MediaFormat.createAudioFormat(mime, sampleRate, channels)
    format.setInteger(MediaFormat.KEY_AAC_PROFILE, MediaCodecInfo.CodecProfileLevel.AACObjectLC)
    format.setInteger(MediaFormat.KEY_BIT_RATE, bitrateKbps * 1000)
    format.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 16384)
    codec = MediaCodec.createEncoderByType(mime)
    codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
    codec.start()
  }

  /**
   * Encodes [frames] interleaved 16-bit samples with [sourceChannels]
   * channels whose first sample was captured at [timestampNs]. A mono source
   * is duplicated for a stereo target, a stereo source is averaged for mono.
   */
  fun encode(pcm: ShortArray, frames: Int, sourceChannels: Int, timestampNs: Long) {
    if (released || frames <= 0) return
    val needed = frames * channels * 2
    if (scratch.capacity() < needed) scratch = ByteBuffer.allocateDirect(needed).order(ByteOrder.nativeOrder())
    scratch.clear()
    when {
      sourceChannels == channels -> for (i in 0 until frames * channels) scratch.putShort(pcm[i])
      sourceChannels == 1 && channels == 2 -> for (i in 0 until frames) {
        scratch.putShort(pcm[i])
        scratch.putShort(pcm[i])
      }
      sourceChannels == 2 && channels == 1 -> for (i in 0 until frames) {
        scratch.putShort(((pcm[2 * i].toInt() + pcm[2 * i + 1].toInt()) / 2).toShort())
      }
      else -> return
    }
    scratch.flip()
    try {
      var index = codec.dequeueInputBuffer(0)
      if (index < 0) {
        drain()
        index = codec.dequeueInputBuffer(0)
      }
      if (index < 0) {
        droppedChunks += 1
        return
      }
      val input = codec.getInputBuffer(index) ?: return
      input.clear()
      input.put(scratch)
      codec.queueInputBuffer(index, 0, needed, timestampNs / 1000, 0)
      drain()
    } catch (e: IllegalStateException) {
      listener.onEncoderError("AAC encoder: ${e.message}")
    } catch (e: MediaCodec.CodecException) {
      listener.onEncoderError("AAC encoder error ${e.errorCode}: ${e.diagnosticInfo}")
    }
  }

  private fun drain() {
    while (true) {
      val index = codec.dequeueOutputBuffer(info, 0)
      if (index == MediaCodec.INFO_TRY_AGAIN_LATER) return
      if (index < 0) continue  // format / buffers changed
      val buffer = codec.getOutputBuffer(index)
      if (buffer != null && info.size > 0 && (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG) == 0) {
        val header = NativeCodec.writeAdtsHeader(1, sampleRate, channels, info.size)
        if (header != null) {
          val adts = ByteArray(header.size + info.size)
          System.arraycopy(header, 0, adts, 0, header.size)
          buffer.position(info.offset)
          buffer.limit(info.offset + info.size)
          buffer.get(adts, header.size, info.size)
          listener.onEncodedAudio(adts, info.presentationTimeUs)
        } else {
          Log.w(TAG, "ADTS header rejected: rate=$sampleRate channels=$channels size=${info.size}")
        }
      }
      codec.releaseOutputBuffer(index, false)
    }
  }

  fun release() {
    if (released) return
    released = true
    try {
      codec.stop()
    } catch (e: IllegalStateException) {
      // already stopped
    }
    codec.release()
  }

  companion object {
    private const val TAG = "nitrortmp.aac"
  }
}
