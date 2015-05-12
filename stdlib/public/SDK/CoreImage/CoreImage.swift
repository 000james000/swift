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
@exported import CoreImage  // Clang module

extension CIFilter {
  @availability(iOS, introduced=8.0)
  @availability(OSX, introduced=10.10)
  convenience init?(
    name: String!, elements: (NSCopying, AnyObject)...
  ) {
    let dict =  NSMutableDictionary()
        for (key, value) in elements {
            dict[key] = value
        }
    self.init(name: name, withInputParameters: dict as [NSObject : AnyObject])
  } 
}
