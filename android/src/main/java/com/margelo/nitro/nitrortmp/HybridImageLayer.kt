package com.margelo.nitro.nitrortmp

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.net.Uri
import androidx.annotation.Keep
import com.facebook.proguard.annotations.DoNotStrip
import com.margelo.nitro.core.Promise

/**
 * The `ImageLayer` HybridObject: one decoded bitmap that
 * the mixer uploads as a 2D texture on its render thread (on the first draw
 * after every `load`). The proof that an overlay is just another layer.
 */
@DoNotStrip
@Keep
class HybridImageLayer : HybridImageLayerSpec(), MixerLayer {
  /** Read by the render thread; replaced whole by `load`. */
  @Volatile internal var bitmap: Bitmap? = null
  @Volatile private var mixer: HybridMixer? = null

  override val kind: LayerKind
    get() = LayerKind.IMAGE

  override val isLoaded: Boolean
    get() = bitmap != null

  override fun load(fileUri: String): Promise<Unit> {
    val promise = Promise<Unit>()
    Thread({
      val path = if (fileUri.startsWith("file://")) Uri.parse(fileUri).path ?: fileUri.removePrefix("file://") else fileUri
      val options = BitmapFactory.Options().apply { inPreferredConfig = Bitmap.Config.ARGB_8888 }
      val decoded = try {
        BitmapFactory.decodeFile(path, options)
      } catch (e: Exception) {
        null
      }
      if (decoded == null) {
        promise.reject(CaptureException(CaptureErrorCode.CONFIGURATIONFAILED, "cannot decode image at $fileUri"))
      } else {
        bitmap = decoded
        promise.resolve(Unit)
      }
    }, "nitrortmp.image").start()
    return promise
  }

  override fun attach(mixer: HybridMixer) {
    this.mixer = mixer
  }

  override fun detach() {
    mixer = null
  }
}
