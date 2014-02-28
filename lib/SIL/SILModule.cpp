//===--- SILModule.cpp - SILModule implementation -------------------------===//
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

#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILExternalSource.h"
#include "swift/Serialization/SerializedSILLoader.h"
#include "swift/SIL/SILValue.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Debug.h"
using namespace swift;

STATISTIC(NumFuncLinked, "Number of SIL functions linked");

namespace swift {
  /// SILTypeList - The uniqued backing store for the SILValue type list.  This
  /// is only exposed out of SILValue as an ArrayRef of types, so it should
  /// never be used outside of libSIL.
  class SILTypeList : public llvm::FoldingSetNode {
  public:
    unsigned NumTypes;
    SILType Types[1];  // Actually variable sized.

    void Profile(llvm::FoldingSetNodeID &ID) const {
      for (unsigned i = 0, e = NumTypes; i != e; ++i) {
        ID.AddPointer(Types[i].getOpaqueValue());
      }
    }
  };
} // end namespace swift.

void SILExternalSource::anchor() {
}

/// SILTypeListUniquingType - This is the type of the folding set maintained by
/// SILModule that these things are uniqued into.
typedef llvm::FoldingSet<SILTypeList> SILTypeListUniquingType;

class SILModule::SerializationCallback : public SerializedSILLoader::Callback {
  void didDeserialize(Module *M, SILFunction *fn) override {
    updateLinkage(fn);
  }

  void didDeserialize(Module *M, SILGlobalVariable *var) override {
    updateLinkage(var);
  }

  void didDeserialize(Module *M, SILVTable *vtable) override {
    // TODO: should vtables get linkage?
    //updateLinkage(vtable);
  }

  template <class T> void updateLinkage(T *decl) {
    switch (decl->getLinkage()) {
    case SILLinkage::Public:
      decl->setLinkage(SILLinkage::PublicExternal);
      return;
    case SILLinkage::Hidden:
      decl->setLinkage(SILLinkage::HiddenExternal);
      return;
    case SILLinkage::Shared:
      decl->setLinkage(SILLinkage::Shared);
    case SILLinkage::Private: // ?
    case SILLinkage::PublicExternal:
    case SILLinkage::HiddenExternal:
      return;
    }
  }
};

SILModule::SILModule(Module *SwiftModule)
  : TheSwiftModule(SwiftModule), Stage(SILStage::Raw),
    Callback(new SILModule::SerializationCallback()), Types(*this) {
  TypeListUniquing = new SILTypeListUniquingType();
  SILLoader = SerializedSILLoader::create(getASTContext(), this,
                                          Callback.get());

}

SILModule::~SILModule() {
  delete (SILTypeListUniquingType*)TypeListUniquing;
}

std::pair<SILWitnessTable *, ArrayRef<Substitution>>
SILModule::lookUpWitnessTable(const ProtocolConformance *C) {
  // Walk down to the base NormalProtocolConformance.
  const ProtocolConformance *ParentC = C;
  ArrayRef<Substitution> Subs;
  while (!isa<NormalProtocolConformance>(ParentC)) {
    switch (ParentC->getKind()) {
    case ProtocolConformanceKind::Normal:
      llvm_unreachable("should have exited the loop?!");
    case ProtocolConformanceKind::Inherited:
      ParentC = cast<InheritedProtocolConformance>(ParentC)
        ->getInheritedConformance();
      break;
    case ProtocolConformanceKind::Specialized: {
      auto SC = cast<SpecializedProtocolConformance>(ParentC);
      ParentC = SC->getGenericConformance();
      assert(Subs.empty() && "multiple conformance specializations?!");
      Subs = SC->getGenericSubstitutions();
      break;
    }
    }
  }
  const NormalProtocolConformance *NormalC
    = cast<NormalProtocolConformance>(ParentC);

  // If the normal conformance is for a generic type, and we didn't hit a
  // specialized conformance, collect the substitutions from the generic type.
  // FIXME: The AST should do this for us.
  if (NormalC->getType()->isSpecialized() && Subs.empty()) {
    Subs = NormalC->getType()
      ->gatherAllSubstitutions(NormalC->getDeclContext()->getParentModule(),
                               nullptr);
  }

  // Did we already find this?
  auto found = WitnessTableLookupCache.find(NormalC);
  if (found != WitnessTableLookupCache.end())
    return {found->second, Subs};

  // If not, search through the witness table list, caching the entries we
  // visit.
  for (SILWitnessTable &WT : witnessTables) {
    WitnessTableLookupCache[WT.getConformance()] = &WT;

    if (WT.getConformance() == NormalC)
      return {&WT, Subs};
  }
  return {nullptr, Subs};
}

SILFunction *SILModule::getOrCreateSharedFunction(SILLocation loc,
                                                  StringRef name,
                                                  CanSILFunctionType type,
                                                  IsBare_t isBareSILFunction,
                                                IsTransparent_t isTransparent) {
  auto linkage = SILLinkage::Shared;

  if (auto fn = lookUpFunction(name)) {
    assert(fn->getLoweredFunctionType() == type);
    assert(fn->getLinkage() == linkage);
    return fn;
  }

  return SILFunction::create(*this, linkage, name, type, nullptr,
                             loc, isBareSILFunction, isTransparent);
}

ArrayRef<SILType> ValueBase::getTypes() const {
  // No results.
  if (TypeOrTypeList.isNull())
    return ArrayRef<SILType>();
  // Arbitrary list of results.
  if (auto *TypeList = TypeOrTypeList.dyn_cast<SILTypeList*>())
    return ArrayRef<SILType>(TypeList->Types, TypeList->NumTypes);
  // Single result.
  return TypeOrTypeList.get<SILType>();
}



/// getSILTypeList - Get a uniqued pointer to a SIL type list.  This can only
/// be used by SILValue.
SILTypeList *SILModule::getSILTypeList(ArrayRef<SILType> Types) const {
  assert(Types.size() > 1 && "Shouldn't use type list for 0 or 1 types");
  auto UniqueMap = (SILTypeListUniquingType*)TypeListUniquing;

  llvm::FoldingSetNodeID ID;
  for (auto T : Types) {
    ID.AddPointer(T.getOpaqueValue());
  }

  // If we already have this type list, just return it.
  void *InsertPoint = 0;
  if (SILTypeList *TypeList = UniqueMap->FindNodeOrInsertPos(ID, InsertPoint))
    return TypeList;

  // Otherwise, allocate a new one.
  void *NewListP = BPA.Allocate(sizeof(SILTypeList)+
                                sizeof(SILType)*(Types.size()-1),
                                alignof(SILTypeList));
  SILTypeList *NewList = new (NewListP) SILTypeList();
  NewList->NumTypes = Types.size();
  std::copy(Types.begin(), Types.end(), NewList->Types);

  UniqueMap->InsertNode(NewList, InsertPoint);
  return NewList;
}

const IntrinsicInfo &SILModule::getIntrinsicInfo(Identifier ID) {
  unsigned OldSize = IntrinsicIDCache.size();
  IntrinsicInfo &Info = IntrinsicIDCache[ID];

  // If the element was is in the cache, return it.
  if (OldSize == IntrinsicIDCache.size())
    return Info;

  // Otherwise, lookup the ID and Type and store them in the map.
  StringRef NameRef = getBuiltinBaseName(getASTContext(), ID.str(), Info.Types);
  Info.ID =
    (llvm::Intrinsic::ID)getLLVMIntrinsicID(NameRef, !Info.Types.empty());

  return Info;
}

const BuiltinInfo &SILModule::getBuiltinInfo(Identifier ID) {
  unsigned OldSize = BuiltinIDCache.size();
  BuiltinInfo &Info = BuiltinIDCache[ID];

  // If the element was is in the cache, return it.
  if (OldSize == BuiltinIDCache.size())
    return Info;

  // Otherwise, lookup the ID and Type and store them in the map.
  // Find the matching ID.
  StringRef OperationName =
    getBuiltinBaseName(getASTContext(), ID.str(), Info.Types);

  // Several operation names have suffixes and don't match the name from
  // Builtins.def, so handle those first.
  if (OperationName.startswith("fence_"))
    Info.ID = BuiltinValueKind::Fence;
  else if (OperationName.startswith("cmpxchg_"))
    Info.ID = BuiltinValueKind::CmpXChg;
  else if (OperationName.startswith("atomicrmw_"))
    Info.ID = BuiltinValueKind::AtomicRMW;
  else {
    // Switch through the rest of builtins.
    Info.ID = llvm::StringSwitch<BuiltinValueKind>(OperationName)
#define BUILTIN(ID, Name, Attrs) \
      .Case(Name, BuiltinValueKind::ID)
#include "swift/AST/Builtins.def"
      .Default(BuiltinValueKind::None);
  }

  return Info;
}

bool SILModule::linkFunction(SILFunction *Fun, SILModule::LinkingMode Mode) {
  // If we are not linking anything bail.
  if (Mode == LinkingMode::LinkNone)
    return nullptr;

  bool LinkAll = Mode == LinkingMode::LinkAll;
  // First attempt to link in Fun. If we fail, bail.
  auto NewFn = SILLoader->lookupSILFunction(Fun);
  if (!NewFn)
    return false;
  ++NumFuncLinked;

  // Ok, we succeeded in linking in Fun. Transitively link in the functions that
  // Fun references.
  SmallVector<SILFunction *, 128> Worklist;
  Worklist.push_back(NewFn);
  while (!Worklist.empty()) {
    auto Fn = Worklist.pop_back_val();

    for (auto &BB : *Fn)
      for (auto I = BB.begin(), E = BB.end(); I != E; I++) {
        SILFunction *CalleeFunction = nullptr;
        bool TryLinking = false;
        if (ApplyInst *AI = dyn_cast<ApplyInst>(I)) {
          SILValue Callee = AI->getCallee();
          // Handles FunctionRefInst only.
          if (FunctionRefInst *FRI = dyn_cast<FunctionRefInst>(Callee.getDef())) {
            CalleeFunction = FRI->getReferencedFunction();
            // When EnableLinkAll is true, we always link the Callee.
            TryLinking = LinkAll || AI->isTransparent() ||
                         CalleeFunction->getLinkage() == SILLinkage::Shared;
          }
        } else if (PartialApplyInst *PAI = dyn_cast<PartialApplyInst>(I)) {
          SILValue Callee = PAI->getCallee();
          // Handles FunctionRefInst only.
          if (FunctionRefInst *FRI = dyn_cast<FunctionRefInst>(Callee.getDef())) {
            CalleeFunction = FRI->getReferencedFunction();
            // When EnableLinkAll is true, we always link the Callee.
            TryLinking = LinkAll || CalleeFunction->isTransparent() ||
                         CalleeFunction->getLinkage() == SILLinkage::Shared;
          } else {
            continue;
          }
        } else if (FunctionRefInst *FRI = dyn_cast<FunctionRefInst>(I)) {
          // When EnableLinkAll is true, we link the function referenced by
          // FunctionRefInst.
          CalleeFunction = LinkAll ? FRI->getReferencedFunction() :
                                           nullptr;
          TryLinking = LinkAll;
        }

        if (!CalleeFunction)
          continue;

        // The ExternalSource may wish to rewrite non-empty bodies.
        if (ExternalSource)
          if (auto NewFn = ExternalSource->lookupSILFunction(CalleeFunction)) {
            Worklist.push_back(NewFn);
            ++NumFuncLinked;
            continue;
          }

        CalleeFunction->setBare(IsBare);

        if (CalleeFunction->empty())
          // Try to find the definition in a serialized module when callee is
          // currently empty.
          if (TryLinking)
            if (auto NewFn = SILLoader->lookupSILFunction(CalleeFunction)) {
              Worklist.push_back(NewFn);
              ++NumFuncLinked;
              continue;
            }
      }
  }

  return true;
}
