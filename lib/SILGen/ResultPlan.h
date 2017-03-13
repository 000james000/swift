//===--- ResultPlan.h -----------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILGEN_RESULTPLAN_H
#define SWIFT_SILGEN_RESULTPLAN_H

#include "swift/AST/Types.h"
#include "swift/Basic/LLVM.h"
#include "swift/SIL/SILLocation.h"
#include <memory>

namespace swift {

class CanType;
class SILValue;

namespace Lowering {

class AbstractionPattern;
class Initialization;
class ManagedValue;
class RValue;
class SILGenFunction;

/// An abstract class for working with results.of applies.
class ResultPlan {
public:
  virtual RValue finish(SILGenFunction &SGF, SILLocation loc, CanType substType,
                        ArrayRef<ManagedValue> &directResults) = 0;
  virtual ~ResultPlan() = default;
};

using ResultPlanPtr = std::unique_ptr<ResultPlan>;

/// The class for building result plans.
struct ResultPlanBuilder {
  SILGenFunction &SGF;
  SILLocation Loc;
  ArrayRef<SILResultInfo> AllResults;
  SILFunctionTypeRepresentation Rep;
  SmallVectorImpl<SILValue> &IndirectResultAddrs;

  ResultPlanBuilder(SILGenFunction &SGF, SILLocation loc,
                    ArrayRef<SILResultInfo> allResults,
                    SILFunctionTypeRepresentation rep,
                    SmallVectorImpl<SILValue> &resultAddrs)
      : SGF(SGF), Loc(loc), AllResults(allResults), Rep(rep),
        IndirectResultAddrs(resultAddrs) {}

  ResultPlanPtr build(Initialization *emitInto, AbstractionPattern origType,
                      CanType substType);
  ResultPlanPtr buildForTuple(Initialization *emitInto,
                              AbstractionPattern origType,
                              CanTupleType substType);

  ~ResultPlanBuilder() {
    assert(AllResults.empty() && "didn't consume all results!");
  }
};

} // end namespace Lowering
} // end namespace swift

#endif
