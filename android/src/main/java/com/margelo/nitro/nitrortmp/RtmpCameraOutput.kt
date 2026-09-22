package com.margelo.nitro.nitrortmp

import android.annotation.SuppressLint
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.Surface
import androidx.annotation.Keep
import androidx.camera.core.CameraSelector
import androidx.camera.core.Preview
import androidx.camera.core.SurfaceRequest
import androidx.camera.core.resolutionselector.ResolutionSelector
import androidx.camera.core.resolutionselector.ResolutionStrategy
import com.margelo.nitro.camera.CameraOrientation
import com.margelo.nitro.camera.HybridCameraOutputSpec
import com.margelo.nitro.camera.MediaType
import com.margelo.nitro.camera.MirrorMode
import com.margelo.nitro.camera.Size
import com.margelo.nitro.camera.TargetStabilizationMode
import com.margelo.nitro.camera.public.NativeCameraOutput
import com.facebook.proguard.annotations.DoNotStrip
import java.util.concurrent.Executors

/**
 * The VisionCamera output behind `CameraLayer.output`: a CameraX `Preview`
 * use case whose surface is the mixer's `SurfaceTexture`. VisionCamera binds
 * it to the camera it manages; CameraX then asks [surfaceProvider] for a
 * surface, and the layer hands over the mixer's. The buffers are never
 * mirrored (the preview mirrors a front camera in the shader); rotation
 * follows `outputOrientation`, which VisionCamera sets from the device
 * orientation.
 *
 * Everything CameraX calls back runs on a shared camera executor; `Preview`
 * setters that CameraX restricts to the main thread are posted there.
 * Each `SurfaceRequest` gets a new [CameraSurface] and owns it until its
 * result callback, so a late callback of a replaced request can never take
 * the current surface away.
 */
@DoNotStrip
@Keep
@SuppressLint("RestrictedApi")
internal class RtmpCameraOutput(private val layer: HybridCameraLayer) :
  HybridCameraOutputSpec(),
  NativeCameraOutput {
  private val mainHandler = Handler(Looper.getMainLooper())
  @Volatile private var preview: Preview? = null
  @Volatile private var request: SurfaceRequest? = null
  @Volatile private var orientation = CameraOrientation.UP
  @Volatile private var configuredMirrorMode = MirrorMode.AUTO
  @Volatile private var released = false

  // --- HybridCameraOutputSpec -----------------------------------------------------------------

  override val mediaType: MediaType = MediaType.VIDEO

  override var outputOrientation: CameraOrientation
    get() = orientation
    set(value) {
      orientation = value
      preview?.targetRotation = surfaceRotation(value)
    }

  override val currentResolution: Size?
    get() {
      val resolution = preview?.resolutionInfo?.resolution ?: return null
      return Size(resolution.width.toDouble(), resolution.height.toDouble())
    }

  // --- NativeCameraOutput ---------------------------------------------------------------------

  override val mirrorMode: MirrorMode
    get() = configuredMirrorMode

  override fun createUseCase(
    mirrorMode: MirrorMode,
    config: NativeCameraOutput.Config,
  ): NativeCameraOutput.PreparedUseCase {
    configuredMirrorMode = mirrorMode
    val target = layer.targetSize()
    val preview = Preview.Builder().apply {
      setTargetRotation(surfaceRotation(orientation))
      setResolutionSelector(
        ResolutionSelector.Builder()
          .setResolutionStrategy(ResolutionStrategy(target, ResolutionStrategy.FALLBACK_RULE_CLOSEST_HIGHER_THEN_LOWER))
          .build(),
      )
      // The stream is never mirrored; the layer mirrors a front camera in the preview only.
      setMirrorMode(androidx.camera.core.MirrorMode.MIRROR_MODE_OFF)
      when (config.previewStabilizationMode) {
        TargetStabilizationMode.OFF -> setPreviewStabilizationEnabled(false)
        null, TargetStabilizationMode.AUTO -> {}
        else -> setPreviewStabilizationEnabled(true)
      }
    }.build()
    return NativeCameraOutput.PreparedUseCase(preview) {
      // Main thread (VisionCamera binds on it); setSurfaceProvider requires it.
      this.preview = preview
      if (!released) preview.setSurfaceProvider(executor, surfaceProvider)
    }
  }

  // --- layer side -----------------------------------------------------------------------------

  /** The layer joined a mixer: make CameraX request a surface again. */
  fun layerAttached() {
    val p = preview ?: return
    mainHandler.post { if (!released && preview === p) p.setSurfaceProvider(executor, surfaceProvider) }
  }

  /** The layer left its mixer: CameraX must stop writing before the surface goes. */
  fun layerDetached() {
    request?.invalidate()  // CameraX re-requests; the provider answers "no surface" while detached
  }

  fun release() {
    released = true
    request?.invalidate()
    request = null
    val p = preview
    preview = null
    mainHandler.post { p?.setSurfaceProvider(null) }
  }

  // --- CameraX callbacks (executor) -----------------------------------------------------------

  private val surfaceProvider = Preview.SurfaceProvider { request -> onSurfaceRequested(request) }

  private fun onSurfaceRequested(request: SurfaceRequest) {
    this.request = request
    // CameraInternal is library-restricted; VisionCamera reads it the same way.
    val lensFacing = try {
      request.camera.cameraInfo.lensFacing
    } catch (e: RuntimeException) {
      CameraSelector.LENS_FACING_UNKNOWN
    }
    layer.setFrontCamera(lensFacing == CameraSelector.LENS_FACING_FRONT)
    request.setTransformationInfoListener(executor) { info ->
      layer.extraRotationDegrees = if (info.hasCameraTransform()) 0 else info.rotationDegrees
    }
    val resolution = request.resolution
    val surface = layer.acquireSurface(resolution.width, resolution.height)
    if (surface == null) {
      request.willNotProvideSurface()
      return
    }
    // The result callback is the only signal that the camera is done with this
    // Surface (CameraX docs); it may arrive after a newer request took over.
    request.provideSurface(surface.surface, executor) { result ->
      if (this.request === request) this.request = null
      if (result.resultCode == SurfaceRequest.Result.RESULT_INVALID_SURFACE) {
        layer.reportError(CaptureError(CaptureErrorCode.CONFIGURATIONFAILED, "CameraX rejected the mixer surface"))
      }
      layer.cameraFinished(surface)
    }
    layer.setReceiving(true)
    Log.i(TAG, "camera surface ${resolution.width}x${resolution.height} provided (front=${lensFacing == CameraSelector.LENS_FACING_FRONT})")
  }

  private fun surfaceRotation(orientation: CameraOrientation): Int = when (orientation) {
    CameraOrientation.UP -> Surface.ROTATION_0
    CameraOrientation.DOWN -> Surface.ROTATION_180
    CameraOrientation.LEFT -> Surface.ROTATION_270
    CameraOrientation.RIGHT -> Surface.ROTATION_90
  }

  companion object {
    private const val TAG = "nitrortmp.camera"
    /** Shared by every output: CameraX callbacks (including late ones after release) land here. */
    private val executor = Executors.newSingleThreadExecutor { Thread(it, "nitrortmp.camera") }
  }
}
