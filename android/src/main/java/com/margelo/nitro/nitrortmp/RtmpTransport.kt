package com.margelo.nitro.nitrortmp

import android.os.Handler
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.net.SocketTimeoutException
import java.security.cert.CertificateException
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.atomic.AtomicLong
import javax.net.ssl.SNIHostName
import javax.net.ssl.SSLException
import javax.net.ssl.SSLSocket
import javax.net.ssl.SSLSocketFactory

/**
 * One TCP (+TLS) socket for one publish session.
 *
 * Three threads: the read thread connects (and does the TLS handshake), then
 * blocks in `read`; the write thread drains [writeQueue] so a blocking write
 * never stalls the core; the session thread owns the core and receives every
 * callback through [handler]. Nothing is dropped and nothing reconnects;
 * [queuedBytes] only counts what the socket has not written yet.
 */
internal class RtmpTransport(
  private val host: String,
  private val port: Int,
  private val useTls: Boolean,
  private val handler: Handler,
  private val listener: Listener,
) {
  enum class FailureKind { CONNECT_FAILED, TLS_FAILED, SOCKET_CLOSED, TIMEOUT }

  /** All methods run on the session thread. */
  interface Listener {
    fun onReady(transport: RtmpTransport)
    fun onReceive(transport: RtmpTransport, data: ByteArray, length: Int)
    fun onFailure(transport: RtmpTransport, kind: FailureKind, message: String)
  }

  /** Bytes handed to [send] that the write thread has not written yet. */
  val queuedBytes = AtomicLong(0)

  @Volatile private var closed = false
  @Volatile private var ready = false
  private val lock = Any()
  private var socket: Socket? = null
  private val writeQueue = LinkedBlockingQueue<ByteArray>()
  private val finishMarker = ByteArray(0)
  @Volatile private var onDrained: (() -> Unit)? = null

  fun start() {
    Thread({ runReader() }, "nitrortmp.read").start()
  }

  /** Session thread. [data] is owned by the transport from here on. */
  fun send(data: ByteArray) {
    if (closed || data.isEmpty()) return
    queuedBytes.addAndGet(data.size.toLong())
    writeQueue.put(data)
  }

  /**
   * Writes everything queued so far, then closes. [onClosed] runs once on the
   * session thread, after at most [timeoutMs].
   */
  fun finish(timeoutMs: Long, onClosed: () -> Unit) {
    if (closed || !ready) {
      close()
      onClosed()
      return
    }
    var done = false
    val complete = Runnable {
      if (!done) {
        done = true
        close()
        onClosed()
      }
    }
    handler.postDelayed(complete, timeoutMs)
    onDrained = {
      handler.removeCallbacks(complete)
      handler.post(complete)
    }
    writeQueue.put(finishMarker)
  }

  /** Closes the socket now; no listener call is made after this. */
  fun close() {
    synchronized(lock) {
      if (closed) return
      closed = true
    }
    writeQueue.offer(finishMarker)  // wake the write thread
    try {
      socket?.close()  // unblocks connect/read with a SocketException
    } catch (_: IOException) {
    }
  }

  // --- read thread -------------------------------------------------------------

  private fun runReader() {
    val plain = Socket()
    synchronized(lock) {
      if (closed) return
      socket = plain
    }
    val connected: Socket
    try {
      plain.tcpNoDelay = true
      plain.connect(InetSocketAddress(host, port), CONNECT_TIMEOUT_MS)
      connected = if (useTls) wrapTls(plain) else plain
    } catch (e: Exception) {
      if (!closed) fail(classifyConnectFailure(e), describe(e))
      return
    }
    synchronized(lock) {
      if (closed) return
      socket = connected
      ready = true
    }
    // close() can race with connection setup. Acquire both streams inside the
    // error boundary, before launching either I/O loop or announcing readiness.
    val input: InputStream
    val output: OutputStream
    try {
      input = connected.getInputStream()
      output = connected.getOutputStream()
    } catch (e: IOException) {
      if (!closed) fail(FailureKind.SOCKET_CLOSED, describe(e))
      return
    }
    Thread({ runWriter(output) }, "nitrortmp.write").start()
    handler.post { if (!closed) listener.onReady(this) }

    val buffer = ByteArray(64 * 1024)
    while (!closed) {
      val n = try {
        input.read(buffer)
      } catch (e: IOException) {
        if (!closed) fail(FailureKind.SOCKET_CLOSED, describe(e))
        return
      }
      if (n < 0) {
        if (!closed) fail(FailureKind.SOCKET_CLOSED, "the server closed the connection")
        return
      }
      if (n > 0) {
        val copy = buffer.copyOf(n)
        handler.post { if (!closed) listener.onReceive(this, copy, n) }
      }
    }
  }

  private fun wrapTls(plain: Socket): SSLSocket {
    // System trust store only. SSLSocket does not verify the host
    // name unless endpointIdentificationAlgorithm is set.
    val factory = SSLSocketFactory.getDefault() as SSLSocketFactory
    val ssl = factory.createSocket(plain, host, port, true) as SSLSocket
    val parameters = ssl.sslParameters
    parameters.endpointIdentificationAlgorithm = "HTTPS"
    try {
      parameters.serverNames = listOf(SNIHostName(host))  // SNI is the URL host, never the vhost
    } catch (_: IllegalArgumentException) {
      // IP literals carry no SNI.
    }
    ssl.sslParameters = parameters
    ssl.startHandshake()
    return ssl
  }

  private fun classifyConnectFailure(e: Exception): FailureKind {
    if (e is SocketTimeoutException) return FailureKind.TIMEOUT
    var cause: Throwable? = e
    while (cause != null) {
      if (cause is SSLException || cause is CertificateException) return FailureKind.TLS_FAILED
      cause = cause.cause
    }
    return FailureKind.CONNECT_FAILED
  }

  // --- write thread ------------------------------------------------------------

  private fun runWriter(output: OutputStream) {
    try {
      while (true) {
        val chunk = writeQueue.take()
        if (chunk === finishMarker) {
          if (closed) return
          output.flush()
          onDrained?.invoke()
          return
        }
        output.write(chunk)
        queuedBytes.addAndGet(-chunk.size.toLong())
      }
    } catch (e: IOException) {
      if (!closed) fail(FailureKind.SOCKET_CLOSED, "write failed: ${describe(e)}")
    } catch (_: InterruptedException) {
    }
  }

  // --- helpers --------------------------------------------------------------------

  private fun fail(kind: FailureKind, message: String) {
    synchronized(lock) {
      if (closed) return
    }
    close()
    handler.post { listener.onFailure(this, kind, message) }
  }

  private fun describe(e: Throwable): String {
    val name = e.javaClass.simpleName
    val message = e.message
    return if (message.isNullOrEmpty()) name else "$name: $message"
  }

  companion object {
    /** DNS + TCP + TLS; the session's own start() deadline is the same length. */
    const val CONNECT_TIMEOUT_MS = 10_000
  }
}
