import CoreVideo
import Foundation
import Metal
import QuartzCore

/// Something that shows the scene: a `CAMetalLayer` plus how the output frame
/// fits into it.
protocol PreviewTarget: AnyObject {
  var metalLayer: CAMetalLayer { get }
  var previewResizeMode: PreviewResizeMode { get }
}

/// One entry of the scene, in z order. `frame` is in normalized output
/// coordinates (top-left origin); nil means the whole frame.
final class SceneLayer {
  enum Content {
    case camera(HybridCameraSource)
    case image(HybridImageLayer)
  }

  let content: Content
  var frame: LayerFrame?

  init(content: Content, frame: LayerFrame?) {
    self.content = content
    self.frame = frame
  }

  var object: AnyObject {
    switch content {
    case .camera(let camera): return camera
    case .image(let image): return image
    }
  }
}

/// A camera frame waiting for the compositor.
struct CameraFrame {
  let pixelBuffer: CVPixelBuffer
  let time: CFTimeInterval  // host clock seconds
}

/// The Metal compositor. It owns the render
/// queue, draws every layer as one textured quad into an output pixel buffer
/// for the encoder and again into every preview's drawable, and never blocks
/// a capture callback: a frame that arrives while the previous one is still
/// waiting replaces it (`droppedFrames`).
///
/// Threading: everything marked "on queue" runs on `queue` only. `HybridMixer`
/// posts its JS calls there; capture threads only touch `frameAvailable`.
final class VideoMixer {
  let queue = DispatchQueue(label: "nitrortmp.render")
  let device: MTLDevice?

  private let commandQueue: MTLCommandQueue?
  private var pipeline: MTLRenderPipelineState?
  private var textureCache: CVMetalTextureCache?
  private var pool: CVPixelBufferPool?
  private var poolWidth = 0
  private var poolHeight = 0

  // Scene, on queue.
  private(set) var layers: [SceneLayer] = []
  private var previews: [PreviewEntry] = []
  private var encodeHandler: ((CVPixelBuffer, CFTimeInterval) -> Bool)?
  private var timer: DispatchSourceTimer?
  private(set) var outputWidth = 720
  private(set) var outputHeight = 1280
  private var frameRate = 30.0

  // Shared with the capture threads.
  private let lock = NSLock()
  private var renderScheduled = false
  private var pendingTime: CFTimeInterval = 0
  private var capturedFrames: UInt64 = 0
  private var renderedFrames: UInt64 = 0
  private var droppedFrames: UInt64 = 0

  /// A preview plus how many of its drawables the GPU still owns, so a view
  /// nobody displays cannot stall the render queue in `nextDrawable()`.
  private final class PreviewEntry {
    weak var target: PreviewTarget?
    private let lock = NSLock()
    private var inFlight = 0

    init(_ target: PreviewTarget) { self.target = target }

    func acquire() -> Bool {
      lock.lock()
      defer { lock.unlock() }
      if inFlight >= 2 { return false }
      inFlight += 1
      return true
    }

    func release() {
      lock.lock()
      inFlight -= 1
      lock.unlock()
    }
  }

  /// What one layer draws this frame.
  private struct LayerDraw {
    let texture: MTLTexture
    let frame: LayerFrame?
    let aspectFill: Bool     // camera: fill the rect; image: fit inside it
    let mirrorInPreview: Bool
  }

  /// The output frame's placement in a target, NDC (x, top y, width, height).
  private struct Canvas {
    let x: Float
    let top: Float
    let width: Float
    let height: Float
    static let full = Canvas(x: -1, top: 1, width: 2, height: 2)
  }

  init() {
    device = MTLCreateSystemDefaultDevice()
    commandQueue = device?.makeCommandQueue()
    guard let device else { return }
    var cache: CVMetalTextureCache?
    CVMetalTextureCacheCreate(kCFAllocatorDefault, nil, device, nil, &cache)
    textureCache = cache
    do {
      let library = try device.makeLibrary(source: MixerShaders.source, options: nil)
      let descriptor = MTLRenderPipelineDescriptor()
      descriptor.vertexFunction = library.makeFunction(name: "layer_vertex")
      descriptor.fragmentFunction = library.makeFunction(name: "layer_fragment")
      let attachment = descriptor.colorAttachments[0]
      attachment?.pixelFormat = .bgra8Unorm
      // Premultiplied alpha: camera quads are opaque, image quads blend over them.
      attachment?.isBlendingEnabled = true
      attachment?.rgbBlendOperation = .add
      attachment?.alphaBlendOperation = .add
      attachment?.sourceRGBBlendFactor = .one
      attachment?.sourceAlphaBlendFactor = .one
      attachment?.destinationRGBBlendFactor = .oneMinusSourceAlpha
      attachment?.destinationAlphaBlendFactor = .oneMinusSourceAlpha
      pipeline = try device.makeRenderPipelineState(descriptor: descriptor)
    } catch {
      NSLog("nitrortmp: failed to build the compositor pipeline: \(error)")
    }
  }

  deinit {
    timer?.cancel()
  }

  var isAvailable: Bool { pipeline != nil && commandQueue != nil }

  // MARK: - Capture side (any thread)

  /// A camera stored a new frame in its slot. `replacedPending` is true when
  /// the slot still held a frame the compositor had not drawn.
  func frameAvailable(time: CFTimeInterval, replacedPending: Bool) {
    lock.lock()
    capturedFrames += 1
    if replacedPending { droppedFrames += 1 }
    pendingTime = time
    let schedule = !renderScheduled
    renderScheduled = true
    lock.unlock()
    if schedule {
      queue.async { [weak self] in self?.renderPending() }
    }
  }

  /// A camera started or stopped: the timer mode may have to change.
  func cameraStateChanged() {
    queue.async { [weak self] in self?.updateTimer() }
  }

  /// (captured, rendered, dropped)
  var counters: (UInt64, UInt64, UInt64) {
    lock.lock()
    defer { lock.unlock() }
    return (capturedFrames, renderedFrames, droppedFrames)
  }

  // MARK: - Scene (on queue)

  func setOutput(width: Int, height: Int, frameRate: Double) {
    outputWidth = max(2, width)
    outputHeight = max(2, height)
    self.frameRate = frameRate > 0 ? frameRate : 30
    for layer in layers {
      if case .camera(let camera) = layer.content {
        camera.setFrameRate(self.frameRate)
      }
    }
    if let timer, let interval = timerInterval() {
      timer.schedule(deadline: .now(), repeating: interval)
    }
  }

  func addLayer(_ layer: SceneLayer) {
    guard !layers.contains(where: { $0.object === layer.object }) else { return }
    layers.append(layer)
    if case .camera(let camera) = layer.content {
      camera.setFrameRate(frameRate)
    }
    updateTimer()
  }

  func removeLayer(_ object: AnyObject) {
    layers.removeAll { $0.object === object }
    updateTimer()
  }

  func setFrame(_ frame: LayerFrame, of object: AnyObject) {
    layers.first { $0.object === object }?.frame = frame
  }

  var hasCameraLayer: Bool {
    layers.contains {
      if case .camera = $0.content { return true }
      return false
    }
  }

  func addPreview(_ target: PreviewTarget) {
    previews.removeAll { $0.target == nil || $0.target === target }
    previews.append(PreviewEntry(target))
    updateTimer()
  }

  func removePreview(_ target: PreviewTarget) {
    previews.removeAll { $0.target == nil || $0.target === target }
    updateTimer()
  }

  /// Drops entries whose view went away.
  func purgePreviews() {
    previews.removeAll { $0.target == nil }
    updateTimer()
  }

  /// Receives every rendered output frame while set; returns false when the
  /// encoder could not take it (counted as dropped).
  func setEncodeHandler(_ handler: ((CVPixelBuffer, CFTimeInterval) -> Bool)?) {
    encodeHandler = handler
    updateTimer()
  }

  // MARK: - Timer mode (on queue)

  private func timerInterval() -> DispatchTimeInterval? {
    guard frameRate > 0 else { return nil }
    return .nanoseconds(Int(1_000_000_000.0 / frameRate))
  }

  /// Without a running camera the scene has no clock of its own; when someone
  /// wants frames (an encoder or a preview) a timer at the output rate drives it.
  private func updateTimer() {
    let wantsFrames = encodeHandler != nil || previews.contains { $0.target != nil }
    let cameraRunning = layers.contains {
      if case .camera(let camera) = $0.content { return camera.isRunning }
      return false
    }
    if wantsFrames && !cameraRunning {
      guard timer == nil, let interval = timerInterval() else { return }
      let source = DispatchSource.makeTimerSource(queue: queue)
      source.schedule(deadline: .now(), repeating: interval)
      source.setEventHandler { [weak self] in self?.render(time: CACurrentMediaTime()) }
      source.resume()
      timer = source
    } else if let running = timer {
      running.cancel()
      timer = nil
    }
  }

  // MARK: - Rendering (on queue)

  private func renderPending() {
    lock.lock()
    renderScheduled = false
    let time = pendingTime
    lock.unlock()
    render(time: time)
  }

  private func render(time: CFTimeInterval) {
    guard let device, let commandQueue, let pipeline else { return }

    // Gather the layer textures once. CVMetalTextures must outlive the GPU work.
    var keep: [CVMetalTexture] = []
    var draws: [LayerDraw] = []
    for layer in layers {
      switch layer.content {
      case .camera(let camera):
        guard let frame = camera.latestFrame(), let pair = texture(for: frame.pixelBuffer) else { continue }
        keep.append(pair.1)
        draws.append(LayerDraw(texture: pair.0, frame: layer.frame, aspectFill: true,
                               mirrorInPreview: camera.position == .front))
      case .image(let image):
        guard let texture = image.texture(for: device) else { continue }
        draws.append(LayerDraw(texture: texture, frame: layer.frame, aspectFill: false, mirrorInPreview: false))
      }
    }

    guard let commandBuffer = commandQueue.makeCommandBuffer() else { return }

    // 1. The encoder frame: the scene as sent, never mirrored.
    var outputBuffer: CVPixelBuffer?
    if encodeHandler != nil, let pixelBuffer = makeOutputBuffer(), let pair = texture(for: pixelBuffer) {
      keep.append(pair.1)
      outputBuffer = pixelBuffer
      drawScene(draws, into: pair.0, canvas: .full, mirrorCameras: false, commandBuffer: commandBuffer, pipeline: pipeline)
    }

    // 2. Every preview: the same scene, fitted to the view, front camera mirrored.
    var presented: [PreviewEntry] = []
    for entry in previews {
      guard let target = entry.target else { continue }
      let metalLayer = target.metalLayer
      let size = metalLayer.drawableSize
      guard size.width >= 1, size.height >= 1, entry.acquire() else { continue }
      guard let drawable = metalLayer.nextDrawable() else {
        entry.release()
        continue
      }
      let canvas = canvas(forDrawable: size, mode: target.previewResizeMode)
      drawScene(draws, into: drawable.texture, canvas: canvas, mirrorCameras: true,
                commandBuffer: commandBuffer, pipeline: pipeline)
      commandBuffer.present(drawable)
      presented.append(entry)
    }

    commandBuffer.addCompletedHandler { _ in
      for entry in presented { entry.release() }
      withExtendedLifetime(keep) {}
    }
    commandBuffer.commit()

    lock.lock()
    renderedFrames += 1
    lock.unlock()

    if let outputBuffer, let encode = encodeHandler {
      // The encoder reads the buffer on the CPU side of VideoToolbox: wait for the GPU.
      commandBuffer.waitUntilCompleted()
      if !encode(outputBuffer, time) {
        lock.lock()
        droppedFrames += 1
        lock.unlock()
      }
    }
    if let textureCache {
      CVMetalTextureCacheFlush(textureCache, 0)
    }
  }

  private func drawScene(_ draws: [LayerDraw], into target: MTLTexture, canvas: Canvas, mirrorCameras: Bool,
                         commandBuffer: MTLCommandBuffer, pipeline: MTLRenderPipelineState) {
    let pass = MTLRenderPassDescriptor()
    pass.colorAttachments[0].texture = target
    pass.colorAttachments[0].loadAction = .clear
    pass.colorAttachments[0].storeAction = .store
    pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
    guard let encoder = commandBuffer.makeRenderCommandEncoder(descriptor: pass) else { return }
    encoder.setRenderPipelineState(pipeline)
    for draw in draws {
      var uniforms = uniforms(for: draw, canvas: canvas, mirror: mirrorCameras && draw.mirrorInPreview)
      encoder.setVertexBytes(&uniforms, length: MemoryLayout<LayerUniforms>.stride, index: 0)
      encoder.setFragmentTexture(draw.texture, index: 0)
      encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
    }
    encoder.endEncoding()
  }

  /// Places one layer: its `LayerFrame` (0..1 of the output) becomes a quad in
  /// the canvas. Camera layers fill the rect and crop through the texture
  /// coordinates; image layers shrink to fit inside it.
  private func uniforms(for draw: LayerDraw, canvas: Canvas, mirror: Bool) -> LayerUniforms {
    let f = draw.frame
    let fx = Float(clamp01(f?.x ?? 0))
    let fy = Float(clamp01(f?.y ?? 0))
    let fw = Float(min(clamp01(f?.width ?? 1), 1 - Double(fx)))
    let fh = Float(min(clamp01(f?.height ?? 1), 1 - Double(fy)))
    var rect = SIMD4<Float>(canvas.x + fx * canvas.width, canvas.top - fy * canvas.height,
                            fw * canvas.width, fh * canvas.height)
    var texRect = SIMD4<Float>(0, 0, 1, 1)
    let rectAspect = (Double(fw) * Double(outputWidth)) / max(1e-6, Double(fh) * Double(outputHeight))
    let textureAspect = Double(draw.texture.width) / max(1, Double(draw.texture.height))
    if draw.aspectFill {
      if textureAspect > rectAspect {
        let w = Float(rectAspect / textureAspect)
        texRect = SIMD4<Float>((1 - w) / 2, 0, w, 1)
      } else {
        let h = Float(textureAspect / rectAspect)
        texRect = SIMD4<Float>(0, (1 - h) / 2, 1, h)
      }
    } else {
      if textureAspect > rectAspect {
        let h = rect.w * Float(rectAspect / textureAspect)
        rect = SIMD4<Float>(rect.x, rect.y - (rect.w - h) / 2, rect.z, h)
      } else {
        let w = rect.z * Float(textureAspect / rectAspect)
        rect = SIMD4<Float>(rect.x + (rect.z - w) / 2, rect.y, w, rect.w)
      }
    }
    return LayerUniforms(rect: rect, texRect: texRect, mirror: mirror ? 1 : 0)
  }

  /// Where the output frame lands in a drawable: `contain` letterboxes,
  /// `cover` crops; both keep the output aspect.
  private func canvas(forDrawable size: CGSize, mode: PreviewResizeMode) -> Canvas {
    let outputAspect = Double(outputWidth) / Double(outputHeight)
    let drawableAspect = Double(size.width) / Double(size.height)
    let limitWidth = mode == .contain ? outputAspect > drawableAspect : outputAspect <= drawableAspect
    var width = 2.0
    var height = 2.0
    if limitWidth {
      height = 2.0 * drawableAspect / outputAspect
    } else {
      width = 2.0 * outputAspect / drawableAspect
    }
    return Canvas(x: Float(-width / 2), top: Float(height / 2), width: Float(width), height: Float(height))
  }

  private func clamp01(_ value: Double) -> Double {
    guard value.isFinite else { return 0 }
    return min(1, max(0, value))
  }

  // MARK: - Buffers (on queue)

  private func texture(for pixelBuffer: CVPixelBuffer) -> (MTLTexture, CVMetalTexture)? {
    guard let textureCache else { return nil }
    let width = CVPixelBufferGetWidth(pixelBuffer)
    let height = CVPixelBufferGetHeight(pixelBuffer)
    var cvTexture: CVMetalTexture?
    let status = CVMetalTextureCacheCreateTextureFromImage(
      kCFAllocatorDefault, textureCache, pixelBuffer, nil, .bgra8Unorm, width, height, 0, &cvTexture)
    guard status == kCVReturnSuccess, let cvTexture, let texture = CVMetalTextureGetTexture(cvTexture) else {
      return nil
    }
    return (texture, cvTexture)
  }

  /// IOSurface-backed BGRA buffers at the output size: Metal draws into them
  /// and VideoToolbox reads them without a copy.
  private func makeOutputBuffer() -> CVPixelBuffer? {
    if pool == nil || poolWidth != outputWidth || poolHeight != outputHeight {
      let attributes: [CFString: Any] = [
        kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_32BGRA,
        kCVPixelBufferWidthKey: outputWidth,
        kCVPixelBufferHeightKey: outputHeight,
        kCVPixelBufferIOSurfacePropertiesKey: [:] as [CFString: Any],
        kCVPixelBufferMetalCompatibilityKey: true,
      ]
      let poolAttributes: [CFString: Any] = [kCVPixelBufferPoolMinimumBufferCountKey: 3]
      var created: CVPixelBufferPool?
      CVPixelBufferPoolCreate(kCFAllocatorDefault, poolAttributes as CFDictionary, attributes as CFDictionary, &created)
      pool = created
      poolWidth = outputWidth
      poolHeight = outputHeight
    }
    guard let pool else { return nil }
    var pixelBuffer: CVPixelBuffer?
    CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &pixelBuffer)
    return pixelBuffer
  }
}
