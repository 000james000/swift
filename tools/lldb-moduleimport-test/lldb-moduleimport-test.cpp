//===-- lldb-moduleimport-test.cpp - LLDB moduleimport tester -------------===//
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
// This program simulates LLDB importing modules from the __apple_ast
// section in Mach-O files. We use it to test for regressions in the
// deserialization API.
//
//===----------------------------------------------------------------------===//

#include "swift/Basic/Dwarf.h"
#include "swift/Frontend/Frontend.h"
#include "swift/ASTSectionImporter/ASTSectionImporter.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/MachO.h"
#include <stdint.h>
#include <fstream>

static llvm::cl::list<std::string>
InputNames(llvm::cl::Positional, llvm::cl::desc("compiled_swift_file1.o ..."),
           llvm::cl::OneOrMore);

#ifndef SWIFT_MODULES_SDK
#define SWIFT_MODULES_SDK ""
#endif

static llvm::cl::opt<std::string>
SDK("sdk", llvm::cl::desc("path to the SDK to build against"),
    llvm::cl::init(SWIFT_MODULES_SDK));

static llvm::cl::opt<bool>
DumpModule("dump-module",
           llvm::cl::desc("Dump the imported module after checking it imports just fine"));

static llvm::cl::opt<std::string>
ModuleCachePath("module-cache-path", llvm::cl::desc("Clang module cache path"),
                llvm::cl::init(SWIFT_MODULE_CACHE_PATH));


void anchorForGetMainExecutable() {}

using namespace llvm::MachO;

int main(int argc, char **argv) {
  llvm::sys::PrintStackTraceOnErrorSignal();
  llvm::PrettyStackTraceProgram ST(argc, argv);
  llvm::cl::ParseCommandLineOptions(argc, argv);

  // If no SDK was specified via -sdk, check environment variable SDKROOT.
  if (SDK.getNumOccurrences() == 0) {
    const char *SDKROOT = getenv("SDKROOT");
    if (SDKROOT)
      SDK = SDKROOT;
  }

  // Create a Swift compiler.
  llvm::SmallVector<std::string, 4> modules;
  swift::CompilerInstance CI;
  swift::CompilerInvocation Invocation;

  Invocation.setMainExecutablePath(
      llvm::sys::fs::getMainExecutable(argv[0],
          reinterpret_cast<void *>(&anchorForGetMainExecutable)));

  Invocation.setSDKPath(SDK);
  Invocation.setTargetTriple(llvm::sys::getDefaultTargetTriple());
  Invocation.setModuleName("lldbtest");
  Invocation.getClangImporterOptions().ModuleCachePath = ModuleCachePath;

  if (CI.setup(Invocation))
    return 1;

  // Fetch the serialized module bitstreams from the Mach-O files and
  // register them with the module loader.
  for (std::string name : InputNames) {
    // We assume Macho-O 64 bit.
    std::ifstream macho(name);
    if (!macho.good()) {
      llvm::outs() << "Cannot read from " << name << "\n";
      exit(1);
    }
    struct mach_header_64 h;
    macho.read((char*)&h, sizeof(h));
    assert(h.magic == MH_MAGIC_64);
    // Load command.
    for (uint32_t i = 0; i < h.ncmds; ++i) {
      struct load_command lc;
      macho.read((char*)&lc, sizeof(lc));
      // Segment command.
      if (lc.cmd == LC_SEGMENT_64) {
        macho.seekg(-sizeof(lc), macho.cur);
        struct segment_command_64 sc;
        macho.read((char*)&sc, sizeof(sc));
        // Sections.
        for (uint32_t j = 0; j < sc.nsects; ++j) {
          struct section_64 section;
          macho.read((char*)&section, sizeof(section));
          if (llvm::StringRef(swift::MachOASTSectionName) == section.sectname) {
            // Pass the __ast section to the module loader.
            macho.seekg(section.offset, macho.beg);
            assert(macho.good());

            // We can delete this memory only after the
            // SerializedModuleLoader has performed its duty.
            auto data = new char[section.size];
            macho.read(data, section.size);

            if (!parseASTSection(CI.getSerializedModuleLoader(),
                                 llvm::StringRef(data, section.size),
                                 modules)) {
              exit(1);
            }

            for (auto path : modules)
              llvm::outs() << "Loaded module " << path << " from " << name
                           << "\n";
          }
        }
      } else
        macho.seekg(lc.cmdsize-sizeof(lc), macho.cur);
    }
  }

  // Attempt to import all modules we found.
  for (auto path : modules) {
    llvm::outs() << "Importing " << path << "... ";

#ifdef SWIFT_SUPPORTS_SUBMODULES
    std::vector<std::pair<swift::Identifier, swift::SourceLoc> > AccessPath;
    for (auto i = llvm::sys::path::begin(path);
         i != llvm::sys::path::end(path); ++i)
      if (!llvm::sys::path::is_separator((*i)[0]))
          AccessPath.push_back({ CI.getASTContext().getIdentifier(*i),
                                 swift::SourceLoc() });
#else
    std::vector<std::pair<swift::Identifier, swift::SourceLoc> > AccessPath;
    AccessPath.push_back({ CI.getASTContext().getIdentifier(path),
                           swift::SourceLoc() });
#endif

    auto Module = CI.getASTContext().getModule(AccessPath);
    if (!Module) {
      llvm::errs() << "FAIL!\n";
      return 1;
    }
    llvm::outs() << "ok!\n";
    if (DumpModule) {
      llvm::SmallVector<swift::Decl*, 10> Decls;
      Module->getTopLevelDecls(Decls);
      for (auto Decl : Decls) {
        Decl->dump(llvm::outs());
      }
    }
  }
  return 0;
}
