//===--- FrontendOptions.h --------------------------------------*- C++ -*-===//
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

#ifndef SWIFT_FRONTEND_FRONTENDOPTIONS_H
#define SWIFT_FRONTEND_FRONTENDOPTIONS_H

#include "swift/AST/Module.h"
#include "swift/Basic/Optional.h"

#include <string>
#include <vector>

namespace llvm {
  class MemoryBuffer;
}

namespace swift {

class SelectedInput {
public:
  /// The index of the input, in either FrontendOptions::InputFilenames or
  /// FrontendOptions::InputBuffers, depending on this SelectedInput's
  /// InputKind.
  unsigned Index;

  enum class InputKind {
    /// Denotes a file input, in FrontendOptions::InputFilenames
    Filename,

    /// Denotes a buffer input, in FrontendOptions::InputBuffers
    Buffer,
  };

  /// The kind of input which this SelectedInput represents.
  InputKind Kind;

  SelectedInput(unsigned Index, InputKind Kind = InputKind::Filename)
      : Index(Index), Kind(Kind) {}

  /// \returns true if the SelectedInput's Kind is a filename
  bool isFilename() const { return Kind == InputKind::Filename; }

  /// \returns true if the SelectedInput's Kind is a buffer
  bool isBuffer() const { return Kind == InputKind::Buffer; }
};

/// Options for controlling the behavior of the frontend.
class FrontendOptions {
public:
  /// The names of input files to the frontend.
  std::vector<std::string> InputFilenames;

  /// Input buffers which may override the file contents of input files.
  std::vector<llvm::MemoryBuffer *> InputBuffers;

  /// The input for which output should be generated. If not set, output will
  /// be generated for the whole module.
  Optional<SelectedInput> PrimaryInput;

  /// The kind of input on which the frontend should operate.
  SourceFileKind InputKind = SourceFileKind::Main;

  /// The name of the primary output file which should be created
  /// by the frontend.
  std::string OutputFilename;

  /// The name of the module which the frontend is building.
  std::string ModuleName;

  /// The path to which we should emit a serialized module.
  std::string ModuleOutputPath;

  /// The name of the library to link against when using this module.
  std::string ModuleLinkName;

  enum ActionType {
    Parse, ///< Parse and type-check only
    DumpParse, ///< Parse only and dump AST
    DumpAST, ///< Parse, type-check, and dump AST
    PrintAST, ///< Parse, type-check, and pretty-print AST

    EmitSILGen, ///< Emit raw SIL
    EmitSIL, ///< Emit canonical SIL

    EmitModuleOnly, ///< Emit module only

    Immediate, ///< Immediate mode
    REPL, ///< REPL mode

    EmitAssembly, ///< Emit assembly
    EmitIR, ///< Emit LLVM IR
    EmitBC, ///< Emit LLVM BC
    EmitObject, ///< Emit object file
  };

  /// Indicates the action the user requested that the frontend perform.
  ActionType RequestedAction = Parse;

  /// Indicates that type-checking should be disabled.
  bool ParseOnly = false;

  /// Indicates that the input(s) should be parsed as the Swift stdlib.
  bool ParseStdlib = false;

  /// Indicates whether function body parsing should be delayed
  /// until the end of all files.
  bool DelayedFunctionBodyParsing = false;

  /// Indicates that the frontend should emit "verbose" SIL
  /// (if asked to emit SIL).
  bool EmitVerboseSIL = false;

  /// Indicates that all generated SIL should be serialized into a module,
  /// not just code considered fragile.
  bool SILSerializeAll = false;

  /// Indicates how the frontend should perform SIL linking.
  enum SILLinkMode {
    /// Indicates that the frontend should skip SIL linking.
    LinkNone,

    /// Indicates that the frontend should perform normal SIL linking.
    LinkNormal,

    /// Indicates that the frontend should link all functions during SIL
    /// linking.
    LinkAll
  };

  SILLinkMode SILLinking = LinkNormal;

  /// Path to a file which should contain serialized diagnostics for this
  /// frontend invocation.
  std::string SerializedDiagnosticsPath;

  /// Arguments which should be passed in immediate mode.
  std::vector<std::string> ImmediateArgv;

  /// Indicates whether or not the frontend should print statistics upon
  /// termination.
  bool PrintStats = false;

  /// Indicates whether standard help should be shown.
  bool PrintHelp = false;

  /// Indicates whether full help (including "hidden" options) should be shown.
  bool PrintHelpHidden = false;

  /// Indicates whether the RequestedAction has output.
  bool actionHasOutput();

  /// Indicates whether the RequestedAction will immediately run code.
  bool actionIsImmediate();
};

}

#endif
