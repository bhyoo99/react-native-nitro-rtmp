import AVFoundation
import CoreMedia
import Foundation
import NitroModules
import VisionCamera

/// The `CameraLayer` HybridObject: a mixer layer fed by the VisionCamera
/// output it owns (`RtmpCameraOutput`, exposed as `output`). The latest frame
/// waits in a one-slot queue; the compositor takes it on the render queue, and
/// a newer frame replacing an untaken one is a drop. The encoder never sees
/// this object.
final class HybridCameraLayer: HybridCameraLayerSpec {
  private let lock = NSLock()
  private var latest: CameraFrame?
  private var latestConsumed = true
  private weak var mixer: HybridMixer?
  private var frontCamera = false
  private var receiving = false
  private lazy var cameraOutput = RtmpCameraOutput(layer: self)

  func dispose() {
    cameraOutput.invalidate()
  }

  // MARK: - HybridVideoLayerSpec / HybridCameraLayerSpec

  var kind: LayerKind { .camera }

  var output: any HybridCameraOutputSpec { cameraOutput }

  var isFrontCamera: Bool {
    lock.lock()
    defer { lock.unlock() }
    return frontCamera
  }

  var isReceivingFrames: Bool {
    lock.lock()
    defer { lock.unlock() }
    return receiving
  }

  // MARK: - Mixer side

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

  /// The mixer whose output size the camera format should match.
  var attachedMixer: HybridMixer? {
    lock.lock()
    defer { lock.unlock() }
    return mixer
  }

  // MARK: - Output side (camera queue)

  /// Store the frame and wake the compositor (never blocks).
  func frameArrived(_ pixelBuffer: CVPixelBuffer, time: CFTimeInterval) {
    lock.lock()
    let replaced = !latestConsumed
    latest = CameraFrame(pixelBuffer: pixelBuffer, time: time)
    latestConsumed = false
    let mixer = self.mixer
    lock.unlock()
    mixer?.videoMixer.frameAvailable(time: time, replacedPending: replaced)
  }

  func setFrontCamera(_ value: Bool) {
    lock.lock()
    frontCamera = value
    lock.unlock()
  }

  /// Frames started or stopped arriving: the mixer's timer mode may change.
  func setReceiving(_ value: Bool) {
    lock.lock()
    let changed = receiving != value
    receiving = value
    let mixer = self.mixer
    lock.unlock()
    if changed { mixer?.videoMixer.cameraStateChanged() }
  }
}

/// The VisionCamera output behind `CameraLayer.output`: an
/// `AVCaptureVideoDataOutput` that VisionCamera adds to the session it owns and
/// connects to the camera it manages. Frames come out as BGRA `CVPixelBuffer`s
/// rotated to `outputOrientation` (set by VisionCamera from the device
/// orientation) and never mirrored; the layer mirrors a front camera in the
/// preview only. Delivered on `queue`; the delegate only hands the buffer on.
final class RtmpCameraOutput: HybridCameraOutputSpec, NativeCameraOutput {
  typealias Output = AVCaptureVideoDataOutput

  let output = AVCaptureVideoDataOutput()
  let requiresAudioInput = false
  let requiresDepthFormat = false
  let streamType: StreamType = .video

  private let queue = DispatchQueue(label: "nitrortmp.camera")
  private let delegate = FrameDelegate()
  private let lock = NSLock()
  private weak var layer: HybridCameraLayer?
  private var orientation: CameraOrientation = .up
  private var stallTimer: DispatchSourceTimer?
  /// No frame for this long means the session stopped: the mixer's own clock takes over.
  private static let stallTimeout: DispatchTimeInterval = .milliseconds(700)

  init(layer: HybridCameraLayer) {
    self.layer = layer
    super.init()
    output.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA]
    output.alwaysDiscardsLateVideoFrames = true  // the compositor never gets a backlog
    delegate.onFrame = { [weak self] pixelBuffer, time in self?.frameArrived(pixelBuffer, time: time) }
    output.setSampleBufferDelegate(delegate, queue: queue)
  }

  /// The layer is gone: stop delivering. VisionCamera may still hold the output.
  /// Everything here runs on the camera queue: `onFrame` is read there by every
  /// frame, so it is only ever written there too (set once before the delegate
  /// is registered, cleared here).
  func invalidate() {
    queue.async { [self] in
      delegate.onFrame = nil
      stallTimer?.cancel()
      stallTimer = nil
      layer?.setReceiving(false)
    }
  }

  // MARK: - HybridCameraOutputSpec

  var mediaType: MediaType { .video }

  var outputOrientation: CameraOrientation {
    get {
      lock.lock()
      defer { lock.unlock() }
      return orientation
    }
    set {
      lock.lock()
      orientation = newValue
      lock.unlock()
      applyOrientation()
    }
  }

  var currentResolution: Size? {
    guard let port = output.connection(with: .video)?.inputPorts.first,
          let description = port.formatDescription else { return nil }
    let dimensions = CMVideoFormatDescriptionGetDimensions(description)
    return Size(width: Double(dimensions.width), height: Double(dimensions.height))
  }

  // MARK: - ResolutionNegotiationParticipant

  /// The mixer's output size, as a landscape camera format (the connection
  /// rotates it). 1280x720 until the layer is added to a mixer.
  var targetResolution: ResolutionRule {
    let settings = layer?.attachedMixer?.video
    let width = settings?.width ?? 1280
    let height = settings?.height ?? 720
    return .closestTo(Size(width: max(width, height), height: min(width, height)))
  }

  // MARK: - NativeCameraOutput

  /// Inside VisionCamera's begin/commitConfiguration, after the connection exists.
  func configure(config: OutputConfiguration) {
    applyOrientation()
    updateFrontCamera()
  }

  // MARK: - Internals

  /// Upright for `outputOrientation`, never mirrored.
  private func applyOrientation() {
    guard let connection = output.connection(with: .video) else { return }
    lock.lock()
    let orientation = self.orientation
    lock.unlock()
    if #available(iOS 17.0, *) {
      let angle = RtmpCameraOutput.rotationAngle(for: orientation)
      if connection.isVideoRotationAngleSupported(angle) {
        connection.videoRotationAngle = angle
      }
    } else if connection.isVideoOrientationSupported {
      connection.videoOrientation = RtmpCameraOutput.videoOrientation(for: orientation)
    }
    if connection.isVideoMirroringSupported {
      connection.automaticallyAdjustsVideoMirroring = false
      connection.isVideoMirrored = false
    }
  }

  private func updateFrontCamera() {
    let input = output.connection(with: .video)?.inputPorts.first?.input as? AVCaptureDeviceInput
    layer?.setFrontCamera(input?.device.position == .front)
  }

  /// Camera queue.
  private func frameArrived(_ pixelBuffer: CVPixelBuffer, time: CFTimeInterval) {
    guard let layer else { return }
    if stallTimer == nil {
      updateFrontCamera()
      let timer = DispatchSource.makeTimerSource(queue: queue)
      timer.setEventHandler { [weak self] in self?.layer?.setReceiving(false) }
      timer.resume()
      stallTimer = timer
    }
    stallTimer?.schedule(deadline: .now() + RtmpCameraOutput.stallTimeout)
    layer.setReceiving(true)
    layer.frameArrived(pixelBuffer, time: time)
  }

  /// The same mapping VisionCamera uses for its own outputs.
  private static func videoOrientation(for orientation: CameraOrientation) -> AVCaptureVideoOrientation {
    switch orientation {
    case .up: return .portrait
    case .down: return .portraitUpsideDown
    case .left: return .landscapeRight
    case .right: return .landscapeLeft
    }
  }

  private static func rotationAngle(for orientation: CameraOrientation) -> CGFloat {
    switch orientation {
    case .up: return 90
    case .down: return 270
    case .left: return 0
    case .right: return 180
    }
  }
}

/// `AVCaptureVideoDataOutput` wants an NSObject delegate; the output is a Swift class.
private final class FrameDelegate: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate {
  /// Called on the output's queue with the frame and its host-clock time in seconds.
  var onFrame: ((CVPixelBuffer, CFTimeInterval) -> Void)?

  func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer,
                     from connection: AVCaptureConnection) {
    guard let pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }
    // Camera timestamps are on the host clock, like CACurrentMediaTime and the microphone.
    onFrame?(pixelBuffer, CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sampleBuffer)))
  }
}
