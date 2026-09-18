import AVFoundation
import Foundation
import UIKit

/// The AVCaptureSession behind `CameraSource`.
///
/// Frames come out as upright BGRA `CVPixelBuffer`s: the connection is rotated
/// to the interface orientation read when the capture starts, never mirrored
/// (the front camera is mirrored in the preview only). Every session call runs
/// on `queue`; the delegate delivers on it too and only hands the buffer on.
final class CameraSource: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate {
  let queue = DispatchQueue(label: "nitrortmp.camera")
  private let session = AVCaptureSession()
  private let output = AVCaptureVideoDataOutput()
  private var input: AVCaptureDeviceInput?
  private var orientation: AVCaptureVideoOrientation = .portrait
  private var rotationAngle: CGFloat = 90
  private var frameRate = 30.0  // queue only; retained across camera switches

  /// Called on `queue` with the frame and its host-clock time in seconds.
  var onFrame: ((CVPixelBuffer, CFTimeInterval) -> Void)?

  override init() {
    super.init()
    output.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA]
    output.alwaysDiscardsLateVideoFrames = true  // the compositor never gets a backlog
    output.setSampleBufferDelegate(self, queue: queue)
  }

  // MARK: - Permission

  static func requestAccess(_ completion: @escaping (Bool) -> Void) {
    switch AVCaptureDevice.authorizationStatus(for: .video) {
    case .authorized:
      completion(true)
    case .notDetermined:
      AVCaptureDevice.requestAccess(for: .video, completionHandler: completion)
    default:
      completion(false)
    }
  }

  // MARK: - Lifecycle

  /// Configures and starts the session. `completion` runs on `queue`.
  func start(position: AVCaptureDevice.Position, preferHD: Bool, frameRate: Double,
             completion: @escaping (CaptureFailure?) -> Void) {
    // The interface orientation lives on the main thread.
    DispatchQueue.main.async { [weak self] in
      let orientation = CameraSource.currentInterfaceOrientation()
      self?.queue.async {
        guard let self else { return }
        self.orientation = orientation
        self.rotationAngle = CameraSource.rotationAngle(for: orientation)
        self.frameRate = frameRate
        completion(self.configureAndStart(position: position, preferHD: preferHD))
      }
    }
  }

  /// Swaps the camera while running (the clock is unchanged).
  func setPosition(_ position: AVCaptureDevice.Position, completion: ((CaptureFailure?) -> Void)? = nil) {
    queue.async { [weak self] in
      guard let self else { return }
      guard self.session.isRunning else {
        completion?(nil)
        return
      }
      completion?(self.replaceInput(position: position))
    }
  }

  /// Applies mixer settings to an already running camera as well as the next start.
  func setFrameRate(_ frameRate: Double, completion: @escaping (CaptureFailure?) -> Void) {
    queue.async { [weak self] in
      guard let self else { return }
      guard self.frameRate != frameRate else { completion(nil); return }
      self.frameRate = frameRate
      guard self.session.isRunning, let device = self.input?.device else { completion(nil); return }
      completion(self.configureFrameRate(device))
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

  private func configureAndStart(position: AVCaptureDevice.Position, preferHD: Bool) -> CaptureFailure? {
    session.beginConfiguration()
    let preset: AVCaptureSession.Preset = preferHD ? .hd1920x1080 : .hd1280x720
    if session.canSetSessionPreset(preset) {
      session.sessionPreset = preset
    } else if session.canSetSessionPreset(.hd1280x720) {
      session.sessionPreset = .hd1280x720
    }
    if !session.outputs.contains(output) {
      guard session.canAddOutput(output) else {
        session.commitConfiguration()
        return CaptureFailure(code: .configurationfailed, message: "the session refused the video data output")
      }
      session.addOutput(output)
    }
    if let failure = replaceInputLocked(position: position) {
      session.commitConfiguration()
      return failure
    }
    session.commitConfiguration()
    // Committing a preset can reset the device's frame duration, so set it afterwards.
    if let device = input?.device, let failure = configureFrameRate(device) {
      return failure
    }
    if !session.isRunning {
      session.startRunning()
    }
    return nil
  }

  private func replaceInput(position: AVCaptureDevice.Position) -> CaptureFailure? {
    session.beginConfiguration()
    let failure = replaceInputLocked(position: position)
    session.commitConfiguration()
    if let failure { return failure }
    return input.flatMap { configureFrameRate($0.device) }
  }

  /// Inside begin/commitConfiguration: swaps the device input and configures
  /// the (new) connection. Keeps the old input when the new device is missing.
  private func replaceInputLocked(position: AVCaptureDevice.Position) -> CaptureFailure? {
    guard let device = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: position) else {
      return CaptureFailure(code: .cameraunavailable,
                            message: "no \(position == .front ? "front" : "back") camera on this device")
    }
    let newInput: AVCaptureDeviceInput
    do {
      newInput = try AVCaptureDeviceInput(device: device)
    } catch {
      return CaptureFailure(code: .cameraunavailable, message: "camera cannot be opened: \(error.localizedDescription)")
    }
    let previous = input
    if let previous {
      session.removeInput(previous)
    }
    guard session.canAddInput(newInput) else {
      if let previous, session.canAddInput(previous) {
        session.addInput(previous)
      }
      return CaptureFailure(code: .configurationfailed, message: "the session refused the camera input")
    }
    session.addInput(newInput)
    input = newInput
    configureConnection()
    return nil
  }

  private func configureFrameRate(_ device: AVCaptureDevice) -> CaptureFailure? {
    guard frameRate.isFinite, frameRate > 0 else {
      return CaptureFailure(code: .configurationfailed, message: "camera frame rate must be finite and positive")
    }
    #if targetEnvironment(simulator)
    // The simulator has no camera hardware; the only capture devices it ever
    // hands out are injected stand-ins (serve-sim) whose frame-duration setters
    // raise an Objective-C exception that Swift cannot catch. Frame pacing is
    // left to the stand-in.
    return nil
    #else
    let supported = device.activeFormat.videoSupportedFrameRateRanges.contains {
      $0.minFrameRate <= frameRate && $0.maxFrameRate >= frameRate
    }
    guard supported else {
      return CaptureFailure(code: .configurationfailed,
                            message: "the camera format does not support \(frameRate) fps")
    }
    do {
      try device.lockForConfiguration()
    } catch {
      return CaptureFailure(code: .configurationfailed, message: "camera frame rate: \(error.localizedDescription)")
    }
    defer { device.unlockForConfiguration() }
    // Preserve fractional rates such as 29.97 instead of rounding to an integer.
    let duration = CMTime(seconds: 1 / frameRate, preferredTimescale: 1_000_000_000)
    device.activeVideoMinFrameDuration = duration
    device.activeVideoMaxFrameDuration = duration
    return nil
    #endif
  }

  /// Upright for the interface orientation, never mirrored.
  private func configureConnection() {
    guard let connection = output.connection(with: .video) else { return }
    if #available(iOS 17.0, *) {
      if connection.isVideoRotationAngleSupported(rotationAngle) {
        connection.videoRotationAngle = rotationAngle
      }
    } else if connection.isVideoOrientationSupported {
      connection.videoOrientation = orientation
    }
    if connection.isVideoMirroringSupported {
      connection.automaticallyAdjustsVideoMirroring = false
      connection.isVideoMirrored = false
    }
  }

  // MARK: - Orientation

  /// Main thread only.
  private static func currentInterfaceOrientation() -> AVCaptureVideoOrientation {
    let scene = UIApplication.shared.connectedScenes
      .compactMap { $0 as? UIWindowScene }
      .first { $0.activationState == .foregroundActive }
    switch scene?.interfaceOrientation ?? .portrait {
    case .portraitUpsideDown: return .portraitUpsideDown
    case .landscapeLeft: return .landscapeLeft
    case .landscapeRight: return .landscapeRight
    default: return .portrait
    }
  }

  private static func rotationAngle(for orientation: AVCaptureVideoOrientation) -> CGFloat {
    switch orientation {
    case .portrait: return 90
    case .portraitUpsideDown: return 270
    case .landscapeRight: return 0
    case .landscapeLeft: return 180
    @unknown default: return 90
    }
  }

  // MARK: - AVCaptureVideoDataOutputSampleBufferDelegate (on queue)

  func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer,
                     from connection: AVCaptureConnection) {
    guard let pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }
    // Camera timestamps are on the host clock, like CACurrentMediaTime.
    onFrame?(pixelBuffer, CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sampleBuffer)))
  }
}
