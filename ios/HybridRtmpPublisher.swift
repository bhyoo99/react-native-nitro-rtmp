import Foundation
import NitroModules

/// `start()` rejections carry the error code in front of the message; the JS
/// side (`createPublisher`) turns "<code>: <message>" back into a PublisherError.
struct PublisherFailure: Error, CustomStringConvertible {
  let code: PublisherErrorCode
  let message: String
  var description: String { "\(code.stringValue): \(message)" }
}

/// Where the mixer's encoders deliver: the session
/// itself. Both methods only post to the session queue and return; the bytes
/// are already a copy owned by the caller. Not part of the Nitro spec.
protocol EncodedFrameSink: AnyObject {
  func pushEncodedVideo(_ annexb: Data, ptsMs: UInt32, dtsMs: UInt32)
  func pushEncodedAudio(_ adts: Data, ptsMs: UInt32)
}

/// The `RtmpPublisher` HybridObject: one serial session
/// queue, one `RtmpTransport`, one core publisher behind the C facade.
/// `setMixer` attaches a mixer: its encoders run exactly while the
/// session is `publishing` and feed the core through `EncodedFrameSink`.
///
/// Threading: every core call happens on `queue`. JS calls are posted to
/// it; the socket delivers on it; core callbacks run synchronously inside core
/// calls on it and never call back into the core. `state` and `stats` read a
/// snapshot under a lock and never touch the core from the JS thread.
final class HybridRtmpPublisher: HybridRtmpPublisherSpec {
  /// start() → publishing, DNS/TCP/TLS and the RTMP exchange included.
  private static let startTimeout: TimeInterval = 10
  /// stop(): time given to the socket to flush FCUnpublish/deleteStream.
  private static let stopTimeout: TimeInterval = 2

  private let queue = DispatchQueue(label: "nitrortmp.session")
  private var core: OpaquePointer?  // nitrortmp_publisher_t*, created and destroyed on `queue`
  private var transport: RtmpTransport?
  private var startPromise: Promise<Void>?
  private var deadline: DispatchWorkItem?
  private var isStopping = false        // keep the socket while the core announces `stopped`
  private var suppressCoreState = false // the core is put to rest after a transport failure
  private var stateCallback: ((PublisherState) -> Void)?
  private var errorCallback: ((PublisherError) -> Void)?
  private var mixer: HybridMixer?  // encoders on while publishing; JS owns its lifetime

  private let lock = NSLock()
  private var snapshotState: PublisherState = .idle
  private var snapshotStats = HybridRtmpPublisher.makeStats(nil, queuedBytes: 0)

  // MARK: - Lifecycle

  override init() {
    super.init()
    queue.async { [weak self] in
      guard let self else { return }
      var callbacks = HybridRtmpPublisher.coreCallbacks
      self.core = nitrortmp_publisher_create(&callbacks, Unmanaged.passUnretained(self).toOpaque())
    }
  }

  deinit {
    // Nothing else references this object any more; hand what it owns to the
    // queue so the socket and the core go away in order.
    let core = self.core
    let transport = self.transport
    let deadline = self.deadline
    let startPromise = self.startPromise
    let mixer = self.mixer
    queue.async {
      mixer?.stopEncoding()
      deadline?.cancel()
      transport?.cancel()
      startPromise?.reject(withError: PublisherFailure(code: .notready, message: "publisher was released"))
      if let core { nitrortmp_publisher_destroy(core) }
    }
  }

  func dispose() {
    queue.async { [weak self] in self?.teardownOnQueue(reason: "publisher was disposed") }
  }

  private func teardownOnQueue(reason: String) {
    mixer?.stopEncoding()
    mixer = nil
    deadline?.cancel()
    deadline = nil
    transport?.cancel()
    transport = nil
    if let promise = startPromise {
      startPromise = nil
      promise.reject(withError: PublisherFailure(code: .notready, message: reason))
    }
    if let core {
      nitrortmp_publisher_destroy(core)
      self.core = nil
    }
  }

  // MARK: - HybridRtmpPublisherSpec

  var state: PublisherState {
    lock.lock()
    defer { lock.unlock() }
    return snapshotState
  }

  var stats: PublisherStats {
    lock.lock()
    defer { lock.unlock() }
    return snapshotStats
  }

  func start(url: String) throws -> Promise<Void> {
    let promise = Promise<Void>()
    queue.async { [weak self] in
      guard let self else {
        promise.reject(withError: PublisherFailure(code: .notready, message: "publisher was released"))
        return
      }
      self.startOnQueue(url: url, promise: promise)
    }
    return promise
  }

  func stop() throws -> Promise<Void> {
    let promise = Promise<Void>()
    queue.async { [weak self] in
      guard let self else {
        promise.resolve()
        return
      }
      self.stopOnQueue(promise)
    }
    return promise
  }

  func setMetadata(metadata: StreamMetadata) throws {
    // Copy the fields out of the C++ struct on the caller's thread.
    let width = metadata.width ?? 0
    let height = metadata.height ?? 0
    let frameRate = metadata.frameRate ?? 0
    let videoBitrateKbps = metadata.videoBitrateKbps ?? 0
    let audioSampleRate = metadata.audioSampleRate ?? 0
    let audioChannels = metadata.audioChannels ?? 0
    let audioBitrateKbps = metadata.audioBitrateKbps ?? 0
    let encoder = metadata.encoder
    queue.async { [weak self] in
      guard let self, let core = self.core else { return }
      var m = nitrortmp_metadata_t()
      m.width = Int32(clamping: Int(width))
      m.height = Int32(clamping: Int(height))
      m.frame_rate = frameRate
      m.video_bitrate_kbps = videoBitrateKbps
      m.audio_sample_rate = Int32(clamping: Int(audioSampleRate))
      m.audio_channels = Int32(clamping: Int(audioChannels))
      m.audio_bitrate_kbps = audioBitrateKbps
      if let encoder {
        encoder.withCString { cString in
          m.encoder = cString
          nitrortmp_publisher_set_metadata(core, &m)
        }
      } else {
        nitrortmp_publisher_set_metadata(core, &m)
      }
      self.refreshStats()
    }
  }

  func setMixer(mixer: (any HybridMixerSpec)?) throws {
    let next = mixer as? HybridMixer
    queue.async { [weak self] in
      guard let self else { return }
      if let current = self.mixer, current !== next {
        current.stopEncoding()
      }
      self.mixer = next
      if self.state == .publishing, let next {
        next.startEncoding(sink: self)
      }
    }
  }

  func onStateChange(callback: @escaping (PublisherState) -> Void) throws {
    queue.async { [weak self] in self?.stateCallback = callback }
  }

  func onError(callback: @escaping (PublisherError) -> Void) throws {
    queue.async { [weak self] in self?.errorCallback = callback }
  }

  func pushVideo(frame: ArrayBuffer, ptsMs: Double, dtsMs: Double) throws {
    // The JS buffer is only valid during this call: copy first.
    let data = Data(bytes: frame.data, count: frame.size)
    let pts = HybridRtmpPublisher.timestamp(ptsMs)
    let dts = HybridRtmpPublisher.timestamp(dtsMs)
    queue.async { [weak self] in
      guard let self, let core = self.core else { return }
      data.withUnsafeBytes { buffer in
        guard let base = buffer.baseAddress else { return }
        _ = nitrortmp_publisher_push_video(core, base.assumingMemoryBound(to: UInt8.self), buffer.count, pts, dts)
      }
      self.refreshStats()
    }
  }

  func pushAudio(frame: ArrayBuffer, ptsMs: Double) throws {
    let data = Data(bytes: frame.data, count: frame.size)
    let pts = HybridRtmpPublisher.timestamp(ptsMs)
    queue.async { [weak self] in
      guard let self, let core = self.core else { return }
      data.withUnsafeBytes { buffer in
        guard let base = buffer.baseAddress else { return }
        _ = nitrortmp_publisher_push_audio(core, base.assumingMemoryBound(to: UInt8.self), buffer.count, pts)
      }
      self.refreshStats()
    }
  }

  // MARK: - Session (all on `queue`)

  private func startOnQueue(url: String, promise: Promise<Void>) {
    guard core != nil else {
      promise.reject(withError: PublisherFailure(code: .notready, message: "publisher was disposed"))
      return
    }
    let current = state
    if startPromise != nil || current == .connecting || current == .connected || current == .publishing {
      promise.reject(withError: PublisherFailure(
        code: .notready, message: "start() while \(current.stringValue); call stop() first"))
      return
    }
    var endpoint = nitrortmp_endpoint_t()
    guard nitrortmp_url_endpoint(url, &endpoint) == 1 else {
      promise.reject(withError: PublisherFailure(
        code: .invalidurl, message: "not an rtmp:// or rtmps:// URL with app and stream: \(url)"))
      return
    }
    let host = withUnsafePointer(to: &endpoint.host) { pointer in
      String(cString: UnsafeRawPointer(pointer).assumingMemoryBound(to: CChar.self))
    }

    // A failed session may have left its socket open.
    transport?.cancel()
    transport = nil

    startPromise = promise
    isStopping = false
    setState(.connecting)

    let transport = RtmpTransport(host: host, port: endpoint.port, useTls: endpoint.use_tls != 0, queue: queue)
    transport.onReady = { [weak self] in self?.socketReady(url: url) }
    transport.onReceive = { [weak self] data in self?.socketReceived(data) }
    transport.onFailure = { [weak self] failure in self?.socketFailed(failure) }
    transport.onQueuedBytesChange = { [weak self] bytes in self?.updateQueuedBytes(bytes) }
    self.transport = transport

    let deadline = DispatchWorkItem { [weak self] in self?.deadlineFired() }
    self.deadline = deadline
    queue.asyncAfter(deadline: .now() + HybridRtmpPublisher.startTimeout, execute: deadline)
    transport.start()
  }

  private func stopOnQueue(_ promise: Promise<Void>) {
    deadline?.cancel()
    deadline = nil
    if let pending = startPromise {
      startPromise = nil
      pending.reject(withError: PublisherFailure(code: .notready, message: "stop() was called before publishing started"))
    }
    guard let transport else {
      promise.resolve()  // idle, stopped or failed: nothing to close
      return
    }
    isStopping = true
    if let core {
      nitrortmp_publisher_stop(core)  // FCUnpublish + deleteStream go out through on_send
      refreshStats()
    }
    setState(.stopped)  // no-op when the core already announced it
    isStopping = false
    transport.finish(timeout: HybridRtmpPublisher.stopTimeout) { [weak self] in
      if let self, self.transport === transport {
        self.transport = nil
        self.refreshStats()
      }
      promise.resolve()
    }
  }

  private func socketReady(url: String) {
    guard let core, transport != nil else { return }
    let started = nitrortmp_publisher_start(core, url)  // C0+C1 through on_send
    refreshStats()
    if started == 0 {
      // on_error already settled the promise; make sure the socket goes away.
      transport?.cancel()
      transport = nil
      setState(.failed)
    }
  }

  private func socketReceived(_ data: Data) {
    guard let core, transport != nil else { return }
    data.withUnsafeBytes { buffer in
      guard let base = buffer.baseAddress else { return }
      nitrortmp_publisher_on_receive(core, base.assumingMemoryBound(to: UInt8.self), buffer.count)
    }
    refreshStats()
  }

  private func socketFailed(_ failure: RtmpTransport.Failure) {
    transport = nil  // it cancelled itself
    switch failure {
    case .connectFailed(let message): failSession(code: .connectfailed, message: message)
    case .tlsFailed(let message): failSession(code: .tlsfailed, message: message)
    case .socketClosed(let message): failSession(code: .socketclosed, message: message)
    case .timeout(let message): failSession(code: .timeout, message: message)
    }
  }

  private func deadlineFired() {
    deadline = nil
    guard startPromise != nil else { return }
    let phase = transport?.isReady == true ? "publish" : "connect"
    var message = "\(phase) timed out after \(Int(HybridRtmpPublisher.startTimeout)) s"
    if let reason = transport?.lastWaitReason {
      message += " (\(reason))"
    }
    failSession(code: .timeout, message: message)
  }

  /// A transport-side failure: report it once, put the core to rest without
  /// announcing `stopped`, and show `failed`.
  private func failSession(code: PublisherErrorCode, message: String) {
    deadline?.cancel()
    deadline = nil
    transport?.cancel()
    transport = nil
    reportError(code: code, message: message)
    if let core {
      suppressCoreState = true
      nitrortmp_publisher_stop(core)  // its FCUnpublish is dropped: there is no socket
      suppressCoreState = false
      refreshStats()
    }
    setState(.failed)
  }

  /// Before `publishing` the pending start() takes the error; afterwards onError does.
  private func reportError(code: PublisherErrorCode, message: String) {
    if let promise = startPromise {
      startPromise = nil
      deadline?.cancel()
      deadline = nil
      promise.reject(withError: PublisherFailure(code: code, message: message))
    } else {
      errorCallback?(PublisherError(code: code, message: message))
    }
  }

  private func setState(_ state: PublisherState) {
    lock.lock()
    let previous = snapshotState
    snapshotState = state
    lock.unlock()
    if state == previous { return }
    switch state {
    case .publishing:
      if let promise = startPromise {
        startPromise = nil
        deadline?.cancel()
        deadline = nil
        promise.resolve()
      }
      mixer?.startEncoding(sink: self)  // The first encoded frame is a keyframe.
    case .stopped, .failed:
      mixer?.stopEncoding()
      // The core ended the session on its own (server closed, protocol error).
      if !isStopping {
        transport?.cancel()
        transport = nil
      }
    default:
      break
    }
    stateCallback?(state)
  }

  // MARK: - Core callbacks (synchronous, inside core calls on `queue`)

  private static let coreCallbacks: nitrortmp_callbacks_t = {
    var callbacks = nitrortmp_callbacks_t()
    callbacks.on_send = { ctx, data, size in
      guard let ctx, let data, size > 0 else { return }
      let publisher = Unmanaged<HybridRtmpPublisher>.fromOpaque(ctx).takeUnretainedValue()
      publisher.transport?.send(Data(bytes: data, count: size))
    }
    callbacks.on_state = { ctx, state in
      guard let ctx else { return }
      let publisher = Unmanaged<HybridRtmpPublisher>.fromOpaque(ctx).takeUnretainedValue()
      publisher.coreStateChanged(state)
    }
    callbacks.on_error = { ctx, code, message in
      guard let ctx else { return }
      let publisher = Unmanaged<HybridRtmpPublisher>.fromOpaque(ctx).takeUnretainedValue()
      publisher.coreError(code, message: message.map { String(cString: $0) } ?? "")
    }
    return callbacks
  }()

  private func coreStateChanged(_ raw: Int32) {
    guard !suppressCoreState,
          let name = nitrortmp_state_name(raw),
          let state = PublisherState(fromString: String(cString: name)) else { return }
    setState(state)
  }

  private func coreError(_ raw: Int32, message: String) {
    guard let name = nitrortmp_error_name(raw),
          let code = PublisherErrorCode(fromString: String(cString: name)) else { return }
    reportError(code: code, message: message)
  }

  // MARK: - Snapshot

  private func refreshStats() {
    guard let core else { return }
    var s = nitrortmp_stats_t()
    nitrortmp_publisher_stats(core, &s)
    let stats = HybridRtmpPublisher.makeStats(s, queuedBytes: transport?.queuedBytes ?? 0)
    lock.lock()
    snapshotStats = stats
    lock.unlock()
  }

  private func updateQueuedBytes(_ bytes: Int) {
    lock.lock()
    let s = snapshotStats
    snapshotStats = PublisherStats(
      bytesSent: s.bytesSent, videoTags: s.videoTags, audioTags: s.audioTags,
      rejectedFrames: s.rejectedFrames, droppedBeforeKeyframe: s.droppedBeforeKeyframe,
      invalidFrames: s.invalidFrames, timestampClamps: s.timestampClamps,
      queuedBytes: Double(bytes),
      lastVideoTimestamp: s.lastVideoTimestamp, lastAudioTimestamp: s.lastAudioTimestamp)
    lock.unlock()
  }

  private static func makeStats(_ s: nitrortmp_stats_t?, queuedBytes: Int) -> PublisherStats {
    let s = s ?? nitrortmp_stats_t()
    return PublisherStats(
      bytesSent: Double(s.bytes_sent), videoTags: Double(s.video_tags), audioTags: Double(s.audio_tags),
      rejectedFrames: Double(s.rejected_frames), droppedBeforeKeyframe: Double(s.dropped_before_keyframe),
      invalidFrames: Double(s.invalid_frames), timestampClamps: Double(s.timestamp_clamps),
      queuedBytes: Double(queuedBytes),
      lastVideoTimestamp: Double(s.last_video_timestamp), lastAudioTimestamp: Double(s.last_audio_timestamp))
  }

  /// JS milliseconds to the core's uint32 clock.
  private static func timestamp(_ ms: Double) -> UInt32 {
    guard ms.isFinite, ms > 0 else { return 0 }
    return ms >= Double(UInt32.max) ? UInt32.max : UInt32(ms)
  }
}

// MARK: - EncodedFrameSink

extension HybridRtmpPublisher: EncodedFrameSink {
  /// Encoder output thread: post and return. Frames still in the encoder's
  /// pipeline when the session ends are discarded here, so `rejectedFrames`
  /// keeps counting only the external `pushVideo`/`pushAudio` misuse.
  func pushEncodedVideo(_ annexb: Data, ptsMs: UInt32, dtsMs: UInt32) {
    queue.async { [weak self] in
      guard let self, let core = self.core, self.state == .publishing else { return }
      annexb.withUnsafeBytes { buffer in
        guard let base = buffer.baseAddress else { return }
        _ = nitrortmp_publisher_push_video(core, base.assumingMemoryBound(to: UInt8.self), buffer.count, ptsMs, dtsMs)
      }
      self.refreshStats()
    }
  }

  func pushEncodedAudio(_ adts: Data, ptsMs: UInt32) {
    queue.async { [weak self] in
      guard let self, let core = self.core, self.state == .publishing else { return }
      adts.withUnsafeBytes { buffer in
        guard let base = buffer.baseAddress else { return }
        _ = nitrortmp_publisher_push_audio(core, base.assumingMemoryBound(to: UInt8.self), buffer.count, ptsMs)
      }
      self.refreshStats()
    }
  }
}
