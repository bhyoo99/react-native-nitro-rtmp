import AVFoundation
import Foundation
import NitroModules

/// The `CameraSource` HybridObject: a mixer layer fed by
/// `CameraSource`. The latest frame waits in a one-slot queue; the compositor
/// takes it on the render queue, and a newer frame replacing an untaken one
/// is a drop. The encoder never sees this object.
final class HybridCameraSource: HybridCameraSourceSpec {
  private let source = CameraSource()
  private let lock = NSLock()
  private var desiredPosition: CameraPosition = .back
  private var running = false
  private var latest: CameraFrame?
  private var latestConsumed = true
  private weak var mixer: HybridMixer?

  override init() {
    super.init()
    source.onFrame = { [weak self] pixelBuffer, time in self?.frameArrived(pixelBuffer, time: time) }
  }

  deinit {
    source.stop()
  }

  func dispose() {
    setRunning(false)
    source.stop()
  }

  // MARK: - HybridVideoLayerSpec / HybridCameraSourceSpec

  var kind: LayerKind { .camera }

  var position: CameraPosition {
    get {
      lock.lock()
      defer { lock.unlock() }
      return desiredPosition
    }
    set {
      lock.lock()
      desiredPosition = newValue
      let running = self.running
      lock.unlock()
      if running {
        source.setPosition(HybridCameraSource.avPosition(newValue)) { [weak self] failure in
          if let failure { self?.reportConfigurationFailure(failure) }
        }
      }
    }
  }

  var isRunning: Bool {
    lock.lock()
    defer { lock.unlock() }
    return running
  }

  func start() throws -> Promise<Void> {
    let promise = Promise<Void>()
    let position = self.position
    CameraSource.requestAccess { [weak self] granted in
      guard let self else {
        promise.reject(withError: CaptureFailure(code: .configurationfailed, message: "camera source was released"))
        return
      }
      guard granted else {
        promise.reject(withError: CaptureFailure(code: .permissiondenied, message: "camera access was denied"))
        return
      }
      let settings = self.mixerForStart?.video
      let preferHD = (settings.map { $0.width * $0.height } ?? 0) > 1280 * 720
      self.source.start(position: HybridCameraSource.avPosition(position), preferHD: preferHD,
                        frameRate: settings?.frameRate ?? 30) { failure in
        if let failure {
          promise.reject(withError: failure)
        } else {
          self.setRunning(true)
          promise.resolve()
        }
      }
    }
    return promise
  }

  func stop() throws -> Promise<Void> {
    let promise = Promise<Void>()
    setRunning(false)
    source.stop { promise.resolve() }
    return promise
  }

  // MARK: - Mixer side

  func setFrameRate(_ frameRate: Double) {
    source.setFrameRate(frameRate) { [weak self] failure in
      if let failure { self?.reportConfigurationFailure(failure) }
    }
  }

  private func reportConfigurationFailure(_ failure: CaptureFailure) {
    mixerForStart?.reportCaptureFailure(failure)
  }

  func attach(to mixer: HybridMixer) {
    lock.lock()
    let previous = self.mixer
    self.mixer = mixer
    lock.unlock()
    if let previous, previous !== mixer {
      // A layer belongs to one mixer at a time.
      previous.videoMixer.queue.async { previous.videoMixer.removeLayer(self) }
    }
  }

  func detach(from mixer: HybridMixer) {
    lock.lock()
    if self.mixer === mixer { self.mixer = nil }
    lock.unlock()
  }

  /// Render queue: the newest frame, or nil when none arrived yet. The frame
  /// stays available for further renders; only the "untaken" flag is cleared.
  func latestFrame() -> CameraFrame? {
    lock.lock()
    defer { lock.unlock() }
    latestConsumed = true
    return latest
  }

  // MARK: - Internals

  private var mixerForStart: HybridMixer? {
    lock.lock()
    defer { lock.unlock() }
    return mixer
  }

  private func setRunning(_ value: Bool) {
    lock.lock()
    running = value
    let mixer = self.mixer
    lock.unlock()
    mixer?.videoMixer.cameraStateChanged()
  }

  /// Camera queue: store the frame and wake the compositor (never blocks).
  private func frameArrived(_ pixelBuffer: CVPixelBuffer, time: CFTimeInterval) {
    lock.lock()
    let replaced = !latestConsumed
    latest = CameraFrame(pixelBuffer: pixelBuffer, time: time)
    latestConsumed = false
    let mixer = self.mixer
    lock.unlock()
    mixer?.videoMixer.frameAvailable(time: time, replacedPending: replaced)
  }

  private static func avPosition(_ position: CameraPosition) -> AVCaptureDevice.Position {
    position == .front ? .front : .back
  }
}
