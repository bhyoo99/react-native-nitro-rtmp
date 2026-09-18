import Foundation
import ImageIO
import Metal
import NitroModules
import UIKit

/// The `ImageLayer` HybridObject: one PNG/JPEG drawn into
/// its `LayerFrame`, the proof that an overlay is just another layer. The
/// file is decoded off the render thread into premultiplied BGRA; the
/// texture is created lazily on the mixer's device when first drawn.
final class HybridImageLayer: HybridImageLayerSpec {
  private let lock = NSLock()
  private var bitmap: LayerBitmap?
  private var texture: MTLTexture?
  private var textureDevice: ObjectIdentifier?
  private var loaded = false
  private weak var mixer: HybridMixer?

  // MARK: - HybridVideoLayerSpec / HybridImageLayerSpec

  var kind: LayerKind { .image }

  var isLoaded: Bool {
    lock.lock()
    defer { lock.unlock() }
    return loaded
  }

  func load(fileUri: String) throws -> Promise<Void> {
    let promise = Promise<Void>()
    DispatchQueue.global(qos: .userInitiated).async { [weak self] in
      guard let self else {
        promise.reject(withError: CaptureFailure(code: .configurationfailed, message: "image layer was released"))
        return
      }
      let path = HybridImageLayer.path(from: fileUri)
      guard let image = HybridImageLayer.decode(path: path), let bitmap = LayerTexture.bitmap(from: image) else {
        promise.reject(withError: CaptureFailure(code: .configurationfailed, message: "cannot decode image at \(path)"))
        return
      }
      self.lock.lock()
      self.bitmap = bitmap
      self.texture = nil  // re-uploaded at the next draw
      self.textureDevice = nil
      self.loaded = true
      self.lock.unlock()
      promise.resolve()
    }
    return promise
  }

  var memorySize: Int {
    lock.lock()
    defer { lock.unlock() }
    return bitmap?.data.count ?? 0
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

  /// Render queue: the texture for this device, uploaded on first use.
  func texture(for device: MTLDevice) -> MTLTexture? {
    lock.lock()
    defer { lock.unlock() }
    if let texture, textureDevice == ObjectIdentifier(device) {
      return texture
    }
    guard let bitmap else { return nil }
    texture = LayerTexture.makeTexture(bitmap, device: device)
    textureDevice = ObjectIdentifier(device)
    return texture
  }

  // MARK: - Decoding

  private static func path(from uri: String) -> String {
    if let url = URL(string: uri), url.isFileURL {
      return url.path
    }
    return uri.hasPrefix("file://") ? String(uri.dropFirst("file://".count)) : uri
  }

  private static func decode(path: String) -> CGImage? {
    let url = URL(fileURLWithPath: path)
    guard let source = CGImageSourceCreateWithURL(url as CFURL, nil) else { return nil }
    return CGImageSourceCreateImageAtIndex(source, 0, [kCGImageSourceShouldCache: false] as CFDictionary)
  }
}
