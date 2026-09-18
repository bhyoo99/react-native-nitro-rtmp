package com.margelo.nitro.nitrortmp.mixer

import android.graphics.Bitmap
import android.graphics.SurfaceTexture
import android.opengl.EGL14
import android.opengl.EGLSurface
import android.opengl.GLES11Ext
import android.opengl.GLES20
import android.opengl.GLUtils
import android.opengl.Matrix
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.util.Log
import android.view.Surface
import com.margelo.nitro.nitrortmp.CameraPosition
import com.margelo.nitro.nitrortmp.HybridCameraSource
import com.margelo.nitro.nitrortmp.HybridImageLayer
import com.margelo.nitro.nitrortmp.HybridVideoLayerSpec
import com.margelo.nitro.nitrortmp.LayerFrame
import com.margelo.nitro.nitrortmp.MixerCounters
import com.margelo.nitro.nitrortmp.PreviewResizeMode
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min

/**
 * The GPU compositor: one render thread that
 * owns the EGL context, the camera `SurfaceTexture`s, the image textures,
 * the encoder's input window surface and every preview surface.
 *
 * A camera frame arrives as `onFrameAvailable` on the render handler; the
 * render draws every layer (one quad each, z order = insertion order) into
 * the encoder surface with the frame's timestamp, then draws the same scene
 * into every preview (front camera mirrored there only). Queue length
 * is one: a frame that arrives while a render is pending is dropped and
 * counted. Without a running camera the render runs
 * on a timer at the output frame rate.
 *
 * Methods marked "posted" may be called from any thread.
 */
internal class VideoMixer(private val counters: MixerCounters) {
  /** What a preview view gives the mixer (HybridPreviewView implements it). */
  interface PreviewTarget {
    /** The `SurfaceTexture` of the TextureView; null once destroyed. */
    val previewSurfaceTexture: SurfaceTexture?
    val previewResizeMode: PreviewResizeMode
  }

  private class LayerSlot(val layer: HybridVideoLayerSpec, var frame: LayerFrame?) {
    // camera
    var surfaceTexture: SurfaceTexture? = null
    var surface: Surface? = null
    var oesTexture = 0
    val transform = FloatArray(16).also { Matrix.setIdentityM(it, 0) }
    var bufferWidth = 0
    var bufferHeight = 0
    var frameAvailable = false
    var hasFrame = false
    var lastTimestampNs = 0L
    // image
    var texture2d = 0
    var uploadedBitmap: Bitmap? = null
  }

  private class PreviewSlot(val target: PreviewTarget, val surface: Surface, val eglSurface: EGLSurface)

  private val thread = HandlerThread("nitrortmp.render").apply { start() }
  val handler = Handler(thread.looper)

  // Render-thread state
  private var egl: EglCore? = null
  private var oesProgram: GlProgram? = null
  private var imageProgram: GlProgram? = null
  private val layers = ArrayList<LayerSlot>()
  private val previews = ArrayList<PreviewSlot>()
  private var encoderSurface: EGLSurface? = null
  private var encoderWindow: Surface? = null
  private var renderPending = false
  private var timerRunning = false
  @Volatile private var released = false

  @Volatile private var outputWidth = 720
  @Volatile private var outputHeight = 1280
  @Volatile private var frameRate = 30.0

  /** Called from the encoder pass when the encoder surface is unusable (posted to the owner). */
  var onEncoderSurfaceFailed: ((message: String) -> Unit)? = null

  private val mvp = FloatArray(16)
  private val tex = FloatArray(16)
  private val crop = FloatArray(16)
  private val mirror = FloatArray(16).also {
    Matrix.setIdentityM(it, 0)
    Matrix.translateM(it, 0, 1f, 0f, 0f)
    Matrix.scaleM(it, 0, -1f, 1f, 1f)
  }
  private val flipY = FloatArray(16).also {
    Matrix.setIdentityM(it, 0)
    Matrix.translateM(it, 0, 0f, 1f, 0f)
    Matrix.scaleM(it, 0, 1f, -1f, 1f)
  }
  private val scratch = FloatArray(16)

  private val renderRunnable = Runnable { render(null) }
  private val timerRunnable = object : Runnable {
    override fun run() {
      if (!timerRunning || released) return
      render(System.nanoTime())
      handler.postDelayed(this, (1000.0 / max(1.0, frameRate)).toLong())
    }
  }

  // --- configuration (any thread) -----------------------------------------------------

  fun setOutput(width: Int, height: Int, fps: Double) {
    outputWidth = max(2, width)
    outputHeight = max(2, height)
    frameRate = fps
  }

  val currentOutputWidth: Int get() = outputWidth
  val currentOutputHeight: Int get() = outputHeight

  // --- layers (posted) ----------------------------------------------------------------

  fun addLayer(layer: HybridVideoLayerSpec, frame: LayerFrame?) = post {
    if (layers.any { it.layer === layer }) return@post
    ensureGl() ?: return@post
    layers.add(LayerSlot(layer, frame))
    updateTimer()
  }

  fun removeLayer(layer: HybridVideoLayerSpec) = post {
    val index = layers.indexOfFirst { it.layer === layer }
    if (index < 0) return@post
    releaseSlot(layers.removeAt(index))
    updateTimer()
  }

  fun setLayerFrame(layer: HybridVideoLayerSpec, frame: LayerFrame?) = post {
    layers.firstOrNull { it.layer === layer }?.frame = frame
  }

  // --- camera surfaces ------------------------------------------------------------------

  /**
   * The Surface a camera layer captures into: a `SurfaceTexture` on an OES
   * texture of the render context. Blocks the caller until the render
   * thread created it (at most one second). Null when the layer is not added
   * or the GL context could not be created.
   */
  fun acquireCameraSurface(layer: HybridCameraSource, width: Int, height: Int): Surface? {
    if (released) return null
    if (Looper.myLooper() === handler.looper) return acquireOnRender(layer, width, height)
    var result: Surface? = null
    val latch = CountDownLatch(1)
    if (!handler.post {
        result = acquireOnRender(layer, width, height)
        latch.countDown()
      }
    ) return null
    latch.await(1, TimeUnit.SECONDS)
    return result
  }

  private fun acquireOnRender(layer: HybridCameraSource, width: Int, height: Int): Surface? {
    ensureGl() ?: return null
    val slot = layers.firstOrNull { it.layer === layer } ?: return null
    slot.bufferWidth = width
    slot.bufferHeight = height
    slot.surfaceTexture?.let { existing ->
      existing.setDefaultBufferSize(width, height)
      return slot.surface
    }
    slot.oesTexture = GlProgram.createTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES)
    val st = SurfaceTexture(slot.oesTexture)
    st.setDefaultBufferSize(width, height)
    st.setOnFrameAvailableListener({ onFrameAvailable(slot) }, handler)
    slot.surfaceTexture = st
    slot.surface = Surface(st)
    slot.hasFrame = false
    updateTimer()
    return slot.surface
  }

  /** The camera closed: the layer goes dark until it starts again. Posted. */
  fun releaseCameraSurface(layer: HybridCameraSource) = post {
    val slot = layers.firstOrNull { it.layer === layer } ?: return@post
    releaseCameraOfSlot(slot)
    updateTimer()
  }

  /** Render thread (delivered through the handler). Queue length one. */
  private fun onFrameAvailable(slot: LayerSlot) {
    if (released || slot.surfaceTexture == null) return
    counters.capturedFrames.incrementAndGet()
    slot.frameAvailable = true
    if (renderPending) {
      counters.droppedFrames.incrementAndGet()
      return
    }
    renderPending = true
    handler.post(renderRunnable)
  }

  // --- encoder surface (render thread only) ------------------------------------------------

  /** Must be called on the render thread (HybridMixer posts its start/stop there). */
  fun setEncoderSurface(surface: Surface?) {
    check(Looper.myLooper() === handler.looper) { "setEncoderSurface off the render thread" }
    encoderSurface?.let { old ->
      egl?.makeNothingCurrent()
      egl?.releaseSurface(old)
    }
    encoderSurface = null
    encoderWindow = null
    if (surface != null) {
      val core = ensureGl() ?: return
      encoderSurface = core.createWindowSurface(surface)
      encoderWindow = surface
    }
    updateTimer()
  }

  // --- previews -------------------------------------------------------------------------

  fun addPreview(target: PreviewTarget) = post {
    if (previews.any { it.target === target }) return@post
    val st = target.previewSurfaceTexture ?: return@post
    val core = ensureGl() ?: return@post
    val surface = Surface(st)
    try {
      previews.add(PreviewSlot(target, surface, core.createWindowSurface(surface)))
    } catch (e: RuntimeException) {
      Log.w(TAG, "preview surface failed: ${e.message}")
      surface.release()
    }
    updateTimer()
    if (!renderPending) {
      renderPending = true
      handler.post(renderRunnable)
    }
  }

  /**
   * Releases the preview's EGL surface on the render thread and waits for it
   * (at most one second) so `TextureView` may destroy its SurfaceTexture.
   */
  fun removePreview(target: PreviewTarget) {
    if (Looper.myLooper() === handler.looper) {
      removePreviewOnRender(target)
      return
    }
    val latch = CountDownLatch(1)
    if (!handler.post {
        removePreviewOnRender(target)
        latch.countDown()
      }
    ) return
    latch.await(1, TimeUnit.SECONDS)
  }

  private fun removePreviewOnRender(target: PreviewTarget) {
    val index = previews.indexOfFirst { it.target === target }
    if (index < 0) return
    val slot = previews.removeAt(index)
    egl?.makeNothingCurrent()
    egl?.releaseSurface(slot.eglSurface)
    slot.surface.release()
    updateTimer()
  }

  // --- lifecycle --------------------------------------------------------------------------

  fun release() {
    released = true
    handler.post {
      timerRunning = false
      handler.removeCallbacks(timerRunnable)
      for (slot in layers) releaseSlot(slot)
      layers.clear()
      for (slot in previews) {
        egl?.releaseSurface(slot.eglSurface)
        slot.surface.release()
      }
      previews.clear()
      encoderSurface?.let { egl?.releaseSurface(it) }
      encoderSurface = null
      encoderWindow = null
      egl?.makeNothingCurrent()
      oesProgram?.release()
      imageProgram?.release()
      oesProgram = null
      imageProgram = null
      egl?.release()
      egl = null
      thread.quitSafely()
    }
  }

  // --- render thread internals ----------------------------------------------------------------

  private fun post(block: () -> Unit) {
    if (released) return
    handler.post {
      if (!released) block()
    }
  }

  private fun ensureGl(): EglCore? {
    egl?.let { return it }
    return try {
      val core = EglCore()
      oesProgram = GlProgram(external = true)
      imageProgram = GlProgram(external = false)
      egl = core
      core
    } catch (e: RuntimeException) {
      Log.e(TAG, "GL setup failed: ${e.message}")
      null
    }
  }

  private fun releaseCameraOfSlot(slot: LayerSlot) {
    slot.surface?.release()
    slot.surfaceTexture?.setOnFrameAvailableListener(null)
    slot.surfaceTexture?.release()
    slot.surface = null
    slot.surfaceTexture = null
    GlProgram.deleteTexture(slot.oesTexture)
    slot.oesTexture = 0
    slot.hasFrame = false
    slot.frameAvailable = false
  }

  private fun releaseSlot(slot: LayerSlot) {
    releaseCameraOfSlot(slot)
    GlProgram.deleteTexture(slot.texture2d)
    slot.texture2d = 0
    slot.uploadedBitmap = null
  }

  /** Timer mode: no running camera, but someone wants frames. */
  private fun updateTimer() {
    val cameraRunning = layers.any { it.surfaceTexture != null }
    val wanted = !cameraRunning && (encoderSurface != null || previews.isNotEmpty())
    if (wanted == timerRunning) return
    timerRunning = wanted
    handler.removeCallbacks(timerRunnable)
    if (wanted) handler.post(timerRunnable)
  }

  private fun render(timerTimestampNs: Long?) {
    renderPending = false
    if (released) return
    val core = egl ?: return
    var timestampNs = 0L
    for (slot in layers) {
      val st = slot.surfaceTexture ?: continue
      if (slot.frameAvailable) {
        slot.frameAvailable = false
        try {
          st.updateTexImage()
        } catch (e: RuntimeException) {
          Log.w(TAG, "updateTexImage failed: ${e.message}")
          continue
        }
        st.getTransformMatrix(slot.transform)
        slot.hasFrame = true
        slot.lastTimestampNs = st.timestamp
      }
      if (slot.hasFrame) timestampNs = max(timestampNs, slot.lastTimestampNs)
    }
    if (timerTimestampNs != null || timestampNs == 0L) timestampNs = timerTimestampNs ?: System.nanoTime()

    var drewSomething = false
    encoderSurface?.let { surface ->
      try {
        core.makeCurrent(surface)
        GLES20.glViewport(0, 0, outputWidth, outputHeight)
        drawScene(outputWidth, outputHeight, mirrorFront = false, scaleX = 1f, scaleY = 1f)
        core.setPresentationTime(surface, timestampNs)
        if (core.swapBuffers(surface)) {
          drewSomething = true
        } else {
          counters.droppedFrames.incrementAndGet()
          Log.w(TAG, "encoder swapBuffers failed: 0x${Integer.toHexString(EGL14.eglGetError())}")
        }
      } catch (e: RuntimeException) {
        counters.droppedFrames.incrementAndGet()
        onEncoderSurfaceFailed?.invoke(e.message ?: "encoder surface failed")
      }
    }
    val outputAspect = outputWidth.toFloat() / outputHeight.toFloat()
    for (preview in ArrayList(previews)) {
      try {
        core.makeCurrent(preview.eglSurface)
        val (width, height) = core.querySurfaceSize(preview.eglSurface)
        if (width <= 0 || height <= 0) continue
        GLES20.glViewport(0, 0, width, height)
        val viewAspect = width.toFloat() / height.toFloat()
        val ratio = outputAspect / viewAspect
        val (scaleX, scaleY) = when (preview.target.previewResizeMode) {
          PreviewResizeMode.CONTAIN -> min(1f, ratio) to min(1f, 1f / ratio)
          PreviewResizeMode.COVER -> max(1f, ratio) to max(1f, 1f / ratio)
        }
        drawScene(width, height, mirrorFront = true, scaleX = scaleX, scaleY = scaleY)
        core.swapBuffers(preview.eglSurface)
        drewSomething = true
      } catch (e: RuntimeException) {
        Log.w(TAG, "preview draw failed: ${e.message}")
      }
    }
    if (drewSomething) counters.renderedFrames.incrementAndGet()
  }

  /**
   * Draws every layer into the current surface. Output coordinates are the
   * normalized `LayerFrame` space (origin top-left); [scaleX]/[scaleY] map
   * the output frame onto the viewport (cover/contain for previews).
   */
  private fun drawScene(viewportWidth: Int, viewportHeight: Int, mirrorFront: Boolean, scaleX: Float, scaleY: Float) {
    GLES20.glClearColor(0f, 0f, 0f, 1f)
    GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)
    GLES20.glEnable(GLES20.GL_BLEND)
    GLES20.glBlendFunc(GLES20.GL_ONE, GLES20.GL_ONE_MINUS_SRC_ALPHA)
    Matrix.setIdentityM(mvp, 0)
    Matrix.scaleM(mvp, 0, scaleX, scaleY, 1f)
    val outW = outputWidth.toFloat()
    val outH = outputHeight.toFloat()
    for (slot in layers) {
      val frame = slot.frame
      val rx = frame?.x?.toFloat() ?: 0f
      val ry = frame?.y?.toFloat() ?: 0f
      val rw = frame?.width?.toFloat() ?: 1f
      val rh = frame?.height?.toFloat() ?: 1f
      if (rw <= 0f || rh <= 0f) continue
      val rectAspect = (rw * outW) / (rh * outH)
      when (val layer = slot.layer) {
        is HybridCameraSource -> {
          if (!slot.hasFrame || slot.oesTexture == 0) continue
          val program = oesProgram ?: continue
          // Aspect-fill: crop the (rotated) camera image around its center.
          val rotated = abs(slot.transform[0]) < 0.01f
          val imageAspect = if (rotated) {
            slot.bufferHeight.toFloat() / max(1, slot.bufferWidth)
          } else {
            slot.bufferWidth.toFloat() / max(1, slot.bufferHeight)
          }
          var cropW = 1f
          var cropH = 1f
          if (imageAspect > rectAspect) cropW = rectAspect / imageAspect else cropH = imageAspect / rectAspect
          Matrix.setIdentityM(crop, 0)
          Matrix.translateM(crop, 0, (1f - cropW) / 2f, (1f - cropH) / 2f, 0f)
          Matrix.scaleM(crop, 0, cropW, cropH, 1f)
          if (mirrorFront && layer.position == CameraPosition.FRONT) {
            Matrix.multiplyMM(scratch, 0, crop, 0, mirror, 0)
            Matrix.multiplyMM(tex, 0, slot.transform, 0, scratch, 0)
          } else {
            Matrix.multiplyMM(tex, 0, slot.transform, 0, crop, 0)
          }
          program.draw(slot.oesTexture, 2f * rx - 1f, 1f - 2f * (ry + rh), 2f * (rx + rw) - 1f, 1f - 2f * ry, mvp, tex)
        }
        is HybridImageLayer -> {
          val bitmap = layer.bitmap ?: continue
          val program = imageProgram ?: continue
          if (slot.uploadedBitmap !== bitmap || slot.texture2d == 0) {
            if (slot.texture2d == 0) slot.texture2d = GlProgram.createTexture(GLES20.GL_TEXTURE_2D)
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, slot.texture2d)
            GLUtils.texImage2D(GLES20.GL_TEXTURE_2D, 0, bitmap, 0)
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, 0)
            slot.uploadedBitmap = bitmap
          }
          // Aspect-fit inside the rect, centered.
          val imageAspect = bitmap.width.toFloat() / max(1, bitmap.height)
          val pw = rw * outW
          val ph = rh * outH
          val fw: Float
          val fh: Float
          if (imageAspect > rectAspect) {
            fw = pw
            fh = pw / imageAspect
          } else {
            fh = ph
            fw = ph * imageAspect
          }
          val nw = fw / outW
          val nh = fh / outH
          val nx = rx + (rw - nw) / 2f
          val ny = ry + (rh - nh) / 2f
          program.draw(slot.texture2d, 2f * nx - 1f, 1f - 2f * (ny + nh), 2f * (nx + nw) - 1f, 1f - 2f * ny, mvp, flipY)
        }
        else -> {}
      }
    }
    GLES20.glDisable(GLES20.GL_BLEND)
  }

  companion object {
    private const val TAG = "nitrortmp.mixer"
  }
}
