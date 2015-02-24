//===--- Tools.h - Tool Implementations -------------------------*- C++ -*-===//
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

#ifndef SWIFT_DRIVER_TOOLS_H
#define SWIFT_DRIVER_TOOLS_H

#include "swift/Basic/LLVM.h"
#include "swift/Driver/Tool.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/Compiler.h"

namespace swift {
namespace driver {

namespace toolchains {
  class Darwin;
  class Linux;
}

namespace tools {

class LLVM_LIBRARY_VISIBILITY Swift : public Tool {
public:
  explicit Swift(const ToolChain &TC) : Tool("swift", "swift frontend", TC) {}

  virtual bool hasGoodDiagnostics() const { return true; }

  virtual Job *constructJob(const JobAction &JA,
                            std::unique_ptr<JobList> Inputs,
                            std::unique_ptr<CommandOutput> Output,
                            const ActionList &InputActions,
                            const llvm::opt::ArgList &Args,
                            const OutputInfo &OI) const;
};

class LLVM_LIBRARY_VISIBILITY MergeModule : public Tool {
public:
  explicit MergeModule(const ToolChain &TC)
    : Tool("merge-module", TC) {}

  virtual bool hasGoodDiagnostics() const { return true; }

  virtual Job *constructJob(const JobAction &JA,
                            std::unique_ptr<JobList> Inputs,
                            std::unique_ptr<CommandOutput> Output,
                            const ActionList &InputActions,
                            const llvm::opt::ArgList &Args,
                            const OutputInfo &OI) const;
};

/// A ToolchainTool may be installed in a location relative to the driver
/// binary. The relative tool should be preferred over the one in the user's
/// \$PATH.
class LLVM_LIBRARY_VISIBILITY ToolchainTool : public Tool {
  mutable std::string NameOrPath;
  struct {
    mutable unsigned DidCheckRelativeToDriver : 1;
    mutable unsigned IsPresentRelativeToDriver : 1;
  } Bits;

protected:
  ToolchainTool(StringRef BinaryName, StringRef ToolName, StringRef DiagName,
                const ToolChain &TC)
      : Tool(ToolName, DiagName, TC), NameOrPath(BinaryName), Bits() {}
  ToolchainTool(StringRef Name, const ToolChain &TC)
      : ToolchainTool(Name, Name, Name, TC) {}

public:
  const char *getPath() const;
  bool isPresentRelativeToDriver() const;
};

class LLVM_LIBRARY_VISIBILITY LLDB : public ToolchainTool {
public:
  explicit LLDB(const ToolChain &TC)
      : ToolchainTool("lldb", "LLDB", "LLDB REPL", TC) {}

  virtual Job *constructJob(const JobAction &JA,
                            std::unique_ptr<JobList> Inputs,
                            std::unique_ptr<CommandOutput> Output,
                            const ActionList &InputActions,
                            const llvm::opt::ArgList &Args,
                            const OutputInfo &OI) const;
};

class LLVM_LIBRARY_VISIBILITY Dsymutil : public ToolchainTool {
public:
  explicit Dsymutil(const ToolChain &TC) : ToolchainTool("dsymutil", TC) {}

  virtual Job *constructJob(const JobAction &JA,
                            std::unique_ptr<JobList> Inputs,
                            std::unique_ptr<CommandOutput> Output,
                            const ActionList &InputActions,
                            const llvm::opt::ArgList &Args,
                            const OutputInfo &OI) const;
};

class LLVM_LIBRARY_VISIBILITY AutolinkExtract : public ToolchainTool {
public:
  explicit AutolinkExtract(const ToolChain &TC)
    : ToolchainTool("swift-autolink-extract", TC) {}

  virtual Job *constructJob(const JobAction &JA,
                            std::unique_ptr<JobList> Inputs,
                            std::unique_ptr<CommandOutput> Output,
                            const ActionList &InputActions,
                            const llvm::opt::ArgList &Args,
                            const OutputInfo &OI) const;
};

namespace darwin {

llvm::Triple::ArchType getArchTypeForDarwinArchName(StringRef DarwinArchName);

class LLVM_LIBRARY_VISIBILITY Linker : public ToolchainTool {
public:
  explicit Linker(const ToolChain &TC)
    : ToolchainTool("ld", "darwin::Linker", "linker", TC) {}

  virtual Job *constructJob(const JobAction &JA,
                            std::unique_ptr<JobList> Inputs,
                            std::unique_ptr<CommandOutput> Output,
                            const ActionList &InputActions,
                            const llvm::opt::ArgList &Args,
                            const OutputInfo &OI) const;
};

} // end namespace darwin

namespace linux {

class LLVM_LIBRARY_VISIBILITY Linker : public Tool {
public:
  explicit Linker(const ToolChain &TC)
    : Tool("linux::Linker", "linker", TC) {}

  virtual Job *constructJob(const JobAction &JA,
                            std::unique_ptr<JobList> Inputs,
                            std::unique_ptr<CommandOutput> Output,
                            const ActionList &InputActions,
                            const llvm::opt::ArgList &Args,
                            const OutputInfo &OI) const;
};

} // end namespace linux

} // end namespace tools

} // end namespace driver
} // end namespace swift

#endif
