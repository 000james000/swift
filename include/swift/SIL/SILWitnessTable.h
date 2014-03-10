//===--- SILWitnessTable.h - Defines the SILWitnessTable class --*- C++ -*-===//
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
// This file defines the SILWitnessTable class, which is used to map a protocol
// conformance for a type to its implementing SILFunctions. This information is
// (FIXME will be) used by IRGen to create witness tables for protocol dispatch.
// It can also be used by generic specialization and existential
// devirtualization passes to promote witness_method and protocol_method
// instructions to static function_refs.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SIL_SILWITNESSTABLE_H
#define SWIFT_SIL_SILWITNESSTABLE_H

#include "swift/SIL/SILAllocated.h"
#include "swift/SIL/SILDeclRef.h"
#include "llvm/ADT/ilist_node.h"
#include "llvm/ADT/ilist.h"
#include <string>

namespace swift {
  class ClassDecl;
  class SILFunction;
  class SILModule;
  class NormalProtocolConformance;
  
/// A mapping from each requirement of a protocol to the SIL-level entity
/// satisfying the requirement for a concrete type.
class SILWitnessTable : public llvm::ilist_node<SILWitnessTable>,
                        public SILAllocated<SILWitnessTable>
{
public:
  /// A witness table entry describing the witness for a method.
  struct MethodWitness {
    /// The method required.
    SILDeclRef Requirement;
    /// The witness for the method.
    SILFunction *Witness;
  };
  
  /// A witness table entry describing the witness for an associated type.
  struct AssociatedTypeWitness {
    /// The associated type required.
    AssociatedTypeDecl *Requirement;
    /// The concrete semantic type of the witness.
    CanType Witness;
  };
  
  /// A witness table entry describing the witness for an associated type's
  /// protocol requirement.
  struct AssociatedTypeProtocolWitness {
    /// The associated type required.
    AssociatedTypeDecl *Requirement;
    /// The protocol requirement on the type.
    ProtocolDecl *Protocol;
    /// The ProtocolConformance satisfying the requirement. Null if the
    /// conformance is dependent.
    ProtocolConformance *Witness;
  };
  
  /// A witness table entry referencing the protocol conformance for a refined
  /// base protocol.
  struct BaseProtocolWitness {
    /// The base protocol.
    ProtocolDecl *Requirement;
    /// The ProtocolConformance for the base protocol.
    ProtocolConformance *Witness;
  };
  
  /// A witness table entry kind.
  enum WitnessKind {
    Invalid,
    Method,
    AssociatedType,
    AssociatedTypeProtocol,
    BaseProtocol,
  };
  
  /// A witness table entry.
  class Entry {
    WitnessKind Kind;
    union {
      MethodWitness Method;
      AssociatedTypeWitness AssociatedType;
      AssociatedTypeProtocolWitness AssociatedTypeProtocol;
      BaseProtocolWitness BaseProtocol;
    };
    
  public:
    Entry() : Kind(WitnessKind::Invalid) {}
    
    Entry(const MethodWitness &Method)
      : Kind(WitnessKind::Method), Method(Method)
    {}
    
    Entry(const AssociatedTypeWitness &AssociatedType)
      : Kind(WitnessKind::AssociatedType), AssociatedType(AssociatedType)
    {}
    
    Entry(const AssociatedTypeProtocolWitness &AssociatedTypeProtocol)
      : Kind(WitnessKind::AssociatedTypeProtocol),
        AssociatedTypeProtocol(AssociatedTypeProtocol)
    {}
    
    Entry(const BaseProtocolWitness &BaseProtocol)
      : Kind(WitnessKind::BaseProtocol),
        BaseProtocol(BaseProtocol)
    {}
    
    WitnessKind getKind() const { return Kind; }
    
    const MethodWitness &getMethodWitness() const {
      assert(Kind == WitnessKind::Method);
      return Method;
    }
    const AssociatedTypeWitness &getAssociatedTypeWitness() const {
      assert(Kind == WitnessKind::AssociatedType);
      return AssociatedType;
    }
    const AssociatedTypeProtocolWitness &
    getAssociatedTypeProtocolWitness() const {
      assert(Kind == WitnessKind::AssociatedTypeProtocol);
      return AssociatedTypeProtocol;
    }
    const BaseProtocolWitness &
    getBaseProtocolWitness() const {
      assert(Kind == WitnessKind::BaseProtocol);
      return BaseProtocol;
    }
  };
  
private:
  SILLinkage Linkage;
  NormalProtocolConformance *Conformance;
  ArrayRef<Entry> Entries;

  SILWitnessTable(SILModule &M,
                  SILLinkage Linkage,
                  NormalProtocolConformance *Conformance,
                  ArrayRef<Entry> entries);

public:
  /// Create a new SILWitnessTable with the given entries.
  static SILWitnessTable *create(SILModule &M,
                                 SILLinkage Linkage,
                                 NormalProtocolConformance *Conformance,
                                 ArrayRef<Entry> entries);

  ~SILWitnessTable();
  
  /// Return the AST ProtocolConformance this witness table represents.
  NormalProtocolConformance *getConformance() const { return Conformance; }
  
  /// Return all of the witness table entries.
  ArrayRef<Entry> getEntries() const { return Entries; }
  
  /// Verify that the witness table is well-formed.
  void verify(const SILModule &M) const;
  
  /// Get the linkage of the witness table.
  SILLinkage getLinkage() const { return Linkage; }
  void setLinkage(SILLinkage l) { Linkage = l; }
  
  /// Print the witness table.
  void print(llvm::raw_ostream &OS, bool Verbose = false) const;
  void dump() const;
};
  
}

//===----------------------------------------------------------------------===//
// ilist_traits for SILWitnessTable
//===----------------------------------------------------------------------===//

namespace llvm {
  
template <>
struct ilist_traits<::swift::SILWitnessTable> :
public ilist_default_traits<::swift::SILWitnessTable> {
  typedef ::swift::SILWitnessTable SILWitnessTable;

private:
  mutable ilist_half_node<SILWitnessTable> Sentinel;

public:
  SILWitnessTable *createSentinel() const {
    return static_cast<SILWitnessTable*>(&Sentinel);
  }
  void destroySentinel(SILWitnessTable *) const {}

  SILWitnessTable *provideInitialHead() const { return createSentinel(); }
  SILWitnessTable *ensureHead(SILWitnessTable*) const { return createSentinel(); }
  static void noteHead(SILWitnessTable*, SILWitnessTable*) {}
  static void deleteNode(SILWitnessTable *V) {}
  
private:
  void createNode(const SILWitnessTable &);
};

} // end llvm namespace

#endif
