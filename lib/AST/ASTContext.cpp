//===--- ASTContext.cpp - ASTContext Implementation -----------------------===//
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
//  This file implements the ASTContext class.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTContext.h"
#include "swift/Strings.h"
#include "swift/AST/ArchetypeBuilder.h"
#include "swift/AST/AST.h"
#include "swift/AST/ConcreteDeclRef.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/ExprHandle.h"
#include "swift/AST/KnownProtocols.h"
#include "swift/AST/LazyResolver.h"
#include "swift/AST/ModuleLoader.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/RawComment.h"
#include "swift/AST/TypeCheckerDebugConsumer.h"
#include "swift/Basic/SourceManager.h"
#include "llvm/Support/Allocator.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringMap.h"
#include <algorithm>
#include <memory>

using namespace swift;

LazyResolver::~LazyResolver() = default;
void ModuleLoader::anchor() {}
void DependencyTracker::anchor() {}

llvm::StringRef swift::getProtocolName(KnownProtocolKind kind) {
  switch (kind) {
#define PROTOCOL(Id) \
  case KnownProtocolKind::Id: \
    return #Id;
#include "swift/AST/KnownProtocols.def"
  }
  llvm_unreachable("bad KnownProtocolKind");
}

namespace {
  typedef std::tuple<ClassDecl *, ObjCSelector, bool> ObjCMethodConflict;
}

struct ASTContext::Implementation {
  Implementation();
  ~Implementation();

  llvm::BumpPtrAllocator Allocator; // used in later initializations

  /// The set of cleanups to be called when the ASTContext is destroyed.
  std::vector<std::function<void(void)>> Cleanups;

  llvm::StringMap<char, llvm::BumpPtrAllocator&> IdentifierTable;

  /// The declaration of Swift.String.
  NominalTypeDecl *StringDecl = nullptr;

  /// The declaration of Swift.Array<T>.
  NominalTypeDecl *ArrayDecl = nullptr;

  /// The declaration of Swift.Set<T>.
  NominalTypeDecl *SetDecl = nullptr;

  /// The declaration of Swift.Dictionary<T>.
  NominalTypeDecl *DictionaryDecl = nullptr;

  /// The declaration of Swift.Optional<T>.
  EnumDecl *OptionalDecl = nullptr;

  /// The declaration of Swift.Optional<T>.Some.
  EnumElementDecl *OptionalSomeDecl = nullptr;

  /// The declaration of Swift.Optional<T>.None.
  EnumElementDecl *OptionalNoneDecl = nullptr;

  /// The declaration of Swift.ImplicitlyUnwrappedOptional<T>.Some.
  EnumElementDecl *ImplicitlyUnwrappedOptionalSomeDecl = nullptr;

  /// The declaration of Swift.ImplicitlyUnwrappedOptional<T>.None.
  EnumElementDecl *ImplicitlyUnwrappedOptionalNoneDecl = nullptr;
  
  /// The declaration of Swift.UnsafeMutablePointer<T>.
  NominalTypeDecl *UnsafeMutablePointerDecl = nullptr;
  
  /// The declaration of Swift.UnsafePointer<T>.
  NominalTypeDecl *UnsafePointerDecl = nullptr;
  
  /// The declaration of Swift.AutoreleasingUnsafeMutablePointer<T>.
  NominalTypeDecl *AutoreleasingUnsafeMutablePointerDecl = nullptr;

  /// The declaration of Swift.CFunctionPointer<T -> U>.
  NominalTypeDecl *CFunctionPointerDecl = nullptr;

  /// The declaration of Swift.Void.
  TypeAliasDecl *VoidDecl = nullptr;

  /// The declaration of NSObject.
  NominalTypeDecl *NSObjectDecl = nullptr;

  // Declare cached declarations for each of the known declarations.
#define FUNC_DECL(Name, Id) FuncDecl *Get##Name = nullptr;
#include "swift/AST/KnownDecls.def"
  
  /// func _preconditionOptionalHasValue<T>(inout v: Optional<T>)
  FuncDecl *PreconditionOptionalHasValueDecls[NumOptionalTypeKinds] = {};

  /// func _doesOptionalHaveValue<T>(inout v: Optional<T>) -> Builtin.Int1
  FuncDecl *DoesOptionalHaveValueDecls[NumOptionalTypeKinds] = {};

  /// func _doesOptionalHaveValueAsBool<T>(v: Optional<T>) -> Bool
  FuncDecl *DoesOptionalHaveValueAsBoolDecls[NumOptionalTypeKinds] = {};

  /// func _getOptionalValue<T>(v : Optional<T>) -> T
  FuncDecl *GetOptionalValueDecls[NumOptionalTypeKinds] = {};

  /// The declaration of Swift.ImplicitlyUnwrappedOptional<T>.
  EnumDecl *ImplicitlyUnwrappedOptionalDecl = nullptr;

  /// func _getBool(Builtin.Int1) -> Bool
  FuncDecl *GetBoolDecl = nullptr;
  
  /// func ==(Int, Int) -> Bool
  FuncDecl *EqualIntDecl = nullptr;

  /// The declaration of Swift.nil.
  VarDecl *NilDecl = nullptr;

  /// func _unimplemented_initializer(className: StaticString).
  FuncDecl *UnimplementedInitializerDecl = nullptr;

  /// func _stdlib_isOSVersionAtLeast(Builtin.Word,Builtin.Word, Builtin.word)
  //    -> Builtin.Int1
  FuncDecl *IsOSVersionAtLeastDecl = nullptr;
  
  /// \brief The set of known protocols, lazily populated as needed.
  ProtocolDecl *KnownProtocols[NumKnownProtocols] = { };

  /// \brief The various module loaders that import external modules into this
  /// ASTContext.
  SmallVector<std::unique_ptr<swift::ModuleLoader>, 4> ModuleLoaders;

  /// \brief The module loader used to load Clang modules.
  ClangModuleLoader *TheClangModuleLoader = nullptr;

  /// \brief Map from Swift declarations to raw comments.
  llvm::DenseMap<const Decl *, RawComment> RawComments;

  /// \brief Map from Swift declarations to brief comments.
  llvm::DenseMap<const Decl *, StringRef> BriefComments;

  /// \brief Map from local declarations to their discriminators.
  /// Missing entries implicitly have value 0.
  llvm::DenseMap<const ValueDecl *, unsigned> LocalDiscriminators;

  /// \brief A cached unused pattern-binding initializer context.
  PatternBindingInitializer *UnusedPatternBindingContext = nullptr;

  /// \brief A cached unused default-argument initializer context.
  DefaultArgumentInitializer *UnusedDefaultArgumentContext = nullptr;

  /// \brief Structure that captures data that is segregated into different
  /// arenas.
  struct Arena {
    llvm::FoldingSet<TupleType> TupleTypes;
    llvm::DenseMap<std::pair<Type,char>, MetatypeType*> MetatypeTypes;
    llvm::DenseMap<std::pair<Type,char>,
                   ExistentialMetatypeType*> ExistentialMetatypeTypes;
    llvm::DenseMap<std::pair<Type,std::pair<Type,char>>, FunctionType*>
      FunctionTypes;
    llvm::DenseMap<Type, ArraySliceType*> ArraySliceTypes;
    llvm::DenseMap<std::pair<Type, Type>, DictionaryType *> DictionaryTypes;
    llvm::DenseMap<Type, OptionalType*> OptionalTypes;
    llvm::DenseMap<Type, ImplicitlyUnwrappedOptionalType*> ImplicitlyUnwrappedOptionalTypes;
    llvm::DenseMap<Type, ParenType*> ParenTypes;
    llvm::DenseMap<uintptr_t, ReferenceStorageType*> ReferenceStorageTypes;
    llvm::DenseMap<Type, LValueType*> LValueTypes;
    llvm::DenseMap<Type, InOutType*> InOutTypes;
    llvm::DenseMap<std::pair<Type, Type>, SubstitutedType *> SubstitutedTypes;
    llvm::DenseMap<std::pair<Type, void*>, DependentMemberType *>
      DependentMemberTypes;
    llvm::DenseMap<Type, DynamicSelfType *> DynamicSelfTypes;
    llvm::FoldingSet<EnumType> EnumTypes;
    llvm::FoldingSet<StructType> StructTypes;
    llvm::FoldingSet<ClassType> ClassTypes;
    llvm::FoldingSet<UnboundGenericType> UnboundGenericTypes;
    llvm::FoldingSet<BoundGenericType> BoundGenericTypes;

    llvm::DenseMap<BoundGenericType *, ArrayRef<Substitution>>
      BoundGenericSubstitutions;

    /// The set of normal protocol conformances.
    llvm::FoldingSet<NormalProtocolConformance> NormalConformances;

    /// The set of specialized protocol conformances.
    llvm::FoldingSet<SpecializedProtocolConformance> SpecializedConformances;

    /// The set of inherited protocol conformances.
    llvm::FoldingSet<InheritedProtocolConformance> InheritedConformances;

    /// ConformsTo - Caches the results of checking whether a given (canonical)
    /// type conforms to a given protocol.
    ConformsToMap ConformsTo;

    ~Arena() {
      for (auto &conformance : SpecializedConformances)
        conformance.~SpecializedProtocolConformance();
      for (auto &conformance : InheritedConformances)
        conformance.~InheritedProtocolConformance();

      // Call the normal conformance destructors last since they could be
      // referenced by the other conformance types.
      for (auto &conformance : NormalConformances)
        conformance.~NormalProtocolConformance();
    }

    size_t getTotalMemory() const;
  };

  llvm::DenseMap<Module*, ModuleType*> ModuleTypes;
  llvm::DenseMap<std::pair<unsigned, unsigned>, GenericTypeParamType *>
    GenericParamTypes;
  llvm::FoldingSet<GenericFunctionType> GenericFunctionTypes;
  llvm::FoldingSet<SILFunctionType> SILFunctionTypes;
  llvm::DenseMap<CanType, SILBlockStorageType *> SILBlockStorageTypes;
  llvm::DenseMap<BuiltinIntegerWidth, BuiltinIntegerType*> IntegerTypes;
  llvm::FoldingSet<ProtocolCompositionType> ProtocolCompositionTypes;
  llvm::FoldingSet<BuiltinVectorType> BuiltinVectorTypes;
  llvm::FoldingSet<GenericSignature> GenericSignatures;
  llvm::FoldingSet<DeclName::CompoundDeclName> CompoundNames;
  llvm::DenseMap<UUID, ArchetypeType *> OpenedExistentialArchetypes;

  /// List of Objective-C member conflicts we have found during type checking.
  std::vector<ObjCMethodConflict> ObjCMethodConflicts;

  /// List of Objective-C methods created by the type checker (and not
  /// by the Clang importer or deserialized), which is used for
  /// checking unintended Objective-C overrides.
  std::vector<AbstractFunctionDecl *> ObjCMethods;

  /// \brief The permanent arena.
  Arena Permanent;

  /// Temporary arena used for a constraint solver.
  struct ConstraintSolverArena : public Arena {
    /// The allocator used for all allocations within this arena.
    llvm::BumpPtrAllocator &Allocator;

    /// Callback used to get a type member of a type variable.
    GetTypeVariableMemberCallback GetTypeMember;

    ConstraintSolverArena(llvm::BumpPtrAllocator &allocator,
                          GetTypeVariableMemberCallback &&getTypeMember)
      : Allocator(allocator), GetTypeMember(std::move(getTypeMember)) { }

    ConstraintSolverArena(const ConstraintSolverArena &) = delete;
    ConstraintSolverArena(ConstraintSolverArena &&) = delete;
    ConstraintSolverArena &operator=(const ConstraintSolverArena &) = delete;
    ConstraintSolverArena &operator=(ConstraintSolverArena &&) = delete;
  };

  /// \brief The current constraint solver arena, if any.
  std::unique_ptr<ConstraintSolverArena> CurrentConstraintSolverArena;

  Arena &getArena(AllocationArena arena) {
    switch (arena) {
    case AllocationArena::Permanent:
      return Permanent;

    case AllocationArena::ConstraintSolver:
      assert(CurrentConstraintSolverArena && "No constraint solver active?");
      return *CurrentConstraintSolverArena;
    }
    llvm_unreachable("bad AllocationArena");
  }
};

ASTContext::Implementation::Implementation()
 : IdentifierTable(Allocator) {}
ASTContext::Implementation::~Implementation() {
  for (auto &cleanup : Cleanups)
    cleanup();
}

ConstraintCheckerArenaRAII::
ConstraintCheckerArenaRAII(ASTContext &self, llvm::BumpPtrAllocator &allocator,
                           GetTypeVariableMemberCallback getTypeMember)
  : Self(self), Data(self.Impl.CurrentConstraintSolverArena.release())
{
  Self.Impl.CurrentConstraintSolverArena.reset(
    new ASTContext::Implementation::ConstraintSolverArena(
          allocator,
          std::move(getTypeMember)));
}

ConstraintCheckerArenaRAII::~ConstraintCheckerArenaRAII() {
  Self.Impl.CurrentConstraintSolverArena.reset(
    (ASTContext::Implementation::ConstraintSolverArena *)Data);
}

static Module *createBuiltinModule(ASTContext &ctx) {
  auto M = Module::create(ctx.getIdentifier("Builtin"), ctx);
  M->addFile(*new (ctx) BuiltinUnit(*M));
  return M;
}

ASTContext::ASTContext(LangOptions &langOpts, SearchPathOptions &SearchPathOpts,
                       SourceManager &SourceMgr, DiagnosticEngine &Diags)
  : Impl(*new Implementation()),
    LangOpts(langOpts),
    SearchPathOpts(SearchPathOpts),
    SourceMgr(SourceMgr),
    Diags(Diags),
    TheBuiltinModule(createBuiltinModule(*this)),
    StdlibModuleName(getIdentifier(STDLIB_NAME)),
    ObjCModuleName(getIdentifier(OBJC_MODULE_NAME)),
    TypeCheckerDebug(new StderrTypeCheckerDebugConsumer()),
    TheErrorType(new (*this, AllocationArena::Permanent) ErrorType(*this)),
    TheEmptyTupleType(TupleType::get(ArrayRef<TupleTypeElt>(), *this)),
    TheNativeObjectType(new (*this, AllocationArena::Permanent)
                           BuiltinNativeObjectType(*this)),
    TheBridgeObjectType(new (*this, AllocationArena::Permanent)
                           BuiltinBridgeObjectType(*this)),
    TheUnknownObjectType(new (*this, AllocationArena::Permanent)
                         BuiltinUnknownObjectType(*this)),
    TheRawPointerType(new (*this, AllocationArena::Permanent)
                        BuiltinRawPointerType(*this)),
    TheIEEE32Type(new (*this, AllocationArena::Permanent)
                    BuiltinFloatType(BuiltinFloatType::IEEE32,*this)),
    TheIEEE64Type(new (*this, AllocationArena::Permanent)
                    BuiltinFloatType(BuiltinFloatType::IEEE64,*this)),
    TheIEEE16Type(new (*this, AllocationArena::Permanent)
                    BuiltinFloatType(BuiltinFloatType::IEEE16,*this)),
    TheIEEE80Type(new (*this, AllocationArena::Permanent)
                    BuiltinFloatType(BuiltinFloatType::IEEE80,*this)),
    TheIEEE128Type(new (*this, AllocationArena::Permanent)
                    BuiltinFloatType(BuiltinFloatType::IEEE128, *this)),
    ThePPC128Type(new (*this, AllocationArena::Permanent)
                    BuiltinFloatType(BuiltinFloatType::PPC128,*this)) {

  // Initialize all of the known identifiers.
#define IDENTIFIER(Id) Id_##Id = getIdentifier(#Id);
#define IDENTIFIER_WITH_NAME(Name, IdStr) Id_##Name = getIdentifier(IdStr);
#include "swift/AST/KnownIdentifiers.def"
}

ASTContext::~ASTContext() {
  delete &Impl;
}

llvm::BumpPtrAllocator &ASTContext::getAllocator(AllocationArena arena) const {
  switch (arena) {
  case AllocationArena::Permanent:
    return Impl.Allocator;

  case AllocationArena::ConstraintSolver:
    assert(Impl.CurrentConstraintSolverArena.get() != nullptr);
    return Impl.CurrentConstraintSolverArena->Allocator;
  }
  llvm_unreachable("bad AllocationArena");
}

/// getIdentifier - Return the uniqued and AST-Context-owned version of the
/// specified string.
Identifier ASTContext::getIdentifier(StringRef Str) const {
  // Make sure null pointers stay null.
  if (Str.data() == nullptr) return Identifier(0);

  auto I = Impl.IdentifierTable.insert(std::make_pair(Str, char())).first;
  return Identifier(I->getKeyData());
}

void ASTContext::lookupInSwiftModule(
                   StringRef name,
                   SmallVectorImpl<ValueDecl *> &results) const {
  Module *M = getStdlibModule();
  if (!M)
    return;

  // Find all of the declarations with this name in the Swift module.
  auto identifier = getIdentifier(name);
  M->lookupValue({ }, identifier, NLKind::UnqualifiedLookup, results);
}

NominalTypeDecl *ASTContext::getBoolDecl() const {
  SmallVector<ValueDecl*, 1> results;
  lookupInSwiftModule("Bool", results);
  for (auto result : results) {
    if (auto nominal = dyn_cast<NominalTypeDecl>(result)) {
      return nominal;
    }
  }
  return nullptr;
}

NominalTypeDecl *ASTContext::getIntDecl() const {
  SmallVector<ValueDecl*, 1> results;
  lookupInSwiftModule("Int", results);
  for (auto result : results) {
    if (auto nominal = dyn_cast<NominalTypeDecl>(result)) {
      return nominal;
    }
  }
  return nullptr;
}

NominalTypeDecl *ASTContext::getStringDecl() const {
  if (Impl.StringDecl) return Impl.StringDecl;

  SmallVector<ValueDecl*, 1> results;
  lookupInSwiftModule("String", results);
  for (auto result : results) {
    if (auto nominal = dyn_cast<NominalTypeDecl>(result)) {
      Impl.StringDecl = nominal;
      return Impl.StringDecl;
    }
  }
  return nullptr;
}

/// Find the generic implementation declaration for the named syntactic-sugar
/// type.
static NominalTypeDecl *findStdlibType(const ASTContext &ctx, StringRef name) {
  // Find all of the declarations with this name in the Swift module.
  SmallVector<ValueDecl *, 1> results;
  ctx.lookupInSwiftModule(name, results);
  for (auto result : results) {
    if (auto nominal = dyn_cast<NominalTypeDecl>(result)) {
      if (auto params = nominal->getGenericParams()) {
        if (params->size() == 1) {
          // We found it.
          return nominal;
        }
      }
    }
  }

  return nullptr;
}

NominalTypeDecl *ASTContext::getArrayDecl() const {
  if (!Impl.ArrayDecl)
    Impl.ArrayDecl = findStdlibType(*this, "Array");

  return Impl.ArrayDecl;
}

NominalTypeDecl *ASTContext::getSetDecl() const {
  if (!Impl.SetDecl)
    Impl.SetDecl = findStdlibType(*this, "_Set");

  return Impl.SetDecl;
}

NominalTypeDecl *ASTContext::getDictionaryDecl() const {
  if (!Impl.DictionaryDecl) {
    // Find all of the declarations with this name in the Swift module.
    SmallVector<ValueDecl *, 1> results;
    lookupInSwiftModule("Dictionary", results);
    for (auto result : results) {
      if (auto nominal = dyn_cast<NominalTypeDecl>(result)) {
        if (auto params = nominal->getGenericParams()) {
          if (params->size() == 2) {
            Impl.DictionaryDecl = nominal;
            break;
          }
        }
      }
    }

  }

  return Impl.DictionaryDecl;
}

EnumDecl *ASTContext::getOptionalDecl() const {
  if (!Impl.OptionalDecl)
    Impl.OptionalDecl
      = dyn_cast_or_null<EnumDecl>(findStdlibType(*this, "Optional"));

  return Impl.OptionalDecl;
}

static EnumElementDecl *findEnumElement(EnumDecl *e, StringRef name) {
  if (!e) return nullptr;
  auto ident = e->getASTContext().getIdentifier(name);
  for (auto elt : e->getAllElements()) {
    if (elt->getName() == ident)
      return elt;
  }
  return nullptr;
}

EnumElementDecl *ASTContext::getOptionalSomeDecl(OptionalTypeKind kind) const {
  switch (kind) {
  case OTK_Optional:
    return getOptionalSomeDecl();
  case OTK_ImplicitlyUnwrappedOptional:
    return getImplicitlyUnwrappedOptionalSomeDecl();
  case OTK_None:
    llvm_unreachable("getting Some decl for non-optional type?");
  }
  llvm_unreachable("bad OTK");
}

EnumElementDecl *ASTContext::getOptionalNoneDecl(OptionalTypeKind kind) const {
  switch (kind) {
  case OTK_Optional:
    return getOptionalNoneDecl();
  case OTK_ImplicitlyUnwrappedOptional:
    return getImplicitlyUnwrappedOptionalNoneDecl();
  case OTK_None:
    llvm_unreachable("getting None decl for non-optional type?");
  }
  llvm_unreachable("bad OTK");
}

EnumElementDecl *ASTContext::getOptionalSomeDecl() const {
  if (!Impl.OptionalSomeDecl)
    Impl.OptionalSomeDecl = findEnumElement(getOptionalDecl(), "Some");
  return Impl.OptionalSomeDecl;
}

EnumElementDecl *ASTContext::getOptionalNoneDecl() const {
  if (!Impl.OptionalNoneDecl)
    Impl.OptionalNoneDecl = findEnumElement(getOptionalDecl(), "None");
  return Impl.OptionalNoneDecl;
}

EnumDecl *ASTContext::getImplicitlyUnwrappedOptionalDecl() const {
  if (!Impl.ImplicitlyUnwrappedOptionalDecl)
    Impl.ImplicitlyUnwrappedOptionalDecl
      = dyn_cast_or_null<EnumDecl>(
          findStdlibType(*this, "ImplicitlyUnwrappedOptional"));

  return Impl.ImplicitlyUnwrappedOptionalDecl;
}

EnumElementDecl *ASTContext::getImplicitlyUnwrappedOptionalSomeDecl() const {
  if (!Impl.ImplicitlyUnwrappedOptionalSomeDecl)
    Impl.ImplicitlyUnwrappedOptionalSomeDecl =
      findEnumElement(getImplicitlyUnwrappedOptionalDecl(), "Some");
  return Impl.ImplicitlyUnwrappedOptionalSomeDecl;
}

EnumElementDecl *ASTContext::getImplicitlyUnwrappedOptionalNoneDecl() const {
  if (!Impl.ImplicitlyUnwrappedOptionalNoneDecl)
    Impl.ImplicitlyUnwrappedOptionalNoneDecl =
      findEnumElement(getImplicitlyUnwrappedOptionalDecl(), "None");
  return Impl.ImplicitlyUnwrappedOptionalNoneDecl;
}

NominalTypeDecl *ASTContext::getUnsafeMutablePointerDecl() const {
  if (!Impl.UnsafeMutablePointerDecl)
    Impl.UnsafeMutablePointerDecl = findStdlibType(
      *this, "UnsafeMutablePointer");
  
  return Impl.UnsafeMutablePointerDecl;
}

NominalTypeDecl *ASTContext::getUnsafePointerDecl() const {
  if (!Impl.UnsafePointerDecl)
    Impl.UnsafePointerDecl
      = findStdlibType(*this, "UnsafePointer");
  
  return Impl.UnsafePointerDecl;
}

NominalTypeDecl *ASTContext::getAutoreleasingUnsafeMutablePointerDecl() const {
  if (!Impl.AutoreleasingUnsafeMutablePointerDecl)
    Impl.AutoreleasingUnsafeMutablePointerDecl
      = findStdlibType(*this, "AutoreleasingUnsafeMutablePointer");
  
  return Impl.AutoreleasingUnsafeMutablePointerDecl;
}

NominalTypeDecl *ASTContext::getCFunctionPointerDecl() const {
  if (!Impl.CFunctionPointerDecl)
    Impl.CFunctionPointerDecl = findStdlibType(*this, "CFunctionPointer");
  
  return Impl.CFunctionPointerDecl;
}

TypeAliasDecl *ASTContext::getVoidDecl() const {
  if (Impl.VoidDecl) {
    return Impl.VoidDecl;
  }

  // Go find 'Void' in the Swift module.
  SmallVector<ValueDecl *, 1> results;
  lookupInSwiftModule("Void", results);
  for (auto result : results) {
    if (auto typeAlias = dyn_cast<TypeAliasDecl>(result)) {
      Impl.VoidDecl = typeAlias;
      return typeAlias;
    }
  }

  return Impl.VoidDecl;
}

ProtocolDecl *ASTContext::getProtocol(KnownProtocolKind kind) const {
  // Check whether we've already looked for and cached this protocol.
  unsigned index = (unsigned)kind;
  assert(index < NumKnownProtocols && "Number of known protocols is wrong");
  if (Impl.KnownProtocols[index])
    return Impl.KnownProtocols[index];

  // Find all of the declarations with this name in the Swift module.
  SmallVector<ValueDecl *, 1> results;
  lookupInSwiftModule(getProtocolName(kind), results);
  for (auto result : results) {
    if (auto protocol = dyn_cast<ProtocolDecl>(result)) {
      Impl.KnownProtocols[index] = protocol;
      return protocol;
    }
  }

  return nullptr;
}

/// Find the implementation for the given "intrinsic" library function.
static FuncDecl *findLibraryIntrinsic(const ASTContext &ctx,
                                      StringRef name,
                                      LazyResolver *resolver) {
  SmallVector<ValueDecl *, 1> results;
  ctx.lookupInSwiftModule(name, results);
  if (results.size() == 1) {
    if (auto FD = dyn_cast<FuncDecl>(results.front())) {
      if (resolver)
        resolver->resolveDeclSignature(FD);
      return FD;
    }
  }
  return nullptr;
}

static CanType stripImmediateLabels(CanType type) {
  while (auto tuple = dyn_cast<TupleType>(type)) {
    if (tuple->getNumElements() == 1) {
      type = tuple.getElementType(0);
    } else {
      break;
    }
  }
  return type;
}

/// Check whether the given function is non-generic.
static bool isNonGenericIntrinsic(FuncDecl *fn, CanType &input,
                                  CanType &output) {
  auto fnType = dyn_cast<FunctionType>(fn->getType()->getCanonicalType());
  if (!fnType)
    return false;

  input = stripImmediateLabels(fnType.getInput());
  output = stripImmediateLabels(fnType.getResult());
  return true;
}

/// Check whether the given type is Builtin.Int1.
static bool isBuiltinInt1Type(CanType type) {
  if (auto intType = dyn_cast<BuiltinIntegerType>(type))
    return intType->isFixedWidth() && intType->getFixedWidth() == 1;
  return false;
}

/// Check whether the given type is Builtin.Word.
static bool isBuiltinWordType(CanType type) {
  if (auto intType = dyn_cast<BuiltinIntegerType>(type))
    return intType->getWidth().isPointerWidth();
  return false;
}

FuncDecl *ASTContext::getGetBoolDecl(LazyResolver *resolver) const {
  if (Impl.GetBoolDecl)
    return Impl.GetBoolDecl;

  // Look for the function.
  CanType input, output;
  auto decl = findLibraryIntrinsic(*this, "_getBool", resolver);
  if (!decl || !isNonGenericIntrinsic(decl, input, output))
    return nullptr;

  // Input must be Builtin.Int1
  if (!isBuiltinInt1Type(input))
    return nullptr;

  // Output must be a global type named Bool.
  auto nominalType = dyn_cast<NominalType>(output);
  if (!nominalType ||
      nominalType.getParent() ||
      nominalType->getDecl()->getName().str() != "Bool")
    return nullptr;

  Impl.GetBoolDecl = decl;
  return decl;
}

FuncDecl *ASTContext::getEqualIntDecl(LazyResolver *resolver) const {
  if (Impl.EqualIntDecl)
    return Impl.EqualIntDecl;
  
  CanType intType = getIntDecl()->getDeclaredType().getCanonicalTypeOrNull();
  CanType boolType = getBoolDecl()->getDeclaredType().getCanonicalTypeOrNull();
  SmallVector<ValueDecl *, 30> equalFuncs;
  lookupInSwiftModule("==", equalFuncs);
  
  // Find the overload for Int.
  for (ValueDecl *vd : equalFuncs) {
    // All "==" decls should be functions, but who knows...
    FuncDecl *funcDecl = dyn_cast<FuncDecl>(vd);
    if (!funcDecl)
      continue;
    
    if (resolver)
      resolver->resolveDeclSignature(funcDecl);

    CanType input, resultType;
    if (!isNonGenericIntrinsic(funcDecl, input, resultType))
      continue;
    
    // Check for the signature: (Int, Int) -> Bool
    auto tType = dyn_cast<TupleType>(input.getPointer());
    assert(tType);
    if (tType->getNumElements() != 2)
      continue;

    CanType argType1 = tType->getElementType(0).getCanonicalTypeOrNull();
    CanType argType2 = tType->getElementType(1).getCanonicalTypeOrNull();
    if (argType1 == intType && argType2 == intType && resultType == boolType) {
      Impl.EqualIntDecl = funcDecl;
      return funcDecl;
    }
  }
  return nullptr;
}

FuncDecl *
ASTContext::getUnimplementedInitializerDecl(LazyResolver *resolver) const {
  if (Impl.UnimplementedInitializerDecl)
    return Impl.UnimplementedInitializerDecl;

  // Look for the function.
  CanType input, output;
  auto decl = findLibraryIntrinsic(*this, "_unimplemented_initializer",
                                   resolver);
  if (!decl || !isNonGenericIntrinsic(decl, input, output))
    return nullptr;

  // FIXME: Check inputs and outputs.

  Impl.UnimplementedInitializerDecl = decl;
  return decl;
}

FuncDecl *ASTContext::getIsOSVersionAtLeastDecl(LazyResolver *resolver) const {
  if (Impl.IsOSVersionAtLeastDecl)
    return Impl.IsOSVersionAtLeastDecl;

  // Look for the function.
  CanType input, output;
  auto decl =
      findLibraryIntrinsic(*this, "_stdlib_isOSVersionAtLeast", resolver);
  if (!decl || !isNonGenericIntrinsic(decl, input, output))
    return nullptr;

  // Input must be (Builtin.Word, Builtin.Word, Builtin.Word)
  auto inputTuple = dyn_cast<TupleType>(input);
  if (!inputTuple || inputTuple->getNumElements() != 3 ||
      !isBuiltinWordType(
          inputTuple->getElementType(0).getCanonicalTypeOrNull()) ||
      !isBuiltinWordType(
          inputTuple->getElementType(1).getCanonicalTypeOrNull()) ||
      !isBuiltinWordType(
          inputTuple->getElementType(2).getCanonicalTypeOrNull())) {
    return nullptr;
  }

  // Output must be Builtin.Int1
  if (!isBuiltinInt1Type(output))
    return nullptr;

  Impl.IsOSVersionAtLeastDecl = decl;
  return decl;
}

/// Check whether the given function is generic over a single,
/// unconstrained archetype.
static bool isGenericIntrinsic(FuncDecl *fn, CanType &input, CanType &output,
                               CanType &param) {
  auto fnType =
    dyn_cast<GenericFunctionType>(fn->getInterfaceType()->getCanonicalType());
  if (!fnType || fnType->getGenericParams().size() != 1)
    return false;

  bool hasRequirements = std::any_of(fnType->getRequirements().begin(),
                                     fnType->getRequirements().end(),
                                     [](const Requirement &req) -> bool {
    return req.getKind() != RequirementKind::WitnessMarker;
  });
  if (hasRequirements)
    return false;

  param = CanGenericTypeParamType(fnType->getGenericParams().front());
  input = stripImmediateLabels(fnType.getInput());
  output = stripImmediateLabels(fnType.getResult());
  return true;
}

// Find library intrinsic function.
static FuncDecl *findLibraryFunction(const ASTContext &ctx, FuncDecl *&cache, 
                                     StringRef name, LazyResolver *resolver) {
  if (cache) return cache;

  // Look for a generic function.
  cache = findLibraryIntrinsic(ctx, name, resolver);
  return cache;
}

#define FUNC_DECL(Name, Id)                                         \
FuncDecl *ASTContext::get##Name(LazyResolver *resolver) const {     \
  return findLibraryFunction(*this, Impl.Get##Name, Id, resolver);  \
}
#include "swift/AST/KnownDecls.def"

/// Check whether the given type is Optional applied to the given
/// type argument.
static bool isOptionalType(const ASTContext &ctx,
                           OptionalTypeKind optionalKind,
                           CanType type, CanType arg) {
  if (auto boundType = dyn_cast<BoundGenericType>(type)) {
    return (boundType->getDecl()->classifyAsOptionalType() == optionalKind &&
            boundType.getGenericArgs().size() == 1 &&
            boundType.getGenericArgs()[0] == arg);
  }
  return false;
}

/// Turn an OptionalTypeKind into an index into one of the caches.
static unsigned asIndex(OptionalTypeKind optionalKind) {
  assert(optionalKind && "passed a non-optional type kind?");
  return unsigned(optionalKind) - 1;
}

#define getOptionalIntrinsicName(PREFIX, KIND, SUFFIX) \
  ((KIND) == OTK_Optional                              \
    ? (PREFIX "Optional" SUFFIX)                       \
    : (PREFIX "ImplicitlyUnwrappedOptional" SUFFIX))

FuncDecl *ASTContext::getPreconditionOptionalHasValueDecl(
                  LazyResolver *resolver, OptionalTypeKind optionalKind) const {
  auto &cache = Impl.PreconditionOptionalHasValueDecls[asIndex(optionalKind)];
  if (cache) return cache;

  auto name
    = getOptionalIntrinsicName("_precondition", optionalKind, "HasValue");

  // Look for a generic function.
  CanType input, output, param;
  auto decl = findLibraryIntrinsic(*this, name, resolver);
  if (!decl || !isGenericIntrinsic(decl, input, output, param))
    return nullptr;

  // Input must be inout Optional<T>.
  auto inputInOut = dyn_cast<InOutType>(input);
  if (!inputInOut || !isOptionalType(*this, optionalKind,
                                     inputInOut.getObjectType(), param))
    return nullptr;

  // Output must be ().
  if (output != CanType(TheEmptyTupleType))
    return nullptr;

  cache = decl;
  return decl;
}

FuncDecl *ASTContext::getDoesOptionalHaveValueAsBoolDecl(
    LazyResolver *resolver, OptionalTypeKind optionalKind) const {
  auto &cache = Impl.DoesOptionalHaveValueAsBoolDecls[asIndex(optionalKind)];
  if (cache)
    return cache;

  auto name =
      getOptionalIntrinsicName("_does", optionalKind, "HaveValueAsBool");

  // Look for a generic function.
  CanType input, output, param;
  auto decl = findLibraryIntrinsic(*this, name, resolver);
  if (!decl || !isGenericIntrinsic(decl, input, output, param))
    return nullptr;

  // Input must be Optional<T>.
  if (!isOptionalType(*this, optionalKind, input, param))
    return nullptr;

  // Output must be a global type named Bool.
  auto nominalType = dyn_cast<NominalType>(output);
  if (!nominalType || nominalType.getParent() ||
      nominalType->getDecl()->getName().str() != "Bool")
    return nullptr;

  cache = decl;
  return decl;
}

FuncDecl *ASTContext::getGetOptionalValueDecl(LazyResolver *resolver,
                                         OptionalTypeKind optionalKind) const {
  auto &cache = Impl.GetOptionalValueDecls[asIndex(optionalKind)];
  if (cache) return cache;

  auto name = getOptionalIntrinsicName("_get", optionalKind, "Value");

  // Look for the function.
  CanType input, output, param;
  auto decl = findLibraryIntrinsic(*this, name, resolver);
  if (!decl || !isGenericIntrinsic(decl, input, output, param))
    return nullptr;

  // Input must be Optional<T>.
  if (!isOptionalType(*this, optionalKind, input, param))
    return nullptr;

  // Output must be T.
  if (output != param)
    return nullptr;

  cache = decl;
  return decl;
}

static bool hasOptionalIntrinsics(const ASTContext &ctx, LazyResolver *resolver,
                                  OptionalTypeKind optionalKind) {
  return ctx.getPreconditionOptionalHasValueDecl(resolver, optionalKind) &&
         ctx.getGetOptionalValueDecl(resolver, optionalKind);
}

bool ASTContext::hasOptionalIntrinsics(LazyResolver *resolver) const {
  return getOptionalDecl() &&
         getOptionalSomeDecl() &&
         getOptionalNoneDecl() &&
         ::hasOptionalIntrinsics(*this, resolver, OTK_Optional) &&
         ::hasOptionalIntrinsics(*this, resolver, OTK_ImplicitlyUnwrappedOptional);
}

bool ASTContext::hasPointerArgumentIntrinsics(LazyResolver *resolver) const {
  return getUnsafeMutablePointerDecl()
    && getUnsafePointerDecl()
    && (!LangOpts.EnableObjCInterop || getAutoreleasingUnsafeMutablePointerDecl())
    && getConvertPointerToPointerArgument(resolver)
    && getConvertMutableArrayToPointerArgument(resolver)
    && getConvertConstArrayToPointerArgument(resolver)
    && getConvertConstStringToUTF8PointerArgument(resolver)
    && getConvertInOutToPointerArgument(resolver);
}

bool ASTContext::hasArrayLiteralIntrinsics(LazyResolver *resolver) const {
  return getArrayDecl()
    && getAllocateUninitializedArray(resolver);
}

void ASTContext::addedExternalDecl(Decl *decl) {
  ExternalDefinitions.insert(decl);
}

void ASTContext::addCleanup(std::function<void(void)> cleanup) {
  Impl.Cleanups.push_back(std::move(cleanup));
}

bool ASTContext::hadError() const {
  return Diags.hadAnyError();
}

/// \brief Retrieve the arena from which we should allocate storage for a type.
static AllocationArena getArena(RecursiveTypeProperties properties) {
  bool hasTypeVariable = properties.hasTypeVariable();
  return hasTypeVariable? AllocationArena::ConstraintSolver
                        : AllocationArena::Permanent;
}

Optional<ArrayRef<Substitution>>
ASTContext::createTrivialSubstitutions(BoundGenericType *BGT) const {
  assert(BGT->isCanonical() && "Requesting non-canonical substitutions");
  auto Params = BGT->getDecl()->getGenericParams()->getParams();
  assert(Params.size() == 1);
  auto Param = Params[0];
  assert(Param->getArchetype() && "Not type-checked yet");
  Substitution Subst(Param->getArchetype(), BGT->getGenericArgs()[0], {});
  auto Substitutions = AllocateCopy(llvm::makeArrayRef(Subst));
  auto arena = getArena(BGT->getRecursiveProperties());
  Impl.getArena(arena).BoundGenericSubstitutions.
    insert(std::make_pair(BGT, Substitutions));
  return Substitutions;
}

Optional<ArrayRef<Substitution>>
ASTContext::getSubstitutions(BoundGenericType* bound) const {
  auto arena = getArena(bound->getRecursiveProperties());
  assert(bound->isCanonical() && "Requesting non-canonical substitutions");
  auto &boundGenericSubstitutions
    = Impl.getArena(arena).BoundGenericSubstitutions;
  auto known = boundGenericSubstitutions.find(bound);
  if (known != boundGenericSubstitutions.end())
    return known->second;

  // We can trivially create substitutions for Array and Optional.
  if (bound->getDecl() == getArrayDecl() ||
      bound->getDecl() == getOptionalDecl())
    return createTrivialSubstitutions(bound);

  return None;
}

void ASTContext::setSubstitutions(BoundGenericType* Bound,
                                  ArrayRef<Substitution> Subs) const {
  auto arena = getArena(Bound->getRecursiveProperties());
  auto &boundGenericSubstitutions
    = Impl.getArena(arena).BoundGenericSubstitutions;
  assert(Bound->isCanonical() && "Requesting non-canonical substitutions");
  assert(boundGenericSubstitutions.count(Bound) == 0 &&
         "Already have substitutions?");
  boundGenericSubstitutions[Bound] = Subs;
}

Type ASTContext::getTypeVariableMemberType(TypeVariableType *baseTypeVar,
                                           AssociatedTypeDecl *assocType) {
  auto &arena = *Impl.CurrentConstraintSolverArena;
  return arena.GetTypeMember(baseTypeVar, assocType);
}

void ASTContext::addModuleLoader(std::unique_ptr<ModuleLoader> loader,
                                 bool IsClang) {
  if (IsClang) {
    assert(!Impl.TheClangModuleLoader && "Already have a Clang module loader");
    Impl.TheClangModuleLoader =
      static_cast<ClangModuleLoader *>(loader.get());
  }
  Impl.ModuleLoaders.push_back(std::move(loader));
}

void ASTContext::loadExtensions(NominalTypeDecl *nominal,
                                unsigned previousGeneration) {
  for (auto &loader : Impl.ModuleLoaders) {
    loader->loadExtensions(nominal, previousGeneration);
  }
}

void ASTContext::loadObjCMethods(
       ClassDecl *classDecl,
       ObjCSelector selector,
       bool isInstanceMethod,
       unsigned previousGeneration,
       llvm::TinyPtrVector<AbstractFunctionDecl *> &methods) {
  for (auto &loader : Impl.ModuleLoaders) {
    loader->loadObjCMethods(classDecl, selector, isInstanceMethod,
                            previousGeneration, methods);
  }
}

void ASTContext::verifyAllLoadedModules() const {
  for (auto &loader : Impl.ModuleLoaders)
    loader->verifyAllModules();
}

ClangModuleLoader *ASTContext::getClangModuleLoader() const {
  return Impl.TheClangModuleLoader;
}

static void recordKnownProtocol(Module *Stdlib, StringRef Name,
                                KnownProtocolKind Kind) {
  Identifier ID = Stdlib->Ctx.getIdentifier(Name);
  UnqualifiedLookup Lookup(ID, Stdlib, nullptr, SourceLoc(), /*IsType=*/true);
  if (auto Proto = dyn_cast_or_null<ProtocolDecl>(Lookup.getSingleTypeResult()))
    Proto->setKnownProtocolKind(Kind);
}

void ASTContext::recordKnownProtocols(Module *Stdlib) {
#define PROTOCOL(Name) \
  recordKnownProtocol(Stdlib, #Name, KnownProtocolKind::Name);
#include "swift/AST/KnownProtocols.def"
}

void ASTContext::recordConformingDecl(ValueDecl *ConformingD,
                                      ValueDecl *ConformanceD) {
  assert(ConformingD && ConformanceD);
  auto &Vec = ConformingDeclMap[ConformingD];
  // The vector should commonly have few elements.
  if (std::find(Vec.begin(), Vec.end(), ConformanceD) == Vec.end()) {
    Vec.push_back(ConformanceD);
    ConformingD->setConformsToProtocolRequirement();
  }
}

ArrayRef<ValueDecl *> ASTContext::getConformances(const ValueDecl *D) {
  return ConformingDeclMap[D];
}

Module *ASTContext::getLoadedModule(
    ArrayRef<std::pair<Identifier, SourceLoc>> ModulePath) const {
  assert(!ModulePath.empty());

  // TODO: Swift submodules.
  if (ModulePath.size() == 1) {
    return getLoadedModule(ModulePath[0].first);
  }
  return nullptr;
}

Module *ASTContext::getLoadedModule(Identifier ModuleName) const {
  return LoadedModules.lookup(ModuleName);
}

Module *
ASTContext::getModule(ArrayRef<std::pair<Identifier, SourceLoc>> ModulePath) {
  assert(!ModulePath.empty());

  if (auto *M = getLoadedModule(ModulePath))
    return M;

  auto moduleID = ModulePath[0];
  for (auto &importer : Impl.ModuleLoaders) {
    if (Module *M = importer->loadModule(moduleID.second, ModulePath)) {
      if (ModulePath.size() == 1 && ModulePath[0].first == StdlibModuleName)
        recordKnownProtocols(M);
      return M;
    }
  }

  return nullptr;
}

Module *ASTContext::getStdlibModule(bool loadIfAbsent) {
  if (TheStdlibModule)
    return TheStdlibModule;

  if (loadIfAbsent) {
    auto mutableThis = const_cast<ASTContext*>(this);
    TheStdlibModule =
      mutableThis->getModule({ std::make_pair(StdlibModuleName, SourceLoc()) });
  } else {
    TheStdlibModule = getLoadedModule(StdlibModuleName);
  }
  return TheStdlibModule;
}

Optional<RawComment> ASTContext::getRawComment(const Decl *D) {
  auto Known = Impl.RawComments.find(D);
  if (Known == Impl.RawComments.end())
    return None;

  return Known->second;
}

void ASTContext::setRawComment(const Decl *D, RawComment RC) {
  Impl.RawComments[D] = RC;
}

Optional<StringRef> ASTContext::getBriefComment(const Decl *D) {
  auto Known = Impl.BriefComments.find(D);
  if (Known == Impl.BriefComments.end())
    return None;

  return Known->second;
}

void ASTContext::setBriefComment(const Decl *D, StringRef Comment) {
  Impl.BriefComments[D] = Comment;
}

unsigned ValueDecl::getLocalDiscriminator() const {
  assert(getDeclContext()->isLocalContext());
  auto &discriminators = getASTContext().Impl.LocalDiscriminators;
  auto it = discriminators.find(this);
  if (it == discriminators.end())
    return 0;
  return it->second;
}

void ValueDecl::setLocalDiscriminator(unsigned index) {
  assert(getDeclContext()->isLocalContext());
  if (!index) {
    assert(!getASTContext().Impl.LocalDiscriminators.count(this));
    return;
  }
  getASTContext().Impl.LocalDiscriminators.insert({this, index});
}

PatternBindingInitializer *
ASTContext::createPatternBindingContext(PatternBindingDecl *binding) {
  // Check for an existing context we can re-use.
  if (auto existing = Impl.UnusedPatternBindingContext) {
    Impl.UnusedPatternBindingContext = nullptr;
    existing->reset(binding);
    return existing;
  }

  return new (*this) PatternBindingInitializer(binding);
}
void ASTContext::destroyPatternBindingContext(PatternBindingInitializer *DC) {
  // There isn't much value in caching more than one of these.
  Impl.UnusedPatternBindingContext = DC;
}

DefaultArgumentInitializer *
ASTContext::createDefaultArgumentContext(DeclContext *fn, unsigned index) {
  // Check for an existing context we can re-use.
  if (auto existing = Impl.UnusedDefaultArgumentContext) {
    Impl.UnusedDefaultArgumentContext = nullptr;
    existing->reset(fn, index);
    return existing;
  }

  return new (*this) DefaultArgumentInitializer(fn, index);
}
void ASTContext::destroyDefaultArgumentContext(DefaultArgumentInitializer *DC) {
  // There isn't much value in caching more than one of these.
  Impl.UnusedDefaultArgumentContext = DC;
}

Optional<ConformanceEntry> ASTContext::getConformsTo(CanType type,
                                                     ProtocolDecl *proto) {
  auto arena = getArena(type->getRecursiveProperties());
  auto &conformsTo = Impl.getArena(arena).ConformsTo;
  auto known = conformsTo.find({type, proto});
  if (known == conformsTo.end())
    return None;

  return known->second;
}

void ASTContext::setConformsTo(CanType type, ProtocolDecl *proto,
                               ConformanceEntry entry) {
  assert(!type->is<GenericTypeParamType>());
  auto arena = getArena(type->getRecursiveProperties());
  auto &conformsTo = Impl.getArena(arena).ConformsTo;
  conformsTo[{type, proto}] = entry;
}

NormalProtocolConformance *
ASTContext::getConformance(Type conformingType,
                           ProtocolDecl *protocol,
                           SourceLoc loc,
                           DeclContext *dc,
                           ProtocolConformanceState state) {
  llvm::FoldingSetNodeID id;
  NormalProtocolConformance::Profile(id, conformingType, protocol, dc);

  // Did we already record the normal conformance?
  void *insertPos;
  auto &normalConformances =
    Impl.getArena(AllocationArena::Permanent).NormalConformances;
  if (auto result = normalConformances.FindNodeOrInsertPos(id, insertPos))
    return result;

  // Build a new normal protocol conformance.
  auto result
    = new (*this, AllocationArena::Permanent)
      NormalProtocolConformance(conformingType, protocol, loc, dc, state);
  normalConformances.InsertNode(result, insertPos);

  return result;
}

SpecializedProtocolConformance *
ASTContext::getSpecializedConformance(Type type,
                                      ProtocolConformance *generic,
                                      ArrayRef<Substitution> substitutions) {
  llvm::FoldingSetNodeID id;
  SpecializedProtocolConformance::Profile(id, type, generic);

  // Figure out which arena this conformance should go into.
  AllocationArena arena = getArena(type->getRecursiveProperties());

  // Did we already record the specialized conformance?
  void *insertPos;
  auto &specializedConformances = Impl.getArena(arena).SpecializedConformances;
  if (auto result = specializedConformances.FindNodeOrInsertPos(id, insertPos))
    return result;

  // Build a new specialized conformance.
  substitutions = AllocateCopy(substitutions, arena);
  auto result
    = new (*this, arena) SpecializedProtocolConformance(type, generic,
                                                        substitutions);
  specializedConformances.InsertNode(result, insertPos);
  return result;
}

InheritedProtocolConformance *
ASTContext::getInheritedConformance(Type type, ProtocolConformance *inherited) {
  llvm::FoldingSetNodeID id;
  InheritedProtocolConformance::Profile(id, type, inherited);

  // Figure out which arena this conformance should go into.
  AllocationArena arena = getArena(type->getRecursiveProperties());

  // Did we already record the normal protocol conformance?
  void *insertPos;
  auto &inheritedConformances = Impl.getArena(arena).InheritedConformances;
  if (auto result
        = inheritedConformances.FindNodeOrInsertPos(id, insertPos))
    return result;

  // Build a new normal protocol conformance.
  auto result = new (*this, arena) InheritedProtocolConformance(type, inherited);
  inheritedConformances.InsertNode(result, insertPos);
  return result;
}

size_t ASTContext::getTotalMemory() const {
  size_t Size = sizeof(*this) +
    //LoadedModules ?
    // ExternalDefinitions ?
    llvm::capacity_in_bytes(ConformingDeclMap) +
    llvm::capacity_in_bytes(CanonicalGenericTypeParamTypeNames) +
    // RemappedTypes ?
    sizeof(Impl) +
    Impl.Allocator.getTotalMemory() +
    Impl.Cleanups.capacity() +
    llvm::capacity_in_bytes(Impl.ModuleLoaders) +
    llvm::capacity_in_bytes(Impl.RawComments) +
    llvm::capacity_in_bytes(Impl.BriefComments) +
    llvm::capacity_in_bytes(Impl.LocalDiscriminators) +
    llvm::capacity_in_bytes(Impl.ModuleTypes) +
    llvm::capacity_in_bytes(Impl.GenericParamTypes) +
    // Impl.GenericFunctionTypes ?
    // Impl.SILFunctionTypes ?
    llvm::capacity_in_bytes(Impl.SILBlockStorageTypes) +
    llvm::capacity_in_bytes(Impl.IntegerTypes) +
    // Impl.ProtocolCompositionTypes ?
    // Impl.BuiltinVectorTypes ?
    // Impl.GenericSignatures ?
    // Impl.CompoundNames ?
    Impl.OpenedExistentialArchetypes.getMemorySize() +
    Impl.Permanent.getTotalMemory();

    Size += getSolverMemory();

    return Size;
}

size_t ASTContext::getSolverMemory() const {
  size_t Size = 0;
  
  if (Impl.CurrentConstraintSolverArena) {
    Size += Impl.CurrentConstraintSolverArena->getTotalMemory();
  }
  
  return Size;
}

size_t ASTContext::Implementation::Arena::getTotalMemory() const {
  return sizeof(*this) +
    // TupleTypes ?
    llvm::capacity_in_bytes(MetatypeTypes) +
    llvm::capacity_in_bytes(ExistentialMetatypeTypes) +
    llvm::capacity_in_bytes(FunctionTypes) +
    llvm::capacity_in_bytes(ArraySliceTypes) +
    llvm::capacity_in_bytes(DictionaryTypes) +
    llvm::capacity_in_bytes(OptionalTypes) +
    llvm::capacity_in_bytes(ImplicitlyUnwrappedOptionalTypes) +
    llvm::capacity_in_bytes(ParenTypes) +
    llvm::capacity_in_bytes(ReferenceStorageTypes) +
    llvm::capacity_in_bytes(LValueTypes) +
    llvm::capacity_in_bytes(InOutTypes) +
    llvm::capacity_in_bytes(SubstitutedTypes) +
    llvm::capacity_in_bytes(DependentMemberTypes) +
    llvm::capacity_in_bytes(DynamicSelfTypes) +
    // EnumTypes ?
    // StructTypes ?
    // ClassTypes ?
    // UnboundGenericTypes ?
    // BoundGenericTypes ?
    llvm::capacity_in_bytes(BoundGenericSubstitutions) +
    // NormalConformances ?
    // SpecializedConformances ?
    // InheritedConformances ?
    llvm::capacity_in_bytes(ConformsTo);
}

namespace {
  /// Produce a deterministic ordering of the given declarations.
  class OrderDeclarations {
    SourceManager &SrcMgr;

  public:
    OrderDeclarations(SourceManager &srcMgr) : SrcMgr(srcMgr) { }

    bool operator()(ValueDecl *lhs, ValueDecl *rhs) const {
      // If the declarations come from different modules, order based on the
      // module.
      Module *lhsModule = lhs->getDeclContext()->getParentModule();
      Module *rhsModule = rhs->getDeclContext()->getParentModule();
      if (lhsModule != rhsModule) {
        return lhsModule->Name.str() < rhsModule->Name.str();
      }

      // If the two declarations are in the same source file, order based on
      // location within that source file.
      SourceFile *lhsSF = lhs->getDeclContext()->getParentSourceFile();
      SourceFile *rhsSF = rhs->getDeclContext()->getParentSourceFile();
      if (lhsSF == rhsSF) {
        // If only one location is valid, the valid location comes first.
        if (lhs->getLoc().isValid() != rhs->getLoc().isValid()) {
          return lhs->getLoc().isValid();
        }

        // Prefer the declaration that comes first in the source file.
        return SrcMgr.isBeforeInBuffer(lhs->getLoc(), rhs->getLoc());
      }

      // The declarations are in different source files (or unknown source
      // files) of the same module. Order based on name.
      // FIXME: This isn't a total ordering.
      return lhs->getFullName() < rhs->getFullName();
    }
  };

  /// Produce a deterministic ordering of the given declarations with
  /// a bias that favors declarations in the given source file and
  /// members of a class.
  class OrderDeclarationsWithSourceFileAndClassBias {
    SourceManager &SrcMgr;
    SourceFile &SF;

  public:
    OrderDeclarationsWithSourceFileAndClassBias(SourceManager &srcMgr, 
                                                SourceFile &sf)
      : SrcMgr(srcMgr), SF(sf) { }

    bool operator()(ValueDecl *lhs, ValueDecl *rhs) const {
      // Check whether the declarations are in a class.
      bool lhsInClass = isa<ClassDecl>(lhs->getDeclContext());
      bool rhsInClass = isa<ClassDecl>(rhs->getDeclContext());
      if (lhsInClass != rhsInClass)
        return lhsInClass;

      // If the two declarations are in different source files, and one of those
      // source files is the source file we're biasing toward, prefer that
      // declaration.
      SourceFile *lhsSF = lhs->getDeclContext()->getParentSourceFile();
      SourceFile *rhsSF = rhs->getDeclContext()->getParentSourceFile();
      if (lhsSF != rhsSF) {
        if (lhsSF == &SF) return true;
        if (rhsSF == &SF) return false;
      }

      // Fall back to the normal deterministic ordering.
      return OrderDeclarations(SrcMgr)(lhs, rhs);
    }
  };
}

/// Compute the information used to describe an Objective-C redeclaration.
static std::pair<unsigned, DeclName> getObjCRedeclInfo(
                                       AbstractFunctionDecl *member) {
  if (isa<ConstructorDecl>(member))
    return { 0 + member->isImplicit(), member->getFullName() };

  if (isa<DestructorDecl>(member))
    return { 2 + member->isImplicit(), member->getFullName() };

  auto func = dyn_cast<FuncDecl>(member);
  switch (func->getAccessorKind()) {
  case AccessorKind::IsAddressor:
  case AccessorKind::IsDidSet:
  case AccessorKind::IsMaterializeForSet:
  case AccessorKind::IsMutableAddressor:
  case AccessorKind::IsWillSet:
    llvm_unreachable("Not an Objective-C entry point");

  case AccessorKind::IsGetter:
    if (auto var = dyn_cast<VarDecl>(func->getAccessorStorageDecl()))
      return { 5, var->getFullName() };

    return { 6, Identifier() };

  case AccessorKind::IsSetter:
    if (auto var = dyn_cast<VarDecl>(func->getAccessorStorageDecl()))
      return { 7, var->getFullName() };
    return { 8, Identifier() };

  case AccessorKind::NotAccessor:
    // Normal method.
    return { 4, func->getFullName() };
  }
}

void ASTContext::recordObjCMethod(AbstractFunctionDecl *func) {
  // If this method comes from Objective-C, ignore it.
  if (func->hasClangNode())
    return;

  Impl.ObjCMethods.push_back(func);
}

/// Lookup for an Objective-C method with the given selector in the
/// given class type or any of its superclasses.
static AbstractFunctionDecl *lookupObjCMethodInType(Type classType,
                                                    ObjCSelector selector,
                                                    bool isInstanceMethod,
                                                    SourceManager &srcMgr) {
  // Dig out the declaration of the class.
  auto classDecl = classType->getClassOrBoundGenericClass();
  if (!classDecl)
    return nullptr;

  // Look for an Objective-C method in this class.
  auto methods = classDecl->lookupDirect(selector, isInstanceMethod);
  if (!methods.empty()) {
    return *std::min_element(methods.begin(), methods.end(),
                             OrderDeclarations(srcMgr));
  }

  // Recurse into the superclass.
  if (!classDecl->hasSuperclass())
    return nullptr;

  return lookupObjCMethodInType(classDecl->getSuperclass(), selector,
                                isInstanceMethod, srcMgr);
}

bool ASTContext::diagnoseUnintendedObjCMethodOverrides(SourceFile &sf) {
  // Capture the methods in this source file.
  llvm::SmallVector<AbstractFunctionDecl *, 4> methods;
  auto captureMethodInSourceFile = [&](AbstractFunctionDecl *method) -> bool {
    if (method->getDeclContext()->getParentSourceFile() == &sf) {
      methods.push_back(method);
      return true;
    }

    return false;
  };
  Impl.ObjCMethods.erase(std::remove_if(Impl.ObjCMethods.begin(),
                                        Impl.ObjCMethods.end(),
                                        captureMethodInSourceFile),
                         Impl.ObjCMethods.end());

  // If no Objective-C methods were defined in this file, we're done.
  if (methods.empty())
    return false;

  // Sort the methods by declaration order.
  std::sort(methods.begin(), methods.end(), OrderDeclarations(SourceMgr));

  // For each Objective-C method declared in this file, check whether
  // it overrides something in one of its superclasses. We
  // intentionally don't respect access control here, since everything
  // is visible to the Objective-C runtime.
  bool diagnosedAny = false;
  for (auto method : methods) {
    // If the method has an @objc override, we don't need to do any
    // more checking.
    if (auto overridden = method->getOverriddenDecl()) {
      if (overridden->isObjC())
        continue;
    }

    // Skip deinitializers.
    if (isa<DestructorDecl>(method))
      continue;

    // Skip invalid declarations.
    if (method->isInvalid())
      continue;

    // Skip declarations with an invalid 'override' attribute on them.
    if (auto attr = method->getAttrs().getAttribute<OverrideAttr>(true)) {
      if (attr->isInvalid())
        continue;
    }

    auto classDecl = method->getDeclContext()->isClassOrClassExtensionContext();
    if (!classDecl)
      continue; // error-recovery path, only

    if (!classDecl->hasSuperclass())
      continue;

    // Look for a method that we have overridden in one of our
    // superclasses.
    auto selector = method->getObjCSelector();
    bool isInstanceMethod
      = method->isInstanceMember() || isa<ConstructorDecl>(method);
    AbstractFunctionDecl *overriddenMethod
      = lookupObjCMethodInType(classDecl->getSuperclass(),
                               selector, isInstanceMethod,
                               SourceMgr);
    if (!overriddenMethod)
      continue;

    // Ignore stub implementations.
    if (auto overriddenCtor = dyn_cast<ConstructorDecl>(overriddenMethod)) {
      if (overriddenCtor->hasStubImplementation())
        continue;
    }

    // Diagnose the override.
    auto methodRedeclInfo = getObjCRedeclInfo(method);
    auto overriddenRedeclInfo = getObjCRedeclInfo(overriddenMethod);
    Diags.diagnose(method->getLoc(), diag::objc_override_other,
                   methodRedeclInfo.first,
                   methodRedeclInfo.second,
                   selector,
                   overriddenMethod->getDeclContext()
                     ->getDeclaredInterfaceType());
    Diags.diagnose(overriddenMethod->getLoc(), diag::objc_override_other_here,
                   overriddenRedeclInfo.first,
                   overriddenRedeclInfo.second,
                   selector);

    diagnosedAny = true;
  }

  return diagnosedAny;
}

void ASTContext::recordObjCMethodConflict(ClassDecl *classDecl,
                                          ObjCSelector selector,
                                          bool isInstance) {
  Impl.ObjCMethodConflicts.push_back(std::make_tuple(classDecl, selector,
                                                     isInstance));
}

/// Retrieve the source file for the given Objective-C member conflict.
static MutableArrayRef<AbstractFunctionDecl *>
getObjCMethodConflictDecls(const ObjCMethodConflict &conflict) {
  ClassDecl *classDecl = std::get<0>(conflict);
  ObjCSelector selector = std::get<1>(conflict);
  bool isInstanceMethod = std::get<2>(conflict);

  return classDecl->lookupDirect(selector, isInstanceMethod);
}

/// Given a set of conflicting Objective-C methods, remove any methods
/// that are legitimately overridden in Objective-C, i.e., because
/// they occur in different modules, one is defined in the class, and
/// the other is defined in an extension (category) thereof.
static void removeValidObjCConflictingMethods(
              MutableArrayRef<AbstractFunctionDecl *> &methods) {
  // Erase any invalid or stub declarations. We don't want to complain about
  // them, because we might already have complained about
  // redeclarations based on Swift matching.
  auto newEnd = std::remove_if(methods.begin(), methods.end(),
                               [&](AbstractFunctionDecl *method) {
                                 if (method->isInvalid())
                                   return true;

                                 if (auto func = dyn_cast<FuncDecl>(method)) {
                                   if (func->isAccessor()) {
                                     return func->getAccessorStorageDecl()
                                             ->isInvalid();
                                   }

                                   return false;
                                 } 
                                 
                                 if (auto ctor 
                                       = dyn_cast<ConstructorDecl>(method)) {
                                   if (ctor->hasStubImplementation())
                                     return true;

                                   return false;
                                 }

                                 return false;
                               });
  methods = methods.slice(0, newEnd - methods.begin());

  // Identify the method that occurs in a class definition (not an
  // extension).
  Optional<unsigned> methodInClass;
  for (unsigned i = 0, n = methods.size(); i != n; ++i) {
    auto method = methods[i];

    if (isa<ClassDecl>(method->getDeclContext())) {
      if (methodInClass)
        return;

      methodInClass = i;
      break;
    }
  }

  if (!methodInClass)
    return;

  // Verify that at least one other declaration comes from a different module.
  auto classModule
    = methods[*methodInClass]->getDeclContext()->getParentModule();
  for (auto method : methods) {
    if (method->getDeclContext()->getParentModule() != classModule) {
      // Remove the method that comes from the class.
      std::copy(methods.begin() + *methodInClass + 1, methods.end(),
                methods.begin() + *methodInClass);
      methods = methods.slice(0, methods.size() - 1);
      return;
    }
  }
}

/// Determine whether the should associate a conflict among the given
/// set of methods with the specified source file.
static bool shouldAssociateConflictWithSourceFile(
              SourceFile &sf, 
              ArrayRef<AbstractFunctionDecl *> methods) {
  bool anyInSourceFile = false;
  bool anyInOtherSourceFile = false;
  bool anyClassMethodsInSourceFile = false;
  for (auto method : methods) {
    // Skip methods in the class itself; we want to only diagnose
    // those if there is a conflict within that file.
    if (isa<ClassDecl>(method->getDeclContext())) {
      if (method->getParentSourceFile() == &sf)
        anyClassMethodsInSourceFile = true;
      continue;
    }

    if (method->getParentSourceFile() == &sf)
      anyInSourceFile = true;
    else
      anyInOtherSourceFile = true;
  }

  return anyInSourceFile || 
    (!anyInOtherSourceFile && anyClassMethodsInSourceFile);
}

bool ASTContext::diagnoseObjCMethodConflicts(SourceFile &sf) {
  // If there were no conflicts, we're done.
  if (Impl.ObjCMethodConflicts.empty())
    return false;

  // Partition the set of conflicts to put the conflicts that involve
  // this source file at the end.
  auto firstLocalConflict
    = std::partition(Impl.ObjCMethodConflicts.begin(),
                     Impl.ObjCMethodConflicts.end(),
                     [&](const ObjCMethodConflict &conflict) -> bool {
                       auto decls = getObjCMethodConflictDecls(conflict);
                       if (shouldAssociateConflictWithSourceFile(sf, decls)) {
                         // It's in this source file. Sort the conflict
                         // declarations. We'll use this later.
                         std::sort(
                           decls.begin(), decls.end(),
                           OrderDeclarationsWithSourceFileAndClassBias(
                             SourceMgr, sf));

                         return false;
                       }

                       return true;
                     });

  // If there were no local conflicts, we're done.
  unsigned numLocalConflicts
    = Impl.ObjCMethodConflicts.end() - firstLocalConflict;
  if (numLocalConflicts == 0)
    return false;

  // Sort the set of conflicts so we get a deterministic order for
  // diagnostics. We use the first conflicting declaration in each set to
  // perform the sort.
  MutableArrayRef<ObjCMethodConflict> localConflicts(&*firstLocalConflict,
                                                     numLocalConflicts);
  std::sort(localConflicts.begin(), localConflicts.end(),
            [&](const ObjCMethodConflict &lhs, const ObjCMethodConflict &rhs) {
              OrderDeclarations ordering(SourceMgr);
              return ordering(getObjCMethodConflictDecls(lhs)[1],
                              getObjCMethodConflictDecls(rhs)[1]);
            });

  // Diagnose each conflict.
  bool anyConflicts = false;
  for (const ObjCMethodConflict &conflict : localConflicts) {
    ObjCSelector selector = std::get<1>(conflict);

    auto methods = getObjCMethodConflictDecls(conflict);

    // Prune out cases where it is acceptable to have a conflict.
    removeValidObjCConflictingMethods(methods);
    if (methods.size() < 2)
      continue;

    // Diagnose the conflict.
    anyConflicts = true;
    auto originalMethod = methods.front();
    auto conflictingMethods = methods.slice(1);

    auto origRedeclInfo = getObjCRedeclInfo(originalMethod);
    for (auto conflictingDecl : conflictingMethods) {
      auto redeclInfo = getObjCRedeclInfo(conflictingDecl);
      Diags.diagnose(conflictingDecl->getLoc(), diag::objc_redecl,
                     redeclInfo.first,
                     redeclInfo.second,
                     selector);
      Diags.diagnose(originalMethod->getLoc(), diag::objc_redecl_prev,
                     origRedeclInfo.first,
                     origRedeclInfo.second,
                     selector);
    }
  }

  // Erase the local conflicts from the list of conflicts.
  Impl.ObjCMethodConflicts.erase(firstLocalConflict,
                                 Impl.ObjCMethodConflicts.end());

  return anyConflicts;
}

//===----------------------------------------------------------------------===//
// Type manipulation routines.
//===----------------------------------------------------------------------===//

// Simple accessors.
Type ErrorType::get(const ASTContext &C) { return C.TheErrorType; }

BuiltinIntegerType *BuiltinIntegerType::get(BuiltinIntegerWidth BitWidth,
                                            const ASTContext &C) {
  BuiltinIntegerType *&Result = C.Impl.IntegerTypes[BitWidth];
  if (Result == 0)
    Result = new (C, AllocationArena::Permanent) BuiltinIntegerType(BitWidth,C);
  return Result;
}

BuiltinVectorType *BuiltinVectorType::get(const ASTContext &context,
                                          Type elementType,
                                          unsigned numElements) {
  llvm::FoldingSetNodeID id;
  BuiltinVectorType::Profile(id, elementType, numElements);

  void *insertPos;
  if (BuiltinVectorType *vecType
        = context.Impl.BuiltinVectorTypes.FindNodeOrInsertPos(id, insertPos))
    return vecType;

  assert(elementType->isCanonical() && "Non-canonical builtin vector?");
  BuiltinVectorType *vecTy
    = new (context, AllocationArena::Permanent)
       BuiltinVectorType(context, elementType, numElements);
  context.Impl.BuiltinVectorTypes.InsertNode(vecTy, insertPos);
  return vecTy;
}


ParenType *ParenType::get(const ASTContext &C, Type underlying) {
  auto properties = underlying->getRecursiveProperties();
  auto arena = getArena(properties);
  ParenType *&Result = C.Impl.getArena(arena).ParenTypes[underlying];
  if (Result == 0) {
    Result = new (C, arena) ParenType(underlying, properties);
  }
  return Result;
}

CanTupleType TupleType::getEmpty(const ASTContext &C) {
  return cast<TupleType>(CanType(C.TheEmptyTupleType));
}

void TupleType::Profile(llvm::FoldingSetNodeID &ID,
                        ArrayRef<TupleTypeElt> Fields) {
  ID.AddInteger(Fields.size());
  for (const TupleTypeElt &Elt : Fields) {
    ID.AddPointer(Elt.NameAndVariadic.getOpaqueValue());
    ID.AddPointer(Elt.TyAndDefaultArg.getOpaqueValue());
  }
}

/// getTupleType - Return the uniqued tuple type with the specified elements.
Type TupleType::get(ArrayRef<TupleTypeElt> Fields, const ASTContext &C) {
  if (Fields.size() == 1 && !Fields[0].isVararg() && !Fields[0].hasName()
      && Fields[0].getDefaultArgKind() == DefaultArgumentKind::None)
    return ParenType::get(C, Fields[0].getType());

  RecursiveTypeProperties properties;
  for (const TupleTypeElt &Elt : Fields) {
    if (Elt.getType())
      properties += Elt.getType()->getRecursiveProperties();
  }

  auto arena = getArena(properties);


  void *InsertPos = 0;
  // Check to see if we've already seen this tuple before.
  llvm::FoldingSetNodeID ID;
  TupleType::Profile(ID, Fields);

  if (TupleType *TT
        = C.Impl.getArena(arena).TupleTypes.FindNodeOrInsertPos(ID,InsertPos))
    return TT;

  // Make a copy of the fields list into ASTContext owned memory.
  TupleTypeElt *FieldsCopy =
    C.AllocateCopy<TupleTypeElt>(Fields.begin(), Fields.end(), arena);

  bool IsCanonical = true;   // All canonical elts means this is canonical.
  for (const TupleTypeElt &Elt : Fields) {
    if (Elt.getType().isNull() || !Elt.getType()->isCanonical()) {
      IsCanonical = false;
      break;
    }
  }

  Fields = ArrayRef<TupleTypeElt>(FieldsCopy, Fields.size());

  TupleType *New = new (C, arena) TupleType(Fields, IsCanonical ? &C : 0,
                                            properties);
  C.Impl.getArena(arena).TupleTypes.InsertNode(New, InsertPos);
  return New;
}

void UnboundGenericType::Profile(llvm::FoldingSetNodeID &ID,
                                 NominalTypeDecl *TheDecl, Type Parent) {
  ID.AddPointer(TheDecl);
  ID.AddPointer(Parent.getPointer());
}

UnboundGenericType* UnboundGenericType::get(NominalTypeDecl *TheDecl,
                                            Type Parent,
                                            const ASTContext &C) {
  llvm::FoldingSetNodeID ID;
  UnboundGenericType::Profile(ID, TheDecl, Parent);
  void *InsertPos = 0;
  RecursiveTypeProperties properties;
  if (Parent) properties += Parent->getRecursiveProperties();
  auto arena = getArena(properties);

  if (auto unbound = C.Impl.getArena(arena).UnboundGenericTypes
                        .FindNodeOrInsertPos(ID, InsertPos))
    return unbound;

  auto result = new (C, arena) UnboundGenericType(TheDecl, Parent, C,
                                                  properties);
  C.Impl.getArena(arena).UnboundGenericTypes.InsertNode(result, InsertPos);
  return result;
}

void BoundGenericType::Profile(llvm::FoldingSetNodeID &ID,
                               NominalTypeDecl *TheDecl, Type Parent,
                               ArrayRef<Type> GenericArgs,
                               RecursiveTypeProperties &properties) {
  ID.AddPointer(TheDecl);
  ID.AddPointer(Parent.getPointer());
  if (Parent) properties += Parent->getRecursiveProperties();
  ID.AddInteger(GenericArgs.size());
  for (Type Arg : GenericArgs) {
    ID.AddPointer(Arg.getPointer());
    properties += Arg->getRecursiveProperties();
  }
}

BoundGenericType::BoundGenericType(TypeKind theKind,
                                   NominalTypeDecl *theDecl,
                                   Type parent,
                                   ArrayRef<Type> genericArgs,
                                   const ASTContext *context,
                                   RecursiveTypeProperties properties)
  : TypeBase(theKind, context, properties),
    TheDecl(theDecl), Parent(parent), GenericArgs(genericArgs)
{
}

BoundGenericType *BoundGenericType::get(NominalTypeDecl *TheDecl,
                                        Type Parent,
                                        ArrayRef<Type> GenericArgs) {
  ASTContext &C = TheDecl->getDeclContext()->getASTContext();
  llvm::FoldingSetNodeID ID;
  RecursiveTypeProperties properties;
  BoundGenericType::Profile(ID, TheDecl, Parent, GenericArgs, properties);

  auto arena = getArena(properties);

  void *InsertPos = 0;
  if (BoundGenericType *BGT =
        C.Impl.getArena(arena).BoundGenericTypes.FindNodeOrInsertPos(ID,
                                                                     InsertPos))
    return BGT;

  ArrayRef<Type> ArgsCopy = C.AllocateCopy(GenericArgs, arena);
  bool IsCanonical = !Parent || Parent->isCanonical();
  if (IsCanonical) {
    for (Type Arg : GenericArgs) {
      if (!Arg->isCanonical()) {
        IsCanonical = false;
        break;
      }
    }
  }

  BoundGenericType *newType;
  if (auto theClass = dyn_cast<ClassDecl>(TheDecl)) {
    newType = new (C, arena) BoundGenericClassType(theClass, Parent, ArgsCopy,
                                                   IsCanonical ? &C : 0,
                                                   properties);
  } else if (auto theStruct = dyn_cast<StructDecl>(TheDecl)) {
    newType = new (C, arena) BoundGenericStructType(theStruct, Parent, ArgsCopy,
                                                    IsCanonical ? &C : 0,
                                                    properties);
  } else {
    auto theEnum = cast<EnumDecl>(TheDecl);
    newType = new (C, arena) BoundGenericEnumType(theEnum, Parent, ArgsCopy,
                                                   IsCanonical ? &C : 0,
                                                   properties);
  }
  C.Impl.getArena(arena).BoundGenericTypes.InsertNode(newType, InsertPos);

  return newType;
}

NominalType *NominalType::get(NominalTypeDecl *D, Type Parent, const ASTContext &C) {
  switch (D->getKind()) {
  case DeclKind::Enum:
    return EnumType::get(cast<EnumDecl>(D), Parent, C);
  case DeclKind::Struct:
    return StructType::get(cast<StructDecl>(D), Parent, C);
  case DeclKind::Class:
    return ClassType::get(cast<ClassDecl>(D), Parent, C);
  case DeclKind::Protocol: {
    return ProtocolType::get(cast<ProtocolDecl>(D), C);
  }

  default:
    llvm_unreachable("Not a nominal declaration!");
  }
}

EnumType::EnumType(EnumDecl *TheDecl, Type Parent, const ASTContext &C,
                     RecursiveTypeProperties properties)
  : NominalType(TypeKind::Enum, &C, TheDecl, Parent, properties) { }

EnumType *EnumType::get(EnumDecl *D, Type Parent, const ASTContext &C) {
  llvm::FoldingSetNodeID id;
  EnumType::Profile(id, D, Parent);

  RecursiveTypeProperties properties;
  if (Parent) properties += Parent->getRecursiveProperties();
  auto arena = getArena(properties);

  void *insertPos = 0;
  if (auto enumTy
        = C.Impl.getArena(arena).EnumTypes.FindNodeOrInsertPos(id, insertPos))
    return enumTy;

  auto enumTy = new (C, arena) EnumType(D, Parent, C, properties);
  C.Impl.getArena(arena).EnumTypes.InsertNode(enumTy, insertPos);
  return enumTy;
}

void EnumType::Profile(llvm::FoldingSetNodeID &ID, EnumDecl *D, Type Parent) {
  ID.AddPointer(D);
  ID.AddPointer(Parent.getPointer());
}

StructType::StructType(StructDecl *TheDecl, Type Parent, const ASTContext &C,
                       RecursiveTypeProperties properties)
  : NominalType(TypeKind::Struct, &C, TheDecl, Parent, properties) { }

StructType *StructType::get(StructDecl *D, Type Parent, const ASTContext &C) {
  llvm::FoldingSetNodeID id;
  StructType::Profile(id, D, Parent);

  RecursiveTypeProperties properties;
  if (Parent) properties += Parent->getRecursiveProperties();
  auto arena = getArena(properties);

  void *insertPos = 0;
  if (auto structTy
        = C.Impl.getArena(arena).StructTypes.FindNodeOrInsertPos(id, insertPos))
    return structTy;

  auto structTy = new (C, arena) StructType(D, Parent, C, properties);
  C.Impl.getArena(arena).StructTypes.InsertNode(structTy, insertPos);
  return structTy;
}

void StructType::Profile(llvm::FoldingSetNodeID &ID, StructDecl *D, Type Parent) {
  ID.AddPointer(D);
  ID.AddPointer(Parent.getPointer());
}

ClassType::ClassType(ClassDecl *TheDecl, Type Parent, const ASTContext &C,
                     RecursiveTypeProperties properties)
  : NominalType(TypeKind::Class, &C, TheDecl, Parent, properties) { }

ClassType *ClassType::get(ClassDecl *D, Type Parent, const ASTContext &C) {
  llvm::FoldingSetNodeID id;
  ClassType::Profile(id, D, Parent);

  RecursiveTypeProperties properties;
  if (Parent) properties += Parent->getRecursiveProperties();
  auto arena = getArena(properties);

  void *insertPos = 0;
  if (auto classTy
        = C.Impl.getArena(arena).ClassTypes.FindNodeOrInsertPos(id, insertPos))
    return classTy;

  auto classTy = new (C, arena) ClassType(D, Parent, C, properties);
  C.Impl.getArena(arena).ClassTypes.InsertNode(classTy, insertPos);
  return classTy;
}

void ClassType::Profile(llvm::FoldingSetNodeID &ID, ClassDecl *D, Type Parent) {
  ID.AddPointer(D);
  ID.AddPointer(Parent.getPointer());
}

ProtocolCompositionType *
ProtocolCompositionType::build(const ASTContext &C, ArrayRef<Type> Protocols) {
  // Check to see if we've already seen this protocol composition before.
  void *InsertPos = 0;
  llvm::FoldingSetNodeID ID;
  ProtocolCompositionType::Profile(ID, Protocols);
  if (ProtocolCompositionType *Result
        = C.Impl.ProtocolCompositionTypes.FindNodeOrInsertPos(ID, InsertPos))
    return Result;

  bool isCanonical = true;
  for (Type t : Protocols) {
    if (!t->isCanonical())
      isCanonical = false;
  }

  // Create a new protocol composition type.
  ProtocolCompositionType *New
    = new (C, AllocationArena::Permanent)
        ProtocolCompositionType(isCanonical ? &C : nullptr,
                                C.AllocateCopy(Protocols));
  C.Impl.ProtocolCompositionTypes.InsertNode(New, InsertPos);
  return New;
}

ReferenceStorageType *ReferenceStorageType::get(Type T, Ownership ownership,
                                                const ASTContext &C) {
  assert(ownership != Ownership::Strong &&
         "ReferenceStorageType is unnecessary for strong ownership");
  assert(!T->hasTypeVariable()); // not meaningful in type-checker
  auto arena = AllocationArena::Permanent;

  auto key = uintptr_t(T.getPointer()) | unsigned(ownership);
  auto &entry = C.Impl.getArena(arena).ReferenceStorageTypes[key];
  if (entry) return entry;

  auto properties = T->getRecursiveProperties();

  switch (ownership) {
  case Ownership::Strong: llvm_unreachable("not possible");
  case Ownership::Unowned:
    return entry =
      new (C, arena) UnownedStorageType(T, T->isCanonical() ? &C : 0,
                                        properties);
  case Ownership::Weak:
    return entry =
      new (C, arena) WeakStorageType(T, T->isCanonical() ? &C : 0,
                                     properties);
  case Ownership::Unmanaged:
    return entry =
      new (C, arena) UnmanagedStorageType(T, T->isCanonical() ? &C : 0,
                                          properties);
  }
  llvm_unreachable("bad ownership");
}

AnyMetatypeType::AnyMetatypeType(TypeKind kind, const ASTContext *C,
                                 RecursiveTypeProperties properties,
                                 Type instanceType,
                                 Optional<MetatypeRepresentation> repr)
    : TypeBase(kind, C, properties), InstanceType(instanceType) {
  if (repr) {
    AnyMetatypeTypeBits.Representation = static_cast<char>(*repr) + 1;
  } else {
    AnyMetatypeTypeBits.Representation = 0;
  }
}

MetatypeType *MetatypeType::get(Type T, Optional<MetatypeRepresentation> Repr,
                                const ASTContext &Ctx) {
  auto properties = T->getRecursiveProperties();
  auto arena = getArena(properties);

  char reprKey;
  if (Repr.hasValue())
    reprKey = static_cast<char>(*Repr) + 1;
  else
    reprKey = 0;

  MetatypeType *&Entry = Ctx.Impl.getArena(arena).MetatypeTypes[{T, reprKey}];
  if (Entry) return Entry;

  return Entry = new (Ctx, arena) MetatypeType(T,
                                               T->isCanonical() ? &Ctx : 0,
                                               properties, Repr);
}

MetatypeType::MetatypeType(Type T, const ASTContext *C,
                           RecursiveTypeProperties properties,
                           Optional<MetatypeRepresentation> repr)
  : AnyMetatypeType(TypeKind::Metatype, C, properties, T, repr) {
}

ExistentialMetatypeType *
ExistentialMetatypeType::get(Type T, Optional<MetatypeRepresentation> repr,
                             const ASTContext &ctx) {
  auto properties = T->getRecursiveProperties();
  auto arena = getArena(properties);

  char reprKey;
  if (repr.hasValue())
    reprKey = static_cast<char>(*repr) + 1;
  else
    reprKey = 0;

  auto &entry = ctx.Impl.getArena(arena).ExistentialMetatypeTypes[{T, reprKey}];
  if (entry) return entry;

  return entry = new (ctx, arena) ExistentialMetatypeType(T,
                                               T->isCanonical() ? &ctx : 0,
                                               properties, repr);
}

ExistentialMetatypeType::ExistentialMetatypeType(Type T,
                                                 const ASTContext *C,
                                       RecursiveTypeProperties properties,
                                       Optional<MetatypeRepresentation> repr)
  : AnyMetatypeType(TypeKind::ExistentialMetatype, C, properties, T, repr) {
  if (repr) {
    assert(*repr != MetatypeRepresentation::Thin &&
           "creating a thin existential metatype?");
  }
}

ModuleType *ModuleType::get(Module *M) {
  ASTContext &C = M->getASTContext();

  ModuleType *&Entry = C.Impl.ModuleTypes[M];
  if (Entry) return Entry;

  return Entry = new (C, AllocationArena::Permanent) ModuleType(M, C);
}

DynamicSelfType *DynamicSelfType::get(Type selfType, const ASTContext &ctx) {
  auto properties = selfType->getRecursiveProperties()
                    - RecursiveTypeProperties::IsNotMaterializable;
  auto arena = getArena(properties);

  auto &dynamicSelfTypes = ctx.Impl.getArena(arena).DynamicSelfTypes;
  auto known = dynamicSelfTypes.find(selfType);
  if (known != dynamicSelfTypes.end())
    return known->second;

  auto result = new (ctx, arena) DynamicSelfType(selfType, ctx, properties);
  dynamicSelfTypes.insert({selfType, result});
  return result;
}

static void checkFunctionRecursiveProperties(Type Input,
                                             Type Result) {
  // TODO: Would be nice to be able to assert these, but they trip during
  // constraint solving:
  //assert(!Input->getRecursiveProperties().isLValue()
  //       && "function should not take lvalues directly as parameters");
  //assert(Result->getRecursiveProperties().isMaterializable()
  //       && "function return should be materializable");
}

static RecursiveTypeProperties getFunctionRecursiveProperties(Type Input,
                                                              Type Result) {
  checkFunctionRecursiveProperties(Input, Result);

  auto properties = Input->getRecursiveProperties()
    + Result->getRecursiveProperties()
    - RecursiveTypeProperties::IsNotMaterializable
    - RecursiveTypeProperties::IsLValue;
  return properties;
}

/// FunctionType::get - Return a uniqued function type with the specified
/// input and result.
FunctionType *FunctionType::get(Type Input, Type Result,
                                const ExtInfo &Info) {
  auto properties = getFunctionRecursiveProperties(Input, Result);
  auto arena = getArena(properties);
  char attrKey = Info.getFuncAttrKey();

  const ASTContext &C = Input->getASTContext();

  FunctionType *&Entry
    = C.Impl.getArena(arena).FunctionTypes[{Input, {Result, attrKey} }];
  if (Entry) return Entry;

  return Entry = new (C, arena) FunctionType(Input, Result,
                                             properties,
                                             Info);
}

// If the input and result types are canonical, then so is the result.
FunctionType::FunctionType(Type input, Type output,
                           RecursiveTypeProperties properties,
                           const ExtInfo &Info)
: AnyFunctionType(TypeKind::Function,
                  (input->isCanonical() && output->isCanonical()) ?
                  &input->getASTContext() : 0,
                  input, output,
                  properties,
                  Info)
{ }


/// FunctionType::get - Return a uniqued function type with the specified
/// input and result.
PolymorphicFunctionType *PolymorphicFunctionType::get(Type input, Type output,
                                                      GenericParamList *params,
                                                      const ExtInfo &Info) {
  auto properties = getFunctionRecursiveProperties(input, output);
  auto arena = getArena(properties);

  const ASTContext &C = input->getASTContext();

  return new (C, arena) PolymorphicFunctionType(input, output, params,
                                                Info, C, properties);
}

PolymorphicFunctionType::PolymorphicFunctionType(Type input, Type output,
                                                 GenericParamList *params,
                                                 const ExtInfo &Info,
                                                 const ASTContext &C,
                                        RecursiveTypeProperties properties)
  : AnyFunctionType(TypeKind::PolymorphicFunction,
                    (input->isCanonical() && output->isCanonical()) ?&C : 0,
                    input, output, properties,
                    Info),
    Params(params)
{
  assert(!input->hasTypeVariable() && !output->hasTypeVariable());
}

void GenericFunctionType::Profile(llvm::FoldingSetNodeID &ID,
                                  GenericSignature *sig,
                                  Type input,
                                  Type result,
                                  const ExtInfo &info) {
  ID.AddPointer(sig);
  ID.AddPointer(input.getPointer());
  ID.AddPointer(result.getPointer());
  ID.AddInteger(info.getFuncAttrKey());
}

GenericFunctionType *
GenericFunctionType::get(GenericSignature *sig,
                         Type input,
                         Type output,
                         const ExtInfo &info) {
  assert(sig && "no generic signature for generic function type?!");
  assert(!input->hasTypeVariable() && !output->hasTypeVariable());

  llvm::FoldingSetNodeID id;
  GenericFunctionType::Profile(id, sig, input, output, info);

  const ASTContext &ctx = input->getASTContext();

  // Do we already have this generic function type?
  void *insertPos;
  if (auto result
        = ctx.Impl.GenericFunctionTypes.FindNodeOrInsertPos(id, insertPos))
    return result;

  // We have to construct this generic function type. Determine whether
  // it's canonical.
  bool isCanonical = sig->isCanonical()
    && input->isCanonical()
    && output->isCanonical();

  // Allocate storage for the object.
  void *mem = ctx.Allocate(sizeof(GenericFunctionType),
                           alignof(GenericFunctionType));

  // For now, generic function types cannot be dependent (in fact,
  // they erase dependence) or contain type variables, and they're
  // always materializable.
  checkFunctionRecursiveProperties(input, output);
  RecursiveTypeProperties properties;
  static_assert(RecursiveTypeProperties::BitWidth == 5,
                "revisit this if you add new recursive type properties");

  auto result = new (mem) GenericFunctionType(sig, input, output, info,
                                              isCanonical ? &ctx : nullptr,
                                              properties);
  ctx.Impl.GenericFunctionTypes.InsertNode(result, insertPos);
  return result;
}

GenericFunctionType::GenericFunctionType(
                       GenericSignature *sig,
                       Type input,
                       Type result,
                       const ExtInfo &info,
                       const ASTContext *ctx,
                       RecursiveTypeProperties properties)
  : AnyFunctionType(TypeKind::GenericFunction, ctx, input, result,
                    properties, info),
    Signature(sig)
{}

GenericTypeParamType *GenericTypeParamType::get(unsigned depth, unsigned index,
                                                const ASTContext &ctx) {
  auto known = ctx.Impl.GenericParamTypes.find({ depth, index });
  if (known != ctx.Impl.GenericParamTypes.end())
    return known->second;

  auto result = new (ctx, AllocationArena::Permanent)
                  GenericTypeParamType(depth, index, ctx);
  ctx.Impl.GenericParamTypes[{depth, index}] = result;
  return result;
}

ArrayRef<GenericTypeParamType *> GenericFunctionType::getGenericParams() const{
  return Signature->getGenericParams();
}

/// Retrieve the requirements of this polymorphic function type.
ArrayRef<Requirement> GenericFunctionType::getRequirements() const {
  return Signature->getRequirements();
}

void SILFunctionType::Profile(llvm::FoldingSetNodeID &id,
                              GenericSignature *genericParams,
                              ExtInfo info,
                              ParameterConvention calleeConvention,
                              ArrayRef<SILParameterInfo> params,
                              SILResultInfo result) {
  id.AddPointer(genericParams);
  id.AddInteger(info.getFuncAttrKey());
  id.AddInteger(unsigned(calleeConvention));
  id.AddInteger(params.size());
  for (auto param : params)
    param.profile(id);
  result.profile(id);
}

SILFunctionType::SILFunctionType(GenericSignature *genericSig,
                                 ExtInfo ext,
                                 ParameterConvention calleeConvention,
                                 ArrayRef<SILParameterInfo> interfaceParams,
                                 SILResultInfo interfaceResult,
                                 const ASTContext &ctx,
                                 RecursiveTypeProperties properties)
  : TypeBase(TypeKind::SILFunction, &ctx, properties),
    GenericSig(genericSig),
    InterfaceResult(interfaceResult) {
  SILFunctionTypeBits.ExtInfo = ext.Bits;
  SILFunctionTypeBits.NumParameters = interfaceParams.size();
  assert(!isIndirectParameter(calleeConvention));
  SILFunctionTypeBits.CalleeConvention = unsigned(calleeConvention);
  memcpy(getMutableParameters().data(), interfaceParams.data(),
         interfaceParams.size() * sizeof(SILParameterInfo));

  // Make sure the interface types are sane.
#ifndef NDEBUG
  if (genericSig) {
    for (auto gparam : genericSig->getGenericParams()) {
      (void)gparam;
      assert(gparam->isCanonical() && "generic signature is not canonicalized");
    }

    for (auto param : getParameters()) {
      (void)param;
      assert(!param.getType().findIf([](Type t) {
        return t->is<ArchetypeType>()
          && !t->castTo<ArchetypeType>()->getSelfProtocol();
      }) && "interface type of generic type should not contain context archetypes");
    }
    assert(!getResult().getType().findIf([](Type t) {
      return t->is<ArchetypeType>();
    }) && "interface type of generic type should not contain context archetypes");
  }
#endif
}

CanSILBlockStorageType SILBlockStorageType::get(CanType captureType) {
  ASTContext &ctx = captureType->getASTContext();
  auto found = ctx.Impl.SILBlockStorageTypes.find(captureType);
  if (found != ctx.Impl.SILBlockStorageTypes.end())
    return CanSILBlockStorageType(found->second);
  
  void *mem = ctx.Allocate(sizeof(SILBlockStorageType),
                           alignof(SILBlockStorageType));
  
  SILBlockStorageType *storageTy = new (mem) SILBlockStorageType(captureType);
  ctx.Impl.SILBlockStorageTypes.insert({captureType, storageTy});
  return CanSILBlockStorageType(storageTy);
}

CanSILFunctionType SILFunctionType::get(GenericSignature *genericSig,
                                        ExtInfo ext, ParameterConvention callee,
                                        ArrayRef<SILParameterInfo> interfaceParams,
                                        SILResultInfo interfaceResult,
                                        const ASTContext &ctx) {
  llvm::FoldingSetNodeID id;
  SILFunctionType::Profile(id, genericSig, ext, callee,
                           interfaceParams, interfaceResult);

  // Do we already have this generic function type?
  void *insertPos;
  if (auto result
        = ctx.Impl.SILFunctionTypes.FindNodeOrInsertPos(id, insertPos))
    return CanSILFunctionType(result);

  // All SILFunctionTypes are canonical.

  // Allocate storage for the object.
  // FIXME: 2*params.size() so we can stash interface types.
  size_t bytes = sizeof(SILFunctionType)
               + sizeof(SILParameterInfo) * interfaceParams.size();
  void *mem = ctx.Allocate(bytes, alignof(SILFunctionType));

  // Right now, generic SIL function types cannot be dependent or contain type
  // variables, and they're always materializable.
  // FIXME: If we ever have first-class polymorphic values, we'll need to
  // revisit this.
  RecursiveTypeProperties properties;
  static_assert(RecursiveTypeProperties::BitWidth == 5,
                "revisit this if you add new recursive type properties");
  if (!genericSig) {
    // Nongeneric SIL functions are dependent if they have dependent argument
    // or return types. They still never contain type variables and are always
    // materializable.
    properties += interfaceResult.getType()->getRecursiveProperties();

    for (auto &param : interfaceParams) {
      properties += param.getType()->getRecursiveProperties();
    }
  }

  auto fnType =
    new (mem) SILFunctionType(genericSig, ext, callee,
                              interfaceParams, interfaceResult,
                              ctx, properties);
  ctx.Impl.SILFunctionTypes.InsertNode(fnType, insertPos);
  return CanSILFunctionType(fnType);
}


ArraySliceType *ArraySliceType::get(Type base) {
  auto properties = base->getRecursiveProperties();
  auto arena = getArena(properties);

  const ASTContext &C = base->getASTContext();

  ArraySliceType *&entry = C.Impl.getArena(arena).ArraySliceTypes[base];
  if (entry) return entry;

  return entry = new (C, arena) ArraySliceType(C, base, properties);
}

DictionaryType *DictionaryType::get(Type keyType, Type valueType) {
  auto properties = keyType->getRecursiveProperties() 
                  + valueType->getRecursiveProperties();
  auto arena = getArena(properties);

  const ASTContext &C = keyType->getASTContext();

  DictionaryType *&entry
    = C.Impl.getArena(arena).DictionaryTypes[{keyType, valueType}];
  if (entry) return entry;

  return entry = new (C, arena) DictionaryType(C, keyType, valueType, 
                                               properties);
}

Type OptionalType::get(OptionalTypeKind which, Type valueType) {
  switch (which) {
  // It wouldn't be unreasonable for this method to just ignore
  // OTK_None if we made code more convenient to write.
  case OTK_None: llvm_unreachable("building a non-optional type!");
  case OTK_Optional: return OptionalType::get(valueType);
  case OTK_ImplicitlyUnwrappedOptional: return ImplicitlyUnwrappedOptionalType::get(valueType);
  }
  llvm_unreachable("bad optional type kind");
}

OptionalType *OptionalType::get(Type base) {
  auto properties = base->getRecursiveProperties();
  auto arena = getArena(properties);

  const ASTContext &C = base->getASTContext();

  OptionalType *&entry = C.Impl.getArena(arena).OptionalTypes[base];
  if (entry) return entry;

  return entry = new (C, arena) OptionalType(C, base, properties);
}

ImplicitlyUnwrappedOptionalType *ImplicitlyUnwrappedOptionalType::get(Type base) {
  auto properties = base->getRecursiveProperties();
  auto arena = getArena(properties);

  const ASTContext &C = base->getASTContext();

  auto *&entry = C.Impl.getArena(arena).ImplicitlyUnwrappedOptionalTypes[base];
  if (entry) return entry;

  return entry = new (C, arena) ImplicitlyUnwrappedOptionalType(C, base, properties);
}

ProtocolType *ProtocolType::get(ProtocolDecl *D, const ASTContext &C) {
  if (auto declaredTy = D->getDeclaredType())
    return declaredTy->castTo<ProtocolType>();

  auto protoTy = new (C, AllocationArena::Permanent) ProtocolType(D, C);
  D->setDeclaredType(protoTy);
  return protoTy;
}

ProtocolType::ProtocolType(ProtocolDecl *TheDecl, const ASTContext &Ctx)
  : NominalType(TypeKind::Protocol, &Ctx, TheDecl, /*Parent=*/Type(),
                RecursiveTypeProperties()) { }

LValueType *LValueType::get(Type objectTy) {
  assert(!objectTy->is<ErrorType>() &&
         "can not have ErrorType wrapped inside LValueType");
  assert(!objectTy->is<LValueType>() && !objectTy->is<InOutType>() &&
         "can not have 'inout' or @lvalue wrapped inside an @lvalue");

  auto properties = objectTy->getRecursiveProperties()
                    + RecursiveTypeProperties::IsNotMaterializable
                    + RecursiveTypeProperties::IsLValue;
  auto arena = getArena(properties);

  auto &C = objectTy->getASTContext();
  auto &entry = C.Impl.getArena(arena).LValueTypes[objectTy];
  if (entry)
    return entry;

  const ASTContext *canonicalContext = objectTy->isCanonical() ? &C : nullptr;
  return entry = new (C, arena) LValueType(objectTy, canonicalContext,
                                           properties);
}

InOutType *InOutType::get(Type objectTy) {
  assert(!objectTy->is<ErrorType>() &&
         "can not have ErrorType wrapped inside InOutType");
  assert(!objectTy->is<LValueType>() && !objectTy->is<InOutType>() &&
         "can not have 'inout' or @lvalue wrapped inside an 'inout'");

  auto properties = objectTy->getRecursiveProperties()
                    + RecursiveTypeProperties::IsNotMaterializable
                    - RecursiveTypeProperties::IsLValue;
  auto arena = getArena(properties);

  auto &C = objectTy->getASTContext();
  auto &entry = C.Impl.getArena(arena).InOutTypes[objectTy];
  if (entry)
    return entry;

  const ASTContext *canonicalContext = objectTy->isCanonical() ? &C : nullptr;
  return entry = new (C, arena) InOutType(objectTy, canonicalContext,
                                          properties);
}

/// Return a uniqued substituted type.
SubstitutedType *SubstitutedType::get(Type Original, Type Replacement,
                                      const ASTContext &C) {
  auto properties = Replacement->getRecursiveProperties();
  auto arena = getArena(properties);

  SubstitutedType *&Known
    = C.Impl.getArena(arena).SubstitutedTypes[{Original, Replacement}];
  if (!Known) {
    Known = new (C, arena) SubstitutedType(Original, Replacement,
                                           properties);
  }
  return Known;
}

DependentMemberType *DependentMemberType::get(Type base, Identifier name,
                                              const ASTContext &ctx) {
  auto properties = base->getRecursiveProperties();
  auto arena = getArena(properties);

  llvm::PointerUnion<Identifier, AssociatedTypeDecl *> stored(name);
  auto *&known = ctx.Impl.getArena(arena).DependentMemberTypes[
                                            {base, stored.getOpaqueValue()}];
  if (!known) {
    const ASTContext *canonicalCtx = base->isCanonical() ? &ctx : nullptr;
    known = new (ctx, arena) DependentMemberType(base, name, canonicalCtx,
                                                 properties);
  }
  return known;
}

DependentMemberType *DependentMemberType::get(Type base,
                                              AssociatedTypeDecl *assocType,
                                              const ASTContext &ctx) {
  auto properties = base->getRecursiveProperties();
  auto arena = getArena(properties);

  llvm::PointerUnion<Identifier, AssociatedTypeDecl *> stored(assocType);
  auto *&known = ctx.Impl.getArena(arena).DependentMemberTypes[
                                            {base, stored.getOpaqueValue()}];
  if (!known) {
    const ASTContext *canonicalCtx = base->isCanonical() ? &ctx : nullptr;
    known = new (ctx, arena) DependentMemberType(base, assocType, canonicalCtx,
                                                 properties);
  }
  return known;
}

ArchetypeType *ArchetypeType::getOpened(Type existential,
                                        Optional<UUID> knownID) {
  auto &ctx = existential->getASTContext();
  auto &openedExistentialArchetypes = ctx.Impl.OpenedExistentialArchetypes;
  // If we know the ID already...
  if (knownID) {
    // ... and we already have an archetype for that ID, return it.
    auto found = openedExistentialArchetypes.find(*knownID);
    
    if (found != openedExistentialArchetypes.end()) {
      auto result = found->second;
      assert(result->getOpenedExistentialType()->isEqual(existential) &&
             "Retrieved the wrong opened existential type?");
      return result;
    }
  } else {
    // Create a new ID.
    knownID = UUID::fromTime();
  }

  auto arena = AllocationArena::Permanent;
  llvm::SmallVector<ProtocolDecl *, 4> conformsTo;
  assert(existential->isExistentialType());
  existential->getAnyExistentialTypeProtocols(conformsTo);

  // Tail-allocate space for the UUID.
  void *archetypeBuf = ctx.Allocate(sizeof(ArchetypeType) + sizeof(UUID),
                                    alignof(ArchetypeType), arena);
  
  auto result = ::new (archetypeBuf) ArchetypeType(ctx, existential,
                                       ctx.AllocateCopy(conformsTo),
                                       existential->getSuperclass(nullptr));
  result->setOpenedExistentialID(*knownID);
  openedExistentialArchetypes[*knownID] = result;
  return result;
}

void *ExprHandle::operator new(size_t Bytes, ASTContext &C,
                            unsigned Alignment) {
  return C.Allocate(Bytes, Alignment);
}

ExprHandle *ExprHandle::get(ASTContext &Context, Expr *E) {
  return new (Context) ExprHandle(E);
}

void TypeLoc::setInvalidType(ASTContext &C) {
  TAndValidBit.setPointerAndInt(ErrorType::get(C), true);
}

namespace {
class raw_capturing_ostream : public raw_ostream {
  std::string Message;
  uint64_t Pos;
  CapturingTypeCheckerDebugConsumer &Listener;

public:
  raw_capturing_ostream(CapturingTypeCheckerDebugConsumer &Listener)
      : Listener(Listener) {}

  ~raw_capturing_ostream() {
    flush();
  }

  void write_impl(const char *Ptr, size_t Size) override {
    Message.append(Ptr, Size);
    Pos += Size;

    // Check if we have at least one complete line.
    size_t LastNewline = StringRef(Message).rfind('\n');
    if (LastNewline == StringRef::npos)
      return;
    Listener.handleMessage(StringRef(Message.data(), LastNewline + 1));
    Message.erase(0, LastNewline + 1);
  }

  uint64_t current_pos() const override {
    return Pos;
  }
};
} // unnamed namespace

TypeCheckerDebugConsumer::~TypeCheckerDebugConsumer() { }

CapturingTypeCheckerDebugConsumer::CapturingTypeCheckerDebugConsumer()
    : Log(new raw_capturing_ostream(*this)) {
  Log->SetUnbuffered();
}

CapturingTypeCheckerDebugConsumer::~CapturingTypeCheckerDebugConsumer() {
  delete Log;
}

void GenericSignature::Profile(llvm::FoldingSetNodeID &ID,
                               ArrayRef<GenericTypeParamType *> genericParams,
                               ArrayRef<Requirement> requirements) {
  for (auto p : genericParams)
    ID.AddPointer(p);

  for (auto &reqt : requirements) {
    ID.AddPointer(reqt.getFirstType().getPointer());
    if (reqt.getKind() != RequirementKind::WitnessMarker)
      ID.AddPointer(reqt.getSecondType().getPointer());
    ID.AddInteger(unsigned(reqt.getKind()));
  }
}

GenericSignature *GenericSignature::get(ArrayRef<GenericTypeParamType *> params,
                                        ArrayRef<Requirement> requirements) {
  if (params.empty() && requirements.empty())
    return nullptr;

  // Check for an existing generic signature.
  llvm::FoldingSetNodeID ID;
  GenericSignature::Profile(ID, params, requirements);

  auto &ctx = getASTContext(params, requirements);
  void *insertPos;
  if (auto *sig = ctx.Impl.GenericSignatures.FindNodeOrInsertPos(ID, insertPos))
    return sig;

  // Allocate and construct the new signature.
  size_t bytes = sizeof(GenericSignature)
               + sizeof(GenericTypeParamType *) * params.size()
               + sizeof(Requirement) * requirements.size();
  void *mem = ctx.Allocate(bytes, alignof(GenericSignature));
  auto newSig = new (mem) GenericSignature(params, requirements);
  ctx.Impl.GenericSignatures.InsertNode(newSig, insertPos);
  return newSig;
}

CanGenericSignature GenericSignature::getCanonical(
                                        ArrayRef<GenericTypeParamType *> params,
                                        ArrayRef<Requirement> requirements) {
  // Canonicalize the parameters and requirements.
  SmallVector<GenericTypeParamType*, 8> canonicalParams;
  canonicalParams.reserve(params.size());
  for (auto param : params) {
    canonicalParams.push_back(cast<GenericTypeParamType>(param->getCanonicalType()));
  }

  SmallVector<Requirement, 8> canonicalRequirements;
  canonicalRequirements.reserve(requirements.size());
  for (auto &reqt : requirements) {
    canonicalRequirements.push_back(Requirement(reqt.getKind(),
                              reqt.getFirstType()->getCanonicalType(),
                              reqt.getSecondType().getCanonicalTypeOrNull()));
  }
  return CanGenericSignature(get(canonicalParams, canonicalRequirements));
}

void DeclName::CompoundDeclName::Profile(llvm::FoldingSetNodeID &id,
                                         Identifier baseName,
                                         ArrayRef<Identifier> argumentNames) {
  id.AddPointer(baseName.get());
  id.AddInteger(argumentNames.size());
  for (auto arg : argumentNames)
    id.AddPointer(arg.get());
}

DeclName::DeclName(ASTContext &C, Identifier baseName,
                   ArrayRef<Identifier> argumentNames) {
  if (argumentNames.size() == 0) {
    SimpleOrCompound = IdentifierAndCompound(baseName, true);
    return;
  }

  llvm::FoldingSetNodeID id;
  CompoundDeclName::Profile(id, baseName, argumentNames);

  void *insert = nullptr;
  if (CompoundDeclName *compoundName
        = C.Impl.CompoundNames.FindNodeOrInsertPos(id, insert)) {
    SimpleOrCompound = compoundName;
    return;
  }

  auto buf = C.Allocate(sizeof(CompoundDeclName)
                          + argumentNames.size() * sizeof(Identifier),
                        alignof(CompoundDeclName));
  auto compoundName = new (buf) CompoundDeclName(baseName,argumentNames.size());
  std::uninitialized_copy(argumentNames.begin(), argumentNames.end(),
                          compoundName->getArgumentNames().begin());
  SimpleOrCompound = compoundName;
  C.Impl.CompoundNames.InsertNode(compoundName, insert);
}
