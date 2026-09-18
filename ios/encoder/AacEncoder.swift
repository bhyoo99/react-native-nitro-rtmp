import AVFoundation
import CoreMedia
import Foundation

/// AAC-LC encoder over `AVAudioConverter`. Runs
/// synchronously on the microphone's callback queue: PCM in, ADTS frames out
/// to the sink. Resamples first when the hardware format differs from
/// the requested one; the AAC step then always sees the target format.
///
/// Timestamps: the FIFO's head time is re-anchored to every incoming buffer's
/// host-clock presentation time, so packets follow the capture clock even
/// across glitches, and each packet advances by 1024 samples.
final class AacEncoder {
  private static let samplesPerPacket = 1024

  private let sampleRate: Double
  private let channels: Int
  private let bitrate: Int
  private let pcmFormat: AVAudioFormat
  private var inputFormat: AVAudioFormat?
  private var resampler: AVAudioConverter?
  private var aacConverter: AVAudioConverter?
  private var aacFormat: AVAudioFormat?
  private var fifo: [Float] = []  // interleaved
  private var headTime: CFTimeInterval = 0
  private var failed = false

  /// One ADTS packet with the host-clock capture time of its first sample.
  var onPacket: ((Data, CFTimeInterval) -> Void)?
  var onFailure: ((String) -> Void)?

  init?(settings: AudioSettings) {
    sampleRate = settings.sampleRate > 0 ? settings.sampleRate : 48000
    channels = settings.channels >= 2 ? 2 : 1
    bitrate = max(8, Int(settings.bitrateKbps.rounded())) * 1000
    // The sample rate must be one ADTS can name.
    var probe = [UInt8](repeating: 0, count: 7)
    guard nitrortmp_write_adts_header(&probe, 1, Int32(sampleRate), Int32(channels), 0) == 1,
          let pcm = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: sampleRate,
                                  channels: AVAudioChannelCount(channels), interleaved: true) else {
      return nil
    }
    pcmFormat = pcm
    let settingsDictionary: [String: Any] = [
      AVFormatIDKey: kAudioFormatMPEG4AAC,
      AVSampleRateKey: sampleRate,
      AVNumberOfChannelsKey: channels,
      AVEncoderBitRateKey: bitrate,
    ]
    guard let aac = AVAudioFormat(settings: settingsDictionary),
          let converter = AVAudioConverter(from: pcm, to: aac) else { return nil }
    converter.bitRate = bitrate
    aacFormat = aac
    aacConverter = converter
  }

  /// One captured buffer. `muted` zero-fills the copy so the timeline keeps flowing.
  func encode(_ sampleBuffer: CMSampleBuffer, muted: Bool) {
    guard !failed, let aacConverter, let aacFormat,
          let description = CMSampleBufferGetFormatDescription(sampleBuffer) else { return }
    let format = AVAudioFormat(cmAudioFormatDescription: description)
    let frames = CMSampleBufferGetNumSamples(sampleBuffer)
    guard frames > 0, let input = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: AVAudioFrameCount(frames)) else {
      return
    }
    input.frameLength = AVAudioFrameCount(frames)
    let status = CMSampleBufferCopyPCMDataIntoAudioBufferList(sampleBuffer, at: 0, frameCount: Int32(frames),
                                                              into: input.mutableAudioBufferList)
    guard status == noErr else { return }
    if muted {
      zero(input)
    }

    let converted: AVAudioPCMBuffer
    if format.sampleRate == pcmFormat.sampleRate, format.channelCount == pcmFormat.channelCount,
       format.commonFormat == .pcmFormatFloat32, format.isInterleaved == pcmFormat.isInterleaved {
      converted = input
    } else {
      guard let resampled = resample(input, from: format) else { return }
      converted = resampled
    }

    // Re-anchor the FIFO head to this buffer's capture time.
    let presentation = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
    if presentation.isNumeric {
      headTime = CMTimeGetSeconds(presentation) - Double(fifo.count / channels) / sampleRate
    }
    append(converted)

    let packetSamples = AacEncoder.samplesPerPacket * channels
    while fifo.count >= packetSamples {
      guard let chunk = AVAudioPCMBuffer(pcmFormat: pcmFormat, frameCapacity: AVAudioFrameCount(AacEncoder.samplesPerPacket)),
            let data = chunk.floatChannelData else { return }
      chunk.frameLength = AVAudioFrameCount(AacEncoder.samplesPerPacket)
      fifo.withUnsafeBufferPointer { source in
        guard let base = source.baseAddress else { return }
        data[0].update(from: base, count: packetSamples)
      }
      fifo.removeFirst(packetSamples)
      let packetTime = headTime
      headTime += Double(AacEncoder.samplesPerPacket) / sampleRate

      let output = AVAudioCompressedBuffer(format: aacFormat, packetCapacity: 8,
                                           maximumPacketSize: aacConverter.maximumOutputPacketSize)
      var consumed = false
      var error: NSError?
      let result = aacConverter.convert(to: output, error: &error) { _, outStatus in
        if consumed {
          outStatus.pointee = .noDataNow
          return nil
        }
        consumed = true
        outStatus.pointee = .haveData
        return chunk
      }
      if result == .error {
        failed = true
        onFailure?("AAC conversion failed: \(error?.localizedDescription ?? "unknown")")
        return
      }
      emit(output, firstPacketTime: packetTime)
    }
  }

  // MARK: - Helpers

  private func zero(_ buffer: AVAudioPCMBuffer) {
    let list = buffer.mutableAudioBufferList
    let count = Int(list.pointee.mNumberBuffers)
    let buffers = UnsafeMutableAudioBufferListPointer(list)
    for index in 0..<count {
      if let data = buffers[index].mData {
        memset(data, 0, Int(buffers[index].mDataByteSize))
      }
    }
  }

  private func resample(_ input: AVAudioPCMBuffer, from format: AVAudioFormat) -> AVAudioPCMBuffer? {
    if resampler == nil || inputFormat != format {
      resampler = AVAudioConverter(from: format, to: pcmFormat)
      inputFormat = format
    }
    guard let resampler else { return nil }
    let ratio = pcmFormat.sampleRate / format.sampleRate
    let capacity = AVAudioFrameCount(Double(input.frameLength) * ratio) + 64
    guard let output = AVAudioPCMBuffer(pcmFormat: pcmFormat, frameCapacity: capacity) else { return nil }
    var consumed = false
    var error: NSError?
    let result = resampler.convert(to: output, error: &error) { _, outStatus in
      if consumed {
        outStatus.pointee = .noDataNow
        return nil
      }
      consumed = true
      outStatus.pointee = .haveData
      return input
    }
    return result == .error ? nil : output
  }

  private func append(_ buffer: AVAudioPCMBuffer) {
    guard let data = buffer.floatChannelData, buffer.frameLength > 0 else { return }
    let count = Int(buffer.frameLength) * channels
    fifo.append(contentsOf: UnsafeBufferPointer(start: data[0], count: count))
  }

  /// One ADTS frame per packet: header from the C helper + the raw payload.
  private func emit(_ output: AVAudioCompressedBuffer, firstPacketTime: CFTimeInterval) {
    let packets = Int(output.packetCount)
    guard packets > 0 else { return }
    let base = output.data
    var packetTime = firstPacketTime
    for index in 0..<packets {
      var offset = 0
      var size = Int(output.byteLength)
      if let descriptions = output.packetDescriptions {
        offset = Int(descriptions[index].mStartOffset)
        size = Int(descriptions[index].mDataByteSize)
      }
      guard size > 0 else { continue }
      var frame = [UInt8](repeating: 0, count: 7 + size)
      guard nitrortmp_write_adts_header(&frame, 1, Int32(sampleRate), Int32(channels), size) == 1 else { continue }
      frame.withUnsafeMutableBytes { raw in
        guard let destination = raw.baseAddress else { return }
        memcpy(destination + 7, base + offset, size)
      }
      let time = packetTime
      packetTime += Double(AacEncoder.samplesPerPacket) / sampleRate
      onPacket?(Data(frame), time)
    }
  }
}
