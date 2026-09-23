package com.margelo.nitro.nitrortmp

import android.util.Log
import androidx.annotation.Keep
import com.facebook.proguard.annotations.DoNotStrip
import com.margelo.nitro.nitrortmp.capture.MicrophoneSource
import com.margelo.nitro.nitrortmp.encoder.AacEncoder
import com.margelo.nitro.nitrortmp.encoder.H264Encoder
import com.margelo.nitro.nitrortmp.mixer.VideoMixer
import java.util.ArrayDeque
import java.util.concurrent.atomic.AtomicLong
import kotlin.math.roundToInt

/** The `MixerStats` counters, written by the render, encoder and audio threads. */
internal class MixerCounters {
  val capturedFrames = AtomicLong()
  val renderedFrames = AtomicLong()
  val droppedFrames = AtomicLong()
  val encodedVideoFrames = AtomicLong()
  val encodedAudioFrames = AtomicLong()
  val encoderFailures = AtomicLong()
  @Volatile var videoBitrateKbps = 0.0
}

/** What `CameraLayer` and `ImageLayer` implement so the mixer can hook them up. */
internal interface MixerLayer {
  fun attach(mixer: HybridMixer)
  fun detach()
}

/**
 * The `Mixer` HybridObject: the scene (layers, z order,
 * rectangles), the output format, the compositor ([VideoMixer]) and both
 * encoders. Encoding starts when the attached session reaches `publishing`
 * (`startEncoding`) and stops with it; the preview keeps running
 * without a session.
 *
 * Threading: JS calls post to the render thread; encoder output comes
 * on the codec's thread and audio output on the microphone thread, both
 * only post to the session through [EncodedFrameSink]. `stats` and
 * `isEncoding` read atomics.
 */
@DoNotStrip
@Keep
class HybridMixer : HybridMixerSpec() {
  internal val counters = MixerCounters()
  internal val videoMixer = VideoMixer(counters)

  private val lock = Any()
  private var videoSettings = VideoSettings(720.0, 1280.0, 30.0, 2500.0, 2.0)
  private var audioSettings = AudioSettings(48000.0, 1.0, 128.0)
  private val layers = ArrayList<HybridVideoLayerSpec>()
  private var audioSource: HybridMicrophoneSource? = null
  @Volatile private var errorCallback: ((CaptureError) -> Unit)? = null

  // Encoding state: written on the render thread, read on the encoder/audio threads.
  @Volatile private var encoding = false
  @Volatile private var sink: EncodedFrameSink? = null
  @Volatile private var epochNs = 0L
  @Volatile private var firstVideoPushed = false
  /** ADTS packets + capture time (µs) encoded before the first video frame went out; guarded by lock. */
  private val pendingAudio = ArrayDeque<Pair<ByteArray, Long>>()
  private var h264: H264Encoder? = null
  private val audioLock = Any()
  private var aac: AacEncoder? = null
  private val bitrateWindow = ArrayDeque<LongArray>()  // [ms, bytes], last second, guarded by lock

  init {
    videoMixer.setOutput(720, 1280, 30.0)
    videoMixer.onEncoderSurfaceFailed = { message ->
      counters.encoderFailures.incrementAndGet()
      reportError(CaptureError(CaptureErrorCode.ENCODERFAILED, message))
    }
  }

  // --- HybridMixerSpec ---------------------------------------------------------------

  override var video: VideoSettings
    get() = synchronized(lock) { videoSettings }
    set(value) {
      synchronized(lock) { videoSettings = value }
      // Only the next startEncoding picks it up while encoding (spec).
      if (!encoding) videoMixer.setOutput(value.width.toInt(), value.height.toInt(), value.frameRate)
    }

  override var audio: AudioSettings
    get() = synchronized(lock) { audioSettings }
    set(value) = synchronized(lock) { audioSettings = value }

  override val stats: MixerStats
    get() = MixerStats(
      capturedFrames = counters.capturedFrames.get().toDouble(),
      renderedFrames = counters.renderedFrames.get().toDouble(),
      droppedFrames = counters.droppedFrames.get().toDouble(),
      encodedVideoFrames = counters.encodedVideoFrames.get().toDouble(),
      encodedAudioFrames = counters.encodedAudioFrames.get().toDouble(),
      videoBitrateKbps = counters.videoBitrateKbps,
      encoderFailures = counters.encoderFailures.get().toDouble(),
    )

  override val isEncoding: Boolean
    get() = encoding

  override fun addLayer(layer: HybridVideoLayerSpec, frame: LayerFrame?) {
    synchronized(lock) {
      if (layers.any { it === layer }) return
      layers.add(layer)
    }
    // The slot exists on the render thread before the layer may ask for a surface.
    videoMixer.addLayer(layer, frame)
    (layer as? MixerLayer)?.attach(this)
  }

  override fun removeLayer(layer: HybridVideoLayerSpec) {
    synchronized(lock) {
      if (!layers.removeIf { it === layer }) return
    }
    (layer as? MixerLayer)?.detach()
    videoMixer.removeLayer(layer)
  }

  override fun setLayerFrame(layer: HybridVideoLayerSpec, frame: LayerFrame) {
    videoMixer.setLayerFrame(layer, frame)
  }

  override fun setAudioSource(source: HybridMicrophoneSourceSpec?) {
    val next = source as? HybridMicrophoneSource
    val previous = synchronized(lock) {
      val p = audioSource
      audioSource = next
      p
    }
    if (previous === next) return
    previous?.detach()
    next?.attach(this)
  }

  override fun onError(callback: (error: CaptureError) -> Unit) {
    errorCallback = callback
  }

  override fun dispose() {
    stopEncoding()
    val (attached, mic) = synchronized(lock) {
      val copy = ArrayList(layers)
      layers.clear()
      val m = audioSource
      audioSource = null
      copy to m
    }
    for (layer in attached) (layer as? MixerLayer)?.detach()
    mic?.detach()
    videoMixer.release()
    super.dispose()
  }

  // --- session link --------------------------------------------------------------

  /** Called by the session on its queue; idempotent. */
  internal fun startEncoding(sink: EncodedFrameSink) {
    videoMixer.handler.post { startOnRender(sink) }
  }

  /** Called by the session on its queue; idempotent. */
  internal fun stopEncoding() {
    videoMixer.handler.post { stopOnRender() }
  }

  private fun startOnRender(sink: EncodedFrameSink) {
    if (encoding) return
    val v = video
    val a = audio
    videoMixer.setOutput(v.width.toInt(), v.height.toInt(), v.frameRate)
    val encoder = try {
      H264Encoder(
        width = v.width.toInt(),
        height = v.height.toInt(),
        frameRate = v.frameRate.roundToInt().coerceAtLeast(1),
        bitrateKbps = v.bitrateKbps.roundToInt().coerceAtLeast(1),
        keyframeIntervalSeconds = v.keyframeIntervalSeconds ?: 2.0,
        listener = videoListener,
      )
    } catch (e: Exception) {
      counters.encoderFailures.incrementAndGet()
      reportError(CaptureError(CaptureErrorCode.ENCODERFAILED, "H.264 encoder: ${e.message}"))
      return
    }
    try {
      videoMixer.setEncoderSurface(encoder.inputSurface)
    } catch (e: RuntimeException) {
      encoder.release()
      counters.encoderFailures.incrementAndGet()
      reportError(CaptureError(CaptureErrorCode.ENCODERFAILED, "encoder surface: ${e.message}"))
      return
    }
    h264 = encoder
    encoder.requestKeyframe()  // the first frame is a keyframe
    val rate = a.sampleRate.roundToInt()
    if (rate != MicrophoneSource.SAMPLE_RATE) {
      Log.w(TAG, "audio.sampleRate $rate is not supported by the capture; encoding at ${MicrophoneSource.SAMPLE_RATE}")
    }
    synchronized(audioLock) {
      aac = try {
        AacEncoder(
          sampleRate = MicrophoneSource.SAMPLE_RATE,
          channels = a.channels.roundToInt().coerceIn(1, 2),
          bitrateKbps = a.bitrateKbps.roundToInt().coerceAtLeast(8),
          listener = audioListener,
        )
      } catch (e: Exception) {
        counters.encoderFailures.incrementAndGet()
        reportError(CaptureError(CaptureErrorCode.ENCODERFAILED, "AAC encoder: ${e.message}"))
        null
      }
    }
    synchronized(lock) { bitrateWindow.clear() }
    counters.videoBitrateKbps = 0.0
    epochNs = 0L  // established by the first encoded frame
    firstVideoPushed = false
    synchronized(lock) { pendingAudio.clear() }
    this.sink = sink
    encoding = true
  }

  private fun stopOnRender() {
    if (!encoding) return
    encoding = false
    sink = null
    videoMixer.setEncoderSurface(null)
    h264?.release()
    h264 = null
    synchronized(audioLock) {
      aac?.release()
      aac = null
    }
    counters.videoBitrateKbps = 0.0
  }

  // --- media input ---------------------------------------------------------------------------

  /** PCM from the attached microphone, on its thread; encoded synchronously. */
  internal fun audioArrived(pcm: ShortArray, frames: Int, sampleRate: Int, channels: Int, timestampNs: Long) {
    if (!encoding) return
    synchronized(audioLock) {
      aac?.encode(pcm, frames, channels, timestampNs)
    }
  }

  internal fun reportError(error: CaptureError) {
    errorCallback?.invoke(error)
  }

  private fun hasCameraLayer(): Boolean = synchronized(lock) { layers.any { it is HybridCameraLayer } }

  private val videoListener = object : H264Encoder.Listener {
    override fun onEncodedVideo(annexb: ByteArray, presentationTimeUs: Long, keyframe: Boolean) {
      if (!encoding) return
      val s = sink ?: return
      // Under the lock so no audio packet slips into the session before the first video frame.
      synchronized(lock) {
        val epochUs = establishEpochUs(presentationTimeUs)
        val pts = ((presentationTimeUs - epochUs) / 1000).coerceAtLeast(0)
        s.pushEncodedVideo(annexb, pts.toInt(), pts.toInt())
        if (!firstVideoPushed) {
          firstVideoPushed = true
          for ((adts, timeUs) in pendingAudio) pushAudioLocked(s, adts, timeUs, epochUs)
          pendingAudio.clear()
        }
      }
      counters.encodedVideoFrames.incrementAndGet()
      updateBitrate(annexb.size.toLong())
    }

    override fun onEncoderError(message: String) {
      counters.encoderFailures.incrementAndGet()
      reportError(CaptureError(CaptureErrorCode.ENCODERFAILED, message))
    }
  }

  private val audioListener = object : AacEncoder.Listener {
    override fun onEncodedAudio(adts: ByteArray, presentationTimeUs: Long) {
      if (!encoding) return
      val s = sink ?: return
      synchronized(lock) {
        // Audio waits for the first video frame so the stream starts on the
        // keyframe and both clocks share the epoch; the packets are kept,
        // not dropped, so audio starts within one packet of the video.
        if (!firstVideoPushed && hasCameraLayer()) {
          pendingAudio.addLast(adts to presentationTimeUs)
          while (pendingAudio.size > MAX_PENDING_AUDIO) pendingAudio.removeFirst()
          return
        }
        pushAudioLocked(s, adts, presentationTimeUs, establishEpochUs(presentationTimeUs))
      }
    }

    override fun onEncoderError(message: String) {
      counters.encoderFailures.incrementAndGet()
      reportError(CaptureError(CaptureErrorCode.ENCODERFAILED, message))
    }
  }

  /**
   * Pushes a packet unless it ended before the epoch (the first video frame).
   * The packet that spans the epoch is kept at pts 0, so audio starts within
   * one packet of the video. Caller holds lock.
   */
  private fun pushAudioLocked(s: EncodedFrameSink, adts: ByteArray, timeUs: Long, epochUs: Long) {
    if (timeUs + AAC_PACKET_US <= epochUs) return
    val pts = ((timeUs - epochUs) / 1000).coerceAtLeast(0)
    s.pushEncodedAudio(adts, pts.toInt())
    counters.encodedAudioFrames.incrementAndGet()
  }

  /**
   * The session clock's origin: the capture time of the first frame that
   * reaches the sink, not the wall clock at `startEncoding`. Camera frames
   * captured before `startEncoding` would otherwise all clamp to 0 and give
   * the muxer duplicate timestamps.
   */
  private fun establishEpochUs(presentationTimeUs: Long): Long = synchronized(lock) {
    if (epochNs == 0L) epochNs = presentationTimeUs * 1000
    epochNs / 1000
  }

  /** Bytes over the last second, from the encoder thread. */
  private fun updateBitrate(bytes: Long) {
    val now = System.nanoTime() / 1_000_000
    val kbps: Double
    synchronized(lock) {
      bitrateWindow.addLast(longArrayOf(now, bytes))
      while (bitrateWindow.isNotEmpty() && now - bitrateWindow.peekFirst()!![0] > 1000) bitrateWindow.pollFirst()
      var sum = 0L
      for (entry in bitrateWindow) sum += entry[1]
      kbps = sum * 8.0 / 1000.0
    }
    counters.videoBitrateKbps = kbps
  }

  companion object {
    private const val TAG = "nitrortmp.mixer"
    /** About 2 s of AAC packets kept while waiting for the first video frame. */
    private const val MAX_PENDING_AUDIO = 100
    /** One AAC packet: 1024 samples at the capture rate. */
    private const val AAC_PACKET_US = 1024L * 1_000_000L / MicrophoneSource.SAMPLE_RATE
  }
}
