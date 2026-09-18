import AVFoundation
import Foundation

/// The AVCaptureSession behind `MicrophoneSource`: an
/// audio data output on its own queue. The sample buffers carry host-clock
/// timestamps, the same clock as the camera, so the mixer needs no offset.
///
/// The library owns the `AVAudioSession` (`.playAndRecord`, `.default` mode,
/// 48 kHz preferred) because the capture session is told not to configure it.
final class MicrophoneSource: NSObject, AVCaptureAudioDataOutputSampleBufferDelegate {
  let queue = DispatchQueue(label: "nitrortmp.audio")
  private let session = AVCaptureSession()
  private let output = AVCaptureAudioDataOutput()
  private var input: AVCaptureDeviceInput?

  /// Called on `queue` with LPCM sample buffers in the hardware format.
  var onSampleBuffer: ((CMSampleBuffer) -> Void)?

  override init() {
    super.init()
    session.automaticallyConfiguresApplicationAudioSession = false
    output.setSampleBufferDelegate(self, queue: queue)
  }

  // MARK: - Permission

  static func requestAccess(_ completion: @escaping (Bool) -> Void) {
    switch AVCaptureDevice.authorizationStatus(for: .audio) {
    case .authorized:
      completion(true)
    case .notDetermined:
      AVCaptureDevice.requestAccess(for: .audio, completionHandler: completion)
    default:
      completion(false)
    }
  }

  // MARK: - Lifecycle

  func start(completion: @escaping (CaptureFailure?) -> Void) {
    queue.async { [weak self] in
      guard let self else { return }
      completion(self.configureAndStart())
    }
  }

  func stop(completion: (() -> Void)? = nil) {
    queue.async { [weak self] in
      guard let self else {
        completion?()
        return
      }
      if self.session.isRunning {
        self.session.stopRunning()
      }
      completion?()
    }
  }

  // MARK: - Configuration (on queue)

  private func configureAndStart() -> CaptureFailure? {
    if let failure = configureAudioSession() {
      return failure
    }
    guard let device = AVCaptureDevice.default(for: .audio) else {
      return CaptureFailure(code: .cameraunavailable, message: "no microphone on this device")
    }
    if let failure = configure(device: device) {
      return failure
    }
    // Configuration must be committed before starting, or AVFoundation raises
    // an NSGenericException (which cannot be caught as a Swift Error).
    if !session.isRunning {
      session.startRunning()
    }
    return nil
  }

  private func configure(device: AVCaptureDevice) -> CaptureFailure? {
    session.beginConfiguration()
    defer { session.commitConfiguration() }
    if input == nil {
      do {
        let newInput = try AVCaptureDeviceInput(device: device)
        guard session.canAddInput(newInput) else {
          return CaptureFailure(code: .configurationfailed, message: "the session refused the microphone input")
        }
        session.addInput(newInput)
        input = newInput
      } catch {
        return CaptureFailure(code: .configurationfailed, message: "microphone cannot be opened: \(error.localizedDescription)")
      }
    }
    if !session.outputs.contains(output) {
      guard session.canAddOutput(output) else {
        return CaptureFailure(code: .configurationfailed, message: "the session refused the audio data output")
      }
      session.addOutput(output)
    }
    return nil
  }

  /// `.playAndRecord` with the `.default` mode (not `.videoChat`, which
  /// would narrow the band), 48 kHz preferred so the encoder rarely resamples.
  private func configureAudioSession() -> CaptureFailure? {
    let audioSession = AVAudioSession.sharedInstance()
    do {
      try audioSession.setCategory(.playAndRecord, mode: .default, options: [.defaultToSpeaker, .allowBluetooth])
      try audioSession.setPreferredSampleRate(48000)
      try audioSession.setActive(true)
    } catch {
      return CaptureFailure(code: .configurationfailed, message: "audio session: \(error.localizedDescription)")
    }
    return nil
  }

  // MARK: - AVCaptureAudioDataOutputSampleBufferDelegate (on queue)

  func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer,
                     from connection: AVCaptureConnection) {
    onSampleBuffer?(sampleBuffer)
  }
}
