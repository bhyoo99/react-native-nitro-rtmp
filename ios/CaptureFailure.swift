import Foundation

/// Capture side failures. Promise rejections read
/// "<code>: <message>", the same shape as `PublisherFailure`, so the JS side
/// turns them back into a `CaptureError`. They are never mixed into
/// `PublisherError`: a denied camera has nothing to do with the session.
struct CaptureFailure: Error, CustomStringConvertible {
  let code: CaptureErrorCode
  let message: String
  var description: String { "\(code.stringValue): \(message)" }
  var captureError: CaptureError { CaptureError(code: code, message: message) }
}
