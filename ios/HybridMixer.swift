import CoreMedia
import Foundation
import NitroModules
import QuartzCore

/// The `Mixer` HybridObject: the scene,
/// the output settings, the compositor (`VideoMixer`) and both encoders.
///
/// Threading: JS calls that touch the scene are posted to the render queue;
/// `video`/`audio`/`stats` use a lock. The H.264 encoder is fed from the
/// render queue, the AAC encoder from the microphone queue (under
/// `audioLock`), and both encoders hand their output to the session's sink.
final class HybridMixer: HybridMixerSpec {
  let videoMixer = VideoMixer()

  private let lock = NSLock()
  private var videoSettings = VideoSettings(width: 720, height: 1280, frameRate: 30, bitrateKbps: 2500,
                                            keyframeIntervalSeconds: 2)
  private var audioSettings = AudioSettings(sampleRate: 48000, channels: 1, bitrateKbps: 128)
  private var errorCallback: ((CaptureError) -> Void)?
  private var encoding = false
  private var encodedVideoFrames: UInt64 = 0
  private var encodedAudioFrames: UInt64 = 0
  private var encoderFailures: UInt64 = 0
  private var outputWindow: [(time: CFTimeInterval, bytes: Int)] = []  // last second of encoder output
  private var cameraLayerAttached = false
  private var videoStarted = false
  private var epoch: CFTimeInterval?  // the capture time of the first frame handed to the encoders
  private weak var sink: EncodedFrameSink?
  private var pendingAudio: [(adts: Data, time: CFTimeInterval)] = []  // until the first video frame is out
  private static let maxPendingAudio = 100  // about 2 s of AAC packets

  // Render queue only.
  private var h264: H264Encoder?

  // Microphone queue and start/stop, under audioLock.
  private let audioLock = NSLock()
  private var aac: AacEncoder?
  private var microphone: HybridMicrophoneSource?

  override init() {
    super.init()
    let settings = videoSettings
    videoMixer.queue.async { [weak self] in
      self?.videoMixer.setOutput(width: Int(settings.width), height: Int(settings.height), frameRate: settings.frameRate)
    }
  }

  func dispose() {
    stopEncoding()
    videoMixer.queue.async { [weak self] in
      guard let self else { return }
      for layer in self.videoMixer.layers {
        self.detach(layer)
      }
      self.videoMixer.layers.forEach { self.videoMixer.removeLayer($0.object) }
    }
    setAudioSourceInternal(nil)
  }

  // MARK: - HybridMixerSpec

  var video: VideoSettings {
    get {
      lock.lock()
      defer { lock.unlock() }
      return videoSettings
    }
    set {
      lock.lock()
      videoSettings = newValue  // applied at the next startEncoding when already encoding
      let apply = !encoding
      lock.unlock()
      if apply {
        videoMixer.queue.async { [weak self] in
          self?.videoMixer.setOutput(width: Int(newValue.width), height: Int(newValue.height),
                                     frameRate: newValue.frameRate)
        }
      }
    }
  }

  var audio: AudioSettings {
    get {
      lock.lock()
      defer { lock.unlock() }
      return audioSettings
    }
    set {
      lock.lock()
      audioSettings = newValue
      lock.unlock()
    }
  }

  var stats: MixerStats {
    let (captured, rendered, dropped) = videoMixer.counters
    lock.lock()
    let now = CACurrentMediaTime()
    let bytes = outputWindow.filter { now - $0.time <= 1 }.reduce(0) { $0 + $1.bytes }
    let stats = MixerStats(
      capturedFrames: Double(captured), renderedFrames: Double(rendered), droppedFrames: Double(dropped),
      encodedVideoFrames: Double(encodedVideoFrames), encodedAudioFrames: Double(encodedAudioFrames),
      videoBitrateKbps: Double(bytes) * 8 / 1000, encoderFailures: Double(encoderFailures))
    lock.unlock()
    return stats
  }

  var isEncoding: Bool {
    lock.lock()
    defer { lock.unlock() }
    return encoding
  }

  func addLayer(layer: any HybridVideoLayerSpec, frame: LayerFrame?) throws {
    guard let content = content(of: layer) else { return }
    let scene = SceneLayer(content: content, frame: frame)
    videoMixer.queue.async { [weak self] in
      guard let self else { return }
      guard !self.videoMixer.layers.contains(where: { $0.object === scene.object }) else { return }
      self.attach(scene)
      self.videoMixer.addLayer(scene)
      self.updateCameraLayerFlag()
    }
  }

  func removeLayer(layer: any HybridVideoLayerSpec) throws {
    let object = layer as AnyObject
    videoMixer.queue.async { [weak self] in
      guard let self else { return }
      if let scene = self.videoMixer.layers.first(where: { $0.object === object }) {
        self.detach(scene)
      }
      self.videoMixer.removeLayer(object)
      self.updateCameraLayerFlag()
    }
  }

  func setLayerFrame(layer: any HybridVideoLayerSpec, frame: LayerFrame) throws {
    let object = layer as AnyObject
    videoMixer.queue.async { [weak self] in self?.videoMixer.setFrame(frame, of: object) }
  }

  func setAudioSource(source: (any HybridMicrophoneSourceSpec)?) throws {
    setAudioSourceInternal(source as? HybridMicrophoneSource)
  }

  func onError(callback: @escaping (CaptureError) -> Void) throws {
    lock.lock()
    errorCallback = callback
    lock.unlock()
  }

  // MARK: - Session link (called by HybridRtmpPublisher on its queue)

  /// Creates both encoders with the current settings and starts feeding them.
  /// Idempotent. The first video frame is forced to be a keyframe.
  func startEncoding(sink: EncodedFrameSink) {
    videoMixer.queue.async { [weak self] in
      guard let self else { return }
      self.lock.lock()
      let alreadyEncoding = self.encoding
      let videoSettings = self.videoSettings
      let audioSettings = self.audioSettings
      self.lock.unlock()
      guard !alreadyEncoding else { return }

      guard self.videoMixer.isAvailable, let encoder = H264Encoder(settings: videoSettings) else {
        self.reportEncoderFailure("could not create the H.264 encoder for \(Int(videoSettings.width))x\(Int(videoSettings.height))")
        return
      }
      encoder.sink = sink
      encoder.onOutput = { [weak self] bytes in self?.videoFrameEncoded(bytes: bytes) }
      encoder.onFailure = { [weak self] message in self?.reportEncoderFailure(message) }
      self.h264 = encoder

      let aac = AacEncoder(settings: audioSettings)
      aac?.onPacket = { [weak self] adts, time in self?.audioPacketEncoded(adts, time: time) }
      aac?.onFailure = { [weak self] message in self?.reportEncoderFailure(message) }
      if aac == nil {
        self.reportEncoderFailure("could not create the AAC encoder for \(Int(audioSettings.sampleRate)) Hz")
      }
      self.audioLock.lock()
      self.aac = aac
      self.audioLock.unlock()

      self.lock.lock()
      self.encoding = true
      self.sink = sink
      self.epoch = nil
      self.videoStarted = false
      self.pendingAudio.removeAll()
      self.outputWindow.removeAll()
      self.lock.unlock()

      self.videoMixer.setOutput(width: Int(videoSettings.width), height: Int(videoSettings.height),
                                frameRate: videoSettings.frameRate)
      self.videoMixer.setEncodeHandler { [weak self] pixelBuffer, time in
        guard let self, let h264 = self.h264 else { return false }
        return h264.encode(pixelBuffer: pixelBuffer, ptsMs: self.ptsMs(for: time))
      }
    }
  }

  /// Stops feeding the encoders and invalidates them. Idempotent.
  func stopEncoding() {
    videoMixer.queue.async { [weak self] in
      guard let self else { return }
      self.videoMixer.setEncodeHandler(nil)
      self.h264?.invalidate()
      self.h264 = nil
      self.audioLock.lock()
      self.aac = nil
      self.audioLock.unlock()
      self.lock.lock()
      self.encoding = false
      self.sink = nil
      self.pendingAudio.removeAll()
      self.lock.unlock()
    }
  }

  /// Microphone queue: encode synchronously; `audioPacketEncoded` holds
  /// the packets back until the first video frame is out.
  func audioArrived(_ sampleBuffer: CMSampleBuffer, muted: Bool) {
    audioLock.lock()
    defer { audioLock.unlock() }
    aac?.encode(sampleBuffer, muted: muted)
  }

  /// Capture reconfiguration failures are separate from encoder failures.
  func reportCaptureFailure(_ failure: CaptureFailure) {
    lock.lock()
    let callback = errorCallback
    lock.unlock()
    callback?(CaptureError(code: failure.code, message: failure.message))
  }

  // MARK: - Internals

  private func content(of layer: any HybridVideoLayerSpec) -> SceneLayer.Content? {
    if let camera = layer as? HybridCameraSource { return .camera(camera) }
    if let image = layer as? HybridImageLayer { return .image(image) }
    return nil
  }

  private func attach(_ scene: SceneLayer) {
    switch scene.content {
    case .camera(let camera): camera.attach(to: self)
    case .image(let image): image.attach(to: self)
    }
  }

  private func detach(_ scene: SceneLayer) {
    switch scene.content {
    case .camera(let camera): camera.detach(from: self)
    case .image(let image): image.detach(from: self)
    }
  }

  private func updateCameraLayerFlag() {
    let attached = videoMixer.hasCameraLayer
    lock.lock()
    cameraLayerAttached = attached
    lock.unlock()
  }

  private func setAudioSourceInternal(_ source: HybridMicrophoneSource?) {
    audioLock.lock()
    let previous = microphone
    microphone = source
    audioLock.unlock()
    if let previous, previous !== source {
      previous.detach(from: self)
    }
    source?.attach(to: self)
  }

  /// Host clock seconds to the session's millisecond clock. The first
  /// frame handed to the video encoder defines t=0; a wall-clock epoch taken at
  /// `startEncoding` would clamp every already-captured frame to 0 and hand the
  /// muxer duplicate timestamps.
  private func ptsMs(for time: CFTimeInterval) -> UInt32 {
    lock.lock()
    if epoch == nil { epoch = time }
    let epoch = self.epoch ?? time
    lock.unlock()
    return HybridMixer.milliseconds(time - epoch)
  }

  /// One ADTS packet from the AAC encoder (microphone queue). While a camera
  /// layer is attached and no video frame has reached the session yet, packets
  /// are kept (not dropped) so audio starts within one packet of the video;
  /// `videoFrameEncoded` flushes them. Packets captured before the epoch (the
  /// first video frame) are dropped. Without a camera layer the first audio
  /// packet defines the epoch.
  private func audioPacketEncoded(_ adts: Data, time: CFTimeInterval) {
    lock.lock()
    if cameraLayerAttached && !videoStarted {
      pendingAudio.append((adts: adts, time: time))
      if pendingAudio.count > HybridMixer.maxPendingAudio { pendingAudio.removeFirst() }
      lock.unlock()
      return
    }
    if epoch == nil { epoch = time }
    pushAudioLocked(adts, time: time)
    lock.unlock()
  }

  /// Caller holds `lock` and `epoch` is set. A packet that ended before the
  /// epoch is dropped; the one spanning it is kept at pts 0, so audio starts
  /// within one packet of the video.
  private func pushAudioLocked(_ adts: Data, time: CFTimeInterval) {
    guard let epoch, let sink else { return }
    let packetDuration = 1024 / max(audioSettings.sampleRate, 8000)
    guard time + packetDuration > epoch else { return }
    encodedAudioFrames += 1
    sink.pushEncodedAudio(adts, ptsMs: HybridMixer.milliseconds(time - epoch))
  }

  private static func milliseconds(_ seconds: CFTimeInterval) -> UInt32 {
    let ms = seconds * 1000
    guard ms.isFinite, ms > 0 else { return 0 }
    return ms >= Double(UInt32.max) ? UInt32.max : UInt32(ms.rounded())
  }

  /// Encoder output thread, after the frame was posted to the session: the
  /// first frame releases the audio packets kept in `audioPacketEncoded`.
  private func videoFrameEncoded(bytes: Int) {
    let now = CACurrentMediaTime()
    lock.lock()
    encodedVideoFrames += 1
    if !videoStarted {
      videoStarted = true
      for packet in pendingAudio { pushAudioLocked(packet.adts, time: packet.time) }
      pendingAudio.removeAll()
    }
    outputWindow.append((time: now, bytes: bytes))
    outputWindow.removeAll { now - $0.time > 1 }
    lock.unlock()
  }

  private func reportEncoderFailure(_ message: String) {
    lock.lock()
    encoderFailures += 1
    let callback = errorCallback
    lock.unlock()
    callback?(CaptureError(code: .encoderfailed, message: message))
  }
}
