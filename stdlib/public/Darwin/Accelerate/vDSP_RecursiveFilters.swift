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
    
    /// Performs two-pole two-zero recursive filtering; single-precision.
    ///
    /// - Parameter source: single-precision input vector.
    /// - Parameter coefficients: Filter coefficients.
    /// - Parameter result: single-precision output vector.
    ///
    /// This function performs the following calculation:
    ///
    ///                for (n = 2; n < N+2; ++n)
    ///                    C[n] =
    ///                        + A[n-0]*B[0]
    ///                        + A[n-1]*B[1]
    ///                        + A[n-2]*B[2]
    ///                        - C[n-1]*B[3]
    ///                        - C[n-2]*B[4];
    ///
    /// Where `A` is the input vector, `B` is the filter coefficients, and `C`
    /// is the output vector. Note that outputs start with C[2].
    @inline(__always)
    @available(iOS 9999, OSX 9999, tvOS 9999, watchOS 9999, *)
    public static func twoPoleTwoZeroFilter<U, V>(_ source: U,
                                                  coefficients: (Float, Float, Float, Float, Float),
                                                  result: inout V)
        where
        U: _ContiguousCollection,
        V: _MutableContiguousCollection,
        U.Element == Float,
        V.Element == Float {
            
            precondition(source.count == result.count)
            
            let n = vDSP_Length(source.count - 2)
            
            result.withUnsafeMutableBufferPointer { dest in
                source.withUnsafeBufferPointer { src in
                    vDSP_deq22(src.baseAddress!, 1,
                               [coefficients.0, coefficients.1,
                                coefficients.2, coefficients.3,
                                coefficients.4],
                               dest.baseAddress!, 1,
                               n)
                }
            }
    }
    
    /// Performs two-pole two-zero recursive filtering; double-precision.
    ///
    /// - Parameter source: single-precision input vector.
    /// - Parameter coefficients: Filter coefficients.
    /// - Parameter result: single-precision output vector.
    ///
    /// This function performs the following calculation:
    ///
    ///                for (n = 2; n < N+2; ++n)
    ///                    C[n] =
    ///                        + A[n-0]*B[0]
    ///                        + A[n-1]*B[1]
    ///                        + A[n-2]*B[2]
    ///                        - C[n-1]*B[3]
    ///                        - C[n-2]*B[4];
    ///
    /// Where `A` is the input vector, `B` is the filter coefficients, and `C`
    /// is the output vector. Note that outputs start with C[2].
    @inline(__always)
    @available(iOS 9999, OSX 9999, tvOS 9999, watchOS 9999, *)
    public static func twoPoleTwoZeroFilter<U, V>(_ source: U,
                                                  coefficients: (Double, Double, Double, Double, Double),
                                                  result: inout V)
        where
        U: _ContiguousCollection,
        V: _MutableContiguousCollection,
        U.Element == Double,
        V.Element == Double {
            
            precondition(source.count == result.count)
            
            let n = vDSP_Length(source.count - 2)
            
            result.withUnsafeMutableBufferPointer { dest in
                source.withUnsafeBufferPointer { src in
                    vDSP_deq22D(src.baseAddress!, 1,
                                [coefficients.0, coefficients.1,
                                 coefficients.2, coefficients.3,
                                 coefficients.4],
                                dest.baseAddress!, 1,
                                n)
                }
            }
    }
    
}
