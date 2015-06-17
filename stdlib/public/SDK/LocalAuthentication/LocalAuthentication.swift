import Foundation

@available(OSX, introduced=10.10) @available(iOS, introduced=8.0)
extension LAError : _BridgedNSError {
  public static var _NSErrorDomain: String { return LAErrorDomain }
}
