@exported import GameCenter
import Foundation

extension GKErrorCode : _BridgedNSError {
  public static var _NSErrorDomain: String { return GKErrorDomain }
}
