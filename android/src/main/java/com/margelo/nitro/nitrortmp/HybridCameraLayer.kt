package com.margelo.nitro.nitrortmp

import androidx.annotation.Keep
import com.facebook.proguard.annotations.DoNotStrip
import com.margelo.nitro.camera.HybridCameraOutputSpec
import com.margelo.nitro.nitrortmp.mixer.CameraSurface

/**
 * The `CameraLayer` HybridObject: a mixer layer fed by the VisionCamera
 * output it owns ([RtmpCameraOutput], exposed as `output`). CameraX renders
 * the camera straight into a `SurfaceTexture` of the mixer (the layer asks
 * the mixer for a new one whenever CameraX requests a surface), so frames
 * never leave the GPU. The encoder never sees this object.
 *
 * A surface is released only after both the camera (its `SurfaceRequest`
 * result) and the mixer (retiring the slot) are done with it; see
 * [CameraSurface].
 */
@DoNotStrip
@Keep
class HybridCameraLayer : HybridCameraLayerSpec(), MixerLayer {
  private val cameraOutput = RtmpCameraOutput(this)
  @Volatile private var mixer: HybridMixer? = null
  @Volatile private var frontCamera = false
  @Volatile private var receiving = false
  /**
   * Rotation the compositor must add, in degrees clockwise: 0 while the
   * surface carries the camera transform (the usual case), else what CameraX
   * reports for the buffer. Read on the render thread.
   */
  @Volatile internal var extraRotationDegrees = 0
  @Volatile private var disposed = false

  // --- HybridVideoLayerSpec / HybridCameraLayerSpec ------------------------------------------

  override val kind: LayerKind
    get() = LayerKind.CAMERA

  override val output: HybridCameraOutputSpec
    get() = cameraOutput

  override val isFrontCamera: Boolean
    get() = frontCamera

  override val isReceivingFrames: Boolean
    get() = receiving

  override fun dispose() {
    disposed = true
    detach()
    cameraOutput.release()
    super.dispose()
  }

  // --- MixerLayer -----------------------------------------------------------------------------

  override fun attach(mixer: HybridMixer) {
    if (this.mixer === mixer) return
    if (this.mixer != null) detach()
    this.mixer = mixer
    cameraOutput.layerAttached()  // CameraX asks for a surface again
  }

  override fun detach() {
    val m = mixer ?: return
    mixer = null  // a surface request arriving from now on gets no surface
    cameraOutput.layerDetached()  // CameraX gives the surface up and re-requests
    receiving = false
    // Stops drawing now; the Surface itself lives until CameraX reports it done.
    m.videoMixer.retireCameraSurface(this)
  }

  // --- output side (the output's executor) -------------------------------------------------

  /** The mixer's output size, for CameraX's resolution choice. */
  internal fun targetSize(): android.util.Size {
    val video = mixer?.video
    val width = video?.width?.toInt() ?: 1280
    val height = video?.height?.toInt() ?: 720
    // Camera sizes are landscape; the buffer transform rotates them.
    return android.util.Size(maxOf(width, height), minOf(width, height))
  }

  /** A new surface of the attached mixer at the size CameraX chose, or null without a mixer. Blocks at most one second. */
  internal fun acquireSurface(width: Int, height: Int): CameraSurface? {
    if (disposed) return null
    return mixer?.videoMixer?.acquireCameraSurface(this, width, height)
  }

  /**
   * CameraX finished with [surface] (any result). Releases it if the mixer
   * already retired it; otherwise retires it now, unless the slot has moved
   * on to a newer surface (a late callback of a replaced request).
   */
  internal fun cameraFinished(surface: CameraSurface) {
    surface.cameraFinished()
    mixer?.videoMixer?.retireCameraSurface(this, only = surface)
  }

  /** Render thread: the mixer stopped drawing [surface]. */
  internal fun surfaceRetired(surface: CameraSurface) {
    receiving = false
    surface.mixerRetired()
  }

  internal fun setFrontCamera(value: Boolean) {
    frontCamera = value
  }

  internal fun setReceiving(value: Boolean) {
    receiving = value
  }

  internal fun reportError(error: CaptureError) {
    mixer?.reportError(error)
  }
}
