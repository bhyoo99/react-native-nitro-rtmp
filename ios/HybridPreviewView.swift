import Foundation
import Metal
import NitroModules
import QuartzCore
import UIKit

/// The UIView behind `PreviewView`: a `CAMetalLayer` the compositor draws
/// into on the render queue.
final class PreviewUIView: UIView {
  override class var layerClass: AnyClass { CAMetalLayer.self }

  var metalLayer: CAMetalLayer {
    // layerClass guarantees the type.
    layer as! CAMetalLayer  // swiftlint:disable:this force_cast
  }

  override init(frame: CGRect) {
    super.init(frame: frame)
    backgroundColor = .black
    isOpaque = true
    let metal = metalLayer
    metal.device = MTLCreateSystemDefaultDevice()
    metal.pixelFormat = .bgra8Unorm
    metal.framebufferOnly = true
    metal.isOpaque = true
    metal.contentsScale = UIScreen.main.scale
  }

  @available(*, unavailable)
  required init?(coder: NSCoder) {
    nil
  }

  override func layoutSubviews() {
    super.layoutSubviews()
    let scale = window?.screen.scale ?? UIScreen.main.scale
    metalLayer.contentsScale = scale
    metalLayer.drawableSize = CGSize(width: bounds.width * scale, height: bounds.height * scale)
  }
}

/// The `PreviewView` HybridView: shows the mixer's scene as it is sent, with
/// the front camera mirrored. Several previews may share one mixer.
final class HybridPreviewView: HybridPreviewViewSpec, PreviewTarget {
  let view: PreviewUIView = PreviewUIView(frame: .zero)
  private let lock = NSLock()
  private var attachedMixer: HybridMixer?
  private var storedMixer: (any HybridMixerSpec)?
  private var storedResizeMode: PreviewResizeMode?

  deinit {
    // The mixer's entry holds this target weakly; only a purge is needed.
    lock.lock()
    let previous = attachedMixer
    lock.unlock()
    previous?.videoMixer.queue.async { previous?.videoMixer.purgePreviews() }
  }

  // MARK: - HybridPreviewViewSpec

  var mixer: (any HybridMixerSpec)? {
    get {
      lock.lock()
      defer { lock.unlock() }
      return storedMixer
    }
    set {
      lock.lock()
      storedMixer = newValue
      let previous = attachedMixer
      let next = newValue as? HybridMixer
      attachedMixer = next
      lock.unlock()
      if let previous, previous !== next {
        previous.videoMixer.queue.async { [weak self] in
          guard let self else { return }
          previous.videoMixer.removePreview(self)
        }
      }
      if let next, next !== previous {
        next.videoMixer.queue.async { [weak self] in
          guard let self else { return }
          next.videoMixer.addPreview(self)
        }
      }
    }
  }

  var resizeMode: PreviewResizeMode? {
    get {
      lock.lock()
      defer { lock.unlock() }
      return storedResizeMode
    }
    set {
      lock.lock()
      storedResizeMode = newValue
      lock.unlock()
    }
  }

  func onDropView() {
    detach()
  }

  // MARK: - PreviewTarget (render queue)

  var metalLayer: CAMetalLayer { view.metalLayer }

  var previewResizeMode: PreviewResizeMode {
    lock.lock()
    defer { lock.unlock() }
    return storedResizeMode ?? .cover
  }

  // MARK: - Internals

  private func detach() {
    lock.lock()
    let previous = attachedMixer
    attachedMixer = nil
    storedMixer = nil
    lock.unlock()
    guard let previous else { return }
    previous.videoMixer.queue.async { [weak self] in
      guard let self else { return }
      previous.videoMixer.removePreview(self)
    }
  }
}
