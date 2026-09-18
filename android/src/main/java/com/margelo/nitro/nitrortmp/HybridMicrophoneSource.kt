package com.margelo.nitro.nitrortmp

import android.Manifest
import android.content.pm.PackageManager
import androidx.annotation.Keep
import com.facebook.proguard.annotations.DoNotStrip
import com.margelo.nitro.NitroModules
import com.margelo.nitro.core.Promise
import com.margelo.nitro.nitrortmp.capture.MicrophoneSource

/**
 * The `MicrophoneSource` HybridObject: AudioRecord
 * PCM delivered to the attached mixer, which encodes it synchronously on
 * the capture thread. Permission is checked, not requested.
 */
@DoNotStrip
@Keep
class HybridMicrophoneSource : HybridMicrophoneSourceSpec() {
  private val context = NitroModules.applicationContext
    ?: throw IllegalStateException("NitroModules has no application context yet")
  @Volatile private var mixer: HybridMixer? = null
  private val source = MicrophoneSource(object : MicrophoneSource.Listener {
    override fun onPcm(pcm: ShortArray, frames: Int, sampleRate: Int, channels: Int, timestampNs: Long) {
      mixer?.audioArrived(pcm, frames, sampleRate, channels, timestampNs)
    }
  })

  override var muted: Boolean
    get() = source.muted
    set(value) {
      source.muted = value
    }

  override val isRunning: Boolean
    get() = source.isRunning

  override fun start(): Promise<Unit> {
    val promise = Promise<Unit>()
    if (context.checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
      promise.reject(
        CaptureException(CaptureErrorCode.PERMISSIONDENIED, "android.permission.RECORD_AUDIO is not granted; the app must request it before start()"),
      )
      return promise
    }
    try {
      source.start()
      promise.resolve(Unit)
    } catch (e: IllegalStateException) {
      promise.reject(CaptureException(CaptureErrorCode.CONFIGURATIONFAILED, e.message ?: "AudioRecord failed"))
    }
    return promise
  }

  override fun stop(): Promise<Unit> {
    source.stop()
    return Promise.resolved(Unit)
  }

  override fun dispose() {
    source.stop()
    mixer = null
    super.dispose()
  }

  internal fun attach(mixer: HybridMixer) {
    this.mixer = mixer
  }

  internal fun detach() {
    mixer = null
  }
}
