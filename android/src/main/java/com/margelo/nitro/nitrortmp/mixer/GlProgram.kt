package com.margelo.nitro.nitrortmp.mixer

import android.opengl.GLES11Ext
import android.opengl.GLES20
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer

/**
 * One textured-quad program: a layer is one quad with
 * a position transform (`uMvp`) and a texture-coordinate transform (`uTex`).
 * Two instances exist on the render thread: one sampling the camera's
 * external OES texture, one sampling regular 2D textures (images).
 */
internal class GlProgram(private val external: Boolean) {
  private val program: Int
  private val aPosition: Int
  private val aTexCoord: Int
  private val uMvp: Int
  private val uTex: Int
  private val uTexture: Int

  private val positions: FloatBuffer = floatBuffer(8)
  private val texCoords: FloatBuffer = floatBuffer(8).apply {
    // Bottom-left, bottom-right, top-left, top-right (GL: v = 0 at the bottom).
    put(floatArrayOf(0f, 0f, 1f, 0f, 0f, 1f, 1f, 1f)).position(0)
  }

  init {
    val vertex = compile(GLES20.GL_VERTEX_SHADER, VERTEX_SHADER)
    val fragment = compile(GLES20.GL_FRAGMENT_SHADER, if (external) FRAGMENT_SHADER_OES else FRAGMENT_SHADER_2D)
    program = GLES20.glCreateProgram()
    GLES20.glAttachShader(program, vertex)
    GLES20.glAttachShader(program, fragment)
    GLES20.glLinkProgram(program)
    val status = IntArray(1)
    GLES20.glGetProgramiv(program, GLES20.GL_LINK_STATUS, status, 0)
    GLES20.glDeleteShader(vertex)
    GLES20.glDeleteShader(fragment)
    if (status[0] != GLES20.GL_TRUE) {
      val log = GLES20.glGetProgramInfoLog(program)
      GLES20.glDeleteProgram(program)
      throw RuntimeException("GL program link failed: $log")
    }
    aPosition = GLES20.glGetAttribLocation(program, "aPosition")
    aTexCoord = GLES20.glGetAttribLocation(program, "aTexCoord")
    uMvp = GLES20.glGetUniformLocation(program, "uMvp")
    uTex = GLES20.glGetUniformLocation(program, "uTex")
    uTexture = GLES20.glGetUniformLocation(program, "sTexture")
  }

  /**
   * Draws the quad whose corners are (left, bottom)..(right, top) in NDC,
   * transformed by [mvp]; texture coordinates (0..1) go through [tex].
   */
  fun draw(textureId: Int, left: Float, bottom: Float, right: Float, top: Float, mvp: FloatArray, tex: FloatArray) {
    positions.put(floatArrayOf(left, bottom, right, bottom, left, top, right, top)).position(0)
    GLES20.glUseProgram(program)
    GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
    GLES20.glBindTexture(if (external) GLES11Ext.GL_TEXTURE_EXTERNAL_OES else GLES20.GL_TEXTURE_2D, textureId)
    GLES20.glUniform1i(uTexture, 0)
    GLES20.glUniformMatrix4fv(uMvp, 1, false, mvp, 0)
    GLES20.glUniformMatrix4fv(uTex, 1, false, tex, 0)
    GLES20.glEnableVertexAttribArray(aPosition)
    GLES20.glVertexAttribPointer(aPosition, 2, GLES20.GL_FLOAT, false, 0, positions)
    GLES20.glEnableVertexAttribArray(aTexCoord)
    GLES20.glVertexAttribPointer(aTexCoord, 2, GLES20.GL_FLOAT, false, 0, texCoords)
    GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)
    GLES20.glDisableVertexAttribArray(aPosition)
    GLES20.glDisableVertexAttribArray(aTexCoord)
    GLES20.glBindTexture(if (external) GLES11Ext.GL_TEXTURE_EXTERNAL_OES else GLES20.GL_TEXTURE_2D, 0)
    GLES20.glUseProgram(0)
  }

  fun release() {
    GLES20.glDeleteProgram(program)
  }

  companion object {
    private const val VERTEX_SHADER = """
      uniform mat4 uMvp;
      uniform mat4 uTex;
      attribute vec4 aPosition;
      attribute vec4 aTexCoord;
      varying vec2 vTexCoord;
      void main() {
        gl_Position = uMvp * aPosition;
        vTexCoord = (uTex * aTexCoord).xy;
      }
    """
    private const val FRAGMENT_SHADER_2D = """
      precision mediump float;
      varying vec2 vTexCoord;
      uniform sampler2D sTexture;
      void main() {
        gl_FragColor = texture2D(sTexture, vTexCoord);
      }
    """
    private const val FRAGMENT_SHADER_OES = """
      #extension GL_OES_EGL_image_external : require
      precision mediump float;
      varying vec2 vTexCoord;
      uniform samplerExternalOES sTexture;
      void main() {
        gl_FragColor = texture2D(sTexture, vTexCoord);
      }
    """

    private fun floatBuffer(count: Int): FloatBuffer =
      ByteBuffer.allocateDirect(count * 4).order(ByteOrder.nativeOrder()).asFloatBuffer()

    private fun compile(type: Int, source: String): Int {
      val shader = GLES20.glCreateShader(type)
      GLES20.glShaderSource(shader, source)
      GLES20.glCompileShader(shader)
      val status = IntArray(1)
      GLES20.glGetShaderiv(shader, GLES20.GL_COMPILE_STATUS, status, 0)
      if (status[0] != GLES20.GL_TRUE) {
        val log = GLES20.glGetShaderInfoLog(shader)
        GLES20.glDeleteShader(shader)
        throw RuntimeException("GL shader compile failed: $log")
      }
      return shader
    }

    /** A texture name with linear filtering and clamped edges. */
    fun createTexture(target: Int): Int {
      val names = IntArray(1)
      GLES20.glGenTextures(1, names, 0)
      GLES20.glBindTexture(target, names[0])
      GLES20.glTexParameteri(target, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
      GLES20.glTexParameteri(target, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
      GLES20.glTexParameteri(target, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
      GLES20.glTexParameteri(target, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)
      GLES20.glBindTexture(target, 0)
      return names[0]
    }

    fun deleteTexture(name: Int) {
      if (name != 0) GLES20.glDeleteTextures(1, intArrayOf(name), 0)
    }
  }
}
