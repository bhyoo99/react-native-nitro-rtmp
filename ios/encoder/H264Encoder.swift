import CoreMedia
import Foundation
import VideoToolbox

/// VideoToolbox H.264 encoder: real time, Main
/// profile, no B-frames, one keyframe every `keyframeIntervalSeconds`. Its
/// output is normalized for the core: AVCC becomes Annex-B and every keyframe
/// carries SPS and PPS in front (the C helpers in nitrortmp_c.h).
///
/// `encode` runs on the render queue; the output handler runs on a
/// VideoToolbox thread, copies the bytes once and hands them to the sink,
/// which posts to the session queue.
final class H264Encoder {
  private var session: VTCompressionSession?
  private let width: Int32
  private let height: Int32
  private let frameRate: Double
  private var forceKeyframe = true
  private var invalidated = false
  private let lock = NSLock()

  weak var sink: EncodedFrameSink?
  /// Called with the size of every frame handed to the sink (for the bitrate window).
  var onOutput: ((Int) -> Void)?
  /// Called once per failure with a description; the mixer reports `encoderFailed`.
  var onFailure: ((String) -> Void)?

  init?(settings: VideoSettings) {
    width = Int32(max(2, settings.width.rounded()))
    height = Int32(max(2, settings.height.rounded()))
    frameRate = settings.frameRate > 0 ? settings.frameRate : 30
    let keyframeInterval = settings.keyframeIntervalSeconds ?? 2
    let bitrate = max(1, Int(settings.bitrateKbps.rounded())) * 1000

    let imageAttributes: [CFString: Any] = [
      kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_32BGRA,
      kCVPixelBufferWidthKey: width,
      kCVPixelBufferHeightKey: height,
    ]
    var created: VTCompressionSession?
    let status = VTCompressionSessionCreate(
      allocator: kCFAllocatorDefault, width: width, height: height, codecType: kCMVideoCodecType_H264,
      encoderSpecification: nil, imageBufferAttributes: imageAttributes as CFDictionary,
      compressedDataAllocator: nil, outputCallback: nil, refcon: nil, compressionSessionOut: &created)
    guard status == noErr, let created else { return nil }
    session = created

    // Real time, no reordering, CBR-like limits over a 1 s window, GOP = keyframe interval.
    VTSessionSetProperty(created, key: kVTCompressionPropertyKey_RealTime, value: kCFBooleanTrue)
    VTSessionSetProperty(created, key: kVTCompressionPropertyKey_ProfileLevel, value: kVTProfileLevel_H264_Main_AutoLevel)
    VTSessionSetProperty(created, key: kVTCompressionPropertyKey_AllowFrameReordering, value: kCFBooleanFalse)
    VTSessionSetProperty(created, key: kVTCompressionPropertyKey_AverageBitRate, value: NSNumber(value: bitrate))
    VTSessionSetProperty(created, key: kVTCompressionPropertyKey_DataRateLimits,
                         value: [NSNumber(value: bitrate / 8), NSNumber(value: 1)] as CFArray)
    VTSessionSetProperty(created, key: kVTCompressionPropertyKey_ExpectedFrameRate, value: NSNumber(value: frameRate))
    VTSessionSetProperty(created, key: kVTCompressionPropertyKey_MaxKeyFrameInterval,
                         value: NSNumber(value: max(1, Int(frameRate * keyframeInterval))))
    VTSessionSetProperty(created, key: kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration,
                         value: NSNumber(value: keyframeInterval))
    VTCompressionSessionPrepareToEncodeFrames(created)
  }

  deinit {
    invalidate()
  }

  func invalidate() {
    lock.lock()
    invalidated = true
    let session = self.session
    self.session = nil
    lock.unlock()
    if let session {
      VTCompressionSessionCompleteFrames(session, untilPresentationTimeStamp: .invalid)
      VTCompressionSessionInvalidate(session)
    }
  }

  /// One output frame. False when VideoToolbox did not take it (the caller
  /// counts a drop). The first frame is forced to be a keyframe.
  func encode(pixelBuffer: CVPixelBuffer, ptsMs: UInt32) -> Bool {
    lock.lock()
    guard !invalidated, let session else {
      lock.unlock()
      return false
    }
    var properties: CFDictionary?
    if forceKeyframe {
      properties = [kVTEncodeFrameOptionKey_ForceKeyFrame as String: true] as CFDictionary
      forceKeyframe = false
    }
    lock.unlock()

    let pts = CMTime(value: CMTimeValue(ptsMs), timescale: 1000)
    let duration = CMTime(value: 1, timescale: CMTimeScale(frameRate.rounded()))
    let status = VTCompressionSessionEncodeFrame(
      session, imageBuffer: pixelBuffer, presentationTimeStamp: pts, duration: duration,
      frameProperties: properties, infoFlagsOut: nil
    ) { [weak self] status, flags, sampleBuffer in
      self?.handleOutput(status: status, dropped: H264Encoder.isDropped(flags), sampleBuffer: sampleBuffer)
    }
    if status == kVTInvalidSessionErr {
      onFailure?("the VideoToolbox session became invalid (\(status))")
      invalidate()
      return false
    }
    if status != noErr {
      lock.lock()
      forceKeyframe = true  // the stream has a hole; the next frame must be a keyframe
      lock.unlock()
      return false
    }
    return true
  }

  // MARK: - Output (VideoToolbox thread)

  /// The SDK hands the block's flags over as the option set or as its raw value.
  private static func isDropped(_ flags: Any) -> Bool {
    if let set = flags as? VTEncodeInfoFlags { return set.contains(.frameDropped) }
    if let raw = flags as? UInt32 { return VTEncodeInfoFlags(rawValue: raw).contains(.frameDropped) }
    return false
  }

  private func handleOutput(status: OSStatus, dropped: Bool, sampleBuffer: CMSampleBuffer?) {
    if status != noErr {
      onFailure?("VideoToolbox failed to encode a frame (\(status))")
      return
    }
    if dropped {
      return
    }
    guard let sampleBuffer, CMSampleBufferDataIsReady(sampleBuffer),
          let dataBuffer = CMSampleBufferGetDataBuffer(sampleBuffer),
          let format = CMSampleBufferGetFormatDescription(sampleBuffer) else { return }

    // AVCC bytes out of the block buffer (one copy, 11).
    let length = CMBlockBufferGetDataLength(dataBuffer)
    guard length > 0 else { return }
    var avcc = [UInt8](repeating: 0, count: length)
    let copied = avcc.withUnsafeMutableBytes { raw -> OSStatus in
      guard let base = raw.baseAddress else { return -1 }
      return CMBlockBufferCopyDataBytes(dataBuffer, atOffset: 0, dataLength: length, destination: base)
    }
    guard copied == kCMBlockBufferNoErr else { return }

    // Parameter sets and the NALU length size from the format description.
    var lengthSize: Int32 = 4
    var sps: [UInt8] = []
    var pps: [UInt8] = []
    var count = 0
    if CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, parameterSetIndex: 0, parameterSetPointerOut: nil,
                                                          parameterSetSizeOut: nil, parameterSetCountOut: &count,
                                                          nalUnitHeaderLengthOut: &lengthSize) == noErr {
      for index in 0..<count {
        var pointer: UnsafePointer<UInt8>?
        var size = 0
        guard CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, parameterSetIndex: index,
                                                                 parameterSetPointerOut: &pointer,
                                                                 parameterSetSizeOut: &size, parameterSetCountOut: nil,
                                                                 nalUnitHeaderLengthOut: nil) == noErr,
              let pointer, size > 0 else { continue }
        let nalu = Array(UnsafeBufferPointer(start: pointer, count: size))
        switch nalu[0] & 0x1F {
        case 7: if sps.isEmpty { sps = nalu }
        case 8: if pps.isEmpty { pps = nalu }
        default: break
        }
      }
    }

    let annexbSize = nitrortmp_avcc_to_annexb(avcc, avcc.count, lengthSize, nil, 0)
    guard annexbSize > 0 else { return }
    var annexb = [UInt8](repeating: 0, count: annexbSize)
    _ = nitrortmp_avcc_to_annexb(avcc, avcc.count, lengthSize, &annexb, annexb.count)

    var output = annexb
    if isKeyframe(sampleBuffer) && nitrortmp_annexb_has_parameter_sets(annexb, annexb.count) == 0 {
      // Every keyframe carries SPS/PPS so a reconnect can resend the sequence header.
      let total = nitrortmp_prepend_parameter_sets(sps, sps.count, pps, pps.count, annexb, annexb.count, nil, 0)
      var withSets = [UInt8](repeating: 0, count: total)
      _ = nitrortmp_prepend_parameter_sets(sps, sps.count, pps, pps.count, annexb, annexb.count, &withSets, withSets.count)
      output = withSets
    }

    let pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
    let ptsMs = pts.isNumeric ? UInt32(clamping: Int64((CMTimeGetSeconds(pts) * 1000).rounded())) : 0
    sink?.pushEncodedVideo(Data(output), ptsMs: ptsMs, dtsMs: ptsMs)  // dts = pts: no B-frames
    // The mixer releases pending audio here; the first video must already be
    // queued so the core establishes its timestamp origin from that frame.
    onOutput?(output.count)
  }

  private func isKeyframe(_ sampleBuffer: CMSampleBuffer) -> Bool {
    guard let attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, createIfNecessary: false)
            as? [[String: Any]],
          let first = attachments.first else { return true }
    let notSync = first[kCMSampleAttachmentKey_NotSync as String] as? Bool ?? false
    return !notSync
  }
}
