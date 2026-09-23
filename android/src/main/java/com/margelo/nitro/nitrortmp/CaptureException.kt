package com.margelo.nitro.nitrortmp

/**
 * Capture side failures: what `MicrophoneSource.start()` and
 * `ImageLayer.load()` reject with. fbjni turns
 * a Throwable into the JS Error message with `toString()`, so the message
 * reads "<code>: <message>" and the JS side turns it back into a
 * `CaptureError`. Never mixed into `PublisherError`.
 */
internal class CaptureException(val code: CaptureErrorCode, val detail: String) :
  Exception("${codeName(code)}: $detail") {
  override fun toString(): String = message ?: ""

  // fbjni appends the Java stack trace to the JS message; there is nothing to
  // debug in it, and the message must stay one line ("<code>: <message>").
  override fun fillInStackTrace(): Throwable = this

  fun toError(): CaptureError = CaptureError(code, detail)

  companion object {
    /** The JS `CaptureErrorCode` union strings, in enum order. */
    fun codeName(code: CaptureErrorCode): String = when (code) {
      CaptureErrorCode.PERMISSIONDENIED -> "permissionDenied"
      CaptureErrorCode.CAMERAUNAVAILABLE -> "cameraUnavailable"
      CaptureErrorCode.CONFIGURATIONFAILED -> "configurationFailed"
      CaptureErrorCode.ENCODERFAILED -> "encoderFailed"
    }
  }
}
