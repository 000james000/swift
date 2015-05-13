//===--- Job.h - Commands to Execute ----------------------------*- C++ -*-===//
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

#ifndef SWIFT_DRIVER_JOB_H
#define SWIFT_DRIVER_JOB_H

#include "swift/Basic/LLVM.h"
#include "swift/Driver/Types.h"
#include "swift/Driver/Util.h"
#include "llvm/Option/Option.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/TimeValue.h"

#include <memory>

namespace swift {
namespace driver {

class Action;
class InputAction;
class Job;
class Tool;

class JobList {
  typedef SmallVector<Job *, 4> list_type;
    
public:
  typedef list_type::size_type size_type;
  typedef list_type::iterator iterator;
  typedef list_type::const_iterator const_iterator;

private:
  list_type Jobs;
  bool OwnsJobs;

public:
  JobList() : OwnsJobs(true) {}
  ~JobList();

  bool getOwnsJobs() const { return OwnsJobs; }
  void setOwnsJobs(bool Value) { OwnsJobs = Value; }

  void addJob(Job *J) { Jobs.push_back(J); }

  void clear();

  ArrayRef<Job *> getJobs() const { return Jobs; }

  size_type size() const { return Jobs.size(); }
  bool empty() const { return Jobs.empty(); }
  iterator begin() { return Jobs.begin(); }
  const_iterator begin() const { return Jobs.begin(); }
  iterator end() { return Jobs.end(); }
  const_iterator end() const { return Jobs.end(); }
  Job *front() const { return Jobs.front(); }
  Job *back() const { return Jobs.back(); }
};

class CommandOutput {
  types::ID PrimaryOutputType;
  
  /// The primary output files of the command.
  /// Usually a command has only a single output file. Only the compiler in
  /// multi-threaded compilation produces multiple output files.
  SmallVector<std::string, 1> PrimaryOutputFilenames;

  /// For each primary output file there is a base input. This is the input file
  /// from which the output file is derived.
  SmallVector<StringRef, 1> BaseInputs;

  llvm::SmallDenseMap<types::ID, std::string, 4> AdditionalOutputsMap;

public:
  CommandOutput(types::ID PrimaryOutputType)
      : PrimaryOutputType(PrimaryOutputType) { }

  types::ID getPrimaryOutputType() const { return PrimaryOutputType; }

  void addPrimaryOutput(StringRef FileName, StringRef BaseInput) {
    PrimaryOutputFilenames.push_back(FileName);
    BaseInputs.push_back(BaseInput);
  }
  
  // This returns a std::string instead of a StringRef so that users can rely
  // on the data buffer being null-terminated.
  const std::string &getPrimaryOutputFilename() const {
    assert(PrimaryOutputFilenames.size() == 1);
    return PrimaryOutputFilenames[0];
  }

  ArrayRef<std::string> getPrimaryOutputFilenames() const {
    return PrimaryOutputFilenames;
  }
  
  void setAdditionalOutputForType(types::ID type, StringRef OutputFilename);
  const std::string &getAdditionalOutputForType(types::ID type) const;

  const std::string &getAnyOutputForType(types::ID type) const;

  StringRef getBaseInput(int Index) const { return BaseInputs[Index]; }
};

class Job {
public:
  enum class Condition {
    Always,
    RunWithoutCascading,
    CheckDependencies
  };

private:
  /// The action which caused the creation of this Job.
  const Action &Source;

  /// The tool which created this Job, and the conditions under which it must
  /// be run.
  llvm::PointerIntPair<const Tool *, 2, Condition> CreatorAndCondition;

  /// The list of other Jobs which are inputs to this Job.
  std::unique_ptr<JobList> Inputs;

  /// The output of this command.
  std::unique_ptr<CommandOutput> Output;

  /// The executable to run.
  const char *Executable;

  /// The list of program arguments (not including the implicit first argument,
  /// which will be the Executable).
  llvm::opt::ArgStringList Arguments;

  /// An estimate of the latest possible time this job was previously run.
  llvm::sys::TimeValue MaxPreviousBuildTime = llvm::sys::TimeValue::MinTime();

public:
  Job(const Action &Source, const Tool &Creator,
      std::unique_ptr<JobList> Inputs, std::unique_ptr<CommandOutput> Output,
      const char *Executable, llvm::opt::ArgStringList &Arguments)
      : Source(Source), CreatorAndCondition(&Creator, Condition::Always),
        Inputs(std::move(Inputs)), Output(std::move(Output)),
        Executable(Executable), Arguments(Arguments) {}

  const Action &getSource() const { return Source; }
  const Tool &getCreator() const { return *CreatorAndCondition.getPointer(); }

  const char *getExecutable() const { return Executable; }
  const llvm::opt::ArgStringList &getArguments() const { return Arguments; }

  const JobList &getInputs() const { return *Inputs; }
  const CommandOutput &getOutput() const { return *Output; }

  Condition getCondition() const {
    return CreatorAndCondition.getInt();
  }
  void setCondition(Condition Cond) {
    CreatorAndCondition.setInt(Cond);
  }

  /// Updates the estimated timestamp of the previous execution of this job.
  ///
  /// \returns true if the new time value is later than the old time value.
  bool updatePreviousBuildTime(llvm::sys::TimeValue NewTime) {
    if (MaxPreviousBuildTime >= NewTime)
      return false;
    MaxPreviousBuildTime = NewTime;
    return true;
  }

  llvm::sys::TimeValue getPreviousBuildTime() const {
    return MaxPreviousBuildTime;
  }

  /// Print the command line for this Job to the given \p stream,
  /// terminating output with the given \p terminator.
  void printCommandLine(raw_ostream &Stream, StringRef Terminator = "\n") const;

  static void printArguments(raw_ostream &Stream,
                             const llvm::opt::ArgStringList &Args);
};

} // end namespace driver
} // end namespace swift

#endif
