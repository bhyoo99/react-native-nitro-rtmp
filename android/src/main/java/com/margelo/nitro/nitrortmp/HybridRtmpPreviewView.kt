package com.margelo.nitro.nitrortmp

import android.graphics.SurfaceTexture
import android.view.TextureView
import android.view.View
import androidx.annotation.Keep
import com.facebook.proguard.annotations.DoNotStrip
import com.facebook.react.uimanager.ThemedReactContext
import com.margelo.nitro.nitrortmp.mixer.VideoMixer

/**
 * The `RtmpPreviewView` HybridView: a `TextureView` the
 * mixer draws the composited scene into (a front camera mirrored there only).
 * Changing the `mixer` prop moves the surface to the other mixer;
 * several previews may share one mixer.
 */
@DoNotStrip
@Keep
class HybridRtmpPreviewView(context: ThemedReactContext) : HybridRtmpPreviewViewSpec(), VideoMixer.PreviewTarget {
  private val textureView = TextureView(context).apply {
    isOpaque = true
    surfaceTextureListener = object : TextureView.SurfaceTextureListener {
      override fun onSurfaceTextureAvailable(surface: SurfaceTexture, width: Int, height: Int) {
        previewTexture = surface
        attachedMixer?.videoMixer?.addPreview(this@HybridRtmpPreviewView)
      }

      override fun onSurfaceTextureSizeChanged(surface: SurfaceTexture, width: Int, height: Int) {
        // The EGL window surface follows the buffer size; the mixer queries it per frame.
      }

      override fun onSurfaceTextureDestroyed(surface: SurfaceTexture): Boolean {
        attachedMixer?.videoMixer?.removePreview(this@HybridRtmpPreviewView)  // blocks until released
        previewTexture = null
        return true
      }

      override fun onSurfaceTextureUpdated(surface: SurfaceTexture) {}
    }
  }

  @Volatile private var previewTexture: SurfaceTexture? = null
  @Volatile private var attachedMixer: HybridMixer? = null
  @Volatile private var mixerValue: HybridMixerSpec? = null
  @Volatile private var resizeModeValue: PreviewResizeMode? = PreviewResizeMode.COVER

  override val view: View
    get() = textureView

  override var mixer: HybridMixerSpec?
    get() = mixerValue
    set(value) {
      mixerValue = value
      val next = value as? HybridMixer
      if (attachedMixer === next) return
      attachedMixer?.videoMixer?.removePreview(this)
      attachedMixer = next
      if (previewTexture != null) next?.videoMixer?.addPreview(this)
    }

  override var resizeMode: PreviewResizeMode?
    get() = resizeModeValue
    set(value) {
      resizeModeValue = value
    }

  override fun onDropView() {
    attachedMixer?.videoMixer?.removePreview(this)
    attachedMixer = null
    mixerValue = null
  }

  // --- VideoMixer.PreviewTarget -----------------------------------------------------------

  override val previewSurfaceTexture: SurfaceTexture?
    get() = previewTexture

  override val previewResizeMode: PreviewResizeMode
    get() = resizeModeValue ?: PreviewResizeMode.COVER
}
