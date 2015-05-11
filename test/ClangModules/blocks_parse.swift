// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -parse -verify %s

// REQUIRES: objc_interop

import blocks
import Foundation

var someNSString : NSString
func useString(s: String) {}

dispatch_async(dispatch_get_current_queue()) { }
someNSString.enumerateLinesUsingBlock {(s:String?) in }
someNSString.enumerateLinesUsingBlock {s in }
someNSString.enumerateLinesUsingBlock({ useString($0) })

dispatch_async(dispatch_get_current_queue(), /*not a block=*/()) // expected-error{{cannot invoke 'dispatch_async' with an argument list of type '(dispatch_queue_t, ())'}} expected-note {{expected an argument list of type '(dispatch_queue_t, dispatch_block_t!)'}}

func testNoEscape(@noescape f: @convention(block) () -> Void, nsStr: NSString,
                  @noescape fStr: (String!) -> Void) {
  dispatch_async(dispatch_get_current_queue(), f) // expected-error{{invalid use of non-escaping function in escaping context '@convention(block) () -> Void'}}
  dispatch_sync(dispatch_get_current_queue(), f) // okay: dispatch_sync is noescape

  // rdar://problem/19818617
  nsStr.enumerateLinesUsingBlock(fStr) // okay due to @noescape

  var _: Int = nsStr.enumerateLinesUsingBlock // expected-error{{'(@noescape (String!) -> Void) -> Void' is not convertible}}
}
