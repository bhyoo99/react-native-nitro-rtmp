package com.margelo.nitro.nitrortmp.mixer

import android.graphics.SurfaceTexture
import android.view.Surface

/**
 * One Surface handed to CameraX for one `SurfaceRequest`: a `SurfaceTexture`
 * on an OES texture of the mixer's render context. CameraX must not reuse a
 * Surface across requests, so every request gets a new one.
 *
 * Two owners must let go before it is released: the camera (the
 * `provideSurface` result callback, which may arrive well after the mixer
 * moved on) and the mixer (which detaches the texture from its GL context
 * when the slot switches surfaces, the layer leaves or the mixer dies).
 * Whichever comes last releases the Surface and the SurfaceTexture, from
 * any thread: the texture is detached from GL by then.
 */
internal class CameraSurface(val surface: Surface, val texture: SurfaceTexture, var oesTexture: Int) {
  private var cameraDone = false
  private var mixerDone = false

  /** The camera no longer uses the surface (any `SurfaceRequest.Result`). */
  fun cameraFinished() = finish(camera = true)

  /** Render thread, after `detachFromGLContext`: the mixer no longer draws it. */
  fun mixerRetired() = finish(camera = false)

  private fun finish(camera: Boolean) {
    val release = synchronized(this) {
      if (camera) cameraDone = true else mixerDone = true
      cameraDone && mixerDone
    }
    if (release) {
      surface.release()
      texture.release()
    }
  }
}
