// RUN: %target-run-simple-swift
// REQUIRES: executable_test

// REQUIRES: objc_interop
// UNSUPPORTED: OS=watchos

import StdlibUnittest
import Accelerate

var AccelerateTests = TestSuite("Accelerate")

if #available(iOS 10.0, OSX 10.12, tvOS 10.0, watchOS 4.0, *) {
  
  AccelerateTests.test("BNNS/ImageStackDescriptor") {
    var succeed = BNNSImageStackDescriptor(width: 0, height: 0, channels: 0,
                                           row_stride: 0, image_stride: 0,
                                           data_type: .int8)
    expectEqual(succeed.data_scale, 1)
    expectEqual(succeed.data_bias, 0)
    succeed = BNNSImageStackDescriptor(width: 0, height: 0, channels: 0,
                                       row_stride: 0, image_stride: 0,
                                       data_type: .int16,
                                       data_scale: 0.5, data_bias: 0.5)
    expectEqual(succeed.data_scale, 0.5)
    expectEqual(succeed.data_bias, 0.5)
    expectCrashLater()
    //  indexed8 is not allowed as an imageStack data type.
    let _ = BNNSImageStackDescriptor(width: 0, height: 0, channels: 0,
                                     row_stride: 0, image_stride: 0,
                                     data_type: .indexed8)
  }
  
  AccelerateTests.test("BNNS/VectorDescriptor") {
    var succeed = BNNSVectorDescriptor(size: 0, data_type: .int8)
    expectEqual(succeed.data_scale, 1)
    expectEqual(succeed.data_bias, 0)
    succeed = BNNSVectorDescriptor(size: 0, data_type: .int8,
                                   data_scale: 0.5, data_bias: 0.5)
    expectEqual(succeed.data_scale, 0.5)
    expectEqual(succeed.data_bias, 0.5)
    expectCrashLater()
    //  indexed8 is not allowed as a vector data type.
    let _ = BNNSVectorDescriptor(size: 0, data_type: .indexed8)
  }
  
  AccelerateTests.test("BNNS/LayerData") {
    //  The zero layer should have data == nil.
    expectEqual(BNNSLayerData.zero.data, nil)
    var succeed = BNNSLayerData(data: nil, data_type: .int8)
    expectEqual(succeed.data_scale, 1)
    expectEqual(succeed.data_bias, 0)
    succeed = BNNSLayerData(data: nil, data_type: .int8, data_scale: 0.5,
                            data_bias: 0.5, data_table: nil)
    expectEqual(succeed.data_scale, 0.5)
    expectEqual(succeed.data_bias, 0.5)
    var table: [Float] = [1.0]
    succeed = BNNSLayerData.indexed8(data: nil, data_table: &table)
    expectCrashLater()
    // indexed8 requires a non-nil data table.
    let _ = BNNSLayerData(data: nil, data_type: .indexed8)
  }
  
  AccelerateTests.test("BNNS/Activation") {
    expectEqual(BNNSActivation.identity.function, .identity)
    let id = BNNSActivation(function: .identity)
    expectTrue(id.alpha.isNaN)
    expectTrue(id.beta.isNaN)
  }
  
}

//===----------------------------------------------------------------------===//
//
//  vDSP Single Precision Arithmetic
//
//===----------------------------------------------------------------------===//

if #available(iOS 9999, macOS 9999, tvOS 9999, watchOS 9999, *) {
    
    var result = [Float.nan]
    
    var vectorA = [Float.nan]
    var vectorB = [Float.nan]
    var vectorC = [Float.nan]
    
    var scalarA = Float.nan
    var scalarB = Float.nan
    
    // c[i] = a[i] + b
    AccelerateTests.test("vDSP/vsadd") {
        scalarA = 100
        vectorB = [500]
        
        vDSP.add(scalarA,
                 vectorB,
                 result: &result)
        
        expectEqual(Int(result.first!), 600)
    }
    
    // c[i] = a[i] + b[i]
    AccelerateTests.test("vDSP/vadd") {
        vectorA = [750]
        vectorB = [250]
        
        vDSP.add(vectorA,
                 vectorB,
                 result: &result)
        
        expectEqual(Int(result.first!), 1000)
    }
    
    // c[i] = a[i] - b[i]
    AccelerateTests.test("vDSP/vsub") {
        vectorA = [5]
        vectorB = [30]
        
        vDSP.subtract(vectorA,
                      vectorB,
                      result: &result)
        
        expectEqual(Int(result.first!), -25)
    }
    
    // c[i] = a[i] * b
    AccelerateTests.test("vDSP/vsmul") {
        scalarA = 100
        vectorB = [3]
        
        vDSP.multiply(scalarA,
                      vectorB,
                      result: &result)
        
        expectEqual(Int(result.first!), 300)
    }
    
    // a[i] * b[i]
    AccelerateTests.test("vDSP/vmul") {
        vectorA = [6]
        vectorB = [10]
        
        vDSP.multiply(vectorA,
                      vectorB,
                      result: &result)
        
        expectEqual(Int(result.first!), 60)
    }
    
    // c[i] = a[i] / b
    AccelerateTests.test("vDSP/vsdiv") {
        scalarA = 4
        vectorB = [100]
        
        vDSP.divide(vectorB,
                    scalarA,
                    result: &result)
        
        expectEqual(Int(result.first!), 25)
    }
    
    // c[i] = a / b[i]
    AccelerateTests.test("vDSP/svdiv") {
        scalarA = 200
        vectorB = [4]
        
        vDSP.divide(scalarA,
                    vectorB,
                    result: &result)
        
        expectEqual(Int(result.first!), 50)
    }
    
    // c[i] = a[i] / b[i]
    AccelerateTests.test("vDSP/vdiv") {
        vectorA = [600]
        vectorB = [3]
        
        vDSP.divide(vectorA,
                    vectorB,
                    result: &result)
        
        expectEqual(Int(result.first!), 200)
    }
    
    // o0[i] = i1[i] + i0[i]; o1[i] = i1[i] - i0[i]
    AccelerateTests.test("vDSP/vaddsub") {
        vectorA = [16]
        vectorB = [4]
        var subtractResult = [ Float.nan ]
        
        vDSP.addSubtract(vectorA,
                         vectorB,
                         addResult: &result,
                         subtractResult: &subtractResult)
        
        expectEqual(Int(result.first!), 20)
        expectEqual(Int(subtractResult.first!), 12)
        
    }
    
    // d[i] = (a[i] + b[i]) * c
    AccelerateTests.test("vDSP/vasm") {
        vectorA = [4]
        vectorB = [16]
        scalarA = 4
        
        vDSP.multiply(addition: (vectorA, vectorB),
                      scalarA,
                      result: &result)
        
        expectEqual(Int(result.first!), 80)
    }
    
    // d[i] = (a[i] + b[i]) * c[i]
    AccelerateTests.test("vDSP/vam") {
        vectorA = [4]
        vectorB = [16]
        vectorC = [5]
        
        vDSP.multiply(addition: (vectorA, vectorB),
                      vectorC,
                      result: &result)
        
        expectEqual(Int(result.first!), 100)
    }
    
    // d[i] = (a[i] - b[i]) * c
    AccelerateTests.test("vDSP/vsbsm") {
        vectorA = [20]
        vectorB = [5]
        scalarA = 2
        
        vDSP.multiply(subtraction: (vectorA, vectorB),
                      scalarA,
                      result: &result)
        
        expectEqual(Int(result.first!), 30)
        
    }
    
    // d[i] = (a[i] - b[i]) * c[i]
    AccelerateTests.test("vDSP/vsbm") {
        vectorA = [20]
        vectorB = [5]
        vectorC = [2]
        
        vDSP.multiply(subtraction: (vectorA, vectorB),
                      vectorC,
                      result: &result)
        
        expectEqual(Int(result.first!), 30)
    }
    
    // a[i]*b[i] + c
    AccelerateTests.test("vDSP/vmsa") {
        vectorA = [18]
        vectorB = [10]
        scalarA = 20
        
        vDSP.add(multiplication: (vectorA, vectorB),
                 scalarA,
                 result: &result)
        
        expectEqual(Int(result.first!), 200)
    }
    
    // (a[i] * b) + c[i]
    AccelerateTests.test("vDSP/vsma") {
        vectorA = [18]
        scalarB = 10
        vectorC = [120]
        
        vDSP.add(multiplication: (vectorA, scalarB),
                 vectorC,
                 result: &result)
        
        expectEqual(Int(result.first!), 300)
    }
    
    // d[i] = (a[i] * b[i]) + c[i]
    AccelerateTests.test("vDSP/vma") {
        vectorA = [5]
        vectorB = [20]
        vectorC = [6]
        
        vDSP.add(multiplication: (vectorA, vectorB),
                 vectorC,
                 result: &result)
        
        expectEqual(Int(result.first!), 106)
    }
    
    // d[i] = (a[i] * b[i]) - c[i]
    AccelerateTests.test("vDSP/vmsb") {
        vectorA = [25]
        vectorB = [2]
        vectorC = [50]
        
        vDSP.subtract(multiplication: (vectorB, vectorC),
                      vectorA,
                      result: &result)
        
        expectEqual(Int(result.first!), 75)
    }
    
    // e[i] = (a[i] * b) + (c[i] * d)
    AccelerateTests.test("vDSP/vsmsma") {
        vectorA = [5]
        scalarA = 100
        vectorB = [50]
        scalarB = 4
        
        vDSP.add(multiplication: (vectorA, scalarA),
                 multiplication: (vectorB, scalarB),
                 result: &result)
        
        expectEqual(Int(result.first!), 700)
    }
    
    // E[n] = A[n]*B[n] + C[n]*D[n]
    AccelerateTests.test("vDSP/vmma") {
        vectorA = [2]
        vectorB = [10]
        vectorC = [3]
        let vectorD: [Float] = [20]
        
        vDSP.add(multiplication: (vectorA, vectorB),
                 multiplication: (vectorC, vectorD),
                 result: &result)
        
        expectEqual(Int(result.first!), 80)
    }
    
    // e[i] = (a[i] + b[i]) * (c[i] + d[i])
    AccelerateTests.test("vDSP/vaam") {
        vectorA = [2]
        vectorB = [8]
        vectorC = [3]
        let vectorD: [Float] = [17]
        
        vDSP.multiply(addition: (vectorA, vectorB),
                      addition: (vectorC, vectorD),
                      result: &result)
    }
    
    // e[i] = (a[i] * b[i]) - (c[i] * d[i])
    AccelerateTests.test("vDSP/vmmsb") {
        vectorA = [2]
        vectorB = [3]
        vectorC = [5]
        let vectorD: [Float] = [15]
        
        vDSP.subtract(multiplication: (vectorA, vectorB),
                      multiplication: (vectorC, vectorD),
                      result: &result)
        
        expectEqual(Int(result.first!), -69)
    }
    
    // e[i] = (a[i] - b[i]) * (c[i] - d[i])
    AccelerateTests.test("vDSP/vsbsbm") {
        vectorA = [12]
        vectorB = [2]
        vectorC = [20]
        let vectorD: [Float] = [3]
        
        vDSP.multiply(subtraction: (vectorA, vectorB),
                      subtraction: (vectorC, vectorD),
                      result: &result)
        
        expectEqual(Int(result.first!), 170)
    }
    
    // e[i] = (a[i] + b[i]) * (c[i] - d[i])
    AccelerateTests.test("vDSP/vasbm") {
        vectorA = [16]
        vectorB = [4]
        vectorC = [102]
        let vectorD: [Float] = [2]
        
        vDSP.multiply(addition: (vectorA, vectorB),
                      subtraction: (vectorC, vectorD),
                      result: &result)
        
        expectEqual(Int(result.first!), 2000)
    }
    
    // d[n] = a[n]*b + c
    AccelerateTests.test("vDSP/vsmsa") {
        vectorA = [12]
        scalarA = 2
        scalarB = 6
        
        vDSP.add(multiplication: (vectorA, scalarA),
                 scalarB,
                 result: &result)
        
        expectEqual(Int(result.first!), 30)
    }
    
    // D[n] = A[n]*B - C[n];
    AccelerateTests.test("vDSP/vsmsb") {
        vectorA = [10]
        scalarB = 5
        vectorC = [25]
        
        vDSP.subtract(multiplication: (vectorA, scalarB),
                      vectorC,
                      result: &result)
        
        expectEqual(Int(result.first!), 25)
    }
}


//===----------------------------------------------------------------------===//
//
//  vDSP Double Precision Arithmetic
//
//===----------------------------------------------------------------------===//

if #available(iOS 9999, macOS 9999, tvOS 9999, watchOS 9999, *) {
    
    var result = [Double.nan]
    
    var vectorA = [Double.nan]
    var vectorB = [Double.nan]
    var vectorC = [Double.nan]
    
    var scalarA = Double.nan
    var scalarB = Double.nan
    
    // c[i] = a[i] + b
    AccelerateTests.test("vDSP/vsaddD") {
        scalarA = 100
        vectorB = [500]
        
        vDSP.add(scalarA,
                 vectorB,
                 result: &result)
        
        expectEqual(Int(result.first!), 600)
    }
    
    // c[i] = a[i] + b[i]
    AccelerateTests.test("vDSP/vaddD") {
        vectorA = [750]
        vectorB = [250]
        
        vDSP.add(vectorA,
                 vectorB,
                 result: &result)
        
        expectEqual(Int(result.first!), 1000)
    }
    
    // c[i] = a[i] - b[i]
    AccelerateTests.test("vDSP/vsubD") {
        vectorA = [5]
        vectorB = [30]
        
        vDSP.subtract(vectorA,
                      vectorB,
                      result: &result)
        
        expectEqual(Int(result.first!), -25)
    }
    
    // c[i] = a[i] * b
    AccelerateTests.test("vDSP/vsmulD") {
        scalarA = 100
        vectorB = [3]
        
        vDSP.multiply(scalarA,
                      vectorB,
                      result: &result)
        
        expectEqual(Int(result.first!), 300)
    }
    
    // a[i] * b[i]
    AccelerateTests.test("vDSP/vmulD") {
        vectorA = [6]
        vectorB = [10]
        
        vDSP.multiply(vectorA,
                      vectorB,
                      result: &result)
        
        expectEqual(Int(result.first!), 60)
    }
    
    // c[i] = a[i] / b
    AccelerateTests.test("vDSP/vsdivD") {
        scalarA = 4
        vectorB = [100]
        
        vDSP.divide(vectorB,
                    scalarA,
                    result: &result)
        
        expectEqual(Int(result.first!), 25)
    }
    
    // c[i] = a / b[i]
    AccelerateTests.test("vDSP/svdivD") {
        scalarA = 200
        vectorB = [4]
        
        vDSP.divide(scalarA,
                    vectorB,
                    result: &result)
        
        expectEqual(Int(result.first!), 50)
    }
    
    // c[i] = a[i] / b[i]
    AccelerateTests.test("vDSP/vdivD") {
        vectorA = [600]
        vectorB = [3]
        
        vDSP.divide(vectorA,
                    vectorB,
                    result: &result)
        
        expectEqual(Int(result.first!), 200)
    }
    
    // o0[i] = i1[i] + i0[i]; o1[i] = i1[i] - i0[i]
    AccelerateTests.test("vDSP/vaddsubD") {
        vectorA = [16]
        vectorB = [4]
        var subtractResult = [ Double.nan ]
        
        vDSP.addSubtract(vectorA,
                         vectorB,
                         addResult: &result,
                         subtractResult: &subtractResult)
        
        expectEqual(Int(result.first!), 20)
        expectEqual(Int(subtractResult.first!), 12)
    }
    
    // d[i] = (a[i] + b[i]) * c
    AccelerateTests.test("vDSP/vasmD") {
        vectorA = [4]
        vectorB = [16]
        scalarA = 4
        
        vDSP.multiply(addition: (vectorA, vectorB),
                      scalarA,
                      result: &result)
        
        expectEqual(Int(result.first!), 80)
    }
    
    // d[i] = (a[i] + b[i]) * c[i]
    AccelerateTests.test("vDSP/vamD") {
        vectorA = [4]
        vectorB = [16]
        vectorC = [5]
        
        vDSP.multiply(addition: (vectorA, vectorB),
                      vectorC,
                      result: &result)
        
        expectEqual(Int(result.first!), 100)
    }
    
    // d[i] = (a[i] - b[i]) * c
    AccelerateTests.test("vDSP/vsbsmD") {
        vectorA = [20]
        vectorB = [5]
        scalarA = 2
        
        vDSP.multiply(subtraction: (vectorA, vectorB),
                      scalarA,
                      result: &result)
        
        expectEqual(Int(result.first!), 30)
        
    }
    
    // d[i] = (a[i] - b[i]) * c[i]
    AccelerateTests.test("vDSP/vsbmD") {
        vectorA = [20]
        vectorB = [5]
        vectorC = [2]
        
        vDSP.multiply(subtraction: (vectorA, vectorB),
                      vectorC,
                      result: &result)
        
        expectEqual(Int(result.first!), 30)
    }
    
    // a[i]*b[i] + c
    AccelerateTests.test("vDSP/vmsaD") {
        vectorA = [18]
        vectorB = [10]
        scalarA = 20
        
        vDSP.add(multiplication: (vectorA, vectorB),
                 scalarA,
                 result: &result)
        
        expectEqual(Int(result.first!), 200)
    }
    
    // (a[i] * b) + c[i]
    AccelerateTests.test("vDSP/vsmaD") {
        vectorA = [18]
        scalarB = 10
        vectorC = [120]
        
        vDSP.add(multiplication: (vectorA, scalarB),
                 vectorC,
                 result: &result)
        
        expectEqual(Int(result.first!), 300)
    }
    
    // d[i] = (a[i] * b[i]) + c[i]
    AccelerateTests.test("vDSP/vmaD") {
        vectorA = [5]
        vectorB = [20]
        vectorC = [6]
        
        vDSP.add(multiplication: (vectorA, vectorB),
                 vectorC,
                 result: &result)
        
        expectEqual(Int(result.first!), 106)
    }
    
    // d[i] = (a[i] * b[i]) - c[i]
    AccelerateTests.test("vDSP/vmsbD") {
        vectorA = [25]
        vectorB = [2]
        vectorC = [50]
        
        vDSP.subtract(multiplication: (vectorB, vectorC),
                      vectorA,
                      result: &result)
        
        expectEqual(Int(result.first!), 75)
    }
    
    // e[i] = (a[i] * b) + (c[i] * d)
    AccelerateTests.test("vDSP/vsmsmaD") {
        vectorA = [5]
        scalarA = 100
        vectorB = [50]
        scalarB = 4
        
        vDSP.add(multiplication: (vectorA, scalarA),
                 multiplication: (vectorB, scalarB),
                 result: &result)
        
        expectEqual(Int(result.first!), 700)
    }
    
    // E[n] = A[n]*B[n] + C[n]*D[n]
    AccelerateTests.test("vDSP/vmmaD") {
        vectorA = [2]
        vectorB = [10]
        vectorC = [3]
        let vectorD: [Double] = [20]
        
        vDSP.add(multiplication: (vectorA, vectorB),
                 multiplication: (vectorC, vectorD),
                 result: &result)
        
        expectEqual(Int(result.first!), 80)
    }
    
    // e[i] = (a[i] + b[i]) * (c[i] + d[i])
    AccelerateTests.test("vDSP/vaamD") {
        vectorA = [2]
        vectorB = [8]
        vectorC = [3]
        let vectorD: [Double] = [17]
        
        vDSP.multiply(addition: (vectorA, vectorB),
                      addition: (vectorC, vectorD),
                      result: &result)
    }
    
    // e[i] = (a[i] * b[i]) - (c[i] * d[i])
    AccelerateTests.test("vDSP/vmmsbD") {
        vectorA = [2]
        vectorB = [3]
        vectorC = [5]
        let vectorD: [Double] = [15]
        
        vDSP.subtract(multiplication: (vectorA, vectorB),
                      multiplication: (vectorC, vectorD),
                      result: &result)
        
        expectEqual(Int(result.first!), -69)
    }
    
    // e[i] = (a[i] - b[i]) * (c[i] - d[i])
    AccelerateTests.test("vDSP/vsbsbmD") {
        vectorA = [12]
        vectorB = [2]
        vectorC = [20]
        let vectorD: [Double] = [3]
        
        vDSP.multiply(subtraction: (vectorA, vectorB),
                      subtraction: (vectorC, vectorD),
                      result: &result)
        
        expectEqual(Int(result.first!), 170)
    }
    
    // e[i] = (a[i] + b[i]) * (c[i] - d[i])
    AccelerateTests.test("vDSP/vasbmD") {
        vectorA = [16]
        vectorB = [4]
        vectorC = [102]
        let vectorD: [Double] = [2]
        
        vDSP.multiply(addition: (vectorA, vectorB),
                      subtraction: (vectorC, vectorD),
                      result: &result)
        
        expectEqual(Int(result.first!), 2000)
    }
    
    // d[n] = a[n]*b + c
    AccelerateTests.test("vDSP/vsmsaD") {
        vectorA = [12]
        scalarA = 2
        scalarB = 6
        
        vDSP.add(multiplication: (vectorA, scalarA),
                 scalarB,
                 result: &result)
        
        expectEqual(Int(result.first!), 30)
    }
    
    // D[n] = A[n]*B - C[n];
    AccelerateTests.test("vDSP/vsmsbD") {
        vectorA = [10]
        scalarB = 5
        vectorC = [25]
        
        vDSP.subtract(multiplication: (vectorA, scalarB),
                      vectorC,
                      result: &result)
        
        expectEqual(Int(result.first!), 25)
    }
}


runAllTests()
