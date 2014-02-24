//===-- SILExtract.cpp - SIL function extraction utility ------------------===//
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
// This utility is meant to help simplify the extraction of test cases from sil
// files by removing (currently only) functions that do not match a
// string. Eventually this should have additional capabilities like stripping
// globals, vtables, etc.
//
//===----------------------------------------------------------------------===//

#include "swift/Subsystems.h"
#include "swift/Frontend/DiagnosticVerifier.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Frontend/PrintingDiagnosticConsumer.h"
#include "swift/Serialization/SerializedModuleLoader.h"
#include "swift/Serialization/SerializedSILLoader.h"
#include "swift/SILPasses/Passes.h"
#include "swift/SIL/SILUndef.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Signals.h"
using namespace swift;

static llvm::cl::opt<std::string>
InputFilename(llvm::cl::desc("input file"), llvm::cl::init("-"),
              llvm::cl::Positional);

static llvm::cl::opt<std::string>
OutputFilename("o", llvm::cl::desc("output filename"), llvm::cl::init("-"));


static llvm::cl::opt<bool>
EmitVerboseSIL("emit-verbose-sil",
               llvm::cl::desc("Emit locations during sil emission."));

static llvm::cl::opt<std::string>
FunctionName("func", llvm::cl::desc("Function name to extract."));

static llvm::cl::list<std::string>
ImportPaths("I", llvm::cl::desc("add a directory to the import search path"));

static llvm::cl::opt<std::string>
ModuleName("module-name", llvm::cl::desc("The name of the module if processing"
                                         " a module. Necessary for processing "
                                         "stdin."));

// This function isn't referenced outside its translation unit, but it
// can't use the "static" keyword because its address is used for
// getMainExecutable (since some platforms don't support taking the
// address of main, and some platforms can't implement getMainExecutable
// without being given the address of a function in the main executable).
void anchorForGetMainExecutable() {}

int main(int argc, char **argv) {
  // Print a stack trace if we signal out.
  llvm::sys::PrintStackTraceOnErrorSignal();
  llvm::PrettyStackTraceProgram X(argc, argv);

  llvm::cl::ParseCommandLineOptions(argc, argv, "Swift SIL Extractor\n");

  // Call llvm_shutdown() on exit to print stats and free memory.
  llvm::llvm_shutdown_obj Y;

  CompilerInvocation Invocation;

  Invocation.setMainExecutablePath(
      llvm::sys::fs::getMainExecutable(argv[0],
          reinterpret_cast<void *>(&anchorForGetMainExecutable)));

  // Give the context the list of search paths to use for modules.
  Invocation.setImportSearchPaths(ImportPaths);

  // Load the input file.
  llvm::OwningPtr<llvm::MemoryBuffer> InputFile;
  if (llvm::MemoryBuffer::getFileOrSTDIN(InputFilename, InputFile)) {
    fprintf(stderr, "Error! Failed to open file: %s\n", InputFilename.c_str());
    exit(-1);
  }

  // If it looks like we have an AST, set the source file kind to SIL and the
  // name of the module to the file's name.
  Invocation.addInputBuffer(InputFile.get());
  bool IsModule = false;
  if (SerializedModuleLoader::isSerializedAST(InputFile.get()->getBuffer())) {
    IsModule = true;
    const StringRef Stem = ModuleName.size() ?
                             StringRef(ModuleName) :
                             llvm::sys::path::stem(InputFilename);
    Invocation.setModuleName(Stem);
    Invocation.setInputKind(SourceFileKind::Library);
  } else {
    Invocation.setModuleName("main");
    Invocation.setInputKind(SourceFileKind::SIL);
  }

  CompilerInstance CI;
  PrintingDiagnosticConsumer PrintDiags;
  CI.addDiagnosticConsumer(&PrintDiags);

  if (CI.setup(Invocation))
    return 1;
  CI.performParse();

  // If parsing produced an error, don't run any passes.
  if (CI.getASTContext().hadError())
    return 1;

  // Load the SIL if we have a module. We have to do this after SILParse
  // creating the unfortunate double if statement.
  if (IsModule) {
    assert(!CI.hasSILModule() &&
           "performParse() should not create a SILModule.");
    CI.setSILModule(SILModule::createEmptyModule(CI.getMainModule()));
    SerializedSILLoader *SL = SerializedSILLoader::create(CI.getASTContext(),
                                                          CI.getSILModule(),
                                                          nullptr);
    SL->getAll();
    delete SL;
  }

  assert(CI.hasSILModule() && "CI must have a sil module to extract from.\n");

  SILModule *M = CI.getSILModule();

  std::vector<SILFunction *> DeadFunctions;
  for (auto &F : M->getFunctionList()) {
    if (FunctionName != F.getName().str()) {
      if (F.size()) {
        SILBasicBlock &BB = F.front();

        SILLocation Loc = BB.getInstList().back().getLoc();
        BB.splitBasicBlock(BB.begin());
        // Make terminator unreachable.
        BB.getInstList().push_front(new (*M) UnreachableInst(Loc));

        DeadFunctions.push_back(&F);
      }
    }
  }

  // After running this pass all of the functions to remove should consist only
  // of one BB with an unreachable.
  performSILDeadCodeElimination(M);

  // Now clear those functions.
  for (auto &F : DeadFunctions) {
    F->getBlocks().clear();
  }

  std::string ErrorInfo;
  llvm::raw_fd_ostream OS(OutputFilename.c_str(), ErrorInfo,
                          llvm::sys::fs::F_None);
  if (!ErrorInfo.empty()) {
    llvm::errs() << "while opening '" << OutputFilename << "': "
                 << ErrorInfo << '\n';
    return 1;
  }
  CI.getSILModule()->print(OS, EmitVerboseSIL, CI.getMainModule());

  return CI.getASTContext().hadError();
}
