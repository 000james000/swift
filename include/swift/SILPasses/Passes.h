//===-------- Passes.h - Swift Compiler SIL Pass Entrypoints ----*- C++ -*-===//
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
//
//  This file declares the main entrypoints to SIL passes.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILPASSES_PASSES_H
#define SWIFT_SILPASSES_PASSES_H

#include "swift/SIL/SILModule.h"

namespace swift {
  class SILOptions;
  class SILTransform;

  /// \brief Run all the SIL diagnostic passes on \p M.
  ///
  /// \returns true if the diagnostic passes produced an error
  bool runSILDiagnosticPasses(SILModule &M, const SILOptions &Options);

  /// \brief Run all the SIL performance optimization passes on \p M.
  void runSILOptimizationPasses(SILModule &M, const SILOptions &Options);

  /// \brief Detect and remove unreachable code. Diagnose provably unreachable
  /// user code.
  void performSILDiagnoseUnreachable(SILModule *M);

  /// \brief Link a SILFunction declaration to the actual definition in the
  /// serialized modules.
  ///
  /// \param M the SILModule on which to operate
  /// \param LinkAll when true, always link. For testing purposes.
  void performSILLinking(SILModule *M, bool LinkAll = false);

  /// \brief Cleanup instructions/builtin calls not suitable for IRGen.
  void performSILCleanup(SILModule *M);

  /// \brief Eliminate unused SILFunctions, SILVTables and SILWitnessTables.
  bool performSILElimination(SILModule *M);

  // Diagnostics transformations.
  SILTransform *createCapturePromotion();
  SILTransform *createInOutDeshadowing();
  SILTransform *createDefiniteInitialization();
  SILTransform *createPredictableMemoryOptimizations();
  SILTransform *createDiagnosticConstantPropagation();
  SILTransform *createNoReturnFolding();
  SILTransform *createDiagnoseUnreachable();
  SILTransform *createMandatoryInlining();
  SILTransform *createSILCleanup();
  SILTransform *createEmitDFDiagnostics();

  // Performance transformations.
  SILTransform *createSILCombine();
  SILTransform *createDeadFunctionElimination();
  SILTransform *createGlobalOpt();
  SILTransform *createLowerAggregate();
  SILTransform *createSROA();
  SILTransform *createMem2Reg();
  SILTransform *createCSE();
  SILTransform *createCodeMotion();
  SILTransform *createPerfInliner();
  SILTransform *createGenericSpecializer();
  SILTransform *createARCOpts();
  SILTransform *createSimplifyCFG();
  SILTransform *createDevirtualization();
  SILTransform *createEarlyBinding();
  SILTransform *createAllocBoxToStack();
  SILTransform *createDeadObjectElimination();
  SILTransform *createLoadStoreOpts();
  SILTransform *createPerformanceConstantPropagation();
  SILTransform *createGlobalARCOpts();
  SILTransform *createDCE();
  SILTransform *createEnumSimplification();
  SILTransform *createFunctionSignatureOpts();

  // Utilities
  SILTransform *createStripDebug();
  SILTransform *createSILInstCount();
  SILTransform *createSILAADumper();
  SILTransform *createSILLinker();
  SILTransform *createLoopInfoPrinter();
} // end namespace swift

#endif
