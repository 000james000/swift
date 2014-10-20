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

#define DEBUG_TYPE "sil-module"
#include "swift/SIL/SILDebugScope.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILExternalSource.h"
#include "swift/SIL/SILVisitor.h"
#include "swift/Serialization/SerializedSILLoader.h"
#include "swift/SIL/SILValue.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Debug.h"
using namespace swift;
using namespace Lowering;

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
    
    // For globals we currently do not support available_externally.
    // In the interpreter it would result in two instances for a single global:
    // one in the imported module and one in the main module.
    var->setDeclaration(true);
  }

  void didDeserialize(Module *M, SILVTable *vtable) override {
    // TODO: should vtables get linkage?
    //updateLinkage(vtable);
  }

  void didDeserialize(Module *M, SILWitnessTable *wt) override {
    updateLinkage(wt);
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
      decl->setLinkage(SILLinkage::SharedExternal);
      return;
    case SILLinkage::Private:
        decl->setLinkage(SILLinkage::PrivateExternal);
        return;
    case SILLinkage::PublicExternal:
    case SILLinkage::HiddenExternal:
    case SILLinkage::SharedExternal:
    case SILLinkage::PrivateExternal:
      return;
    }
  }
};

SILModule::SILModule(Module *SwiftModule, const DeclContext *associatedDC,
                     bool wholeModule)
  : TheSwiftModule(SwiftModule), AssociatedDeclContext(associatedDC),
    Stage(SILStage::Raw), Callback(new SILModule::SerializationCallback()),
    wholeModule(wholeModule), Types(*this) {
  TypeListUniquing = new SILTypeListUniquingType();
}

SILModule::~SILModule() {
  // Decrement ref count for each SILGlobalVariable with static initializers.
  for (SILGlobalVariable &v : silGlobals)
    if (v.getInitializer())
      v.getInitializer()->decrementRefCount();

  // Drop everything functions in this module reference.
  //
  // This is necessary since the functions may reference each other.  We don't
  // need to worry about sil_witness_tables since witness tables reference each
  // other via protocol conformances and sil_vtables don't reference each other
  // at all.
  for (SILFunction &F : *this)
    F.dropAllReferences();

  delete (SILTypeListUniquingType*)TypeListUniquing;
}

SILWitnessTable *
SILModule::createWitnessTableDeclaration(ProtocolConformance *C,
                                         SILLinkage linkage) {
  // If we are passed in a null conformance (a valid value), just return nullptr
  // since we can not map a witness table to it.
  if (!C)
    return nullptr;

  // Walk down to the base NormalProtocolConformance.
  ProtocolConformance *ParentC = C;
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
  NormalProtocolConformance *NormalC
    = cast<NormalProtocolConformance>(ParentC);

  SILWitnessTable *WT = SILWitnessTable::create(*this,
                                                linkage,
                                                NormalC);
  return WT;
}

std::pair<SILWitnessTable *, ArrayRef<Substitution>>
SILModule::
lookUpWitnessTable(const ProtocolConformance *C, bool deserializeLazily) {
  // If we have a null conformance passed in (a legal value), just return
  // nullptr.
  ArrayRef<Substitution> Subs;
  if (!C)
    return {nullptr, Subs};

  // Walk down to the base NormalProtocolConformance.
  const ProtocolConformance *ParentC = C;
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

  // Attempt to lookup the witness table from the table.
  auto found = WitnessTableLookupCache.find(NormalC);
  if (found == WitnessTableLookupCache.end()) {
#ifndef NDEBUG
    // Make sure that all witness tables are in the witness table lookup
    // cache.
    //
    // This code should not be hit normally since we add witness tables to the
    // lookup cache when we create them. We don't just assert here since there
    // is the potential for a conformance without a witness table to be passed
    // to this function.
    for (SILWitnessTable &WT : witnessTables)
      assert(WT.getConformance() != NormalC &&
             "Found witness table that is not"
             " in the witness table lookup cache.");
#endif
    return {nullptr, Subs};
  }

  SILWitnessTable *wT = found->second;
  assert(wT != nullptr && "Should never map a conformance to a null witness"
                          " table.");

  // If we have a definition, return it.
  if (wT->isDefinition())
    return {wT, Subs};

  // Otherwise try to deserialize it. If we succeed return the deserialized
  // function.
  //
  // *NOTE* In practice, wT will be deserializedTable, but I do not want to rely
  // on that behavior for now.
  if (deserializeLazily)
    if (auto deserializedTable = getSILLoader()->lookupWitnessTable(wT))
      return {deserializedTable, Subs};

  // If we fail, just return the declaration.
  return {wT, Subs};
}

SILFunction *SILModule::getOrCreateFunction(SILLocation loc,
                                            StringRef name,
                                            SILLinkage linkage,
                                            CanSILFunctionType type,
                                            IsBare_t isBareSILFunction,
                                            IsTransparent_t isTransparent,
                                            IsFragile_t isFragile) {
  if (auto fn = lookUpFunction(name)) {
    assert(fn->getLoweredFunctionType() == type);
    assert(fn->getLinkage() == linkage);
    return fn;
  }

  auto fn = SILFunction::create(*this, linkage, name, type, nullptr,
                                loc, isBareSILFunction, isTransparent, isFragile);
  fn->setDebugScope(new (*this) SILDebugScope(loc, *fn));
  return fn;
}

SILFunction *SILModule::getOrCreateSharedFunction(SILLocation loc,
                                                  StringRef name,
                                                  CanSILFunctionType type,
                                                  IsBare_t isBareSILFunction,
                                                  IsTransparent_t isTransparent,
                                                  IsFragile_t isFragile) {
  return getOrCreateFunction(loc, name, SILLinkage::Shared,
                             type, isBareSILFunction, isTransparent, isFragile);
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

namespace {

/// Visitor that knows how to link in dependencies of SILInstructions.
class SILLinkerVisitor : public SILInstructionVisitor<SILLinkerVisitor, bool> {
  using LinkingMode = SILModule::LinkingMode;

  /// The SILModule that we are loading from.
  SILModule &Mod;

  /// The SILLoader that this visitor is using to link.
  SerializedSILLoader *Loader;

  /// The external SIL source to use when linking this module.
  SILExternalSource *ExternalSource = nullptr;

  /// Worklist of SILFunctions we are processing.
  llvm::SmallVector<SILFunction *, 128> Worklist;

  /// A list of callees of the current instruction being visited. cleared after
  /// every instruction is visited.
  llvm::SmallVector<SILFunction *, 4> FunctionDeserializationWorklist;

  /// The current linking mode.
  LinkingMode Mode;
public:

  SILLinkerVisitor(SILModule &M, SerializedSILLoader *L,
                   SILModule::LinkingMode LinkingMode,
                   SILExternalSource *E = nullptr)
    : Mod(M), Loader(L), ExternalSource(E), Worklist(),
      FunctionDeserializationWorklist(), Mode(LinkingMode) { }

  /// Process F, recursively deserializing any thing F may reference.
  bool processFunction(SILFunction *F) {
    if (Mode == LinkingMode::LinkNone)
      return false;

    // If F is a declaration, first deserialize it.
    auto NewFn = F->isExternalDeclaration() ? Loader->lookupSILFunction(F) : F;
    if (!NewFn || NewFn->empty())
      return false;

    ++NumFuncLinked;

    // Try to transitively deserialize everything referenced by NewFn.
    Worklist.push_back(NewFn);
    process();

    // Since we successfully processed at least one function, return true.
    return true;
  }

  /// Deserialize the VTable mapped to C if it exists and all SIL the VTable
  /// transitively references.
  ///
  /// This method assumes that the caller made sure that no vtable existed in
  /// Mod.
  SILVTable *processClassDecl(const ClassDecl *C) {
    // If we are not linking anything, bail.
    if (Mode == LinkingMode::LinkNone)
      return nullptr;

    // Attempt to load the VTable from the SerializedSILLoader. If we
    // fail... bail...
    SILVTable *Vtbl = Loader->lookupVTable(C);
    if (!Vtbl)
      return nullptr;

    // Otherwise, add all the vtable functions in Vtbl to the function
    // processing list...
    for (auto &E : Vtbl->getEntries())
      Worklist.push_back(E.second);

    // And then transitively deserialize all SIL referenced by those functions.
    process();

    // Return the deserialized Vtbl.
    return Vtbl;
  }

  /// We do not want to visit callee functions if we just have a value base.
  bool visitValueBase(ValueBase *V) { return false; }

  bool visitApplyInst(ApplyInst *AI) {
    // If we don't have a function ref inst, just return false. We do not have
    // interesting callees.
    auto *FRI = dyn_cast<FunctionRefInst>(AI->getCallee());
    if (!FRI)
      return false;

    // Ok we have a function ref inst, grab the callee.
    SILFunction *Callee = FRI->getReferencedFunction();

    // If the linking mode is not link all, AI is not transparent, and the
    // callee is not shared, we don't want to perform any linking.
    if (!isLinkAll() && !AI->isTransparent() &&
        !hasSharedVisibility(Callee->getLinkage()))
      return false;

    // Otherwise we want to try and link in the callee... Add it to the callee
    // list and return true.
    addFunctionToWorklist(Callee);
    return true;
  }

  bool visitPartialApplyInst(PartialApplyInst *PAI) {
    auto *FRI = dyn_cast<FunctionRefInst>(PAI->getCallee());
    if (!FRI)
      return false;

    SILFunction *Callee = FRI->getReferencedFunction();
    if (!isLinkAll() && !Callee->isTransparent() &&
        !hasSharedVisibility(Callee->getLinkage()))
      return false;

    addFunctionToWorklist(Callee);
    return true;
  }

  bool visitFunctionRefInst(FunctionRefInst *FRI) {
    // Needed to handle closures which are no longer applied, but are left
    // behind as dead code. This shouldn't happen, but if it does don't get into
    // an inconsistent state.
    SILFunction *Callee = FRI->getReferencedFunction();
    if (!isLinkAll() && !Callee->isTransparent() &&
        !hasSharedVisibility(Callee->getLinkage()))
      return false;

    addFunctionToWorklist(FRI->getReferencedFunction());
    return true;
  }

  bool visitProtocolConformance(ProtocolConformance *C,
                                const Optional<SILDeclRef> &Member) {
    // If a null protocol conformance was passed in, just return false.
    if (!C)
      return false;

    // Otherwise try and lookup a witness table for C.
    SILWitnessTable *WT = Mod.lookUpWitnessTable(C).first;

    // If we don't find any witness table for the conformance, bail and return
    // false.
    if (!WT) {
      Mod.createWitnessTableDeclaration(C,
                          TypeConverter::getLinkageForProtocolConformance(
                              C->getRootNormalConformance(), NotForDefinition));
      return false;
    }

    // If the looked up witness table is a declaration, there is nothing we can
    // do here. Just bail and return false.
    if (WT->isDeclaration())
      return false;

    bool performFuncDeserialization = false;
    // For each entry in the witness table...
    for (auto &E : WT->getEntries()) {
      // If the entry is a witness method...
      if (E.getKind() == SILWitnessTable::WitnessKind::Method) {
        // And we are only interested in deserializing a specific requirement
        // and don't have that requirement, don't deserialize this method.
        if (Member.hasValue() && E.getMethodWitness().Requirement != *Member)
          continue;

        // Otherwise if it is the requirement we are looking for or we just want
        // to deserialize everything, add the function to the list of functions
        // to deserialize.
        performFuncDeserialization = true;
        addFunctionToWorklist(E.getMethodWitness().Witness);
      }
    }

    return performFuncDeserialization;
  }

  bool visitWitnessMethodInst(WitnessMethodInst *WMI) {
    return visitProtocolConformance(WMI->getConformance(), WMI->getMember());
  }

  bool visitInitExistentialInst(InitExistentialInst *IEI) {
    // Link in all protocol conformances that this touches.
    //
    // TODO: There might be a two step solution where the init_existential_inst
    // causes the witness table to be brought in as a declaration and then the
    // protocol method inst causes the actual deserialization. For now we are
    // not going to be smart about this to enable avoiding any issues with
    // visiting the open_existential/witness_method before the
    // init_existential_inst.
    bool performFuncDeserialization = false;
    for (ProtocolConformance *C : IEI->getConformances()) {
      performFuncDeserialization |=
        visitProtocolConformance(C, Optional<SILDeclRef>());
    }
    return performFuncDeserialization;
  }

  bool visitInitExistentialRefInst(InitExistentialRefInst *IERI) {
    // Link in all protocol conformances that this touches.
    //
    // TODO: There might be a two step solution where the init_existential_inst
    // causes the witness table to be brought in as a declaration and then the
    // protocol method inst causes the actual deserialization. For now we are
    // not going to be smart about this to enable avoiding any issues with
    // visiting the protocol_method before the init_existential_inst.
    bool performFuncDeserialization = false;
    for (ProtocolConformance *C : IERI->getConformances()) {
      performFuncDeserialization |=
        visitProtocolConformance(C, Optional<SILDeclRef>());
    }
    return performFuncDeserialization;
  }

  bool visitAllocRefInst(AllocRefInst *ARI) {
    // Grab the class decl from the alloc ref inst.
    ClassDecl *D = ARI->getType().getClassOrBoundGenericClass();
    if (!D)
      return false;

    return linkInVTable(D);
  }

  bool visitMetatypeInst(MetatypeInst *MI) {
    CanType instTy = MI->getType().castTo<MetatypeType>().getInstanceType();
    ClassDecl *C = instTy.getClassOrBoundGenericClass();
    if (!C)
      return false;

    return linkInVTable(C);
  }

private:
  /// Add a function to our function worklist for processing.
  void addFunctionToWorklist(SILFunction *F) {
    FunctionDeserializationWorklist.push_back(F);
  }

  /// Is the current mode link all? Link all implies we should try and link
  /// everything, not just transparent/shared functions.
  bool isLinkAll() const { return Mode == LinkingMode::LinkAll; }

  bool linkInVTable(ClassDecl *D) {
    // Attempt to lookup the Vtbl from the SILModule.
    SILVTable *Vtbl = Mod.lookUpVTable(D);

    // If the SILModule does not have the VTable, attempt to deserialize the
    // VTable. If we fail to do that as well, bail.
    if (!Vtbl || !(Vtbl = Loader->lookupVTable(D->getName())))
      return false;

    // Ok we found our VTable. Visit each function referenced by the VTable. If
    // any of the functions are external declarations, add them to the worklist
    // for processing.
    bool Result = false;
    for (auto P : Vtbl->getEntries()) {
      if (P.second->isExternalDeclaration()) {
        Result = true;
        addFunctionToWorklist(P.second);
      }
    }
    return Result;
  }

  // Main loop of the visitor. Called by one of the other *visit* methods.
  bool process() {
    // Process everything transitively referenced by one of the functions in the
    // worklist.
    bool Result = false;
    while (!Worklist.empty()) {
      auto Fn = Worklist.pop_back_val();
      for (auto &BB : *Fn) {
        for (auto &I : BB) {
          // Should we try linking?
          if (visit(&I)) {
            for (auto F : FunctionDeserializationWorklist) {
              // The ExternalSource may wish to rewrite non-empty bodies.
              if (!F->empty() && ExternalSource)
                if (auto NewFn = ExternalSource->lookupSILFunction(F)) {
                  NewFn->verify();
                  Worklist.push_back(NewFn);
                  ++NumFuncLinked;
                  Result = true;
                  continue;
                }

              F->setBare(IsBare);

              if (F->empty())
                if (auto NewFn = Loader->lookupSILFunction(F)) {
                  NewFn->verify();
                  Worklist.push_back(NewFn);
                  Result = true;
                  ++NumFuncLinked;
                }
            }
            FunctionDeserializationWorklist.clear();
          } else {
            assert(FunctionDeserializationWorklist.empty() && "Worklist should "
                   "always be empty if visit does not return true.");
          }
        }
      }
    }

    // If we return true, we deserialized at least one function.
    return Result;
  }
};

} // end anonymous namespace.

SILFunction *SILModule::lookUpFunction(SILDeclRef fnRef) {
  llvm::SmallString<32> name;
  fnRef.mangle(name);
  return lookUpFunction(name);
}

bool SILModule::linkFunction(SILFunction *Fun, SILModule::LinkingMode Mode) {
  return SILLinkerVisitor(*this, getSILLoader(), Mode,
                          ExternalSource).processFunction(Fun);
}

void SILModule::linkAllWitnessTables() {
  getSILLoader()->getAllWitnessTables();
}

void SILModule::linkAllVTables() {
  getSILLoader()->getAllVTables();
}

void SILModule::invalidateSILLoaderCaches() {
  getSILLoader()->invalidateCaches();
}

/// Erase a function from the module.
void SILModule::eraseFunction(SILFunction *F) {

  assert(! F->isZombie() && "zombie function is in list of alive functions");
  if (F->isInlined()) {
    
    // The owner of the function's Name is the FunctionTable key. As we remove
    // the function from the table we have to store the name string elsewhere:
    // in zombieFunctionNames.
    StringRef copiedName = F->getName().copy(zombieFunctionNames);
    FunctionTable.erase(F->getName());
    F->Name = copiedName;
    
    // The function is dead, but as it is inlined we need it later (at IRGen)
    // for debug info generation. So we move it into the zombie list.
    getFunctionList().remove(F);
    zombieFunctions.push_back(F);
    F->markAsZombie();

    // This opens dead-function-removal opportunities for called functions.
    // (References are not needed for debug info generation.)
    F->dropAllReferences();
  } else {
    FunctionTable.erase(F->getName());
    getFunctionList().erase(F);
  }
}

SILVTable *SILModule::lookUpVTable(const ClassDecl *C) {
  if (!C)
    return nullptr;

  // First try to look up R from the lookup table.
  auto R = VTableLookupTable.find(C);
  if (R != VTableLookupTable.end())
    return R->second;

  // If that fails, try to deserialize it. If that fails, return nullptr.
  SILVTable *Vtbl = SILLinkerVisitor(*this, getSILLoader(),
                                     SILModule::LinkingMode::LinkAll,
                                     ExternalSource).processClassDecl(C);
  if (!Vtbl)
    return nullptr;

  // If we succeeded, map C -> VTbl in the table and return VTbl.
  VTableLookupTable[C] = Vtbl;
  return Vtbl;
}

SerializedSILLoader *SILModule::getSILLoader() {
  // If the SILLoader is null, create it.
  if (!SILLoader)
    SILLoader = SerializedSILLoader::create(getASTContext(), this,
                                            Callback.get());
  // Return the SerializedSILLoader.
  return SILLoader.get();
}


/// \brief Given a protocol \p Proto, a member method \p Member and a concrete
/// class type \p ConcreteTy, search the witness tables and return the static
/// function that matches the member with any specializations may be
/// required. Notice that we do not scan the class hierarchy, just the concrete
/// class type.
std::tuple<SILFunction *, SILWitnessTable *, ArrayRef<Substitution>>
SILModule::findFuncInWitnessTable(const ProtocolConformance *C,
                                  SILDeclRef Member) {
  // Look up the witness table associated with our protocol conformance from the
  // SILModule.
  auto Ret = lookUpWitnessTable(C);

  // If no witness table was found, bail.
  if (!Ret.first) {
    DEBUG(llvm::dbgs() << "        Failed speculative lookup of witness for: ";
          C->dump());
    return std::make_tuple(nullptr, nullptr, ArrayRef<Substitution>());
  }

  // Okay, we found the correct witness table. Now look for the method.
  for (auto &Entry : Ret.first->getEntries()) {
    // Look at method entries only.
    if (Entry.getKind() != SILWitnessTable::WitnessKind::Method)
      continue;

    SILWitnessTable::MethodWitness MethodEntry = Entry.getMethodWitness();
    // Check if this is the member we were looking for.
    if (MethodEntry.Requirement != Member)
      continue;

    return std::make_tuple(MethodEntry.Witness, Ret.first, Ret.second);
  }

  return std::make_tuple(nullptr, nullptr, ArrayRef<Substitution>());
}

static ClassDecl *getClassDeclSuperClass(ClassDecl *Class) {
  Type T = Class->getSuperclass();
  if (!T)
    return nullptr;
  return T->getCanonicalType()->getClassOrBoundGenericClass();
}

SILFunction *
SILModule::
lookUpSILFunctionFromVTable(ClassDecl *Class, SILDeclRef Member) {
  // Until we reach the top of the class hierarchy...
  while (Class) {
    // Try to lookup a VTable for Class from the module...
    auto *Vtbl = lookUpVTable(Class);

    // If the lookup fails, skip Class and attempt to resolve the method in
    // the VTable of the super class of Class if it exists...
    if (!Vtbl) {
      Class = getClassDeclSuperClass(Class);
      continue;
    }

    // Ok, we have a VTable. Try to lookup the SILFunction implementation from
    // the VTable.
    if (SILFunction *F = Vtbl->getImplementation(*this, Member))
      return F;

    // If we fail to lookup the SILFunction, again skip Class and attempt to
    // resolve the method in the VTable of the super class of Class if such a
    // super class exists.
    Class = getClassDeclSuperClass(Class);
  }

  return nullptr;
}
