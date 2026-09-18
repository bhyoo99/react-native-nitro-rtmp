package com.margelo.nitro.nitrortmp.capture

import android.media.AudioFormat
import android.media.AudioRecord
import android.media.AudioTimestamp
import android.media.MediaRecorder
import android.util.Log
import java.util.concurrent.atomic.AtomicBoolean

/**
 * AudioRecord capture: 48 kHz, mono, 16-bit PCM in
 * 1024-frame chunks (one AAC frame each), read by its own thread. Chunk
 * timestamps come from `AudioRecord.getTimestamp` (CLOCK_MONOTONIC, the
 * camera's clock) when the device supports it, else from the first read
 *. Muting zero-fills the chunk and never stops the stream.
 */
internal class MicrophoneSource(private val listener: Listener) {
  interface Listener {
    /** [frames] interleaved 16-bit frames whose first sample was captured at [timestampNs]. Audio thread. */
    fun onPcm(pcm: ShortArray, frames: Int, sampleRate: Int, channels: Int, timestampNs: Long)
  }

  @Volatile var muted = false
  private val running = AtomicBoolean(false)
  private var record: AudioRecord? = null
  private var thread: Thread? = null

  val isRunning: Boolean get() = running.get()

  /** @throws IllegalStateException when no AudioRecord can be created. */
  fun start() {
    if (!running.compareAndSet(false, true)) return
    val minBuffer = AudioRecord.getMinBufferSize(SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT)
    val bufferSize = maxOf(minBuffer * 4, CHUNK_FRAMES * 2 * 8)
    val r = create(MediaRecorder.AudioSource.CAMCORDER, bufferSize) ?: create(MediaRecorder.AudioSource.MIC, bufferSize)
    if (r == null) {
      running.set(false)
      throw IllegalStateException("AudioRecord could not be initialized")
    }
    record = r
    try {
      r.startRecording()
    } catch (e: IllegalStateException) {
      r.release()
      record = null
      running.set(false)
      throw e
    }
    val t = Thread({ readLoop(r) }, "nitrortmp.audio")
    thread = t
    t.start()
  }

  fun stop() {
    if (!running.compareAndSet(true, false)) return
    thread?.join(1000)
    thread = null
    record?.let {
      try {
        it.stop()
      } catch (e: IllegalStateException) {
        // never started
      }
      it.release()
    }
    record = null
  }

  private fun create(source: Int, bufferSize: Int): AudioRecord? {
    return try {
      @Suppress("MissingPermission")
      val r = AudioRecord(source, SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT, bufferSize)
      if (r.state == AudioRecord.STATE_INITIALIZED) {
        r
      } else {
        r.release()
        null
      }
    } catch (e: IllegalArgumentException) {
      null
    } catch (e: SecurityException) {
      null
    }
  }

  private fun readLoop(r: AudioRecord) {
    val chunk = ShortArray(CHUNK_FRAMES)
    val timestamp = AudioTimestamp()
    var framesRead = 0L
    // Anchor: (frame position, monotonic ns) pairs from getTimestamp, refreshed
    // about once a second; the fallback anchors the first read.
    var anchorFrame = 0L
    var anchorNs = 0L
    var haveAnchor = false
    var readsSinceAnchor = 0
    while (running.get()) {
      val n = r.read(chunk, 0, CHUNK_FRAMES)
      if (n <= 0) {
        if (n < 0) {
          Log.w(TAG, "AudioRecord.read failed: $n")
          break
        }
        continue
      }
      if (!haveAnchor || readsSinceAnchor >= ANCHOR_REFRESH_READS) {
        if (r.getTimestamp(timestamp, AudioTimestamp.TIMEBASE_MONOTONIC) == AudioRecord.SUCCESS) {
          anchorFrame = timestamp.framePosition
          anchorNs = timestamp.nanoTime
          haveAnchor = true
        } else if (!haveAnchor) {
          anchorFrame = framesRead
          anchorNs = System.nanoTime() - n * NANOS_PER_SECOND / SAMPLE_RATE
          haveAnchor = true
        }
        readsSinceAnchor = 0
      }
      readsSinceAnchor += 1
      val firstSampleNs = anchorNs + (framesRead - anchorFrame) * NANOS_PER_SECOND / SAMPLE_RATE
      framesRead += n
      if (muted) chunk.fill(0, 0, n)
      listener.onPcm(chunk, n, SAMPLE_RATE, 1, firstSampleNs)
    }
  }

  companion object {
    private const val TAG = "nitrortmp.mic"
    const val SAMPLE_RATE = 48000
    /** One AAC frame worth of samples. */
    const val CHUNK_FRAMES = 1024
    private const val NANOS_PER_SECOND = 1_000_000_000L
    private const val ANCHOR_REFRESH_READS = 47  // about one second at 1024 frames per read
  }
}
