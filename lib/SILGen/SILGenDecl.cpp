//===--- SILGenDecl.cpp - Implements Lowering of ASTs -> SIL for Decls ----===//
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

#include "SILGen.h"
#include "Initialization.h"
#include "RValue.h"
#include "Scope.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILType.h"
#include "swift/SIL/TypeLowering.h"
#include "swift/AST/AST.h"
#include "swift/AST/Mangle.h"
#include "swift/AST/NameLookup.h"
#include "swift/Basic/Fallthrough.h"
#include <iterator>
using namespace swift;
using namespace Mangle;
using namespace Lowering;

void Initialization::_anchor() {}

namespace {
  /// A "null" initialization that indicates that any value being initialized
  /// into this initialization should be discarded. This represents AnyPatterns
  /// (that is, 'var (_)') that bind to values without storing them.
  class BlackHoleInitialization : public Initialization {
  public:
    BlackHoleInitialization()
      : Initialization(Initialization::Kind::Ignored)
    {}
    
    SILValue getAddressOrNull() const override { return SILValue(); }
    ArrayRef<InitializationPtr> getSubInitializations() override {
      return {};
    }
  };
  

  /// An Initialization subclass used to destructure tuple initializations.
  class TupleElementInitialization : public SingleBufferInitialization {
  public:
    SILValue ElementAddr;
    
    TupleElementInitialization(SILValue addr)
      : ElementAddr(addr)
    {}
    
    SILValue getAddressOrNull() const override { return ElementAddr; }

    void finishInitialization(SILGenFunction &gen) override {}
  };
}

ArrayRef<InitializationPtr>
Initialization::getSubInitializationsForTuple(SILGenFunction &gen, CanType type,
                                      SmallVectorImpl<InitializationPtr> &buf,
                                      SILLocation Loc) {
  assert(canSplitIntoSubelementAddresses() && "Client shouldn't call this");
  auto tupleTy = cast<TupleType>(type);
  switch (kind) {
  case Kind::Tuple:
    return getSubInitializations();
  case Kind::Ignored:
    // "Destructure" an ignored binding into multiple ignored bindings.
    for (auto fieldType : tupleTy->getElementTypes()) {
      (void) fieldType;
      buf.push_back(InitializationPtr(new BlackHoleInitialization()));
    }
    return buf;
  case Kind::LetValue:
  case Kind::SingleBuffer: {
    // Destructure the buffer into per-element buffers.
    SILValue baseAddr = getAddress();
    for (unsigned i = 0, size = tupleTy->getNumElements(); i < size; ++i) {
      auto fieldType = tupleTy.getElementType(i);
      SILType fieldTy = gen.getLoweredType(fieldType).getAddressType();
      SILValue fieldAddr = gen.B.createTupleElementAddr(Loc,
                                                        baseAddr, i,
                                                        fieldTy);
                          
      buf.push_back(InitializationPtr(new
                                        TupleElementInitialization(fieldAddr)));
    }
    finishInitialization(gen);
    return buf;
  }
  case Kind::Translating:
    // This could actually be done by collecting translated values, if
    // we introduce new needs for translating initializations.
    llvm_unreachable("cannot destructure a translating initialization");
  case Kind::AddressBinding:
    llvm_unreachable("cannot destructure an address binding initialization");
  }
  llvm_unreachable("bad initialization kind");
}

namespace {
  class CleanupClosureConstant : public Cleanup {
    SILValue closure;
  public:
    CleanupClosureConstant(SILValue closure) : closure(closure) {}
    void emit(SILGenFunction &gen, CleanupLocation l) override {
      gen.B.emitStrongRelease(l, closure);
    }
  };
}

ArrayRef<Substitution> SILGenFunction::getForwardingSubstitutions() {
  if (auto gp = F.getContextGenericParams())
    return buildForwardingSubstitutions(gp);
  return {};
}

void SILGenFunction::visitFuncDecl(FuncDecl *fd) {
  // Generate the local function body.
  SGM.emitFunction(fd);
  
  // If there are captures, build the local closure value for the function and
  // store it as a local constant.
  if (fd->getCaptureInfo().hasLocalCaptures()) {
    SILValue closure = emitClosureValue(fd, SILDeclRef(fd),
                                        getForwardingSubstitutions(), fd)
      .forward(*this);
    Cleanups.pushCleanup<CleanupClosureConstant>(closure);
    LocalFunctions[SILDeclRef(fd)] = closure;
  }
}

ArrayRef<InitializationPtr>
SingleBufferInitialization::getSubInitializations() {
  return {};
}

void TemporaryInitialization::finishInitialization(SILGenFunction &gen) {
  if (Cleanup.isValid())
    gen.Cleanups.setCleanupState(Cleanup, CleanupState::Active);
};

namespace {

/// An Initialization of a tuple pattern, such as "var (a,b)".
class TupleInitialization : public Initialization {
public:
  /// The sub-Initializations aggregated by this tuple initialization.
  /// The TupleInitialization object takes ownership of Initializations pushed
  /// here.
  SmallVector<InitializationPtr, 4> subInitializations;

  TupleInitialization() : Initialization(Initialization::Kind::Tuple) {}
  
  SILValue getAddressOrNull() const override {
    if (subInitializations.size() == 1)
      return subInitializations[0]->getAddressOrNull();
    else
      return SILValue();
  }
  
  ArrayRef<InitializationPtr> getSubInitializations() override {
    return subInitializations;
  }
  
  void finishInitialization(SILGenFunction &gen) override {
    for (auto &sub : subInitializations)
      sub->finishInitialization(gen);
  }
};

class StrongReleaseCleanup : public Cleanup {
  SILValue box;
public:
  StrongReleaseCleanup(SILValue box) : box(box) {}
  void emit(SILGenFunction &gen, CleanupLocation l) override {
    gen.B.emitStrongRelease(l, box);
  }
};

class DestroyValueCleanup : public Cleanup {
  SILValue v;
public:
  DestroyValueCleanup(SILValue v) : v(v) {}
  
  void emit(SILGenFunction &gen, CleanupLocation l) override {
    if (v.getType().isAddress())
      gen.B.emitDestroyAddr(l, v);
    else
      gen.B.emitDestroyValueOperation(l, v);
  }
};

/// Cleanup to destroy an initialized variable.
class DeallocStackCleanup : public Cleanup {
  SILValue Addr;
public:
  DeallocStackCleanup(SILValue addr) : Addr(addr) {}

  void emit(SILGenFunction &gen, CleanupLocation l) override {
    gen.B.createDeallocStack(l, Addr);
  }
};

/// Cleanup to destroy an initialized 'var' variable.
class DestroyLocalVariable : public Cleanup {
  VarDecl *Var;
public:
  DestroyLocalVariable(VarDecl *var) : Var(var) {}
  
  void emit(SILGenFunction &gen, CleanupLocation l) override {
    gen.destroyLocalVariable(l, Var);
  }
};

/// Cleanup to destroy an uninitialized local variable.
class DeallocateUninitializedLocalVariable : public Cleanup {
  VarDecl *Var;
public:
  DeallocateUninitializedLocalVariable(VarDecl *var) : Var(var) {}
  
  void emit(SILGenFunction &gen, CleanupLocation l) override {
    gen.deallocateUninitializedLocalVariable(l, Var);
  }
};
  
/// An initialization of a local 'var'.
class LocalVariableInitialization : public SingleBufferInitialization {
  /// The local variable decl being initialized.
  VarDecl *Var;
  SILGenFunction &Gen;

  /// The cleanup we pushed to deallocate the local variable before it
  /// gets initialized.
  CleanupHandle DeallocCleanup;

  /// The cleanup we pushed to destroy and deallocate the local variable.
  CleanupHandle ReleaseCleanup;
  
  bool DidFinish = false;
public:
  /// Sets up an initialization for the allocated box. This pushes a
  /// CleanupUninitializedBox cleanup that will be replaced when
  /// initialization is completed.
  LocalVariableInitialization(VarDecl *var, SILGenFunction &gen)
    : Var(var), Gen(gen) {
    // Push a cleanup to destroy the local variable.  This has to be
    // inactive until the variable is initialized.
    gen.Cleanups.pushCleanupInState<DestroyLocalVariable>(CleanupState::Dormant,
                                                          var);
    ReleaseCleanup = gen.Cleanups.getTopCleanup();

    // Push a cleanup to deallocate the local variable.
    gen.Cleanups.pushCleanup<DeallocateUninitializedLocalVariable>(var);
    DeallocCleanup = gen.Cleanups.getTopCleanup();
  }
  
  ~LocalVariableInitialization() override {
    assert(DidFinish && "did not call VarInit::finishInitialization!");
  }
  
  SILValue getAddressOrNull() const override {
    assert(Gen.VarLocs.count(Var) && "did not emit var?!");
    return Gen.VarLocs[Var].getAddress();
  }

  void finishInitialization(SILGenFunction &gen) override {
    assert(!DidFinish &&
           "called LocalVariableInitialization::finishInitialization twice!");
    Gen.Cleanups.setCleanupState(DeallocCleanup, CleanupState::Dead);
    Gen.Cleanups.setCleanupState(ReleaseCleanup, CleanupState::Active);
    DidFinish = true;
  }
};

/// Initialize a writeback buffer that receives the value of a 'let'
/// declaration.
class LetValueInitialization : public Initialization {
  /// The VarDecl for the let decl.
  VarDecl *vd;

  /// The address of the buffer used for the binding, if this is an address-only
  /// let.
  SILValue address;

  /// The cleanup we pushed to destroy the local variable.
  CleanupHandle DestroyCleanup;
  
  bool DidFinish = false;

public:
  LetValueInitialization(VarDecl *vd, bool isArgument, SILGenFunction &gen)
    : Initialization(Initialization::Kind::LetValue), vd(vd) {

    auto &lowering = gen.getTypeLowering(vd->getType());

    // If this is an address-only let declaration for a real let declaration
    // (not a function argument), create a buffer to bind the expression value
    // assigned into this slot.
    bool needsTemporaryBuffer = lowering.isAddressOnly();

    // If this is a function argument, we don't usually need a temporary buffer
    // because the incoming pointer can be directly bound as our let buffer.
    // However, if this VarDecl has tuple type, then it will be passed to the
    // SILFunction as multiple SILArguments, and those *do* need to be rebound
    // into a temporary buffer.
    if (isArgument && !vd->getType()->is<TupleType>())
      needsTemporaryBuffer = false;

    if (needsTemporaryBuffer) {
      address = gen.emitTemporaryAllocation(vd, lowering.getLoweredType());
      gen.enterDormantTemporaryCleanup(address, lowering);
      gen.VarLocs[vd] = SILGenFunction::VarLoc::getConstant(address);
    } else {
      // Push a cleanup to destroy the let declaration.  This has to be
      // inactive until the variable is initialized: if control flow exits the
      // before the value is bound, we don't want to destroy the value.
      gen.Cleanups.pushCleanupInState<DestroyLocalVariable>(
                                                    CleanupState::Dormant, vd);
    }
    DestroyCleanup = gen.Cleanups.getTopCleanup();
  }
  
  ~LetValueInitialization() override {
    assert(DidFinish && "did not call LetValueInit::finishInitialization!");
  }

  void emitDebugValue(SILValue v, SILGenFunction &gen) {
    // Emit a debug_value[_addr] instruction to record the start of this value's
    // lifetime.
    if (!v.getType().isAddress())
      gen.B.createDebugValue(vd, v);
    else
      gen.B.createDebugValueAddr(vd, v);
  }

  SILValue getAddressOrNull() const override {
    // We only have an address for address-only lets.
    return address;
  }
  ArrayRef<InitializationPtr> getSubInitializations() override {
    return {};
  }
  
  void bindValue(SILValue value, SILGenFunction &gen) override {
    assert(!gen.VarLocs.count(vd) && "Already emitted this vardecl?");
    gen.VarLocs[vd] = SILGenFunction::VarLoc::getConstant(value);

    emitDebugValue(value, gen);
  }

  void finishInitialization(SILGenFunction &gen) override {
    assert(!DidFinish &&
           "called LetValueInit::finishInitialization twice!");
    assert(gen.VarLocs.count(vd) && "Didn't bind a value to this let!");
    gen.Cleanups.setCleanupState(DestroyCleanup, CleanupState::Active);
    DidFinish = true;
  }
};

/// An initialization for a global variable.
class GlobalInitialization : public SingleBufferInitialization {
  /// The physical address of the global.
  SILValue address;
  
public:
  GlobalInitialization(SILValue address) : address(address)
  {}
  
  SILValue getAddressOrNull() const override {
    return address;
  }
  
  void finishInitialization(SILGenFunction &gen) override {
    // Globals don't need to be cleaned up.
  }
};
  
/// Cleanup that writes back to a inout argument on function exit.
class CleanupWriteBackToInOut : public Cleanup {
  VarDecl *var;
  SILValue inoutAddr;

public:
  CleanupWriteBackToInOut(VarDecl *var, SILValue inoutAddr)
    : var(var), inoutAddr(inoutAddr) {}
  
  void emit(SILGenFunction &gen, CleanupLocation l) override {
    // Assign from the local variable to the inout address with an
    // 'autogenerated' copyaddr.
    l.markAutoGenerated();
    gen.B.createCopyAddr(l, gen.VarLocs[var].getAddress(), inoutAddr,
                         IsNotTake, IsNotInitialization);
  }
};
  
/// Initialize a writeback buffer that receives the "in" value of a inout
/// argument on function entry and writes the "out" value back to the inout
/// address on function exit.
class InOutInitialization : public Initialization {
  /// The VarDecl for the inout symbol.
  VarDecl *vd;
public:
  InOutInitialization(VarDecl *vd)
    : Initialization(Initialization::Kind::AddressBinding), vd(vd) {}
  
  SILValue getAddressOrNull() const override {
    llvm_unreachable("inout argument should be bound by bindAddress");
  }
  ArrayRef<InitializationPtr> getSubInitializations() override {
    return {};
  }

  void bindAddress(SILValue address, SILGenFunction &gen,
                   SILLocation loc) override {
    // Allocate the local variable for the inout.
    auto initVar = gen.emitLocalVariableWithCleanup(vd);
    
    // Initialize with the value from the inout with an "autogenerated"
    // copyaddr.
    loc.markAsPrologue();
    loc.markAutoGenerated();
    gen.B.createCopyAddr(loc, address, initVar->getAddress(),
                         IsNotTake, IsInitialization);
    initVar->finishInitialization(gen);
    
    // Set up a cleanup to write back to the inout.
    gen.Cleanups.pushCleanup<CleanupWriteBackToInOut>(vd, address);
  }
};

/// Initialize a variable of reference-storage type.
class ReferenceStorageInitialization : public Initialization {
  InitializationPtr VarInit;
public:
  ReferenceStorageInitialization(InitializationPtr &&subInit)
    : Initialization(Initialization::Kind::Translating),
      VarInit(std::move(subInit)) {}

  ArrayRef<InitializationPtr> getSubInitializations() override { return {}; }
  SILValue getAddressOrNull() const override { return SILValue(); }

  void translateValue(SILGenFunction &gen, SILLocation loc,
                      ManagedValue value) override {
    value.forwardInto(gen, loc, VarInit->getAddress());
  }

  void finishInitialization(SILGenFunction &gen) override {
    VarInit->finishInitialization(gen);
  }
};

/// InitializationForPattern - A visitor for traversing a pattern, generating
/// SIL code to allocate the declared variables, and generating an
/// Initialization representing the needed initializations. 
struct InitializationForPattern
  : public PatternVisitor<InitializationForPattern, InitializationPtr>
{
  SILGenFunction &Gen;
  enum ArgumentOrVar_t { Argument, Var } ArgumentOrVar;
  InitializationForPattern(SILGenFunction &Gen, ArgumentOrVar_t ArgumentOrVar)
    : Gen(Gen), ArgumentOrVar(ArgumentOrVar) {}
  
  // Paren, Typed, and Var patterns are noops, just look through them.
  InitializationPtr visitParenPattern(ParenPattern *P) {
    return visit(P->getSubPattern());
  }
  InitializationPtr visitTypedPattern(TypedPattern *P) {
    return visit(P->getSubPattern());
  }
  InitializationPtr visitVarPattern(VarPattern *P) {
    return visit(P->getSubPattern());
  }
  
  // AnyPatterns (i.e, _) don't require any storage. Any value bound here will
  // just be dropped.
  InitializationPtr visitAnyPattern(AnyPattern *P) {
    return InitializationPtr(new BlackHoleInitialization());
  }
  
  // Bind to a named pattern by creating a memory location and initializing it
  // with the initial value.
  InitializationPtr visitNamedPattern(NamedPattern *P) {
    return Gen.emitInitializationForVarDecl(P->getDecl(),
                                            ArgumentOrVar == Argument,
                                            P->hasType() ? P->getType() : Type());
  }
  
  // Bind a tuple pattern by aggregating the component variables into a
  // TupleInitialization.
  InitializationPtr visitTuplePattern(TuplePattern *P) {
    TupleInitialization *init = new TupleInitialization();
    for (auto &elt : P->getFields())
      init->subInitializations.push_back(visit(elt.getPattern()));
    return InitializationPtr(init);
  }
  
  // TODO: Handle bindings from 'case' labels and match expressions.
#define INVALID_PATTERN(Id, Parent) \
  InitializationPtr visit##Id##Pattern(Id##Pattern *) { \
    llvm_unreachable("pattern not valid in argument or var binding"); \
  }
#define PATTERN(Id, Parent)
#define REFUTABLE_PATTERN(Id, Parent) INVALID_PATTERN(Id, Parent)
#include "swift/AST/PatternNodes.def"
#undef INVALID_PATTERN
};

} // end anonymous namespace

InitializationPtr
SILGenFunction::emitInitializationForVarDecl(VarDecl *vd, bool isArgument,
                                             Type patternType) {
  // If this is a computed variable, we don't need to do anything here.
  // We'll generate the getter and setter when we see their FuncDecls.
  if (!vd->hasStorage())
    return InitializationPtr(new BlackHoleInitialization());

  // If this is a global variable, initialize it without allocations or
  // cleanups.
  if (!vd->getDeclContext()->isLocalContext()) {
    SILValue addr = B.createGlobalAddr(vd, vd,
                                getLoweredType(vd->getType()).getAddressType());
    
    // In a top level context, all global variables must be initialized.
    addr = B.createMarkUninitializedGlobalVar(vd, addr);
    
    VarLocs[vd] = SILGenFunction::VarLoc::getAddress(addr);
    return InitializationPtr(new GlobalInitialization(addr));
  }

  CanType varType = vd->getType()->getCanonicalType();

  // If this is an @inout parameter, set up the writeback variable.
  if (isa<InOutType>(varType))
    return InitializationPtr(new InOutInitialization(vd));

  // If this is a 'let' initialization for a non-address-only type, set up a
  // let binding, which stores the initialization value into VarLocs directly.
  if (vd->isLet()) {
    return InitializationPtr(new LetValueInitialization(vd, isArgument, *this));
  }
  
  // Otherwise, we have a normal local-variable initialization.
  auto varInit = emitLocalVariableWithCleanup(vd);

  // Initializing a @weak or @unowned variable requires a change in type.
  if (isa<ReferenceStorageType>(varType))
    return InitializationPtr(new ReferenceStorageInitialization(
                                                       std::move(varInit)));

  // Otherwise, the pattern type should match the type of the variable.
  // FIXME: why do we ever get patterns without types here?
  assert(!patternType || varType == patternType->getCanonicalType());
  return varInit;
}

void SILGenFunction::visitPatternBindingDecl(PatternBindingDecl *D) {
  // Allocate the variables and build up an Initialization over their
  // allocated storage.
  InitializationPtr initialization =
    InitializationForPattern(*this, InitializationForPattern::Var)
      .visit(D->getPattern());
  
  // If an initial value expression was specified by the decl, emit it into
  // the initialization. Otherwise, leave it uninitialized for DI to resolve.
  if (D->getInit()) {
    FullExpr Scope(Cleanups, CleanupLocation(D->getInit()));
    emitExprInto(D->getInit(), initialization.get());
  } else {
    initialization->finishInitialization(*this);
  }
}

InitializationPtr
SILGenFunction::emitPatternBindingInitialization(Pattern *P) {
  return InitializationForPattern(*this, InitializationForPattern::Var)
      .visit(P);
}

/// Enter a cleanup to deallocate the given location.
CleanupHandle SILGenFunction::enterDeallocStackCleanup(SILValue temp) {
  assert(temp.getType().isLocalStorage() &&
         "must deallocate container operand, not address operand!");
  Cleanups.pushCleanup<DeallocStackCleanup>(temp);
  return Cleanups.getTopCleanup();
}

CleanupHandle SILGenFunction::enterDestroyCleanup(SILValue valueOrAddr) {
  Cleanups.pushCleanup<DestroyValueCleanup>(valueOrAddr);
  return Cleanups.getTopCleanup();
}


namespace {

/// A visitor for traversing a pattern, creating
/// SILArguments, and initializing the local value for each pattern variable
/// in a function argument list.
struct ArgumentInitVisitor :
  public PatternVisitor<ArgumentInitVisitor, /*RetTy=*/ void,
                        /*Args...=*/ Initialization*>
{
  SILGenFunction &gen;
  SILFunction &f;
  SILBuilder &initB;
  ArgumentInitVisitor(SILGenFunction &gen, SILFunction &f)
    : gen(gen), f(f), initB(gen.B) {}

  RValue makeArgument(Type ty, SILBasicBlock *parent, SILLocation l) {
    assert(ty && "no type?!");
    return RValue::emitBBArguments(ty->getCanonicalType(), gen, parent, l);
  }


  /// Create a SILArgument and store its value into the given Initialization,
  /// if not null.
  void makeArgumentInto(Type ty, SILBasicBlock *parent,
                        SILLocation loc, Initialization *I) {
    assert(I && "no initialization?");
    assert(ty && "no type?!");
    loc.markAsPrologue();

    RValue argrv = makeArgument(ty, parent, loc);

    if (I->kind == Initialization::Kind::AddressBinding) {
      SILValue arg = std::move(argrv).forwardAsSingleValue(gen, loc);
      I->bindAddress(arg, gen, loc);
      // If this is an address-only non-inout argument, we take ownership
      // of the referenced value.
      if (!ty->is<InOutType>())
        gen.enterDestroyCleanup(arg);
      I->finishInitialization(gen);
    } else {
      std::move(argrv).forwardInto(gen, I, loc);
    }
  }
    
  // Paren, Typed, and Var patterns are no-ops. Just look through them.
  void visitParenPattern(ParenPattern *P, Initialization *I) {
    visit(P->getSubPattern(), I);
  }
  void visitTypedPattern(TypedPattern *P, Initialization *I) {
    visit(P->getSubPattern(), I);
  }
  void visitVarPattern(VarPattern *P, Initialization *I) {
    visit(P->getSubPattern(), I);
  }

  void visitTuplePattern(TuplePattern *P, Initialization *I) {
    // If the tuple is empty, so should be our initialization. Just pass an
    // empty tuple upwards.
    if (P->getFields().empty()) {
      switch (I->kind) {
      case Initialization::Kind::Ignored:
        break;
      case Initialization::Kind::Tuple:
        assert(I->getSubInitializations().empty() &&
               "empty tuple pattern with non-empty-tuple initializer?!");
        break;
      case Initialization::Kind::AddressBinding:
        llvm_unreachable("empty tuple pattern with inout initializer?!");
      case Initialization::Kind::LetValue:
        llvm_unreachable("empty tuple pattern with letvalue initializer?!");
      case Initialization::Kind::Translating:
        llvm_unreachable("empty tuple pattern with translating initializer?!");
        
      case Initialization::Kind::SingleBuffer:
        assert(I->getAddress().getType().getSwiftRValueType()
                 == P->getType()->getCanonicalType()
               && "empty tuple pattern with non-empty-tuple initializer?!");
        break;
      }
      return;
    }
    
    // Destructure the initialization into per-element Initializations.
    SmallVector<InitializationPtr, 2> buf;
    ArrayRef<InitializationPtr> subInits =
      I->getSubInitializationsForTuple(gen, P->getType()->getCanonicalType(),
                                       buf, RegularLocation(P));

    assert(P->getFields().size() == subInits.size() &&
           "TupleInitialization size does not match tuple pattern size!");
    for (size_t i = 0, size = P->getFields().size(); i < size; ++i)
      visit(P->getFields()[i].getPattern(), subInits[i].get());
  }

  void visitAnyPattern(AnyPattern *P, Initialization *I) {
    // A value bound to _ is unused and can be immediately released.
    assert(I->kind == Initialization::Kind::Ignored &&
           "any pattern should match a black-hole Initialization");
    auto &lowering = gen.getTypeLowering(P->getType());

    auto &AC = gen.getASTContext();
    auto VD = new (AC) VarDecl(/*static*/ false, /*IsLet*/ true, SourceLoc(),
                               // FIXME: we should probably number them.
                               AC.getIdentifier("_"), P->getType(),
                               f.getDeclContext());

    SILValue arg =
      makeArgument(P->getType(), f.begin(), VD).forwardAsSingleValue(gen, VD);
    lowering.emitDestroyRValue(gen.B, P, arg);
  }

  void visitNamedPattern(NamedPattern *P, Initialization *I) {
    makeArgumentInto(P->getType(), f.begin(), P->getDecl(), I);
  }
  
#define PATTERN(Id, Parent)
#define REFUTABLE_PATTERN(Id, Parent) \
  void visit##Id##Pattern(Id##Pattern *, Initialization *) { \
    llvm_unreachable("pattern not valid in argument binding"); \
  }
#include "swift/AST/PatternNodes.def"

};

/// Tuple values captured by a closure are passed as individual arguments to the
/// SILFunction since SILFunctionType canonicalizes away tuple types.
static SILValue
emitReconstitutedConstantCaptureArguments(SILType ty,
                                          ValueDecl *capture,
                                          SILGenFunction &gen) {
  auto TT = ty.getAs<TupleType>();
  if (!TT)
    return new (gen.SGM.M) SILArgument(ty, gen.F.begin(), capture);
  
  SmallVector<SILValue, 4> Elts;
  for (unsigned i = 0, e = TT->getNumElements(); i != e; ++i) {
    auto EltTy = ty.getTupleElementType(i);
    auto EV =
      emitReconstitutedConstantCaptureArguments(EltTy, capture, gen);
    Elts.push_back(EV);
  }
  
  return gen.B.createTuple(capture, ty, Elts);
}
  
static void emitCaptureArguments(SILGenFunction &gen, ValueDecl *capture) {
  ASTContext &c = capture->getASTContext();
  switch (getDeclCaptureKind(capture)) {
  case CaptureKind::None:
    break;

  case CaptureKind::Constant: {
    if (!gen.getTypeLowering(capture->getType()).isAddressOnly()) {
      // Constant decls are captured by value.  If the captured value is a tuple
      // value, we need to reconstitute it before sticking it in VarLocs.
      SILType ty = gen.getLoweredType(capture->getType());
      SILValue val = emitReconstitutedConstantCaptureArguments(ty, capture,gen);
      gen.VarLocs[capture] = SILGenFunction::VarLoc::getConstant(val);
      gen.enterDestroyCleanup(val);
      break;
    }
    // Address-only values we capture by-box since partial_apply doesn't work
    // with @in for address-only types.
    SWIFT_FALLTHROUGH;
  }

  case CaptureKind::Box: {
    // LValues are captured as two arguments: a retained ObjectPointer that owns
    // the captured value, and the address of the value itself.
    SILType ty = gen.getLoweredType(capture->getType()).getAddressType();
    SILValue box = new (gen.SGM.M) SILArgument(SILType::getObjectPointerType(c),
                                               gen.F.begin(), capture);
    SILValue addr = new (gen.SGM.M) SILArgument(ty, gen.F.begin(), capture);
    gen.VarLocs[capture] = SILGenFunction::VarLoc::getAddress(addr, box);
    gen.Cleanups.pushCleanup<StrongReleaseCleanup>(box);
    break;
  }
  case CaptureKind::LocalFunction: {
    // Local functions are captured by value.
    assert(!capture->getType()->is<LValueType>() &&
           !capture->getType()->is<InOutType>() &&
           "capturing inout by value?!");
    const TypeLowering &ti = gen.getTypeLowering(capture->getType());
    SILValue value = new (gen.SGM.M) SILArgument(ti.getLoweredType(),
                                                 gen.F.begin(), capture);
    gen.LocalFunctions[SILDeclRef(capture)] = value;
    gen.enterDestroyCleanup(value);
    break;
  }
  case CaptureKind::GetterSetter: {
    // Capture the setter and getter closures by value.
    Type setTy = cast<AbstractStorageDecl>(capture)->getSetter()->getType();
    SILType lSetTy = gen.getLoweredType(setTy);
    SILValue value = new (gen.SGM.M) SILArgument(lSetTy, gen.F.begin(),capture);
    gen.LocalFunctions[SILDeclRef(capture, SILDeclRef::Kind::Setter)] = value;
    gen.enterDestroyCleanup(value);
    SWIFT_FALLTHROUGH;
  }
  case CaptureKind::Getter: {
    // Capture the getter closure by value.
    Type getTy = cast<AbstractStorageDecl>(capture)->getGetter()->getType();
    SILType lGetTy = gen.getLoweredType(getTy);
    SILValue value = new (gen.SGM.M) SILArgument(lGetTy, gen.F.begin(),capture);
    gen.LocalFunctions[SILDeclRef(capture, SILDeclRef::Kind::Getter)] = value;
    gen.enterDestroyCleanup(value);
    break;
  }
  }
}
  
} // end anonymous namespace

void SILGenFunction::emitProlog(AnyFunctionRef TheClosure,
                                ArrayRef<Pattern *> paramPatterns,
                                Type resultType) {
  emitProlog(paramPatterns, resultType, TheClosure.getAsDeclContext());

  // Emit the capture argument variables. These are placed last because they
  // become the first curry level of the SIL function.
  SmallVector<ValueDecl*, 4> LocalCaptures;
  TheClosure.getCaptureInfo().getLocalCaptures(LocalCaptures);
  for (auto capture : LocalCaptures)
    emitCaptureArguments(*this, capture);
}

void SILGenFunction::emitProlog(ArrayRef<Pattern *> paramPatterns,
                                Type resultType, DeclContext *DeclCtx) {
  // If the return type is address-only, emit the indirect return argument.
  const TypeLowering &returnTI = getTypeLowering(resultType);
  if (returnTI.isReturnedIndirectly()) {
    auto &AC = getASTContext();
    auto VD = new (AC) VarDecl(/*static*/ false, /*IsLet*/ false, SourceLoc(),
                               AC.getIdentifier("$return_value"), resultType,
                               DeclCtx);
    IndirectReturnAddress = new (SGM.M)
      SILArgument(returnTI.getLoweredType(), F.begin(), VD);
  }
  
  // Emit the argument variables in calling convention order.
  for (Pattern *p : reversed(paramPatterns)) {
    // Allocate the local mutable argument storage and set up an Initialization.
    InitializationPtr argInit
      = InitializationForPattern(*this, InitializationForPattern::Argument)
         .visit(p);
    // Add the SILArguments and use them to initialize the local argument
    // values.
    ArgumentInitVisitor(*this, F).visit(p, argInit.get());
  }
}

SILValue SILGenFunction::emitSelfDecl(VarDecl *selfDecl) {
  // Emit the implicit 'self' argument.
  SILType selfType = getLoweredLoadableType(selfDecl->getType());
  SILValue selfValue = new (SGM.M) SILArgument(selfType, F.begin(), selfDecl);
  VarLocs[selfDecl] = VarLoc::getConstant(selfValue);
  B.createDebugValue(selfDecl, selfValue);
  return selfValue;
}

void SILGenFunction::prepareEpilog(Type resultType, CleanupLocation CleanupL) {
  auto *epilogBB = createBasicBlock();
  
  // If we have a non-null, non-void, non-address-only return type, receive the
  // return value via a BB argument.
  NeedsReturn = resultType && !resultType->isVoid();
  if (NeedsReturn) {
    auto &resultTI = getTypeLowering(resultType);
    if (!resultTI.isAddressOnly())
      new (F.getModule()) SILArgument(resultTI.getLoweredType(), epilogBB);
  }
  ReturnDest = JumpDest(epilogBB, getCleanupsDepth(), CleanupL);
}

/// Determine whether this is a generic function that isn't generic simply
/// because it's in a protocol.
///
/// FIXME: This whole thing is a hack. Sema should be properly
/// annotating as [objc] only those functions that can have Objective-C
/// entry points.
static bool isNonProtocolGeneric(AbstractFunctionDecl *func) {
  // If the function type is polymorphic and the context isn't a protocol
  // (where everything has an implicit single level of genericity),
  // it's generic.
  if (func->getType()->is<PolymorphicFunctionType>() &&
      !isa<ProtocolDecl>(func->getDeclContext()))
    return true;

  // Is this a polymorphic function within a non-generic type?
  if (func->getType()->castTo<AnyFunctionType>()->getResult()
        ->is<PolymorphicFunctionType>())
    return true;

  return false;
}

bool SILGenModule::requiresObjCMethodEntryPoint(FuncDecl *method) {
  // Property accessors should be generated alongside the property.
  if (method->isGetterOrSetter())
    return false;
    
  // We don't export generic methods or subclasses to Objective-C yet.
  if (isNonProtocolGeneric(method))
    return false;
    
  if (method->isObjC() || method->getAttrs().isIBAction())
    return true;
  if (auto override = method->getOverriddenDecl())
    return requiresObjCMethodEntryPoint(override);
  return false;
}

bool SILGenModule::requiresObjCMethodEntryPoint(ConstructorDecl *constructor) {
  // We don't export generic methods or subclasses to Objective-C yet.
  if (isNonProtocolGeneric(constructor))
    return false;

  return constructor->isObjC();
}

bool SILGenModule::requiresObjCDispatch(ValueDecl *vd) {
  if (auto *fd = dyn_cast<FuncDecl>(vd)) {
    // If a function has an associated Clang node, it's foreign.
    if (vd->hasClangNode())
      return true;

    return requiresObjCMethodEntryPoint(fd);
  }
  if (auto *cd = dyn_cast<ConstructorDecl>(vd))
    return requiresObjCMethodEntryPoint(cd);
  if (auto *asd = dyn_cast<AbstractStorageDecl>(vd))
    return asd->usesObjCGetterAndSetter();
  return vd->isObjC();
}

bool SILGenModule::requiresObjCSuperDispatch(ValueDecl *vd) {
  if (auto *cd = dyn_cast<ConstructorDecl>(vd)) {
    DeclContext *ctorDC = cd->getDeclContext();
    if (auto *cls = dyn_cast<ClassDecl>(ctorDC)) {
      return cls->isObjC();
    }
  }
  return requiresObjCDispatch(vd);
}

/// An ASTVisitor for populating SILVTable entries from ClassDecl members.
class SILGenVTable : public Lowering::ASTVisitor<SILGenVTable> {
public:
  SILGenModule &SGM;
  ClassDecl *theClass;
  std::vector<SILVTable::Pair> vtableEntries;
  
  SILGenVTable(SILGenModule &SGM, ClassDecl *theClass)
    : SGM(SGM), theClass(theClass)
  {
    // Populate the superclass members, if any.
    Type super = theClass->getSuperclass();
    if (super && super->getClassOrBoundGenericClass())
      visitAncestor(super->getClassOrBoundGenericClass());
  }

  ~SILGenVTable() {
    // Create the vtable.
    SILVTable::create(SGM.M, theClass, vtableEntries);
  }
  
  void visitAncestor(ClassDecl *ancestor) {
    // Recursively visit all our ancestors.
    Type super = ancestor->getSuperclass();
    if (super && super->getClassOrBoundGenericClass())
      visitAncestor(super->getClassOrBoundGenericClass());
    
    for (auto member : ancestor->getMembers())
      visit(member);
  }
  
  // Add an entry to the vtable.
  void addEntry(SILDeclRef member) {
    // Try to find an overridden entry.
    // NB: Mutates vtableEntries in-place
    // FIXME: O(n^2)
    if (auto overridden = member.getOverridden()) {
      // If we overrode an ObjC decl, or a decl from an extension, it won't be
      // in a vtable; create a new entry.
      if (overridden.getDecl()->hasClangNode())
        goto not_overridden;
      // If we overrode a decl from an extension, it won't be in a vtable
      // either. This can occur for extensions to ObjC classes.
      if (isa<ExtensionDecl>(overridden.getDecl()->getDeclContext()))
        goto not_overridden;

      for (SILVTable::Pair &entry : vtableEntries) {
        SILDeclRef ref = overridden;
        
        do {
          // Replace the overridden member.
          if (entry.first == ref) {
            entry = {member, SGM.getFunction(member, NotForDefinition)};
            return;
          }
        } while ((ref = ref.getOverridden()));
      }
      llvm_unreachable("no overridden vtable entry?!");
    }
    
  not_overridden:
    // Otherwise, introduce a new vtable entry.
    vtableEntries.emplace_back(member, SGM.getFunction(member, NotForDefinition));
  }
  
  // Default for members that don't require vtable entries.
  void visitDecl(Decl*) {}
  
  void visitFuncDecl(FuncDecl *fd) {
    // ObjC decls don't go in vtables.
    if (fd->hasClangNode())
      return;
    
    addEntry(SILDeclRef(fd));
  }
  
  void visitConstructorDecl(ConstructorDecl *cd) {
    // FIXME: If this is a dynamically-dispatched constructor, add its
    // initializing entry point to the vtable.
  }
  
  void visitVarDecl(VarDecl *vd) {
    // FIXME: If this is a dynamically-dispatched property, add its getter and
    // setter to the vtable.
  }
  
  void visitSubscriptDecl(SubscriptDecl *sd) {
    // FIXME: If this is a dynamically-dispatched property, add its getter and
    // setter to the vtable.
  }
};

/// An ASTVisitor for generating SIL from method declarations
/// inside nominal types.
class SILGenType : public Lowering::ASTVisitor<SILGenType> {
public:
  SILGenModule &SGM;
  NominalTypeDecl *theType;
  Optional<SILGenVTable> genVTable;
  
  SILGenType(SILGenModule &SGM, NominalTypeDecl *theType)
    : SGM(SGM), theType(theType) {}
  
  /// Emit SIL functions for all the members of the type.
  void emitType() {
    // Start building a vtable if this is a class.
    if (auto theClass = dyn_cast<ClassDecl>(theType))
      genVTable.emplace(SGM, theClass);
    
    for (Decl *member : theType->getMembers()) {
      if (genVTable)
        genVTable->visit(member);
      
      visit(member);
    }
    
    // Emit witness tables for conformances of concrete types. Protocol types
    // are existential and do not have witness tables.
    if (isa<ProtocolDecl>(theType))
      return;
    
    for (auto *conformance : theType->getConformances())
      SGM.getWitnessTable(conformance);
  }
  
  //===--------------------------------------------------------------------===//
  // Visitors for subdeclarations
  //===--------------------------------------------------------------------===//
  void visitNominalTypeDecl(NominalTypeDecl *ntd) {
    SILGenType(SGM, ntd).emitType();
  }
  void visitFuncDecl(FuncDecl *fd) {
    SGM.emitFunction(fd);
    // FIXME: Default implementations in protocols.
    if (SGM.requiresObjCMethodEntryPoint(fd) &&
        !isa<ProtocolDecl>(fd->getDeclContext()))
      SGM.emitObjCMethodThunk(fd);
  }
  void visitConstructorDecl(ConstructorDecl *cd) {
    SGM.emitConstructor(cd);

    if (SGM.requiresObjCMethodEntryPoint(cd) &&
        !isa<ProtocolDecl>(cd->getDeclContext()))
      SGM.emitObjCConstructorThunk(cd);
  }
  void visitDestructorDecl(DestructorDecl *dd) {
    assert(isa<ClassDecl>(theType) && "destructor in a non-class type");
    SGM.emitDestructor(cast<ClassDecl>(theType), dd);
  }
  
  void visitEnumElementDecl(EnumElementDecl *ued) {
    assert(isa<EnumDecl>(theType));
    SGM.emitEnumConstructor(ued);
  }
  
  void visitPatternBindingDecl(PatternBindingDecl *pd) {
    // Emit initializers for static variables.
    if (pd->isStatic() && pd->hasInit()) {
      SGM.emitGlobalInitialization(pd);
    }
  }
  
  void visitVarDecl(VarDecl *vd) {
    // Collect global variables for static properties.
    // FIXME: We can't statically emit a global variable for generic properties.
    if (vd->isStatic() && vd->hasStorage()) {
      assert(!theType->getGenericParams()
             && "generic static properties not implemented");
      assert((isa<StructDecl>(theType) || isa<EnumDecl>(theType))
             && "only value type static properties are implemented");
      
      SGM.addGlobalVariable(vd);
      return;
    }
    
    visitAbstractStorageDecl(vd);
  }

  void visitAbstractStorageDecl(AbstractStorageDecl *asd) {
    // FIXME: Default implementations in protocols.
    if (asd->usesObjCGetterAndSetter() &&
        !isa<ProtocolDecl>(asd->getDeclContext()))
      SGM.emitObjCPropertyMethodThunks(asd);
  }
};

void SILGenModule::visitNominalTypeDecl(NominalTypeDecl *ntd) {
  SILGenType(*this, ntd).emitType();
}

void SILGenFunction::visitNominalTypeDecl(NominalTypeDecl *ntd) {
  SILGenType(SGM, ntd).emitType();
}

void SILGenModule::emitExternalDefinition(Decl *d) {
  switch (d->getKind()) {
  case DeclKind::Func: {
    emitFunction(cast<FuncDecl>(d));
    break;
  }
  case DeclKind::Constructor: {
    emitConstructor(cast<ConstructorDecl>(d));
    break;
  }
  case DeclKind::Enum: {
    // Emit the enum cases and RawRepresentable methods.
    for (auto member : cast<EnumDecl>(d)->getMembers()) {
      if (auto elt = dyn_cast<EnumElementDecl>(member))
        emitEnumConstructor(elt);
      else if (auto func = dyn_cast<FuncDecl>(member))
        emitFunction(func);
    }
    break;
  }
  case DeclKind::Struct:
  case DeclKind::Class:
  case DeclKind::Protocol:
    // Nothing to do in SILGen for other external types.
    break;
      
  case DeclKind::Extension:
  case DeclKind::PatternBinding:
  case DeclKind::EnumCase:
  case DeclKind::EnumElement:
  case DeclKind::TopLevelCode:
  case DeclKind::TypeAlias:
  case DeclKind::AssociatedType:
  case DeclKind::GenericTypeParam:
  case DeclKind::Var:
  case DeclKind::Import:
  case DeclKind::Subscript:
  case DeclKind::Destructor:
  case DeclKind::InfixOperator:
  case DeclKind::PrefixOperator:
  case DeclKind::PostfixOperator:
    llvm_unreachable("Not a valid external definition for SILGen");
  }
}

/// SILGenExtension - an ASTVisitor for generating SIL from method declarations
/// and protocol conformances inside type extensions.
class SILGenExtension : public Lowering::ASTVisitor<SILGenExtension> {
public:
  SILGenModule &SGM;

  SILGenExtension(SILGenModule &SGM)
    : SGM(SGM) {}
  
  /// Emit ObjC thunks necessary for an ObjC protocol conformance.
  void emitObjCConformanceThunks(ProtocolDecl *protocol,
                                 ProtocolConformance *conformance) {
    assert(conformance);
    if (protocol->isObjC()) {
      conformance->forEachValueWitness(nullptr,
                                       [&](ValueDecl *req,
                                           ConcreteDeclRef witness) {
        if (!witness)
          return;
        
        ValueDecl *vd = witness.getDecl();
        if (auto *method = cast<FuncDecl>(vd))
          SGM.emitObjCMethodThunk(method);
        else if (auto *prop = cast<VarDecl>(vd))
          SGM.emitObjCPropertyMethodThunks(prop);
        else
          llvm_unreachable("unexpected conformance mapping");
      });
    }

    for (auto &inherited : conformance->getInheritedConformances())
      emitObjCConformanceThunks(inherited.first, inherited.second);
  }
  
  /// Emit SIL functions for all the members of the extension.
  void emitExtension(ExtensionDecl *e) {
    for (Decl *member : e->getMembers())
      visit(member);

    if (!e->getExtendedType()->isExistentialType()) {
      // Emit witness tables for protocol conformances introduced by the
      // extension.
      for (auto *conformance : e->getConformances())
        SGM.getWitnessTable(conformance);
    }
    
    // ObjC protocol conformances may require ObjC thunks to be introduced for
    // definitions from other contexts.
    for (unsigned i = 0, size = e->getProtocols().size(); i < size; ++i)
      emitObjCConformanceThunks(e->getProtocols()[i],
                                e->getConformances()[i]);
  }
  
  //===--------------------------------------------------------------------===//
  // Visitors for subdeclarations
  //===--------------------------------------------------------------------===//
  void visitNominalTypeDecl(NominalTypeDecl *ntd) {
    SILGenType(SGM, ntd).emitType();
  }
  void visitFuncDecl(FuncDecl *fd) {
    SGM.emitFunction(fd);
    if (SGM.requiresObjCMethodEntryPoint(fd))
      SGM.emitObjCMethodThunk(fd);
  }
  void visitConstructorDecl(ConstructorDecl *cd) {
    SGM.emitConstructor(cd);
    if (SGM.requiresObjCMethodEntryPoint(cd))
      SGM.emitObjCConstructorThunk(cd);
  }
  void visitDestructorDecl(DestructorDecl *dd) {
    llvm_unreachable("destructor in extension?!");
  }
  
  // no-ops. We don't deal with the layout of types here.
  void visitPatternBindingDecl(PatternBindingDecl *) {}
  
  void visitAbstractStorageDecl(AbstractStorageDecl *vd) {
    if (vd->usesObjCGetterAndSetter())
      SGM.emitObjCPropertyMethodThunks(vd);
  }
};

void SILGenModule::visitExtensionDecl(ExtensionDecl *ed) {
  SILGenExtension(*this).emitExtension(ed);
}

void SILGenFunction::emitLocalVariable(VarDecl *vd) {
  assert(vd->getDeclContext()->isLocalContext() &&
         "can't emit a local var for a non-local var decl");
  assert(vd->hasStorage() && "can't emit storage for a computed variable");
  assert(!VarLocs.count(vd) && "Already have an entry for this decl?");

  SILType lType = getLoweredType(vd->getType()->getRValueType());

  // The variable may have its lifetime extended by a closure, heap-allocate it
  // using a box.
  AllocBoxInst *allocBox = B.createAllocBox(vd, lType);
  auto box = SILValue(allocBox, 0);
  auto addr = SILValue(allocBox, 1);
  
  /// Remember that this is the memory location that we're emitting the
  /// decl to.
  VarLocs[vd] = SILGenFunction::VarLoc::getAddress(addr, box);
}

/// Create a LocalVariableInitialization for the uninitialized var.
InitializationPtr SILGenFunction::emitLocalVariableWithCleanup(VarDecl *vd) {
  emitLocalVariable(vd);
  return InitializationPtr(new LocalVariableInitialization(vd, *this));
}

/// Create an Initialization for an uninitialized temporary.
std::unique_ptr<TemporaryInitialization>
SILGenFunction::emitTemporary(SILLocation loc, const TypeLowering &tempTL) {
  SILValue addr = emitTemporaryAllocation(loc, tempTL.getLoweredType());
  CleanupHandle cleanup = enterDormantTemporaryCleanup(addr, tempTL);
  return std::unique_ptr<TemporaryInitialization>(
                                   new TemporaryInitialization(addr, cleanup));
}

CleanupHandle
SILGenFunction::enterDormantTemporaryCleanup(SILValue addr,
                                             const TypeLowering &tempTL) {
  if (tempTL.isTrivial())
    return CleanupHandle::invalid();

  Cleanups.pushCleanupInState<DestroyValueCleanup>(CleanupState::Dormant, addr);
  return Cleanups.getCleanupsDepth();
}

void SILGenFunction::destroyLocalVariable(SILLocation silLoc, VarDecl *vd) {
  assert(vd->getDeclContext()->isLocalContext() &&
         "can't emit a local var for a non-local var decl");
  assert(vd->hasStorage() && "can't emit storage for a computed variable");

  assert(VarLocs.count(vd) && "var decl wasn't emitted?!");
  
  auto loc = VarLocs[vd];
  
  // For 'let' bindings, we emit a destroy_value or destroy_addr, depending on
  // whether we have an address or not.
  if (loc.isConstant()) {
    assert(vd->isLet() && "Mutable vardecl assigned a constant value?");
    SILValue Val = loc.getConstant();
    if (!Val.getType().isAddress())
      B.emitDestroyValueOperation(silLoc, Val);
    else
      B.emitDestroyAddr(silLoc, Val);
    return;
  }
  
  // For a heap variable, the box is responsible for the value. We just need
  // to give up our retain count on it.
  assert(loc.box && "captured var should have been given a box");
  B.emitStrongRelease(silLoc, loc.box);
}

void SILGenFunction::deallocateUninitializedLocalVariable(SILLocation silLoc,
                                                          VarDecl *vd) {
  assert(vd->getDeclContext()->isLocalContext() &&
         "can't emit a local var for a non-local var decl");
  assert(vd->hasStorage() && "can't emit storage for a computed variable");

  assert(VarLocs.count(vd) && "var decl wasn't emitted?!");

  auto loc = VarLocs[vd];
  if (loc.isConstant()) return;

  assert(loc.box && "captured var should have been given a box");
  B.createDeallocBox(silLoc, loc.getAddress().getType().getObjectType(),
                     loc.box);
}

//===----------------------------------------------------------------------===//
// ObjC method thunks
//===----------------------------------------------------------------------===//

static SILValue emitBridgeObjCReturnValue(SILGenFunction &gen,
                                          SILLocation loc,
                                          SILValue result,
                                          CanType origNativeTy,
                                          CanType substNativeTy,
                                          CanType bridgedTy) {
  Scope scope(gen.Cleanups, CleanupLocation::getCleanupLocation(loc));
  
  ManagedValue native = gen.emitManagedRValueWithCleanup(result);
  ManagedValue bridged = gen.emitNativeToBridgedValue(loc, native,
                                                      AbstractCC::ObjCMethod,
                                                      origNativeTy,
                                                      substNativeTy,
                                                      bridgedTy);
  return bridged.forward(gen);
}

/// Take a return value at +1 and adjust it to the retain count expected by
/// the given ownership conventions.
static void emitObjCReturnValue(SILGenFunction &gen,
                                SILLocation loc,
                                SILValue result,
                                CanType nativeTy,
                                SILResultInfo resultInfo) {
  // Bridge the result.
  result = emitBridgeObjCReturnValue(gen, loc, result, nativeTy, nativeTy,
                                     resultInfo.getType());
  
  // Autorelease the bridged result if necessary.
  switch (resultInfo.getConvention()) {
  case ResultConvention::Autoreleased:
    gen.B.createAutoreleaseReturn(loc, result);
    return;
  case ResultConvention::Unowned:
    gen.B.emitDestroyValueOperation(loc, result);
    SWIFT_FALLTHROUGH;
  case ResultConvention::Owned:
    gen.B.createReturn(loc, result);
    return;
  }
}

/// Take an argument at +0 and bring it to +1.
static SILValue emitObjCUnconsumedArgument(SILGenFunction &gen,
                                           SILLocation loc,
                                           SILValue arg) {
  auto &lowering = gen.getTypeLowering(arg.getType());
  // If address-only, make a +1 copy and operate on that.
  if (lowering.isAddressOnly()) {
    auto tmp = gen.emitTemporaryAllocation(loc, arg.getType().getObjectType());
    gen.B.createCopyAddr(loc, arg, tmp, IsNotTake, IsInitialization);
    return tmp;
  }
  
  return lowering.emitCopyValue(gen.B, loc, arg);
}

/// Bridge argument types and adjust retain count conventions for an ObjC thunk.
static SILFunctionType *emitObjCThunkArguments(SILGenFunction &gen,
                                               SILDeclRef thunk,
                                               SmallVectorImpl<SILValue> &args){
  auto objcInfo = gen.SGM.Types.getConstantFunctionType(thunk);
  auto swiftInfo = gen.SGM.Types.getConstantFunctionType(thunk.asForeign(false));

  assert(!objcInfo->isPolymorphic()
         && !swiftInfo->isPolymorphic()
         && "generic objc functions not implemented");
  
  RegularLocation Loc(thunk.getDecl());
  Loc.markAutoGenerated();

  SmallVector<ManagedValue, 8> bridgedArgs;
  bridgedArgs.reserve(objcInfo->getInterfaceParameters().size());
  
  // Emit the indirect return argument, if any.
  if (objcInfo->hasIndirectResult()) {
    auto arg = new (gen.F.getModule())
      SILArgument(objcInfo->getIndirectInterfaceResult().getSILType(), gen.F.begin());
    bridgedArgs.push_back(ManagedValue::forUnmanaged(arg));
  }
  
  // Emit the other arguments, taking ownership of arguments if necessary.
  auto inputs = objcInfo->getInterfaceParametersWithoutIndirectResult();
  assert(!inputs.empty());
  for (unsigned i = 0, e = inputs.size(); i < e; ++i) {
    SILValue arg = new(gen.F.getModule())
                     SILArgument(inputs[i].getSILType(), gen.F.begin());

    // Convert the argument to +1 if necessary.
    if (!inputs[i].isConsumed()) {
      arg = emitObjCUnconsumedArgument(gen, Loc, arg);
    }

    auto managedArg = gen.emitManagedRValueWithCleanup(arg);

    bridgedArgs.push_back(managedArg);
  }

  assert(bridgedArgs.size() == objcInfo->getInterfaceParameters().size() &&
         "objc inputs don't match number of arguments?!");
  assert(bridgedArgs.size() == swiftInfo->getInterfaceParameters().size() &&
         "swift inputs don't match number of arguments?!");

  // Bridge the input types.
  Scope scope(gen.Cleanups, CleanupLocation::getCleanupLocation(Loc));
  for (unsigned i = 0, size = bridgedArgs.size(); i < size; ++i) {
    ManagedValue native =
      gen.emitBridgedToNativeValue(Loc,
                                   bridgedArgs[i],
                                   AbstractCC::ObjCMethod,
                     swiftInfo->getInterfaceParameters()[i].getSILType().getSwiftType());
    args.push_back(native.forward(gen));
  }
  
  return objcInfo;
}

void SILGenFunction::emitObjCMethodThunk(SILDeclRef thunk) {
  SILDeclRef native = thunk.asForeign(false);
  
  SmallVector<SILValue, 4> args;  
  auto objcFnTy = emitObjCThunkArguments(*this, thunk, args);
  auto nativeInfo = getConstantInfo(native);
  assert(!objcFnTy->isPolymorphic()
         && "generic objc functions not yet implemented");
  assert(!nativeInfo.SILFnType->isPolymorphic()
         && "generic objc functions not yet implemented");
  auto swiftResultTy = nativeInfo.SILFnType->getInterfaceResult();
  auto objcResultTy = objcFnTy->getInterfaceResult();
  
  // Call the native entry point.
  RegularLocation loc(thunk.getDecl());
  loc.markAutoGenerated();

  SILValue nativeFn = emitGlobalFunctionRef(loc, native, nativeInfo);
  SILValue result = B.createApply(loc, nativeFn, nativeFn.getType(),
                                  swiftResultTy.getSILType(), {}, args,
                                  thunk.isTransparent());
  emitObjCReturnValue(*this, loc, result, nativeInfo.LoweredType.getResult(),
                      objcResultTy);
}

void SILGenFunction::emitObjCGetter(SILDeclRef getter) {
  SmallVector<SILValue, 2> args;
  auto objcFnTy = emitObjCThunkArguments(*this, getter, args);
  SILDeclRef native = getter.asForeign(false);
  auto nativeInfo = getConstantInfo(native);
  assert(!objcFnTy->isPolymorphic()
         && "generic objc functions not yet implemented");
  assert(!nativeInfo.SILFnType->isPolymorphic()
         && "generic objc functions not yet implemented");
  SILResultInfo swiftResultTy = nativeInfo.SILFnType->getInterfaceResult();
  SILResultInfo objcResultTy = objcFnTy->getInterfaceResult();

  RegularLocation loc(getter.getDecl());
  loc.markAutoGenerated();

  // The property always accessors, forward to the native getter.
  assert(cast<AbstractStorageDecl>(getter.getDecl())->hasAccessorFunctions());
  
  SILValue nativeFn = emitGlobalFunctionRef(loc, native, nativeInfo);
  SILValue result = B.createApply(loc, nativeFn, nativeFn.getType(),
                                  swiftResultTy.getSILType(), {}, args,
                                  getter.isTransparent());
  emitObjCReturnValue(*this, loc, result, nativeInfo.LoweredType.getResult(),
                      objcResultTy);
}

void SILGenFunction::emitObjCSetter(SILDeclRef setter) {
  SmallVector<SILValue, 2> args;
  emitObjCThunkArguments(*this, setter, args);
  SILDeclRef native = setter.asForeign(false);
  auto nativeInfo = getConstantInfo(native);

  RegularLocation loc(setter.getDecl());
  loc.markAutoGenerated();

  // If the native property is computed, store to the native setter.
  assert(cast<AbstractStorageDecl>(setter.getDecl())->hasAccessorFunctions());
  SILValue nativeFn = emitGlobalFunctionRef(loc, native, nativeInfo);
  SILValue result = B.createApply(loc, nativeFn, nativeFn.getType(),
                                  SGM.Types.getEmptyTupleType(),
                                  {}, args, setter.isTransparent());
  // Result should be void.
  B.createReturn(loc, result);
}

void SILGenFunction::emitObjCDestructor(SILDeclRef dtor) {
  auto dd = cast<DestructorDecl>(dtor.getDecl());
  auto cd = cast<ClassDecl>(dd->getDeclContext());
  RegularLocation loc(dd);
  if (dd->isImplicit())
    loc.markAutoGenerated();

  SILValue selfValue = emitSelfDecl(dd->getImplicitSelfDecl());

  // Create a basic block to jump to for the implicit destruction behavior
  // of releasing the elements and calling the superclass destructor.
  // We won't actually emit the block until we finish with the destructor body.
  prepareEpilog(Type(), CleanupLocation::getCleanupLocation(loc));

  // Emit the destructor body.
  visit(dd->getBody());

  Optional<SILValue> maybeReturnValue;
  SILLocation returnLoc(loc);
  llvm::tie(maybeReturnValue, returnLoc) = emitEpilogBB(loc);

  if (!maybeReturnValue)
    return;

  auto cleanupLoc = CleanupLocation::getCleanupLocation(loc);
  
  // Note: the ivar destroyer is responsible for destroying the
  // instance variables before the object is actually deallocated.

  // Form a reference to the superclass -dealloc.
  Type superclassTy = cd->getSuperclass();
  assert(superclassTy && "Emitting Objective-C -dealloc without superclass?");
  ClassDecl *superclass = superclassTy->getClassOrBoundGenericClass();
  auto superclassDtorDecl = superclass->getDestructor();
  SILDeclRef superclassDtor(superclassDtorDecl, 
                            SILDeclRef::Kind::Deallocator,
                            SILDeclRef::ConstructAtNaturalUncurryLevel,
                            /*isForeign=*/true);
  auto superclassDtorType = SGM.getConstantType(superclassDtor);
  SILValue superclassDtorValue = B.createSuperMethod(
                                   cleanupLoc, selfValue, superclassDtor,
                                   superclassDtorType);

  // Call the superclass's -dealloc.
  SILType superclassSILTy = getLoweredLoadableType(superclassTy);
  SILValue superSelf = B.createUpcast(cleanupLoc, selfValue, superclassSILTy);
  B.createApply(cleanupLoc, superclassDtorValue, superclassDtorType,
                superclassDtorType.getFunctionInterfaceResultType(),
                { }, superSelf);

  // Return.
  B.createReturn(returnLoc, emitEmptyTuple(cleanupLoc));
}

//===----------------------------------------------------------------------===//
// Global initialization
//===----------------------------------------------------------------------===//

namespace {
  
/// A visitor for traversing a pattern, creating
/// global accessor functions for all of the global variables declared inside.
struct GenGlobalAccessors : public PatternVisitor<GenGlobalAccessors>
{
  /// The module generator.
  SILGenModule &SGM;
  /// The Builtin.once token guarding the global initialization.
  SILGlobalVariable *OnceToken;
  /// The function containing the initialization code.
  SILFunction *OnceFunc;
  
  /// A reference to the Builtin.once declaration.
  FuncDecl *BuiltinOnceDecl;
  
  GenGlobalAccessors(SILGenModule &SGM,
                     SILGlobalVariable *OnceToken,
                     SILFunction *OnceFunc)
    : SGM(SGM), OnceToken(OnceToken), OnceFunc(OnceFunc)
  {
    // Find Builtin.once.
    auto &C = SGM.M.getASTContext();
    SmallVector<ValueDecl*, 2> found;
    C.TheBuiltinModule
      ->lookupValue({}, C.getIdentifier("once"),
                    NLKind::QualifiedLookup, found);
    
    assert(found.size() == 1 && "didn't find Builtin.once?!");
    
    BuiltinOnceDecl = cast<FuncDecl>(found[0]);
  }
  
  // Walk through non-binding patterns.
  void visitParenPattern(ParenPattern *P) {
    return visit(P->getSubPattern());
  }
  void visitTypedPattern(TypedPattern *P) {
    return visit(P->getSubPattern());
  }
  void visitVarPattern(VarPattern *P) {
    return visit(P->getSubPattern());
  }
  void visitTuplePattern(TuplePattern *P) {
    for (auto &elt : P->getFields())
      visit(elt.getPattern());
  }
  void visitAnyPattern(AnyPattern *P) {}
  
  // When we see a variable binding, emit its global accessor.
  void visitNamedPattern(NamedPattern *P) {
    SGM.emitGlobalAccessor(P->getDecl(), BuiltinOnceDecl, OnceToken, OnceFunc);
  }
  
#define INVALID_PATTERN(Id, Parent) \
  void visit##Id##Pattern(Id##Pattern *) { \
    llvm_unreachable("pattern not valid in argument or var binding"); \
  }
#define PATTERN(Id, Parent)
#define REFUTABLE_PATTERN(Id, Parent) INVALID_PATTERN(Id, Parent)
#include "swift/AST/PatternNodes.def"
#undef INVALID_PATTERN
};
  
} // end anonymous namespace

/// Emit a global initialization.
void SILGenModule::emitGlobalInitialization(PatternBindingDecl *pd) {
  // Generic and dynamic static properties require lazy initialization, which
  // isn't implemented yet.
  if (pd->isStatic()) {
    auto theType = pd->getDeclContext()->getDeclaredTypeInContext();
    assert(!theType->is<BoundGenericType>()
           && "generic static properties not implemented");
    assert((theType->getStructOrBoundGenericStruct()
            || theType->getEnumOrBoundGenericEnum())
           && "only value type static properties are implemented");
    (void)theType;
  }
  
  // Emit the lazy initialization token for the initialization expression.
  auto counter = anonymousSymbolCounter++;
  
  llvm::SmallString<20> onceTokenName;
  {
    llvm::raw_svector_ostream os(onceTokenName);
    os << "globalinit_token" << counter;
    os.flush();
  }
  
  auto onceTy = BuiltinIntegerType::getWordType(M.getASTContext());
  auto onceSILTy
    = SILType::getPrimitiveObjectType(onceTy->getCanonicalType());
  
  auto onceToken = SILGlobalVariable::create(M, SILLinkage::Private,
                                             onceTokenName, onceSILTy);
  
  // Emit the initialization code into a function.
  llvm::SmallString<20> onceFuncName;
  {
    llvm::raw_svector_ostream os(onceFuncName);
    os << "globalinit_func" << counter;
    os.flush();
  }
  
  SILFunction *onceFunc = emitLazyGlobalInitializer(onceFuncName, pd);
  
  // Generate accessor functions for all of the declared variables, which
  // Builtin.once the lazy global initializer we just generated then return
  // the address of the individual variable.
  GenGlobalAccessors(*this, onceToken, onceFunc)
    .visit(pd->getPattern());
}

namespace {
  
// Is this a free function witness satisfying a static method requirement?
static IsFreeFunctionWitness_t isFreeFunctionWitness(ValueDecl *requirement,
                                                     ValueDecl *witness) {
  if (!witness->getDeclContext()->isTypeContext()) {
    assert(!requirement->isInstanceMember()
           && "free function satisfying instance method requirement?!");
    return IsFreeFunctionWitness;
  }
  
  return IsNotFreeFunctionWitness;
}

/// Emit a witness table for a protocol conformance.
class SILGenConformance : public Lowering::ASTVisitor<SILGenConformance> {
public:
  SILGenModule &SGM;
  NormalProtocolConformance *Conformance;
  std::vector<SILWitnessTable::Entry> Entries;

  SILGenConformance(SILGenModule &SGM, ProtocolConformance *C)
    // We only need to emit witness tables for base NormalProtocolConformances.
    : SGM(SGM), Conformance(dyn_cast<NormalProtocolConformance>(C))
  {
    // Not all protocols use witness tables.
    if (!SGM.Types.protocolRequiresWitnessTable(Conformance->getProtocol()))
      Conformance = nullptr;
  }
  
  SILWitnessTable *emit() {
    // Nothing to do if this wasn't a normal conformance.
    if (!Conformance)
      return nullptr;

    // Reference conformances for refined protocols.
    auto protocol = Conformance->getProtocol();
    for (auto base : protocol->getProtocols())
      emitBaseProtocolWitness(base);
    
    // Emit witnesses in protocol declaration order.
    for (auto reqt : protocol->getMembers())
      visit(reqt);
    
    // Create the witness table.
    return SILWitnessTable::create(SGM.M, Conformance, Entries);
  }
  
  void emitBaseProtocolWitness(ProtocolDecl *baseProtocol) {
    // Only include the witness if the base protocol requires it.
    if (!SGM.Types.protocolRequiresWitnessTable(baseProtocol))
      return;
    
    auto foundBaseConformance
      = Conformance->getInheritedConformances().find(baseProtocol);
    assert(foundBaseConformance != Conformance->getInheritedConformances().end()
           && "no inherited conformance for base protocol");
    Entries.push_back(SILWitnessTable::BaseProtocolWitness{
      baseProtocol,
      foundBaseConformance->second
    });
    SGM.getWitnessTable(foundBaseConformance->second);
  }
  
  /// Fallback for unexpected protocol requirements.
  void visitDecl(Decl *d) {
    d->print(llvm::errs());
    llvm_unreachable("unhandled protocol requirement");
  }
  
  void visitFuncDecl(FuncDecl *fd) {
    // FIXME: Emit getter and setter (if settable) witnesses.
    // For now we ignore them, like the IRGen witness table builder did.
    if (fd->isGetterOrSetter())
      return;
    
    // Find the witness in the conformance.
    ConcreteDeclRef witness = Conformance->getWitness(fd, nullptr);

    // Emit the witness thunk and add it to the table.
    SILDeclRef requirementRef(fd, SILDeclRef::Kind::Func);
    // Free function witnesses have an implicit uncurry layer imposed on them by
    // the inserted metatype argument.
    auto isFree = isFreeFunctionWitness(fd, witness.getDecl());
    unsigned witnessUncurryLevel = isFree ? requirementRef.uncurryLevel - 1
                                          : requirementRef.uncurryLevel;
  
    SILDeclRef witnessRef(witness.getDecl(), SILDeclRef::Kind::Func,
                          witnessUncurryLevel);

    SILFunction *witnessFn = SGM.emitProtocolWitness(Conformance,
                                                   requirementRef, witnessRef,
                                                   isFree,
                                                   witness.getSubstitutions());
    Entries.push_back(
                    SILWitnessTable::MethodWitness{requirementRef, witnessFn});
  }
  
  void visitAssociatedTypeDecl(AssociatedTypeDecl *td) {
    // Find the substitution info for the witness type.
    const auto &witness = Conformance->getTypeWitness(td, /*resolver=*/nullptr);

    // Emit the record for the type itself.
    Entries.push_back(SILWitnessTable::AssociatedTypeWitness{td,
                                      witness.Replacement->getCanonicalType()});
    
    // Emit records for the protocol requirements on the type.
    assert(td->getProtocols().size() == witness.Conformance.size()
           && "number of conformances in assoc type substitution do not match "
              "number of requirements on assoc type");
    // The conformances should be all null or all nonnull.
    assert(witness.Conformance.empty()
           || (witness.Conformance[0]
                 ? std::all_of(witness.Conformance.begin(),
                               witness.Conformance.end(),
                               [&](const ProtocolConformance *C) -> bool {
                                 return C;
                               })
                 : std::all_of(witness.Conformance.begin(),
                               witness.Conformance.end(),
                               [&](const ProtocolConformance *C) -> bool {
                                 return !C;
                               })));
    
    for (unsigned i = 0, e = td->getProtocols().size(); i < e; ++i) {
      auto protocol = td->getProtocols()[i];
      
      // Only reference the witness if the protocol requires it.
      if (!SGM.Types.protocolRequiresWitnessTable(protocol))
        continue;

      ProtocolConformance *conformance = nullptr;
      // If the associated type requirement is satisfied by an associated type,
      // these will all be null.
      if (witness.Conformance[0]) {
        auto foundConformance = std::find_if(witness.Conformance.begin(),
                                        witness.Conformance.end(),
                                        [&](ProtocolConformance *c) {
                                          return c->getProtocol() == protocol;
                                        });
        assert(foundConformance != witness.Conformance.end());
        conformance = *foundConformance;
      }
      
      Entries.push_back(SILWitnessTable::AssociatedTypeProtocolWitness{
        td, protocol, conformance
      });
    }
  }
  
  void visitPatternBindingDecl(PatternBindingDecl *pbd) {
    // We only care about the contained VarDecls.
  }

  void visitVarDecl(VarDecl *vd) {
    // FIXME: Emit getter and setter (if settable) witnesses.
    // For now we ignore them, like the IRGen witness table builder did.
  }
};
  
} // end anonymous namespace

SILWitnessTable *
SILGenModule::getWitnessTable(ProtocolConformance *conformance) {
  // If we've already emitted this witness table, return it.
  auto found = emittedWitnessTables.find(conformance);
  if (found != emittedWitnessTables.end())
    return found->second;
  
  SILWitnessTable *table = SILGenConformance(*this, conformance).emit();
  emittedWitnessTables.insert({conformance, table});
  return table;
}

/// FIXME: This should just be a call down to Types.getLoweredType(), but I
/// really don't want to thread an old-type/interface-type pair through all
/// of TypeLowering.
static SILType
getWitnessFunctionType(SILModule &M,
                       AbstractionPattern origRequirementTy,
                       CanAnyFunctionType witnessSubstTy,
                       CanAnyFunctionType witnessSubstIfaceTy,
                       unsigned uncurryLevel) {
  // Lower the types to uncurry and get ExtInfo.
  CanType origLoweredTy;
  if (auto origFTy = dyn_cast<AnyFunctionType>(origRequirementTy.getAsType()))
    origLoweredTy = M.Types.getLoweredASTFunctionType(origFTy,
                                                      uncurryLevel);
  else
    origLoweredTy = origRequirementTy.getAsType();
  auto witnessLoweredTy
    = M.Types.getLoweredASTFunctionType(witnessSubstTy, uncurryLevel);
  auto witnessLoweredIfaceTy
    = M.Types.getLoweredASTFunctionType(witnessSubstIfaceTy, uncurryLevel);
  
  // Convert to SILFunctionType.
  auto fnTy = getNativeSILFunctionType(M, origLoweredTy,
                                       witnessLoweredTy,
                                       witnessLoweredIfaceTy);
  return SILType::getPrimitiveObjectType(fnTy);
}

SILFunction *
SILGenModule::emitProtocolWitness(ProtocolConformance *conformance,
                                  SILDeclRef requirement,
                                  SILDeclRef witness,
                                  IsFreeFunctionWitness_t isFree,
                                  ArrayRef<Substitution> witnessSubs) {
  // Get the type of the protocol requirement and the original type of the
  // witness.
  // FIXME: Rework for interface types.
  auto requirementTy
    = cast<PolymorphicFunctionType>(Types.getConstantFormalType(requirement));
  auto witnessOrigTy = Types.getConstantFormalType(witness);
  unsigned witnessUncurryLevel = witness.uncurryLevel;

  // Substitute the 'self' type into the requirement to get the concrete
  // witness type.
  auto witnessSubstTy = cast<AnyFunctionType>(
    requirementTy
      ->substGenericArgs(conformance->getDeclContext()->getParentModule(),
                         conformance->getType())
      ->getCanonicalType());
  
  GenericParamList *conformanceParams = conformance->getGenericParams();
  
  // If the requirement is generic, reparent its generic parameter list to
  // the generic parameters of the conformance.
  CanType methodTy = witnessSubstTy.getResult();
  if (auto pft = dyn_cast<PolymorphicFunctionType>(methodTy)) {
    auto &reqtParams = pft->getGenericParams();
    // Preserve the depth of generic arguments by adding an empty outer generic
    // param list if the conformance is concrete.
    GenericParamList *outerParams = conformanceParams;
    if (!outerParams)
      outerParams = GenericParamList::getEmpty(getASTContext());
    auto methodParams
      = reqtParams.cloneWithOuterParameters(getASTContext(), outerParams);
    methodTy = CanPolymorphicFunctionType::get(pft.getInput(), pft.getResult(),
                                               methodParams,
                                               pft->getExtInfo());
  }
  
  // If the conformance is generic, its generic parameters apply to
  // the witness as its outer generic param list.
  if (conformanceParams) {
    witnessSubstTy = CanPolymorphicFunctionType::get(witnessSubstTy.getInput(),
                                                   methodTy,
                                                   conformanceParams,
                                                   witnessSubstTy->getExtInfo());
  } else {
    witnessSubstTy = CanFunctionType::get(witnessSubstTy.getInput(),
                                          methodTy,
                                          witnessSubstTy->getExtInfo());
  }
  
  // If the witness is a free function, consider the self argument
  // uncurry level.
  if (isFree)
    ++witnessUncurryLevel;
  
  // The witness SIL function has the type of the AST-level witness, at the
  // abstraction level of the original protocol requirement.
  assert(requirement.uncurryLevel == witnessUncurryLevel
         && "uncurry level of requirement and witness do not match");
      
  // In addition to the usual bevy of abstraction differences, protocol
  // witnesses have potential differences in @inout-ness of self. @mutating
  // value type methods may reassign 'self' as an @inout parameter, but may be
  // conformed to by non-@mutating or class methods that cannot.
  // Handle this special case in the witness type before applying the
  // abstraction change.
  auto inOutSelf = DoesNotHaveInOutSelfAbstractionDifference;
  if (!isa<InOutType>(witnessOrigTy.getInput()) &&
      isa<InOutType>(requirementTy.getInput())) {
    inOutSelf = HasInOutSelfAbstractionDifference;
  }
  
  // Work out the interface type for the witness.
  auto reqtIfaceTy
    = cast<GenericFunctionType>(Types.getConstantInfo(requirement)
                                     .FormalInterfaceType);
  // Substitute the 'self' type into the requirement to get the concrete witness
  // type, leaving the other generic parameters open.
  CanAnyFunctionType witnessSubstIfaceTy = cast<AnyFunctionType>(
    reqtIfaceTy->partialSubstGenericArgs(conformance->getDeclContext()->getParentModule(),
                                         conformance->getInterfaceType())
               ->getCanonicalType());
  
  // If the conformance is generic, its generic parameters apply to the witness.
  ArrayRef<GenericTypeParamType*> ifaceGenericParams;
  ArrayRef<Requirement> ifaceReqts;
  std::tie(ifaceGenericParams, ifaceReqts)
    = conformance->getGenericSignature();
  if (!ifaceGenericParams.empty() && !ifaceReqts.empty()) {
    if (auto gft = dyn_cast<GenericFunctionType>(witnessSubstIfaceTy)) {
      SmallVector<GenericTypeParamType*, 4> allParams(ifaceGenericParams.begin(),
                                                      ifaceGenericParams.end());
      allParams.append(gft->getGenericParams().begin(),
                       gft->getGenericParams().end());
      SmallVector<Requirement, 4> allReqts(ifaceReqts.begin(),
                                           ifaceReqts.end());
      allReqts.append(gft->getRequirements().begin(),
                      gft->getRequirements().end());
      witnessSubstIfaceTy = cast<GenericFunctionType>(
        GenericFunctionType::get(allParams, allReqts,
                                 gft.getInput(), gft.getResult(),
                                 gft->getExtInfo())
          ->getCanonicalType());
    } else {
      assert(isa<FunctionType>(witnessSubstIfaceTy));
      witnessSubstIfaceTy = cast<GenericFunctionType>(
        GenericFunctionType::get(ifaceGenericParams, ifaceReqts,
                                 witnessSubstIfaceTy.getInput(),
                                 witnessSubstIfaceTy.getResult(),
                                 witnessSubstIfaceTy->getExtInfo())
          ->getCanonicalType());
    }
  }
  // Lower the witness type with the requirement's abstraction level.
  // FIXME: We should go through TypeConverter::getLoweredType once we settle
  // on interface types.
  /*
  SILType witnessSILType = Types.getLoweredType(
                                              AbstractionPattern(requirementTy),
                                              witnessSubstTy,
                                              requirement.uncurryLevel);
   */
  SILType witnessSILType = getWitnessFunctionType(M,
                                              AbstractionPattern(requirementTy),
                                              witnessSubstTy,
                                              witnessSubstIfaceTy,
                                              requirement.uncurryLevel);

  // TODO: emit with shared linkage if the type and protocol are non-local.
  SILLinkage linkage = SILLinkage::Private;

  // Mangle the name of the witness thunk.
  llvm::SmallString<128> nameBuffer;
  {
    llvm::raw_svector_ostream nameStream(nameBuffer);
    nameStream << "_TTW";
    Mangler mangler(nameStream);
    mangler.mangleProtocolConformance(conformance);
    assert(isa<FuncDecl>(requirement.getDecl())
           && "need to handle mangling of non-Func SILDeclRefs here");
    mangler.mangleEntity(requirement.getDecl(), ResilienceExpansion::Minimal,
                         requirement.uncurryLevel);
  }
  
  auto *f = SILFunction::create(M, linkage, nameBuffer,
                witnessSILType.castTo<SILFunctionType>(),
                // FIXME: Tease out the logic above for forming the generic
                // param list for the PolymorphicFunctionType and use it to
                // directly get the context generic params here.
                witnessSILType.castTo<SILFunctionType>()->getGenericParams(),
                SILLocation(witness.getDecl()),
                IsNotBare,
                IsNotTransparent);
  
  f->setDebugScope(new (M) SILDebugScope(RegularLocation(witness.getDecl())));
  
  // Create the witness.
  SILGenFunction(*this, *f)
    .emitProtocolWitness(conformance, requirement, witness, witnessSubs,
                         isFree, inOutSelf);
  
  return f;
}

SILFunction *
SILGenModule::getOrCreateReabstractionThunk(SILLocation loc,
                                            CanSILFunctionType thunkType,
                                            CanSILFunctionType fromType,
                                            CanSILFunctionType toType) {
  // Mangle the reabstraction thunk.
  llvm::SmallString<256> buffer;
  {
    llvm::raw_svector_ostream stream(buffer);
    Mangler mangler(stream);

    // This is actually the SIL helper function.  For now, IR-gen
    // makes the actual thunk.
    stream << "_TTR";
    if (auto generics = thunkType->getGenericParams()) {
      stream << 'G';
      mangler.bindGenericParameters(generics, /*mangle*/ true);
    }
    mangler.mangleType(fromType, ResilienceExpansion::Minimal, /*uncurry*/ 0);
    mangler.mangleType(toType, ResilienceExpansion::Minimal, /*uncurry*/ 0);
  }

  // FIXME: Fake up context generic params for the thunk.
  return M.getOrCreateSharedFunction(loc, buffer.str(),
                                     thunkType, thunkType->getGenericParams(),
                                     IsBare, IsTransparent);
}
