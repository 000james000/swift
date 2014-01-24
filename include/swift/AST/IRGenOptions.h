//===--- IRGenOptions.h - Swift Language IR Generation Options --*- C++ -*-===//
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
// This file defines the options which control the generation of IR for
// swift files.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_AST_IRGENOPTIONS_H
#define SWIFT_AST_IRGENOPTIONS_H

#include "swift/AST/LinkLibrary.h"
#include <string>

namespace swift {

enum class IRGenOutputKind : unsigned {
  /// Just generate an LLVM module and return it.
  Module,

  /// Generate an LLVM module and write it out as LLVM assembly.
  LLVMAssembly,

  /// Generate an LLVM module and write it out as LLVM bitcode.
  LLVMBitcode,

  /// Generate an LLVM module and compile it to assembly.
  NativeAssembly,

  /// Generate an LLVM module, compile it, and assemble into an object file.
  ObjectFile
};

/// The set of options supported by IR generation.
class IRGenOptions {
public:
  /// The name of the first input file, used by the debug info.
  std::string MainInputFilename;
  std::string OutputFilename;
  std::string ModuleName;
  std::string Triple;
  // The command line string that is to be stored in the DWARF debug info.
  std::string DWARFDebugFlags;

  /// The libraries and frameworks specified on the command line.
  SmallVector<LinkLibrary, 4> LinkLibraries;

  /// The kind of compilation we should do.
  IRGenOutputKind OutputKind : 3;

  /// Should we spend time verifying that the IR we produce is
  /// well-formed?
  unsigned Verify : 1;

  /// The optimization level, as in -O2.
  unsigned OptLevel : 2;

  /// Whether we should emit debug info.
  unsigned DebugInfo : 1;

  /// Whether we should include the module directly along with the debug info.
  unsigned LegacyDebugInfo : 1;

  /// \brief Whether we're generating IR for the JIT.
  unsigned UseJIT : 1;
  
  /// \brief Whether we allow dynamic value type layout.
  unsigned EnableDynamicValueTypeLayout : 1;

  /// \brief Whether we should run LLVM optimizations after IRGen.
  unsigned DisableLLVMOptzns : 1;
  
  /// \brief Whether we should omit dynamic safety checks from the emitted IR.
  unsigned DisableAllRuntimeChecks : 1;

  IRGenOptions() : OutputKind(IRGenOutputKind::LLVMAssembly), Verify(true),
                   OptLevel(0), DebugInfo(false), LegacyDebugInfo(false),
                   UseJIT(false), EnableDynamicValueTypeLayout(false),
                   DisableLLVMOptzns(false), DisableAllRuntimeChecks(false) {}
};

} // end namespace swift

#endif
