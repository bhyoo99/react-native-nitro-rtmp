package com.margelo.nitro.nitrortmp.mixer

import android.opengl.EGL14
import android.opengl.EGLConfig
import android.opengl.EGLContext
import android.opengl.EGLDisplay
import android.opengl.EGLExt
import android.opengl.EGLSurface
import android.util.Log

/**
 * One EGL display + GLES 2.0 context for the render thread (after grafika's EglCore). The context is created with a recordable
 * config when the driver offers one, so the encoder's input Surface and the
 * preview Surfaces can both be window surfaces of the same context. A 1x1
 * pbuffer keeps the context current while no window surface exists.
 *
 * Every method must be called on the thread that owns the context.
 */
internal class EglCore {
  private var display: EGLDisplay = EGL14.EGL_NO_DISPLAY
  private var context: EGLContext = EGL14.EGL_NO_CONTEXT
  private var config: EGLConfig? = null
  private var pbuffer: EGLSurface = EGL14.EGL_NO_SURFACE

  init {
    display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
    if (display === EGL14.EGL_NO_DISPLAY) throw RuntimeException("eglGetDisplay failed")
    val version = IntArray(2)
    if (!EGL14.eglInitialize(display, version, 0, version, 1)) {
      display = EGL14.EGL_NO_DISPLAY
      throw RuntimeException("eglInitialize failed")
    }
    val chosen = chooseConfig(recordable = true) ?: chooseConfig(recordable = false)
      ?: throw RuntimeException("no EGL config with GLES 2.0 support")
    config = chosen
    val attributes = intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE)
    context = EGL14.eglCreateContext(display, chosen, EGL14.EGL_NO_CONTEXT, attributes, 0)
    checkError("eglCreateContext")
    val pbufferAttributes = intArrayOf(EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1, EGL14.EGL_NONE)
    pbuffer = EGL14.eglCreatePbufferSurface(display, chosen, pbufferAttributes, 0)
    checkError("eglCreatePbufferSurface")
    makeCurrent(pbuffer)
  }

  private fun chooseConfig(recordable: Boolean): EGLConfig? {
    val attributes = mutableListOf(
      EGL14.EGL_RED_SIZE, 8,
      EGL14.EGL_GREEN_SIZE, 8,
      EGL14.EGL_BLUE_SIZE, 8,
      EGL14.EGL_ALPHA_SIZE, 8,
      EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
      EGL14.EGL_SURFACE_TYPE, EGL14.EGL_WINDOW_BIT or EGL14.EGL_PBUFFER_BIT,
    )
    if (recordable) {
      attributes += EGL_RECORDABLE_ANDROID
      attributes += 1
    }
    attributes += EGL14.EGL_NONE
    val configs = arrayOfNulls<EGLConfig>(1)
    val count = IntArray(1)
    if (!EGL14.eglChooseConfig(display, attributes.toIntArray(), 0, configs, 0, 1, count, 0) || count[0] == 0) {
      return null
    }
    return configs[0]
  }

  /** A window surface for a `Surface` or a `SurfaceTexture`. */
  fun createWindowSurface(surface: Any): EGLSurface {
    val attributes = intArrayOf(EGL14.EGL_NONE)
    val result = EGL14.eglCreateWindowSurface(display, config, surface, attributes, 0)
    checkError("eglCreateWindowSurface")
    if (result === EGL14.EGL_NO_SURFACE) throw RuntimeException("eglCreateWindowSurface returned EGL_NO_SURFACE")
    return result
  }

  fun releaseSurface(surface: EGLSurface) {
    if (surface !== EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(display, surface)
  }

  fun makeCurrent(surface: EGLSurface) {
    if (!EGL14.eglMakeCurrent(display, surface, surface, context)) {
      throw RuntimeException("eglMakeCurrent failed: 0x${Integer.toHexString(EGL14.eglGetError())}")
    }
  }

  /** Back to the pbuffer so no window surface stays current after it is released. */
  fun makeNothingCurrent() {
    EGL14.eglMakeCurrent(display, pbuffer, pbuffer, context)
  }

  fun swapBuffers(surface: EGLSurface): Boolean = EGL14.eglSwapBuffers(display, surface)

  /** The frame's timestamp for the encoder's input Surface. */
  fun setPresentationTime(surface: EGLSurface, timestampNs: Long) {
    EGLExt.eglPresentationTimeANDROID(display, surface, timestampNs)
  }

  fun querySurfaceSize(surface: EGLSurface): Pair<Int, Int> {
    val value = IntArray(1)
    EGL14.eglQuerySurface(display, surface, EGL14.EGL_WIDTH, value, 0)
    val width = value[0]
    EGL14.eglQuerySurface(display, surface, EGL14.EGL_HEIGHT, value, 0)
    return width to value[0]
  }

  fun release() {
    if (display !== EGL14.EGL_NO_DISPLAY) {
      EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT)
      if (pbuffer !== EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(display, pbuffer)
      if (context !== EGL14.EGL_NO_CONTEXT) EGL14.eglDestroyContext(display, context)
      EGL14.eglReleaseThread()
      EGL14.eglTerminate(display)
    }
    pbuffer = EGL14.EGL_NO_SURFACE
    context = EGL14.EGL_NO_CONTEXT
    display = EGL14.EGL_NO_DISPLAY
    config = null
  }

  private fun checkError(operation: String) {
    val error = EGL14.eglGetError()
    if (error != EGL14.EGL_SUCCESS) {
      Log.e(TAG, "$operation: EGL error 0x${Integer.toHexString(error)}")
      throw RuntimeException("$operation: EGL error 0x${Integer.toHexString(error)}")
    }
  }

  companion object {
    private const val TAG = "nitrortmp.egl"
    /** EGL_ANDROID_recordable: buffers the video encoder can consume. */
    private const val EGL_RECORDABLE_ANDROID = 0x3142
  }
}
