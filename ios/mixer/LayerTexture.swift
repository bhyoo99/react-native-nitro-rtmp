import CoreGraphics
import Foundation
import Metal

/// A decoded image as premultiplied BGRA bytes, top row first: exactly what a
/// `.bgra8Unorm` texture takes and what the premultiplied blend state expects.
struct LayerBitmap {
  let width: Int
  let height: Int
  let bytesPerRow: Int
  let data: Data
}

/// Bitmap <-> texture helpers for `ImageLayer`. Decoding
/// happens off the render thread; the upload happens on it, on the mixer's device.
enum LayerTexture {
  /// Draws the image into a premultiplied BGRA context. Nil for empty images.
  static func bitmap(from image: CGImage) -> LayerBitmap? {
    let width = image.width
    let height = image.height
    guard width > 0, height > 0 else { return nil }
    let bytesPerRow = width * 4
    var data = Data(count: bytesPerRow * height)
    let drawn = data.withUnsafeMutableBytes { (raw: UnsafeMutableRawBufferPointer) -> Bool in
      guard let base = raw.baseAddress else { return false }
      let info = CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue
      guard let context = CGContext(
        data: base, width: width, height: height, bitsPerComponent: 8, bytesPerRow: bytesPerRow,
        space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: info) else { return false }
      let rect = CGRect(x: 0, y: 0, width: width, height: height)
      context.clear(rect)
      context.draw(image, in: rect)
      return true
    }
    guard drawn else { return nil }
    return LayerBitmap(width: width, height: height, bytesPerRow: bytesPerRow, data: data)
  }

  static func makeTexture(_ bitmap: LayerBitmap, device: MTLDevice) -> MTLTexture? {
    let descriptor = MTLTextureDescriptor.texture2DDescriptor(
      pixelFormat: .bgra8Unorm, width: bitmap.width, height: bitmap.height, mipmapped: false)
    descriptor.usage = .shaderRead
    descriptor.storageMode = .shared
    guard let texture = device.makeTexture(descriptor: descriptor) else { return nil }
    bitmap.data.withUnsafeBytes { raw in
      guard let base = raw.baseAddress else { return }
      texture.replace(
        region: MTLRegionMake2D(0, 0, bitmap.width, bitmap.height), mipmapLevel: 0,
        withBytes: base, bytesPerRow: bitmap.bytesPerRow)
    }
    return texture
  }
}
