import Foundation
import Network

/// One TCP (+TLS) connection for one publish session.
///
/// A thin wrapper over `NWConnection`: every callback runs on the session
/// queue the connection was started on, every failure is mapped to one of the
/// four transport error codes, and `queuedBytes` counts what `send` has been
/// handed but the socket has not written yet. No policy lives here: nothing is
/// dropped and nothing reconnects.
final class RtmpTransport {
  enum Failure {
    case connectFailed(String)  // DNS or TCP failure
    case tlsFailed(String)      // TLS handshake or certificate validation
    case socketClosed(String)   // the peer closed the socket (EOF/RST) or a write failed
    case timeout(String)        // the TCP stack gave up
  }

  private let queue: DispatchQueue
  private let connection: NWConnection
  private var isCancelled = false

  /// True once the socket (and TLS) is up; failures before that are connect failures.
  private(set) var isReady = false
  /// Bytes handed to `send` whose completion has not fired yet.
  private(set) var queuedBytes = 0
  /// The last `.waiting` reason, for the session timeout message.
  private(set) var lastWaitReason: String?

  var onReady: (() -> Void)?
  var onReceive: ((Data) -> Void)?
  var onFailure: ((Failure) -> Void)?
  var onQueuedBytesChange: ((Int) -> Void)?

  init(host: String, port: UInt16, useTls: Bool, queue: DispatchQueue) {
    self.queue = queue
    let tcp = NWProtocolTCP.Options()
    tcp.noDelay = true
    let parameters: NWParameters
    if useTls {
      // System trust store only, SNI = the URL host.
      let tls = NWProtocolTLS.Options()
      sec_protocol_options_set_tls_server_name(tls.securityProtocolOptions, host)
      parameters = NWParameters(tls: tls, tcp: tcp)
    } else {
      parameters = NWParameters(tls: nil, tcp: tcp)
    }
    connection = NWConnection(
      host: NWEndpoint.Host(host),
      port: NWEndpoint.Port(integerLiteral: port),
      using: parameters)
  }

  func start() {
    connection.stateUpdateHandler = { [weak self] state in self?.handle(state) }
    connection.start(queue: queue)
  }

  /// Queues bytes for the socket in order. Safe to call before `.ready`.
  func send(_ data: Data) {
    guard !isCancelled, !data.isEmpty else { return }
    queuedBytes += data.count
    onQueuedBytesChange?(queuedBytes)
    connection.send(content: data, completion: .contentProcessed { [weak self] error in
      guard let self else { return }
      self.queuedBytes -= data.count
      self.onQueuedBytesChange?(self.queuedBytes)
      if let error, !self.isCancelled {
        self.fail(self.failure(for: error))
      }
    })
  }

  /// Delivers everything queued so far, then closes the socket. `completion`
  /// runs once on the queue, after at most `timeout` seconds.
  func finish(timeout: TimeInterval, completion: @escaping () -> Void) {
    guard !isCancelled, isReady else {
      cancel()
      completion()
      return
    }
    var finished = false
    let finishOnce = { [weak self] in
      guard !finished else { return }
      finished = true
      self?.cancel()
      completion()
    }
    let deadline = DispatchWorkItem { finishOnce() }
    queue.asyncAfter(deadline: .now() + timeout, execute: deadline)
    // An empty final message completes after every send queued before it.
    connection.send(
      content: nil, contentContext: .finalMessage, isComplete: true,
      completion: .contentProcessed { _ in
        deadline.cancel()
        finishOnce()
      })
  }

  /// Closes the socket now. No callback fires after this returns.
  func cancel() {
    guard !isCancelled else { return }
    isCancelled = true
    connection.stateUpdateHandler = nil
    connection.cancel()
  }

  // MARK: - NWConnection

  private func handle(_ state: NWConnection.State) {
    guard !isCancelled else { return }
    switch state {
    case .ready:
      isReady = true
      receiveNext()
      onReady?()
    case .waiting(let error):
      // NWConnection reports a refused connection and a failed TLS handshake
      // as `.waiting` and retries forever. Both are final for a publisher (a
      // closed port stays closed, a plaintext server never speaks TLS), so
      // they fail at once; every other wait (no network, DNS retry) is ended
      // by the session's timeout, which quotes the reason kept here.
      switch error {
      case .posix(let code) where code == .ECONNREFUSED:
        fail(.connectFailed(describe(error)))
      case .tls:
        fail(.tlsFailed(describe(error)))
      default:
        lastWaitReason = describe(error)
      }
    case .failed(let error):
      fail(failure(for: error))
    case .setup, .preparing, .cancelled:
      break
    @unknown default:
      break
    }
  }

  private func receiveNext() {
    connection.receive(minimumIncompleteLength: 1, maximumLength: 65536) { [weak self] data, _, isComplete, error in
      guard let self, !self.isCancelled else { return }
      if let data, !data.isEmpty {
        self.onReceive?(data)
        if self.isCancelled { return }  // the session ended while handling these bytes
      }
      if let error {
        self.fail(self.failure(for: error))
        return
      }
      if isComplete {
        self.fail(.socketClosed("the server closed the connection"))
        return
      }
      self.receiveNext()
    }
  }

  private func fail(_ failure: Failure) {
    guard !isCancelled else { return }
    cancel()
    onFailure?(failure)
  }

  private func failure(for error: NWError) -> Failure {
    let text = describe(error)
    switch error {
    case .tls:
      return .tlsFailed(text)
    case .dns:
      return .connectFailed(text)
    case .posix(let code):
      if code == .ETIMEDOUT { return .timeout(text) }
      return isReady ? .socketClosed(text) : .connectFailed(text)
    default:
      return isReady ? .socketClosed(text) : .connectFailed(text)
    }
  }

  private func describe(_ error: NWError) -> String {
    switch error {
    case .posix(let code):
      return "\(String(cString: strerror(code.rawValue))) (POSIX \(code.rawValue))"
    case .dns(let code):
      return "DNS error \(code)"
    case .tls(let status):
      return "TLS error \(status)"
    default:
      return String(describing: error)
    }
  }
}
