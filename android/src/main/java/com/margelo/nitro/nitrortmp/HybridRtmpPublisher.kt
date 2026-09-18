package com.margelo.nitro.nitrortmp

import android.os.Handler
import android.os.HandlerThread
import androidx.annotation.Keep
import com.facebook.proguard.annotations.DoNotStrip
import com.margelo.nitro.core.ArrayBuffer
import com.margelo.nitro.core.Promise

/**
 * `start()` rejections. fbjni turns a Throwable into the JS Error message with
 * `toString()`, so both forms read "<code>: <message>", which the JS side
 * (`createPublisher`) turns back into a PublisherError.
 */
internal class PublisherException(code: PublisherErrorCode, detail: String) :
  Exception("${NativePublisher.errorName(code.value) ?: "protocolError"}: $detail") {
  override fun toString(): String = message ?: ""

  // fbjni appends the Java stack trace to the JS message; there is nothing to
  // debug in it, and the message must stay one line ("<code>: <message>").
  override fun fillInStackTrace(): Throwable = this
}

/**
 * The `RtmpPublisher` HybridObject: one session thread, one
 * [RtmpTransport], one core publisher behind the C facade ([NativePublisher]).
 *
 * Threading: every core call happens on the session thread. JS calls are
 * posted to it; the read thread posts received bytes to it; core callbacks run
 * synchronously inside core calls on it and never call back into the core.
 * `state` and `stats` read volatile snapshots and never touch the core.
 */
@DoNotStrip
@Keep
class HybridRtmpPublisher : HybridRtmpPublisherSpec(), EncodedFrameSink {
  private val thread = HandlerThread("nitrortmp.session").apply { start() }
  private val handler = Handler(thread.looper)

  // Core callbacks: synchronous, inside core calls on the session thread.
  private val coreListener = object : NativePublisher.Listener {
    override fun onNativeSend(data: ByteArray) {
      transport?.send(data)
    }

    override fun onNativeState(state: Int) {
      if (suppressCoreState) return
      val mapped = PublisherState.values().firstOrNull { it.value == state } ?: return
      setState(mapped)
    }

    override fun onNativeError(code: Int, message: String) {
      val mapped = PublisherErrorCode.values().firstOrNull { it.value == code } ?: PublisherErrorCode.PROTOCOLERROR
      reportError(mapped, message)
    }
  }

  // Transport callbacks: posted to the session thread by the read/write threads.
  private val transportListener = object : RtmpTransport.Listener {
    override fun onReady(transport: RtmpTransport) = socketReady(transport)

    override fun onReceive(transport: RtmpTransport, data: ByteArray, length: Int) =
      socketReceived(transport, data, length)

    override fun onFailure(transport: RtmpTransport, kind: RtmpTransport.FailureKind, message: String) =
      socketFailed(transport, kind, message)
  }

  private var core: NativePublisher? = null  // created and destroyed on the session thread
  @Volatile private var transport: RtmpTransport? = null
  private var pendingUrl: String = ""  // handed to the core once the socket is ready
  private var startPromise: Promise<Unit>? = null
  private var deadline: Runnable? = null
  private var isStopping = false        // keep the socket while the core announces `stopped`
  private var suppressCoreState = false // the core is put to rest after a transport failure
  private var stateCallback: ((PublisherState) -> Unit)? = null
  private var errorCallback: ((PublisherError) -> Unit)? = null
  private var mixer: HybridMixer? = null  // its encoders follow the session state
  @Volatile private var disposed = false

  @Volatile private var snapshotState = PublisherState.IDLE
  @Volatile private var snapshotStats = LongArray(10)

  init {
    handler.post { core = NativePublisher(coreListener) }
  }

  // --- HybridRtmpPublisherSpec ---------------------------------------------------

  override val state: PublisherState
    get() = snapshotState

  override val stats: PublisherStats
    get() {
      val s = snapshotStats
      val queued = transport?.queuedBytes?.get() ?: 0L
      return PublisherStats(
        bytesSent = s[0].toDouble(), videoTags = s[1].toDouble(), audioTags = s[2].toDouble(),
        rejectedFrames = s[4].toDouble(), droppedBeforeKeyframe = s[5].toDouble(),
        invalidFrames = s[6].toDouble(), timestampClamps = s[7].toDouble(),
        queuedBytes = queued.toDouble(),
        lastVideoTimestamp = s[8].toDouble(), lastAudioTimestamp = s[9].toDouble(),
      )
    }

  override fun start(url: String): Promise<Unit> {
    val promise = Promise<Unit>()
    if (!post { startOnSession(url, promise) }) {
      promise.reject(PublisherException(PublisherErrorCode.NOTREADY, "publisher was disposed"))
    }
    return promise
  }

  override fun stop(): Promise<Unit> {
    val promise = Promise<Unit>()
    if (!post { stopOnSession(promise) }) {
      promise.resolve(Unit)
    }
    return promise
  }

  override fun setMetadata(metadata: StreamMetadata) {
    post {
      core?.setMetadata(
        width = metadata.width?.toInt() ?: 0,
        height = metadata.height?.toInt() ?: 0,
        frameRate = metadata.frameRate ?: 0.0,
        videoBitrateKbps = metadata.videoBitrateKbps ?: 0.0,
        audioSampleRate = metadata.audioSampleRate?.toInt() ?: 0,
        audioChannels = metadata.audioChannels?.toInt() ?: 0,
        audioBitrateKbps = metadata.audioBitrateKbps ?: 0.0,
        encoder = metadata.encoder,
      )
      refreshStats()
    }
  }

  /**
   * The mixer's encoders start when `publishing` is reached
   * and stop with the session. Replacing or detaching a mixer stops its
   * encoders first; attaching while publishing starts them at once.
   */
  override fun setMixer(mixer: HybridMixerSpec?) {
    val next = mixer as? HybridMixer
    post {
      val previous = this.mixer
      if (previous === next) return@post
      previous?.stopEncoding()
      this.mixer = next
      if (snapshotState == PublisherState.PUBLISHING) next?.startEncoding(this)
    }
  }

  override fun onStateChange(callback: (state: PublisherState) -> Unit) {
    post { stateCallback = callback }
  }

  override fun onError(callback: (error: PublisherError) -> Unit) {
    post { errorCallback = callback }
  }

  override fun pushVideo(frame: ArrayBuffer, ptsMs: Double, dtsMs: Double) {
    val bytes = frame.toByteArray()  // the JS buffer is only valid during this call
    val pts = timestamp(ptsMs)
    val dts = timestamp(dtsMs)
    post {
      core?.pushVideo(bytes, pts, dts)
      refreshStats()
    }
  }

  override fun pushAudio(frame: ArrayBuffer, ptsMs: Double) {
    val bytes = frame.toByteArray()
    val pts = timestamp(ptsMs)
    post {
      core?.pushAudio(bytes, pts)
      refreshStats()
    }
  }

  // --- EncodedFrameSink: encoder threads only post here ------------------
  // Frames still in the encoder's pipeline when the session ends are discarded
  // here, so `rejectedFrames` keeps counting only external push misuse.

  override fun pushEncodedVideo(annexb: ByteArray, ptsMs: Int, dtsMs: Int) {
    post {
      if (snapshotState != PublisherState.PUBLISHING) return@post
      core?.pushVideo(annexb, ptsMs, dtsMs)
      refreshStats()
    }
  }

  override fun pushEncodedAudio(adts: ByteArray, ptsMs: Int) {
    post {
      if (snapshotState != PublisherState.PUBLISHING) return@post
      core?.pushAudio(adts, ptsMs)
      refreshStats()
    }
  }

  override fun dispose() {
    disposed = true
    handler.post {
      teardown("publisher was disposed")
      thread.quitSafely()
    }
    super.dispose()
  }

  @Suppress("ProtectedInFinal", "unused")
  protected fun finalize() {
    // Nothing references this object any more: close the socket and the
    // core on the session thread, then let the thread go.
    if (disposed) return
    disposed = true
    handler.post {
      teardown("publisher was released")
      thread.quitSafely()
    }
  }

  // --- session thread ------------------------------------------------------------------

  private fun post(block: () -> Unit): Boolean {
    if (disposed) return false
    return handler.post(block)
  }

  private fun startOnSession(url: String, promise: Promise<Unit>) {
    if (core == null) {
      promise.reject(PublisherException(PublisherErrorCode.NOTREADY, "publisher was disposed"))
      return
    }
    val current = snapshotState
    if (startPromise != null || current == PublisherState.CONNECTING ||
      current == PublisherState.CONNECTED || current == PublisherState.PUBLISHING
    ) {
      val name = NativePublisher.stateName(current.value) ?: current.name
      promise.reject(PublisherException(PublisherErrorCode.NOTREADY, "start() while $name; call stop() first"))
      return
    }
    val endpoint = NativePublisher.urlEndpoint(url)
    if (endpoint == null || endpoint.size < 3) {
      promise.reject(
        PublisherException(PublisherErrorCode.INVALIDURL, "not an rtmp:// or rtmps:// URL with app and stream: $url"),
      )
      return
    }

    // A failed session may have left its socket open.
    transport?.close()
    transport = null

    startPromise = promise
    isStopping = false
    setState(PublisherState.CONNECTING)

    val t = RtmpTransport(endpoint[0], endpoint[1].toInt(), endpoint[2] == "1", handler, transportListener)
    transport = t
    val d = Runnable { deadlineFired() }
    deadline = d
    handler.postDelayed(d, START_TIMEOUT_MS)
    pendingUrl = url
    t.start()
  }

  private fun stopOnSession(promise: Promise<Unit>) {
    clearDeadline()
    startPromise?.let {
      startPromise = null
      it.reject(PublisherException(PublisherErrorCode.NOTREADY, "stop() was called before publishing started"))
    }
    val t = transport
    if (t == null) {
      promise.resolve(Unit)  // idle, stopped or failed: nothing to close
      return
    }
    isStopping = true
    core?.let {
      it.stop()  // FCUnpublish + deleteStream go out through onNativeSend
      refreshStats()
    }
    setState(PublisherState.STOPPED)  // no-op when the core already announced it
    isStopping = false
    t.finish(STOP_TIMEOUT_MS) {
      if (transport === t) transport = null
      promise.resolve(Unit)
    }
  }

  private fun deadlineFired() {
    deadline = null
    if (startPromise == null) return
    val phase = if (snapshotState == PublisherState.CONNECTING && transport != null) "connect or publish" else "publish"
    failSession(PublisherErrorCode.TIMEOUT, "$phase timed out after ${START_TIMEOUT_MS / 1000} s")
  }

  /** A transport-side failure: report once, put the core to rest silently, show `failed`. */
  private fun failSession(code: PublisherErrorCode, message: String) {
    clearDeadline()
    transport?.close()
    transport = null
    reportError(code, message)
    core?.let {
      suppressCoreState = true
      it.stop()  // its FCUnpublish is dropped: there is no socket
      suppressCoreState = false
      refreshStats()
    }
    setState(PublisherState.FAILED)
  }

  /** Before `publishing` the pending start() takes the error; afterwards onError does. */
  private fun reportError(code: PublisherErrorCode, message: String) {
    val pending = startPromise
    if (pending != null) {
      startPromise = null
      clearDeadline()
      pending.reject(PublisherException(code, message))
    } else {
      errorCallback?.invoke(PublisherError(code, message))
    }
  }

  private fun setState(state: PublisherState) {
    val previous = snapshotState
    snapshotState = state
    if (state == previous) return
    when (state) {
      PublisherState.PUBLISHING -> {
        startPromise?.let {
          startPromise = null
          clearDeadline()
          it.resolve(Unit)
        }
        mixer?.startEncoding(this)  // the first encoded frame is a keyframe
      }
      PublisherState.STOPPED, PublisherState.FAILED -> {
        mixer?.stopEncoding()
        if (!isStopping) {
          // The core ended the session on its own (server closed, protocol error).
          transport?.close()
          transport = null
        }
      }
      else -> {}
    }
    stateCallback?.invoke(state)
  }

  private fun clearDeadline() {
    deadline?.let { handler.removeCallbacks(it) }
    deadline = null
  }

  private fun refreshStats() {
    val c = core ?: return
    snapshotStats = c.stats()
  }

  private fun teardown(reason: String) {
    clearDeadline()
    mixer?.stopEncoding()
    mixer = null
    transport?.close()
    transport = null
    startPromise?.let {
      startPromise = null
      it.reject(PublisherException(PublisherErrorCode.NOTREADY, reason))
    }
    core?.destroy()
    core = null
  }

  // --- transport events (session thread) ----------------------------------------------

  private fun socketReady(transport: RtmpTransport) {
    if (transport !== this.transport) return
    val c = core ?: return
    val started = c.start(pendingUrl)  // C0+C1 through onNativeSend
    refreshStats()
    if (!started) {
      // onNativeError already settled the promise; make sure the socket goes away.
      transport.close()
      this.transport = null
      setState(PublisherState.FAILED)
    }
  }

  private fun socketReceived(transport: RtmpTransport, data: ByteArray, length: Int) {
    if (transport !== this.transport) return
    val c = core ?: return
    c.onReceive(data, length)
    refreshStats()
  }

  private fun socketFailed(transport: RtmpTransport, kind: RtmpTransport.FailureKind, message: String) {
    if (transport !== this.transport) return
    this.transport = null  // it closed itself
    val code = when (kind) {
      RtmpTransport.FailureKind.CONNECT_FAILED -> PublisherErrorCode.CONNECTFAILED
      RtmpTransport.FailureKind.TLS_FAILED -> PublisherErrorCode.TLSFAILED
      RtmpTransport.FailureKind.SOCKET_CLOSED -> PublisherErrorCode.SOCKETCLOSED
      RtmpTransport.FailureKind.TIMEOUT -> PublisherErrorCode.TIMEOUT
    }
    failSession(code, message)
  }

  companion object {
    /** start() → publishing, DNS/TCP/TLS and the RTMP exchange included. */
    private const val START_TIMEOUT_MS = 10_000L
    /** stop(): time given to the write thread to flush FCUnpublish/deleteStream. */
    private const val STOP_TIMEOUT_MS = 2_000L

    /** JS milliseconds to the core's uint32 clock; the JNI side reinterprets the bits. */
    private fun timestamp(ms: Double): Int {
      if (ms.isNaN() || ms <= 0) return 0
      return ms.toLong().coerceAtMost(0xFFFF_FFFFL).toInt()
    }
  }
}
