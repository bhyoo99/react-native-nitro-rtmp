package com.margelo.nitro.nitrortmp

import android.Manifest
import android.content.pm.PackageManager
import android.view.Surface
import androidx.annotation.Keep
import com.facebook.proguard.annotations.DoNotStrip
import com.margelo.nitro.NitroModules
import com.margelo.nitro.core.Promise
import com.margelo.nitro.nitrortmp.capture.CameraSource
import kotlin.math.roundToInt

/**
 * The `CameraSource` HybridObject: a Camera2 device that
 * captures into the mixer's `SurfaceTexture`. The camera only opens while
 * it is attached to a mixer (the mixer owns the capture surface):
 * `start()` before `addLayer` records the intent and the capture begins on
 * attach. Permission is checked, not requested.
 */
@DoNotStrip
@Keep
class HybridCameraSource : HybridCameraSourceSpec(), MixerLayer {
  private val context = NitroModules.applicationContext
    ?: throw IllegalStateException("NitroModules has no application context yet")
  private val source = CameraSource(context, object : CameraSource.Listener {
    override fun onCameraLost(message: String) {
      mixer?.reportError(CaptureError(CaptureErrorCode.CAMERAUNAVAILABLE, message))
    }
  })

  @Volatile private var mixer: HybridMixer? = null
  @Volatile private var wantsRunning = false
  @Volatile private var positionValue = CameraPosition.BACK
  @Volatile private var disposed = false

  // --- HybridCameraSourceSpec --------------------------------------------------------------

  override val kind: LayerKind
    get() = LayerKind.CAMERA

  override var position: CameraPosition
    get() = positionValue
    set(value) {
      if (positionValue == value) return
      positionValue = value
      // Switching while running: close and reopen; timestamps keep their clock.
      if (wantsRunning && mixer != null) open(null)
    }

  override val isRunning: Boolean
    get() = source.isOpen

  override fun start(): Promise<Unit> {
    val promise = Promise<Unit>()
    if (disposed) {
      promise.reject(CaptureException(CaptureErrorCode.CAMERAUNAVAILABLE, "camera source was disposed"))
      return promise
    }
    if (context.checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
      promise.reject(
        CaptureException(CaptureErrorCode.PERMISSIONDENIED, "android.permission.CAMERA is not granted; the app must request it before start()"),
      )
      return promise
    }
    wantsRunning = true
    if (mixer == null) {
      promise.resolve(Unit)  // the capture begins when a mixer adds this layer
      return promise
    }
    open(promise)
    return promise
  }

  override fun stop(): Promise<Unit> {
    val promise = Promise<Unit>()
    wantsRunning = false
    val m = mixer
    source.close {
      m?.videoMixer?.releaseCameraSurface(this)
      promise.resolve(Unit)
    }
    return promise
  }

  override fun dispose() {
    disposed = true
    wantsRunning = false
    val m = mixer
    mixer = null
    source.close { m?.videoMixer?.releaseCameraSurface(this) }
    source.release()
    super.dispose()
  }

  // --- MixerLayer ---------------------------------------------------------------------------

  override fun attach(mixer: HybridMixer) {
    if (this.mixer === mixer) return
    if (this.mixer != null) detach()
    this.mixer = mixer
    if (wantsRunning) open(null)
  }

  override fun detach() {
    val m = mixer ?: return
    mixer = null
    source.close { m.videoMixer.releaseCameraSurface(this) }
  }

  // --- internals ------------------------------------------------------------------------------

  /** Opens (or reopens) the camera into the attached mixer; settles [promise] when given. */
  private fun open(promise: Promise<Unit>?) {
    val m = mixer
    if (m == null) {
      promise?.resolve(Unit)
      return
    }
    val video = m.video
    val provider = object : CameraSource.SurfaceProvider {
      override fun surfaceFor(width: Int, height: Int): Surface? =
        m.videoMixer.acquireCameraSurface(this@HybridCameraSource, width, height)
    }
    source.open(
      positionValue,
      video.width.toInt(),
      video.height.toInt(),
      video.frameRate.roundToInt().coerceAtLeast(1),
      provider,
    ) { failure ->
      if (failure == null) {
        promise?.resolve(Unit)
      } else if (promise != null) {
        promise.reject(failure)
      } else {
        m.reportError(failure.toError())
      }
    }
  }
}
