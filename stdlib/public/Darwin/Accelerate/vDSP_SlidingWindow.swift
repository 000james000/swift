//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import Accelerate

extension vDSP {
    
    /// Vector sliding window sum; single-precision.
    ///
    /// - Parameter source: Single-precision input vector.
    /// - Parameter windowLength: The number of consecutive elements to sum.
    /// - Parameter result: Single-precision output vector.
    @inline(__always)
    @available(iOS 9999, OSX 9999, tvOS 9999, watchOS 9999, *)
    public static func slidingWindowSum<U, V>(_ vector: U,
                                              usingWindowLength windowLength: Int,
                                              result: inout V)
        where
        U: _ContiguousCollection,
        V: _MutableContiguousCollection,
        U.Element == Float,
        V.Element == Float {
            
            let n = result.count
            precondition(vector.count == n + windowLength - 1)
            
            result.withUnsafeMutableBufferPointer { dest in
                vector.withUnsafeBufferPointer { src in
                    vDSP_vswsum(src.baseAddress!, 1,
                                dest.baseAddress!, 1,
                                vDSP_Length(n),
                                vDSP_Length(windowLength))
                }
            }
            
    }
    
    /// Vector sliding window sum; double-precision.
    ///
    /// - Parameter source: Double-precision input vector.
    /// - Parameter windowLength: The number of consecutive elements to sum.
    /// - Parameter result: Double-precision output vector.
    @inline(__always)
    @available(iOS 9999, OSX 9999, tvOS 9999, watchOS 9999, *)
    public static func slidingWindowSum<U, V>(_ vector: U,
                                              usingWindowLength windowLength: Int,
                                              result: inout V)
        where
        U: _ContiguousCollection,
        V: _MutableContiguousCollection,
        U.Element == Double,
        V.Element == Double {
            
            let n = result.count
            precondition(vector.count == n + windowLength - 1)
            
            result.withUnsafeMutableBufferPointer { dest in
                vector.withUnsafeBufferPointer { src in
                    vDSP_vswsumD(src.baseAddress!, 1,
                                 dest.baseAddress!, 1,
                                 vDSP_Length(n),
                                 vDSP_Length(windowLength))
                }
            }
            
    }
}
