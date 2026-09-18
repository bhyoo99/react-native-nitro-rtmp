package com.margelo.nitro.nitrortmp.capture

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.SurfaceTexture
import android.hardware.camera2.CameraAccessException
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.util.Range
import android.util.Size
import android.view.Surface
import com.margelo.nitro.nitrortmp.CameraPosition
import com.margelo.nitro.nitrortmp.CaptureErrorCode
import com.margelo.nitro.nitrortmp.CaptureException
import kotlin.math.abs

/**
 * Camera2 capture into one Surface: the mixer's
 * `SurfaceTexture` is the only consumer, which sidesteps the LEGACY HAL's
 * two-output limit. Everything runs on its own handler thread;
 * results come back through [Listener] on that thread.
 */
internal class CameraSource(private val context: Context, private val listener: Listener) {
  interface Listener {
    /** The camera is gone (disconnected or an error) while it was open. */
    fun onCameraLost(message: String)
  }

  /** The output size the mixer wants and where to capture into. */
  interface SurfaceProvider {
    fun surfaceFor(width: Int, height: Int): Surface?
  }

  private val thread = HandlerThread("nitrortmp.camera").apply { start() }
  val handler = Handler(thread.looper)
  private val manager = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager

  private var device: CameraDevice? = null
  private var session: CameraCaptureSession? = null
  private var generation = 0  // invalidates callbacks of a closed device
  @Volatile var isOpen = false
    private set
  @Volatile var captureSize: Size? = null
    private set

  /**
   * Opens the camera for [position] and starts a repeating request into the
   * Surface from [provider]. [completion] runs on the camera thread with
   * null on success or the failure to reject `start()` with.
   */
  @SuppressLint("MissingPermission")
  fun open(
    position: CameraPosition,
    targetWidth: Int,
    targetHeight: Int,
    fps: Int,
    provider: SurfaceProvider,
    completion: (CaptureException?) -> Unit,
  ) = handler.post {
    closeOnThread()
    val id = findCamera(position)
    if (id == null) {
      completion(CaptureException(CaptureErrorCode.CAMERAUNAVAILABLE, "no ${positionName(position)} camera"))
      return@post
    }
    val characteristics = try {
      manager.getCameraCharacteristics(id)
    } catch (e: CameraAccessException) {
      completion(CaptureException(CaptureErrorCode.CAMERAUNAVAILABLE, "camera $id: ${e.message}"))
      return@post
    }
    val size = chooseSize(characteristics, targetWidth, targetHeight)
    if (size == null) {
      completion(CaptureException(CaptureErrorCode.CONFIGURATIONFAILED, "camera $id offers no SurfaceTexture output"))
      return@post
    }
    val surface = provider.surfaceFor(size.width, size.height)
    if (surface == null) {
      completion(CaptureException(CaptureErrorCode.CONFIGURATIONFAILED, "the mixer gave no capture surface"))
      return@post
    }
    val fpsRange = chooseFpsRange(characteristics, fps)
    val myGeneration = ++generation
    captureSize = size
    try {
      manager.openCamera(id, object : CameraDevice.StateCallback() {
        override fun onOpened(camera: CameraDevice) {
          if (myGeneration != generation) {
            camera.close()
            return
          }
          device = camera
          isOpen = true
          configure(camera, surface, fpsRange, myGeneration, completion)
        }

        override fun onDisconnected(camera: CameraDevice) {
          camera.close()
          if (myGeneration != generation) return
          val wasOpen = device != null
          device = null
          session = null
          isOpen = false
          if (wasOpen) listener.onCameraLost("camera disconnected") else {
            completion(CaptureException(CaptureErrorCode.CAMERAUNAVAILABLE, "camera disconnected while opening"))
          }
        }

        override fun onError(camera: CameraDevice, error: Int) {
          camera.close()
          if (myGeneration != generation) return
          val wasOpen = device != null
          device = null
          session = null
          isOpen = false
          val message = "camera error $error"
          if (wasOpen) listener.onCameraLost(message) else {
            completion(CaptureException(CaptureErrorCode.CAMERAUNAVAILABLE, message))
          }
        }
      }, handler)
    } catch (e: CameraAccessException) {
      completion(CaptureException(CaptureErrorCode.CAMERAUNAVAILABLE, "openCamera: ${e.message}"))
    } catch (e: SecurityException) {
      completion(CaptureException(CaptureErrorCode.PERMISSIONDENIED, "openCamera: ${e.message}"))
    } catch (e: IllegalArgumentException) {
      completion(CaptureException(CaptureErrorCode.CAMERAUNAVAILABLE, "openCamera: ${e.message}"))
    }
  }

  @Suppress("DEPRECATION")
  private fun configure(
    camera: CameraDevice,
    surface: Surface,
    fpsRange: Range<Int>?,
    myGeneration: Int,
    completion: (CaptureException?) -> Unit,
  ) {
    try {
      camera.createCaptureSession(listOf(surface), object : CameraCaptureSession.StateCallback() {
        override fun onConfigured(s: CameraCaptureSession) {
          if (myGeneration != generation) {
            s.close()
            return
          }
          session = s
          try {
            val request = camera.createCaptureRequest(CameraDevice.TEMPLATE_RECORD)
            request.addTarget(surface)
            request.set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO)
            if (fpsRange != null) request.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, fpsRange)
            s.setRepeatingRequest(request.build(), null, handler)
            completion(null)
          } catch (e: CameraAccessException) {
            completion(CaptureException(CaptureErrorCode.CONFIGURATIONFAILED, "repeating request: ${e.message}"))
          } catch (e: IllegalStateException) {
            completion(CaptureException(CaptureErrorCode.CONFIGURATIONFAILED, "repeating request: ${e.message}"))
          }
        }

        override fun onConfigureFailed(s: CameraCaptureSession) {
          if (myGeneration != generation) return
          completion(CaptureException(CaptureErrorCode.CONFIGURATIONFAILED, "capture session configuration failed"))
        }
      }, handler)
    } catch (e: CameraAccessException) {
      completion(CaptureException(CaptureErrorCode.CONFIGURATIONFAILED, "createCaptureSession: ${e.message}"))
    } catch (e: IllegalArgumentException) {
      completion(CaptureException(CaptureErrorCode.CONFIGURATIONFAILED, "createCaptureSession: ${e.message}"))
    }
  }

  /** Closes the session and the device. [completion] runs on the camera thread. */
  fun close(completion: (() -> Unit)? = null) = handler.post {
    closeOnThread()
    completion?.invoke()
  }

  private fun closeOnThread() {
    generation += 1
    try {
      session?.close()
    } catch (e: IllegalStateException) {
      // already closed
    }
    session = null
    device?.close()
    device = null
    isOpen = false
    captureSize = null
  }

  fun release() {
    handler.post {
      closeOnThread()
      thread.quitSafely()
    }
  }

  private fun findCamera(position: CameraPosition): String? {
    val wanted = if (position == CameraPosition.FRONT) CameraCharacteristics.LENS_FACING_FRONT else CameraCharacteristics.LENS_FACING_BACK
    return try {
      manager.cameraIdList.firstOrNull { id ->
        manager.getCameraCharacteristics(id).get(CameraCharacteristics.LENS_FACING) == wanted
      }
    } catch (e: CameraAccessException) {
      Log.w(TAG, "cameraIdList: ${e.message}")
      null
    }
  }

  /**
   * The smallest output size that covers the mixer's output (compared by
   * long/short side, so a landscape sensor size matches a portrait output),
   * preferring the aspect closest to the output; else the largest available.
   */
  private fun chooseSize(characteristics: CameraCharacteristics, targetWidth: Int, targetHeight: Int): Size? {
    val map = characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP) ?: return null
    val sizes = map.getOutputSizes(SurfaceTexture::class.java)?.toList() ?: return null
    if (sizes.isEmpty()) return null
    val targetLong = maxOf(targetWidth, targetHeight)
    val targetShort = minOf(targetWidth, targetHeight)
    val targetAspect = targetLong.toDouble() / targetShort
    val covering = sizes.filter { maxOf(it.width, it.height) >= targetLong && minOf(it.width, it.height) >= targetShort }
    val candidates = covering.ifEmpty { sizes }
    return candidates.minWithOrNull(
      compareBy<Size> { abs(maxOf(it.width, it.height).toDouble() / minOf(it.width, it.height) - targetAspect) }
        .thenBy { if (covering.isEmpty()) -(it.width.toLong() * it.height) else it.width.toLong() * it.height },
    )
  }

  /** The range containing [fps] with the smallest span, else the one whose upper bound is closest. */
  private fun chooseFpsRange(characteristics: CameraCharacteristics, fps: Int): Range<Int>? {
    val ranges = characteristics.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES) ?: return null
    if (ranges.isEmpty()) return null
    val containing = ranges.filter { it.lower <= fps && fps <= it.upper }
    if (containing.isNotEmpty()) return containing.minByOrNull { it.upper - it.lower }
    return ranges.minByOrNull { abs(it.upper - fps) }
  }

  companion object {
    private const val TAG = "nitrortmp.camera"

    fun positionName(position: CameraPosition): String =
      if (position == CameraPosition.FRONT) "front" else "back"
  }
}
