//===--- Substitution.h - Swift Generic Substitution ASTs -------*- C++ -*-===//
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
// This file defines the Substitution class.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_AST_SUBSTITUTION_H
#define SWIFT_AST_SUBSTITUTION_H

#include "swift/AST/Type.h"
#include "llvm/ADT/ArrayRef.h"

namespace llvm {
  class raw_ostream;
}

namespace swift {
  class ArchetypeType;
  class ProtocolConformance;
  
/// Substitution - A substitution into a generic specialization.
class Substitution {
public:
  ArchetypeType *Archetype;
  Type Replacement;
  ArrayRef<ProtocolConformance *> Conformance;

  bool operator!=(const Substitution &Other) const;
  void print(llvm::raw_ostream &os) const;
  void dump() const;
  
  /// Substitute the replacement and conformance types with the given
  /// substitution vector.
  Substitution subst(Module *module,
                     ArrayRef<Substitution> subs) const;
  Substitution subst(Module *module,
                     ArrayRef<Substitution> subs,
                     TypeSubstitutionMap &subMap) const;
};

} // end namespace swift

#endif
