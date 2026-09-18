import CoreMedia
import Foundation
import NitroModules

/// The `MicrophoneSource` HybridObject. PCM goes to the
/// attached mixer's AAC encoder on the microphone queue; `muted` keeps the
/// stream running with silence so the timeline never stops.
final class HybridMicrophoneSource: HybridMicrophoneSourceSpec {
  private let source = MicrophoneSource()
  private let lock = NSLock()
  private var isMuted = false
  private var running = false
  private weak var mixer: HybridMixer?

  override init() {
    super.init()
    source.onSampleBuffer = { [weak self] sampleBuffer in self?.sampleArrived(sampleBuffer) }
  }

  deinit {
    source.stop()
  }

  func dispose() {
    setRunning(false)
    source.stop()
  }

  // MARK: - HybridMicrophoneSourceSpec

  var muted: Bool {
    get {
      lock.lock()
      defer { lock.unlock() }
      return isMuted
    }
    set {
      lock.lock()
      isMuted = newValue
      lock.unlock()
    }
  }

  var isRunning: Bool {
    lock.lock()
    defer { lock.unlock() }
    return running
  }

  func start() throws -> Promise<Void> {
    let promise = Promise<Void>()
    MicrophoneSource.requestAccess { [weak self] granted in
      guard let self else {
        promise.reject(withError: CaptureFailure(code: .configurationfailed, message: "microphone source was released"))
        return
      }
      guard granted else {
        promise.reject(withError: CaptureFailure(code: .permissiondenied, message: "microphone access was denied"))
        return
      }
      self.source.start { failure in
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

  func attach(to mixer: HybridMixer) {
    lock.lock()
    self.mixer = mixer
    lock.unlock()
  }

  func detach(from mixer: HybridMixer) {
    lock.lock()
    if self.mixer === mixer { self.mixer = nil }
    lock.unlock()
  }

  // MARK: - Internals

  private func setRunning(_ value: Bool) {
    lock.lock()
    running = value
    lock.unlock()
  }

  private func sampleArrived(_ sampleBuffer: CMSampleBuffer) {
    lock.lock()
    let mixer = self.mixer
    let muted = isMuted
    lock.unlock()
    mixer?.audioArrived(sampleBuffer, muted: muted)
  }
}
