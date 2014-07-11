//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import Foundation
@exported import AppKit

class REPLApplication : NSApplication {
}

/// Initializes and runs a REPLApplication on the main thread asynchronously.
internal func replApplicationMain() {
  _precondition(NSApp === nil)
  // Create a REPLApplication as the NSApp.
  let app = REPLApplication.sharedApplication() as REPLApplication

  // Set the activation policy so we get a dock icon and can go foreground.
  app.setActivationPolicy(.Regular)

  // Run asynchronously.
  NSOperationQueue.mainQueue().addOperationWithBlock { app.run() }

  // Quit the NSApplication when the REPL quits.
  _atREPLExit({ app.terminate(nil) })

}

struct _NSViewMirror : Mirror {
  static var _views = NSMutableSet()

  var _v : NSView
  
  init(_ v : NSView) {_v = v}
  
  var value: Any { get { return _v } }
  
  var valueType: Any.Type { get { return (_v as Any).dynamicType } }
  
  var objectIdentifier: ObjectIdentifier? { get { return .None } }
  
  var count: Int { get { return 0 } }
  
  subscript(_: Int) -> (String,Mirror) { get { _fatalError("Mirror access out of bounds") } }
  
  var summary: String { get { return ""} }
  
  var quickLookObject: QuickLookObject? { get {
      // adapted from the Xcode QuickLooks implementation
      
      var result: QuickLookObject? = nil
      
      // if you set NSView.needsDisplay, you can get yourself in a recursive scenario where the same view
      // could need to draw itself in order to get a QLObject for itself, which in turn if your code was
      // instrumented to log on-draw, would cause yourself to get back here and so on and so forth
      // until you run out of stack and crash
      // This code checks that we aren't trying to log the same view recursively - and if so just returns
      // nil, which is probably a safer option than crashing
      // FIXME: is there a way to say "cacheDisplayInRect butDoNotRedrawEvenIfISaidSo"?
      switch _NSViewMirror._views.member(_v) {
        case nil:
          _NSViewMirror._views.addObject(_v)

          let bounds = _v.bounds
          if var b = _v.bitmapImageRepForCachingDisplayInRect(bounds) {
              _v.cacheDisplayInRect(bounds, toBitmapImageRep: b)
              result = .Some(.View(b))
          }
        default: ()
      }
      
      _NSViewMirror._views.removeObject(_v)
      
      return result
      
  } }
  
  var disposition : MirrorDisposition { get { return .Aggregate } }
}

extension NSView : Reflectable {
  public func getMirror() -> Mirror {
    return _NSViewMirror(self)
  }
}

// Overlays for variadics.

public extension NSGradient {
  convenience init(colorsAndLocations objects: (NSColor, CGFloat)...) {
    self.init(
      colors: objects.map { $0.0 },
      atLocations: objects.map { $0.1 },
      colorSpace: NSColorSpace.genericRGBColorSpace())
  }
}

// Fix the ARGV type of NSApplicationMain, which nonsensically takes
// argv as a const char**.
@asmname("NSApplicationMain")
public func NSApplicationMain(argc: Int32,
                              argv: UnsafePointer<UnsafePointer<CChar>>)
  -> Int32
