//===--- ASTScopeImpl.h - Swift AST Object-Oriented Scope --------*- C++-*-===//
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
///
/// This file defines the ASTScopeImpl class ontology, which
/// describes the scopes that exist within a Swift AST.
///
/// Each scope has four basic functions: printing for debugging, creation of
/// itself and its children, obtaining its SourceRange (for lookup), and looking
/// up names accessible from that scope.
///
/// Invarients:
///   a child's source range is a subset (proper or improper) of its parent's,
///   children are ordered by source range, and do not overlap,
///   all the names visible within a parent are visible within the child, unless
///   the nesting is illegal. For instance, a protocol nested inside of a class
///   does not get to see the symbols in the class or its ancestors.
///
//===----------------------------------------------------------------------===//
#ifndef SWIFT_AST_AST_SCOPE_H
#define SWIFT_AST_AST_SCOPE_H

#include "swift/AST/ASTNode.h"
#include "swift/AST/NameLookup.h" // for DeclVisibilityKind
#include "swift/Basic/Compiler.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/NullablePtr.h"
#include "swift/Basic/SourceManager.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

namespace swift {

#pragma mark Forward-references

#define DECL(Id, Parent) class Id##Decl;
#define ABSTRACT_DECL(Id, Parent) class Id##Decl;
#include "swift/AST/DeclNodes.def"
#undef DECL
#undef ABSTRACT_DECL

#define EXPR(Id, Parent) class Id##Expr;
#include "swift/AST/ExprNodes.def"
#undef EXPR

#define STMT(Id, Parent) class Id##Stmt;
#define ABSTRACT_STMT(Id, Parent) class Id##Stmt;
#include "swift/AST/StmtNodes.def"
#undef STMT
#undef ABSTRACT_STMT

class GenericParamList;
class TrailingWhereClause;
class ParameterList;
class PatternBindingEntry;
class SpecializeAttr;
class GenericContext;
class DeclName;

namespace ast_scope {
class ASTScopeImpl;
class GenericTypeOrExtensionScope;
class IterableTypeScope;
class TypeAliasScope;
class StatementConditionElementPatternScope;
class ScopeCreator;

#pragma mark the root ASTScopeImpl class

/// Describes a lexical scope within a source file.
///
/// Each \c ASTScopeImpl is a node within a tree that describes all of the
/// lexical scopes within a particular source range. The root of this scope tree
/// is always a \c SourceFile node, and the tree covers the entire source file.
/// The children of a particular node are the lexical scopes immediately
/// nested within that node, and have source ranges that are enclosed within
/// the source range of their parent node. At the leaves are lexical scopes
/// that cannot be subdivided further.
///
/// The tree provides source-location-based query operations, allowing one to
/// find the innermost scope that contains a given source location. Navigation
/// to parent nodes from that scope allows one to walk the lexically enclosing
/// scopes outward to the source file. Given a scope, one can also query the
/// associated \c DeclContext for additional contextual information.
///
/// \code
/// -dump-scope-maps expanded
/// \endcode
class ASTScopeImpl {
  friend class ASTVisitorForScopeCreation;
  friend class Portion;
  friend class GenericTypeOrExtensionWholePortion;
  friend class NomExtDeclPortion;
  friend class GenericTypeOrExtensionWherePortion;
  friend class GenericTypeOrExtensionWherePortion;
  friend class IterableTypeBodyPortion;
  friend class ScopeCreator;

#pragma mark - tree state
protected:
  using Children = SmallVector<ASTScopeImpl *, 4>;
  /// Whether the given parent is the accessor node for an abstract
  /// storage declaration or is directly descended from it.

private:
  /// Always set by the constructor, so that when creating a child
  /// the parent chain is available.
  ASTScopeImpl *parent = nullptr; // null at the root

  /// Child scopes, sorted by source range.
  Children storedChildren;

  // Must be updated after last child is added and after last child's source
  // position is known
  Optional<SourceRange> cachedSourceRange;

  // When ignoring ASTNodes in a scope, they still must count towards a scope's
  // source range. So include their ranges here
  SourceRange sourceRangeOfIgnoredASTNodes;

#pragma mark - constructor / destructor
public:
  ASTScopeImpl(){};
  // TOD: clean up all destructors and deleters
  virtual ~ASTScopeImpl() {}

  ASTScopeImpl(ASTScopeImpl &&) = delete;
  ASTScopeImpl &operator=(ASTScopeImpl &&) = delete;
  ASTScopeImpl(const ASTScopeImpl &) = delete;
  ASTScopeImpl &operator=(const ASTScopeImpl &) = delete;

  // Make vanilla new illegal for ASTScopes.
  void *operator new(size_t bytes) = delete;
  // Need this because have virtual destructors
  void operator delete(void *data) {}

  // Only allow allocation of scopes using the allocator of a particular source
  // file.
  void *operator new(size_t bytes, const ASTContext &ctx,
                     unsigned alignment = alignof(ASTScopeImpl));
  void *operator new(size_t Bytes, void *Mem) {
    assert(Mem);
    return Mem;
  }

#pragma mark - tree declarations
protected:
  NullablePtr<ASTScopeImpl> getParent() { return parent; }
  NullablePtr<const ASTScopeImpl> getParent() const { return parent; }

  const Children &getChildren() const { return storedChildren; }
  void addChild(ASTScopeImpl *child, ASTContext &);

private:
  NullablePtr<ASTScopeImpl> getPriorSibling() const;

public:
  void postOrderDo(function_ref<void(ASTScopeImpl *)>);

#pragma mark - source ranges

public:
  SourceRange getSourceRange(bool forDebugging = false) const;

protected:
  SourceManager &getSourceManager() const;
  bool hasValidSourceRange() const;
  bool hasValidSourceRangeOfIgnoredASTNodes() const;
  bool verifySourceRange() const;
  bool precedesInSource(const ASTScopeImpl *) const;
  bool verifyThatChildrenAreContained() const;
  bool verifyThatThisNodeComeAfterItsPriorSibling() const;

  virtual Decl *getEnclosingAbstractFunctionOrSubscriptDecl() const;

public:
  virtual NullablePtr<ClosureExpr> getClosureIfClosureScope() const;

private:
  SourceRange getUncachedSourceRange(bool forDebugging = false) const;
public: // for PatternEntryDeclScope::expandMe
  void cacheSourceRange();
private:
  void clearSourceRangeCache();
  void cacheSourceRangesOfSlice();
  void clearCachedSourceRangesOfMeAndAncestors();

  /// Even ASTNodes that do not form scopes must be included in a Scope's source
  /// range. Widen the source range of the receiver to include the (ignored)
  /// node.
  void widenSourceRangeForIgnoredASTNode(ASTNode);

  // InterpolatedStringLiteralExprs and EditorPlaceHolders respond to
  // getSourceRange with the starting point. But we might be asked to lookup an
  // identifer within one of them. So, find the real source range of them here.
  SourceRange getEffectiveSourceRange(ASTNode) const;

public: // public for debugging
  virtual SourceRange getChildlessSourceRange() const = 0;

#pragma mark common queries
public:
  virtual ASTContext &getASTContext() const;
  virtual NullablePtr<DeclContext> getDeclContext() const { return nullptr; };
  virtual NullablePtr<Decl> getDecl() const { return nullptr; };

#pragma mark - debugging and printing

public:
  virtual const SourceFile *getSourceFile() const;
  virtual std::string getClassName() const = 0;

  /// Print out this scope for debugging/reporting purposes.
  void print(llvm::raw_ostream &out, unsigned level = 0, bool lastChild = false,
             bool printChildren = true) const;

  void printRange(llvm::raw_ostream &out) const;

protected:
  virtual void printSpecifics(llvm::raw_ostream &out) const {}
  virtual NullablePtr<const void> addressForPrinting() const;

public:
  LLVM_ATTRIBUTE_DEPRECATED(void dump() const LLVM_ATTRIBUTE_USED,
                            "only for use within the debugger");

  void dumpOneScopeMapLocation(std::pair<unsigned, unsigned> lineColumn) const;

private:
  llvm::raw_ostream &verificationError() const;

#pragma mark - Scope tree creation
protected:

  /// expandScope me, sending deferred nodes to my descendants.
  virtual void expandMe(ScopeCreator &);

public:
  // Some nodes (VarDecls and Accessors) are created directly from
  // pattern scope code and should neither be deferred nor should
  // contribute to widenSourceRangeForIgnoredASTNode.
  // Closures and captures are also created directly but are
  // screened out because they are expressions.
  static bool isCreatedDirectly(const ASTNode n);

  virtual NullablePtr<AbstractStorageDecl>
  getEnclosingAbstractStorageDecl() const;

#pragma mark - - creation queries
protected:
  bool areDeferredNodesInANewScope() const {
    // After an abstract storage decl, what was declared is now accessible.
    return isThisAnAbstractStorageDecl();
  }
public:
  virtual bool isThisAnAbstractStorageDecl() const { return false; }

  unsigned depth() const;

#pragma mark - lookup

public:
  using DeclConsumer = namelookup::AbstractASTScopeDeclConsumer &;

  /// Entry point into ASTScopeImpl-land for lookups
  static Optional<bool> unqualifiedLookup(SourceFile *, DeclName, SourceLoc,
                                          const DeclContext *startingContext,
                                          Optional<bool> isCascadingUse,
                                          DeclConsumer);

#pragma mark - - lookup- starting point
private:
  static const ASTScopeImpl *findStartingScopeForLookup(SourceFile *,
                                                        const DeclName name,
                                                        const SourceLoc where,
                                                        const DeclContext *ctx);

protected:
  virtual bool doesContextMatchStartingContext(const DeclContext *) const;

protected:
  const ASTScopeImpl *findInnermostEnclosingScope(SourceLoc) const;

private:
  NullablePtr<const ASTScopeImpl>
  findChildContaining(SourceLoc loc, SourceManager &sourceMgr) const;

#pragma mark - - lookup- per scope
protected:
  /// The main (recursive) lookup function:
  /// Tell DeclConsumer about all names found in this scope and if not done,
  /// recurse for enclosing scopes. Stop lookup if about to look in limit.
  /// Return final value for isCascadingUse
  ///
  /// If the lookup depends on implicit self, selfDC is its context.
  /// (Names in extensions never depend on self.)
  ///
  /// Because a body scope nests in a generic param scope, etc, we might look in
  /// the self type twice. That's why we pass haveAlreadyLookedHere.
  ///
  /// Look in this scope.
  /// \p selfDC is the context for names dependent on dynamic self,
  /// \p limit is a scope into which lookup should not go,
  /// \p haveAlreadyLookedHere is a Decl whose generics and self type has
  /// already been searched, \p isCascadingUse indicates whether the lookup
  /// results will need a cascading dependency or not \p consumer is the object
  /// to which found decls are reported. Returns the isCascadingUse information.
  Optional<bool> lookup(NullablePtr<DeclContext> selfDC,
                        NullablePtr<const ASTScopeImpl> limit,
                        NullablePtr<const Decl> haveAlreadyLookedHere,
                        Optional<bool> isCascadingUse,
                        DeclConsumer consumer) const;

  /// Same as lookup, but handles the steps to recurse into the parent scope.
  Optional<bool> lookupInParent(NullablePtr<DeclContext> selfDC,
                                NullablePtr<const ASTScopeImpl> limit,
                                NullablePtr<const Decl> haveAlreadyLookedHere,
                                Optional<bool> isCascadingUse,
                                DeclConsumer) const;

  /// Return isDone and isCascadingUse
  std::pair<bool, Optional<bool>>
  lookInGenericsAndSelfType(const NullablePtr<DeclContext> selfDC,
                            const Optional<bool> isCascadingUse,
                            DeclConsumer consumer) const;

  virtual NullablePtr<DeclContext>
      computeSelfDCForParent(NullablePtr<DeclContext>) const;

  virtual std::pair<bool, Optional<bool>>
  lookupInSelfType(NullablePtr<DeclContext> selfDC, Optional<bool>,
                   DeclConsumer) const;

  /// The default for anything that does not do the lookup.
  /// Returns isFinished and isCascadingUse
  static std::pair<bool, Optional<bool>>
  dontLookupInSelfType(Optional<bool> isCascadingUse) {
    return {false, isCascadingUse};
  }

  /// Just a placeholder to make it easier to find
  static void dontExpand() {}

  virtual bool lookInGenericParameters(Optional<bool> isCascadingUse,
                                       DeclConsumer) const;

  // Consume the generic parameters in the context and its outer contexts
  static bool lookInMyAndOuterGenericParameters(const GenericContext *,
                                                Optional<bool> isCascadingUse,
                                                DeclConsumer);

  NullablePtr<const ASTScopeImpl> parentIfNotChildOfTopScope() const {
    const auto *p = getParent().get();
    return p->getParent().isNonNull() ? p : nullptr;
  }

  /// The tree is organized by source location and for most nodes this is also
  /// what obtaines for scoping. However, guards are different. The scope after
  /// the guard else must hop into the innermoset scope of the guard condition.
  virtual NullablePtr<const ASTScopeImpl> getLookupParent() const {
    return parent;
  }

#pragma mark - - lookup- local bindings
protected:
  virtual Optional<bool>
  resolveIsCascadingUseForThisScope(Optional<bool>) const;

  // A local binding is a basically a local variable defined in that very scope
  // It is not an instance variable or inherited type.

  /// Return true if consumer returns true
  virtual bool lookupLocalBindings(Optional<bool>, DeclConsumer) const;

  static bool lookupLocalBindingsInPattern(Pattern *p,
                                           Optional<bool> isCascadingUse,
                                           DeclVisibilityKind vis,
                                           DeclConsumer consumer);

  /// When lookup must stop before the outermost scope, return the scope to stop
  /// at. Example, if a protocol is nested in a struct, we must stop before
  /// looking into the struct.
  virtual NullablePtr<const ASTScopeImpl> getLookupLimit() const;

  NullablePtr<const ASTScopeImpl>
  ancestorWithDeclSatisfying(function_ref<bool(const Decl *)> predicate) const;
}; // end of ASTScopeImpl

#pragma mark specific scope classes

  /// The root of the scope tree.
class ASTSourceFileScope final : public ASTScopeImpl {
public:
  SourceFile *const SF;
  ScopeCreator *const scopeCreator;

  ASTSourceFileScope(SourceFile *SF, ScopeCreator *scopeCreator)
      : SF(SF), scopeCreator(scopeCreator) {}

  std::string getClassName() const override;
  SourceRange getChildlessSourceRange() const override;

protected:
  void printSpecifics(llvm::raw_ostream &out) const override;

public:
  virtual NullablePtr<DeclContext> getDeclContext() const override {
    return NullablePtr<DeclContext>(SF);
  }

  const SourceFile *getSourceFile() const override;
  NullablePtr<const void> addressForPrinting() const override { return SF; }

protected:
  void expandMe(ScopeCreator &) override;
  };

  class Portion {
  public:
    const char *portionName;
    Portion(const char *n) : portionName(n) {}
    virtual ~Portion() {}
    
    // Make vanilla new illegal for ASTScopes.
    void *operator new(size_t bytes) = delete;
    // Need this because have virtual destructors
    void operator delete(void *data) {}
    
    // Only allow allocation of scopes using the allocator of a particular source
    // file.
    void *operator new(size_t bytes, const ASTContext &ctx,
                       unsigned alignment = alignof(ASTScopeImpl));
    void *operator new(size_t Bytes, void *Mem) {
      assert(Mem);
      return Mem;
    }

    virtual void expandScope(GenericTypeOrExtensionScope *,
                             ScopeCreator &) const {}

    virtual SourceRange getChildlessSourceRangeOf(
        const GenericTypeOrExtensionScope *scope) const = 0;

    /// Returns isDone and isCascadingUse
    virtual std::pair<bool, Optional<bool>>
    lookupInSelfTypeOf(const GenericTypeOrExtensionScope *scope,
                       NullablePtr<DeclContext> selfDC,
                       const Optional<bool> isCascadingUse,
                       ASTScopeImpl::DeclConsumer consumer) const;

    virtual NullablePtr<const ASTScopeImpl>
    getLookupLimitFor(const GenericTypeOrExtensionScope *) const;
  };

  // For the whole Decl scope of a GenericType or an Extension
  class GenericTypeOrExtensionWholePortion : public Portion {
  public:
    GenericTypeOrExtensionWholePortion() : Portion("Decl") {}
    virtual ~GenericTypeOrExtensionWholePortion() {}

    // Just for TypeAlias
    void expandScope(GenericTypeOrExtensionScope *,
                     ScopeCreator &) const override;

    SourceRange getChildlessSourceRangeOf(
        const GenericTypeOrExtensionScope *) const override;

    NullablePtr<const ASTScopeImpl>
    getLookupLimitFor(const GenericTypeOrExtensionScope *) const override;
};

/// GenericTypeOrExtension = GenericType or Extension
class GenericTypeOrExtensionWhereOrBodyPortion : public Portion {
public:
  GenericTypeOrExtensionWhereOrBodyPortion(const char *n) : Portion(n) {}
  virtual ~GenericTypeOrExtensionWhereOrBodyPortion() {}

  std::pair<bool, Optional<bool>>
  lookupInSelfTypeOf(const GenericTypeOrExtensionScope *scope,
                     NullablePtr<DeclContext> selfDC,
                     const Optional<bool> isCascadingUse,
                     ASTScopeImpl::DeclConsumer consumer) const override;
};

/// Behavior specific to representing the trailing where clause of a
/// GenericTypeDecl or ExtensionDecl scope.
class GenericTypeOrExtensionWherePortion
    : public GenericTypeOrExtensionWhereOrBodyPortion {
public:
  GenericTypeOrExtensionWherePortion()
      : GenericTypeOrExtensionWhereOrBodyPortion("Where") {}

  SourceRange
  getChildlessSourceRangeOf(const GenericTypeOrExtensionScope *) const override;
};

/// Behavior specific to representing the Body of a NominalTypeDecl or
/// ExtensionDecl scope
class IterableTypeBodyPortion final
    : public GenericTypeOrExtensionWhereOrBodyPortion {
public:
  IterableTypeBodyPortion()
      : GenericTypeOrExtensionWhereOrBodyPortion("Body") {}
  void expandScope(GenericTypeOrExtensionScope *,
                   ScopeCreator &) const override;
  SourceRange
  getChildlessSourceRangeOf(const GenericTypeOrExtensionScope *) const override;
};

/// GenericType or Extension scope
/// : Whole type decl, trailing where clause, or body
class GenericTypeOrExtensionScope : public ASTScopeImpl {
public:
  const Portion *const portion;

  GenericTypeOrExtensionScope(const Portion *p) : portion(p) {}
  virtual ~GenericTypeOrExtensionScope() {}

  virtual NullablePtr<IterableDeclContext> getIterableDeclContext() const {
    return nullptr;
  }
  virtual bool shouldHaveABody() const { return false; }

  void expandMe(ScopeCreator &) override;
  SourceRange getChildlessSourceRange() const override;

  std::pair<bool, Optional<bool>>
  lookupInSelfType(NullablePtr<DeclContext> selfDC,
                   const Optional<bool> isCascadingUse,
                   ASTScopeImpl::DeclConsumer consumer) const override;

  virtual GenericContext *getGenericContext() const = 0;
  std::string getClassName() const override;
  virtual std::string declKindName() const = 0;
  virtual bool doesDeclHaveABody() const;
  const char *portionName() const { return portion->portionName; }
  bool lookInGenericParameters(Optional<bool> isCascadingUse,
                               DeclConsumer) const override;
  NullablePtr<DeclContext>
      computeSelfDCForParent(NullablePtr<DeclContext>) const override;
  Optional<bool> resolveIsCascadingUseForThisScope(
      Optional<bool> isCascadingUse) const override;

  // Only for DeclScope, not BodyScope
  virtual ASTScopeImpl *createTrailingWhereClauseScope(ASTScopeImpl *parent,
                                                       ScopeCreator &) {
    return parent;
  }
  NullablePtr<DeclContext> getDeclContext() const override;
  virtual NullablePtr<NominalTypeDecl> getCorrespondingNominalTypeDecl() const {
    return nullptr;
  }

  virtual void createBodyScope(ASTScopeImpl *leaf, ScopeCreator &) {}

protected:
  void printSpecifics(llvm::raw_ostream &out) const override;

public:
  NullablePtr<const ASTScopeImpl> getLookupLimit() const override;
  virtual NullablePtr<const ASTScopeImpl> getLookupLimitForDecl() const;
};

class IterableTypeScope : public GenericTypeOrExtensionScope {
public:
  IterableTypeScope(const Portion *p) : GenericTypeOrExtensionScope(p) {}
  virtual ~IterableTypeScope() {}

  virtual SourceRange getBraces() const = 0;
  bool shouldHaveABody() const override { return true; }
  bool doesDeclHaveABody() const override;
};

class NominalTypeScope : public IterableTypeScope {
public:
  NominalTypeDecl *decl;
  NominalTypeScope(const Portion *p, NominalTypeDecl *e)
      : IterableTypeScope(p), decl(e) {}
  virtual ~NominalTypeScope() {}

  std::string declKindName() const override { return "NominalType"; }
  NullablePtr<IterableDeclContext> getIterableDeclContext() const override {
    return decl;
  }
  NullablePtr<NominalTypeDecl>
  getCorrespondingNominalTypeDecl() const override {
    return decl;
  }
  GenericContext *getGenericContext() const override { return decl; }
  NullablePtr<Decl> getDecl() const override { return decl; }

  SourceRange getBraces() const override;
  NullablePtr<const ASTScopeImpl> getLookupLimitForDecl() const override;

  void createBodyScope(ASTScopeImpl *leaf, ScopeCreator &) override;
  ASTScopeImpl *createTrailingWhereClauseScope(ASTScopeImpl *parent,
                                               ScopeCreator &) override;
};

class ExtensionScope final : public IterableTypeScope {
public:
  ExtensionDecl *const decl;
  ExtensionScope(const Portion *p, ExtensionDecl *e)
      : IterableTypeScope(p), decl(e) {}
  virtual ~ExtensionScope() {}

  GenericContext *getGenericContext() const override { return decl; }
  NullablePtr<IterableDeclContext> getIterableDeclContext() const override {
    return decl;
  }
  NullablePtr<NominalTypeDecl> getCorrespondingNominalTypeDecl() const override;
  std::string declKindName() const override { return "Extension"; }
  SourceRange getBraces() const override;
  ASTScopeImpl *createTrailingWhereClauseScope(ASTScopeImpl *parent,
                                               ScopeCreator &) override;
  void createBodyScope(ASTScopeImpl *leaf, ScopeCreator &) override;
  NullablePtr<Decl> getDecl() const override { return decl; }
};

class TypeAliasScope : public GenericTypeOrExtensionScope {
public:
  TypeAliasDecl *const decl;
  TypeAliasScope(const Portion *p, TypeAliasDecl *e)
      : GenericTypeOrExtensionScope(p), decl(e) {}
  virtual ~TypeAliasScope() {}

  std::string declKindName() const override { return "TypeAlias"; }
  ASTScopeImpl *createTrailingWhereClauseScope(ASTScopeImpl *parent,
                                               ScopeCreator &) override;
  GenericContext *getGenericContext() const override { return decl; }
  NullablePtr<Decl> getDecl() const override { return decl; }
};

class OpaqueTypeScope final : public GenericTypeOrExtensionScope {
public:
  OpaqueTypeDecl *const decl;
  OpaqueTypeScope(const Portion *p, OpaqueTypeDecl *e)
      : GenericTypeOrExtensionScope(p), decl(e) {}
  virtual ~OpaqueTypeScope() {}

  std::string declKindName() const override { return "OpaqueType"; }
  GenericContext *getGenericContext() const override { return decl; }
  NullablePtr<Decl> getDecl() const override { return decl; }
};

/// Since each generic parameter can "see" the preceeding ones,
/// (e.g. <A, B: A>) -- it's not legal but that's how lookup behaves --
/// Each GenericParamScope scopes just ONE parameter, and we next
/// each one within the previous one.
///
/// Here's a wrinkle: for a Subscript, the caller expects this scope (based on
/// source loc) to match requested DeclContexts for starting lookup in EITHER
/// the getter or setter AbstractFunctionDecl (context)
class GenericParamScope final : public ASTScopeImpl {
public:
  /// The declaration that has generic parameters.
  Decl *const holder;
  /// The generic parameters themselves.
  GenericParamList *const paramList;
  /// The index of the current parameter.
  const unsigned index;

  GenericParamScope(Decl *holder, GenericParamList *paramList, unsigned index)
      : holder(holder), paramList(paramList), index(index) {}
  virtual ~GenericParamScope() {}

  /// Actually holder is always a GenericContext, need to test if
  /// ProtocolDecl or SubscriptDecl but will refactor later.
  NullablePtr<DeclContext> getDeclContext() const override;
  std::string getClassName() const override;
  SourceRange getChildlessSourceRange() const override;

protected:
  void printSpecifics(llvm::raw_ostream &out) const override;

public:
  NullablePtr<AbstractStorageDecl>
  getEnclosingAbstractStorageDecl() const override;

  NullablePtr<const void> addressForPrinting() const override {
    return paramList;
  }

protected:
  bool lookupLocalBindings(Optional<bool>, DeclConsumer) const override;
  bool doesContextMatchStartingContext(const DeclContext *) const override;
  Optional<bool>
  resolveIsCascadingUseForThisScope(Optional<bool>) const override;
};

/// Concrete class for a function/initializer/deinitializer
class AbstractFunctionDeclScope : public ASTScopeImpl {
public:
  AbstractFunctionDecl *const decl;
  AbstractFunctionDeclScope(AbstractFunctionDecl *e) : decl(e) {}
  virtual ~AbstractFunctionDeclScope() {}

  void expandMe(ScopeCreator &) override;
  std::string getClassName() const override;
  SourceRange getChildlessSourceRange() const override;

protected:
  void printSpecifics(llvm::raw_ostream &out) const override;

public:
  virtual NullablePtr<DeclContext> getDeclContext() const override {
    return decl;
  }
  virtual NullablePtr<Decl> getDecl() const override { return decl; }

  NullablePtr<AbstractStorageDecl>
  getEnclosingAbstractStorageDecl() const override;

protected:
  Decl *getEnclosingAbstractFunctionOrSubscriptDecl() const override;
  bool lookInGenericParameters(Optional<bool> isCascadingUse,
                               DeclConsumer) const override;
  Optional<bool>
  resolveIsCascadingUseForThisScope(Optional<bool>) const override;
};

/// The parameters for an abstract function (init/func/deinit).
class AbstractFunctionParamsScope final : public ASTScopeImpl {
public:
  ParameterList *const params;
  /// For get functions in subscript declarations,
  /// a lookup into the subscript parameters must count as the get func context.
  const NullablePtr<DeclContext> matchingContext;

  AbstractFunctionParamsScope(ParameterList *params,
                              NullablePtr<DeclContext> matchingContext)
      : params(params), matchingContext(matchingContext) {}
  virtual ~AbstractFunctionParamsScope() {}

  void expandMe(ScopeCreator &) override;
  std::string getClassName() const override;
  SourceRange getChildlessSourceRange() const override;
  virtual NullablePtr<DeclContext> getDeclContext() const override;

  NullablePtr<AbstractStorageDecl>
  getEnclosingAbstractStorageDecl() const override;
  NullablePtr<const void> addressForPrinting() const override { return params; }
};

class AbstractFunctionBodyScope : public ASTScopeImpl {
public:
  AbstractFunctionDecl *const decl;

  AbstractFunctionBodyScope(AbstractFunctionDecl *e) : decl(e) {}
  virtual ~AbstractFunctionBodyScope() {}

  void expandMe(ScopeCreator &) override;
  SourceRange getChildlessSourceRange() const override;
  virtual NullablePtr<DeclContext> getDeclContext() const override {
    return decl;
  }
  virtual NullablePtr<Decl> getDecl() const override { return decl; }

protected:
  bool lookupLocalBindings(Optional<bool>, DeclConsumer) const override;
  Optional<bool>
  resolveIsCascadingUseForThisScope(Optional<bool>) const override;
};

/// Body of methods, functions in types.
class MethodBodyScope final : public AbstractFunctionBodyScope {
public:
  MethodBodyScope(AbstractFunctionDecl *e) : AbstractFunctionBodyScope(e) {}
  std::string getClassName() const override;

protected:
  NullablePtr<DeclContext>
      computeSelfDCForParent(NullablePtr<DeclContext>) const override;
};

/// Body of "pure" functions, functions without an implicit "self".
class PureFunctionBodyScope final : public AbstractFunctionBodyScope {
public:
  PureFunctionBodyScope(AbstractFunctionDecl *e)
      : AbstractFunctionBodyScope(e) {}
  std::string getClassName() const override;
  bool lookupLocalBindings(Optional<bool>,
                           DeclConsumer consumer) const override;

protected:
  NullablePtr<DeclContext>
      computeSelfDCForParent(NullablePtr<DeclContext>) const override;
};

class DefaultArgumentInitializerScope final : public ASTScopeImpl {
public:
  ParamDecl *const decl;

  DefaultArgumentInitializerScope(ParamDecl *e) : decl(e) {}
  ~DefaultArgumentInitializerScope() {}

  void expandMe(ScopeCreator &) override;
  std::string getClassName() const override;
  SourceRange getChildlessSourceRange() const override;
  virtual NullablePtr<DeclContext> getDeclContext() const override;
  virtual NullablePtr<Decl> getDecl() const override { return decl; }

protected:
  Optional<bool>
  resolveIsCascadingUseForThisScope(Optional<bool>) const override;
};

// Consider:
//  @_propertyWrapper
//  struct WrapperWithInitialValue {
//  }
//  struct HasWrapper {
//    @WrapperWithInitialValue var y = 17
//  }
// Lookup has to be able to find the use of WrapperWithInitialValue, that's what
// this scope is for. Because the source positions are screwy.

class AttachedPropertyWrapperScope : public ASTScopeImpl {
public:
  VarDecl *const decl;

  AttachedPropertyWrapperScope(VarDecl *e) : decl(e) {}
  virtual ~AttachedPropertyWrapperScope() {}

  std::string getClassName() const override;
  SourceRange getChildlessSourceRange() const override;
  NullablePtr<const void> addressForPrinting() const override { return decl; }
  virtual NullablePtr<DeclContext> getDeclContext() const override;

  static SourceRange getCustomAttributesSourceRange(const VarDecl *);
};

/// PatternBindingDecl's (PBDs) are tricky (See the comment for \c
/// PatternBindingDecl):
///
/// A PBD contains a list of "patterns", e.g.
///   var (a, b) = foo(), (c,d) = bar() which has two patterns.
///
/// For each pattern, there will be potentially three scopes:
/// always one for the declarations, maybe one for the initializers, and maybe
/// one for users of that pattern.
///
/// If a PBD occurs in code, its initializer can access all prior declarations.
/// Thus, a new scope must be created, nested in the scope of the PBD.
/// In contrast, if a PBD occurs in a type declaration body, its initializer
/// cannot access prior declarations in that body.
///
/// As a further complication, we get VarDecls and their accessors in deferred
/// which really must go into one of the PBD scopes. So we discard them in
/// createIfNeeded, and special-case their creation in
/// addVarDeclScopesAndTheirAccessors.

class AbstractPatternEntryScope : public ASTScopeImpl {
public:
  PatternBindingDecl *const decl;
  const unsigned patternEntryIndex;
  const DeclVisibilityKind vis;

  AbstractPatternEntryScope(PatternBindingDecl *, unsigned entryIndex,
                            DeclVisibilityKind);
  virtual ~AbstractPatternEntryScope() {}

  const PatternBindingEntry &getPatternEntry() const;
  Pattern *getPattern() const;

protected:
  void printSpecifics(llvm::raw_ostream &out) const override;
  void forEachVarDeclWithExplicitAccessors(
      ScopeCreator &scopeCreator, bool dontRegisterAsDuplicate,
      function_ref<void(VarDecl *)> foundOne) const;

public:
  NullablePtr<const void> addressForPrinting() const override { return decl; }
  bool isLastEntry() const;
};

class PatternEntryDeclScope final : public AbstractPatternEntryScope {
public:
  PatternEntryDeclScope(PatternBindingDecl *pbDecl, unsigned entryIndex,
                        DeclVisibilityKind vis)
      : AbstractPatternEntryScope(pbDecl, entryIndex, vis) {}
  virtual ~PatternEntryDeclScope() {}

  void expandMe(ScopeCreator &) override;
  std::string getClassName() const override;
  SourceRange getChildlessSourceRange() const override;
};

class PatternEntryInitializerScope final : public AbstractPatternEntryScope {
public:
  PatternEntryInitializerScope(PatternBindingDecl *pbDecl, unsigned entryIndex,
                               DeclVisibilityKind vis)
      : AbstractPatternEntryScope(pbDecl, entryIndex, vis) {}
  virtual ~PatternEntryInitializerScope() {}

  void expandMe(ScopeCreator &) override;
  std::string getClassName() const override;
  SourceRange getChildlessSourceRange() const override;
  virtual NullablePtr<DeclContext> getDeclContext() const override;
  virtual NullablePtr<Decl> getDecl() const override { return decl; }

protected:
  bool lookupLocalBindings(Optional<bool>, DeclConsumer) const override;
  NullablePtr<DeclContext>
      computeSelfDCForParent(NullablePtr<DeclContext>) const override;
  Optional<bool>
  resolveIsCascadingUseForThisScope(Optional<bool>) const override;
};

class PatternEntryUseScope final : public AbstractPatternEntryScope {
public:
  /// If valid, I must not start before this.
  /// Pattern won't tell me where the initializer really ends because it may end
  /// in an EditorPlaceholder or InterpolatedStringLiteral Those tokens can
  /// contain names to look up after their source locations.
  const SourceLoc initializerEnd;

  PatternEntryUseScope(PatternBindingDecl *pbDecl, unsigned entryIndex,
                       DeclVisibilityKind vis, SourceLoc initializerEnd)
      : AbstractPatternEntryScope(pbDecl, entryIndex, vis),
        initializerEnd(initializerEnd) {}
  virtual ~PatternEntryUseScope() {}

  void expandMe(ScopeCreator &) override;
  std::string getClassName() const override;
  SourceRange getChildlessSourceRange() const override;

protected:
  bool lookupLocalBindings(Optional<bool>, DeclConsumer) const override;
};

/// The scope introduced by a conditional clause in an if/guard/while
/// statement.
/// Since there may be more than one "let foo = ..." in (e.g.) an "if",
/// we allocate a matrushka of these.
class ConditionalClauseScope : public ASTScopeImpl {
public:
  LabeledConditionalStmt *const enclosingStmt;
  Stmt *const stmtAfterAllConditions;

  /// The index of the conditional clause.
  const unsigned index;

  /// The next deepest, if any
  NullablePtr<ConditionalClauseScope> nextConditionalClause;

  NullablePtr<StatementConditionElementPatternScope>
      statementConditionElementPatternScope;

  ConditionalClauseScope(LabeledConditionalStmt *enclosingStmt, unsigned index,
                         Stmt *stmtAfterAllConditions)
      : enclosingStmt(enclosingStmt), index(index),
        stmtAfterAllConditions(stmtAfterAllConditions) {}
  virtual ~ConditionalClauseScope() {}

  void expandMe(ScopeCreator &) override;
  std::string getClassName() const override;

protected:
  void printSpecifics(llvm::raw_ostream &out) const override;

public:
  virtual LabeledConditionalStmt *getContainingStatement() const = 0;
  NullablePtr<const void> addressForPrinting() const override {
    return getContainingStatement();
  }
  void createSubtreeForCondition(ScopeCreator &);
  SourceLoc startLocAccordingToCondition() const;

  ASTScopeImpl *findInnermostConditionScope();

  ConditionalClauseScope *findDeepestConditionalClauseScope();

  NullablePtr<StatementConditionElementPatternScope>
  getStatementConditionElementPatternScope() const;
  SourceRange getChildlessSourceRange() const override;
};

/// If, while, & guard statements all start with a conditional clause, then some
/// later part of the statement, (then, body, or after the guard) circumvents
/// the normal lookup rule to pass the lookup scope into the deepest conditional
/// clause.
class ConditionalClauseUseScope : public ASTScopeImpl {
  ASTScopeImpl *const lookupParent;
  const SourceLoc startLoc;

public:
  ConditionalClauseUseScope(ASTScopeImpl *lookupParent, SourceLoc startLoc)
      : lookupParent(lookupParent), startLoc(startLoc) {}

  SourceRange getChildlessSourceRange() const override;
  std::string getClassName() const override;

protected:
  void printSpecifics(llvm::raw_ostream &out) const override;
  NullablePtr<const ASTScopeImpl> getLookupParent() const override {
    return lookupParent;
  }
};

/// Within a ConditionalClauseScope, there may be a pattern binding
/// StmtConditionElement. If so, it splits the scope into two scopes: one
/// containing the definitions and the other containing the initializer. We must
/// split it because the initializer must not be in scope of the definitions:
/// e.g.: if let a = a {}
/// We need to be able to lookup either a and the second a must not bind to the
/// first one. This scope represents the scope of the variable being
/// initialized.
class StatementConditionElementPatternScope : public ASTScopeImpl {
public:
  Pattern *const pattern;
  StatementConditionElementPatternScope(Pattern *e) : pattern(e) {}
  virtual ~StatementConditionElementPatternScope() {}

  std::string getClassName() const override;
  SourceRange getChildlessSourceRange() const override;

protected:
  void printSpecifics(llvm::raw_ostream &out) const override;

public:
  NullablePtr<const void> addressForPrinting() const override {
    return pattern;
  }

protected:
  bool lookupLocalBindings(Optional<bool>, DeclConsumer) const override;
};

/// Capture lists may contain initializer expressions
/// No local bindings here (other than closures in initializers);
/// rather include these in the params or body local bindings
class CaptureListScope : public ASTScopeImpl {
public:
  CaptureListExpr *const expr;
  CaptureListScope(CaptureListExpr *e) : expr(e) {}
  virtual ~CaptureListScope() {}

  void expandMe(ScopeCreator &) override;
  std::string getClassName() const override;
  SourceRange getChildlessSourceRange() const override;
  NullablePtr<const void> addressForPrinting() const override { return expr; }
  virtual NullablePtr<DeclContext> getDeclContext() const override;
};

// In order for compatibility with existing lookup, closures are represented
// by multiple scopes: An overall scope (including the part before the "in"
// and a body scope, including the part after the "in"
class AbstractClosureScope : public ASTScopeImpl {
public:
  NullablePtr<CaptureListExpr> captureList;
  ClosureExpr *const closureExpr;

  AbstractClosureScope(ClosureExpr *closureExpr,
                       NullablePtr<CaptureListExpr> captureList)
      : captureList(captureList), closureExpr(closureExpr) {}
  virtual ~AbstractClosureScope() {}

  NullablePtr<ClosureExpr> getClosureIfClosureScope() const override;
  NullablePtr<DeclContext> getDeclContext() const override {
    return closureExpr;
  }
  NullablePtr<const void> addressForPrinting() const override {
    return closureExpr;
  }
};

class WholeClosureScope final : public AbstractClosureScope {
public:
  WholeClosureScope(ClosureExpr *closureExpr,
                    NullablePtr<CaptureListExpr> captureList)
      : AbstractClosureScope(closureExpr, captureList) {}
  virtual ~WholeClosureScope() {}

  void expandMe(ScopeCreator &) override;
  std::string getClassName() const override;
  SourceRange getChildlessSourceRange() const override;
};

/// For a closure with named parameters, this scope does the local bindings.
/// Absent if no "in".
class ClosureParametersScope final : public AbstractClosureScope {
public:
  ClosureParametersScope(ClosureExpr *closureExpr,
                         NullablePtr<CaptureListExpr> captureList)
      : AbstractClosureScope(closureExpr, captureList) {}
  virtual ~ClosureParametersScope() {}

  std::string getClassName() const override;
  SourceRange getChildlessSourceRange() const override;

protected:
  bool lookupLocalBindings(Optional<bool>, DeclConsumer) const override;
  Optional<bool> resolveIsCascadingUseForThisScope(
      Optional<bool> isCascadingUse) const override;
};

// The body encompasses the code in the closure; the part after the "in" if
// there is an "in"
class ClosureBodyScope final : public AbstractClosureScope {
public:
  ClosureBodyScope(ClosureExpr *closureExpr,
                   NullablePtr<CaptureListExpr> captureList)
      : AbstractClosureScope(closureExpr, captureList) {}
  virtual ~ClosureBodyScope() {}

  void expandMe(ScopeCreator &) override;
  std::string getClassName() const override;
  SourceRange getChildlessSourceRange() const override;

protected:
  Optional<bool> resolveIsCascadingUseForThisScope(
      Optional<bool> isCascadingUse) const override;
};

class TopLevelCodeScope final : public ASTScopeImpl {
public:
  TopLevelCodeDecl *const decl;
  TopLevelCodeScope(TopLevelCodeDecl *e) : decl(e) {}
  virtual ~TopLevelCodeScope() {}

  void expandMe(ScopeCreator &) override;
  std::string getClassName() const override;
  SourceRange getChildlessSourceRange() const override;
  virtual NullablePtr<DeclContext> getDeclContext() const override {
    return decl;
  }
  virtual NullablePtr<Decl> getDecl() const override { return decl; }
};

/// The \c _@specialize attribute.
class SpecializeAttributeScope final : public ASTScopeImpl {
public:
  SpecializeAttr *const specializeAttr;
  AbstractFunctionDecl *const whatWasSpecialized;

  SpecializeAttributeScope(SpecializeAttr *specializeAttr,
                           AbstractFunctionDecl *whatWasSpecialized)
      : specializeAttr(specializeAttr), whatWasSpecialized(whatWasSpecialized) {
  }
  virtual ~SpecializeAttributeScope() {}

  std::string getClassName() const override;
  SourceRange getChildlessSourceRange() const override;
  NullablePtr<const void> addressForPrinting() const override {
    return specializeAttr;
  }

  NullablePtr<AbstractStorageDecl>
  getEnclosingAbstractStorageDecl() const override;

protected:
  bool lookupLocalBindings(Optional<bool>, DeclConsumer) const override;
};

class SubscriptDeclScope final : public ASTScopeImpl {
public:
  SubscriptDecl *const decl;

  SubscriptDeclScope(SubscriptDecl *e) : decl(e) {}
  virtual ~SubscriptDeclScope() {}

  void expandMe(ScopeCreator &) override;
  std::string getClassName() const override;
  SourceRange getChildlessSourceRange() const override;

protected:
  void printSpecifics(llvm::raw_ostream &out) const override;

public:
  virtual NullablePtr<DeclContext> getDeclContext() const override {
    return decl;
  }
  virtual NullablePtr<Decl> getDecl() const override { return decl; }

protected:
  Decl *getEnclosingAbstractFunctionOrSubscriptDecl() const override;
  bool lookInGenericParameters(Optional<bool> isCascadingUse,
                               DeclConsumer) const override;
  NullablePtr<AbstractStorageDecl>
  getEnclosingAbstractStorageDecl() const override {
    return decl;
  }
public:
  bool isThisAnAbstractStorageDecl() const override { return true; }
};

class VarDeclScope final : public ASTScopeImpl {

public:
  VarDecl *const decl;
  VarDeclScope(VarDecl *e) : decl(e) {}
  virtual ~VarDeclScope() {}

  void expandMe(ScopeCreator &) override;
  std::string getClassName() const override;
  SourceRange getChildlessSourceRange() const override;

protected:
  void printSpecifics(llvm::raw_ostream &out) const override;

public:
  virtual NullablePtr<Decl> getDecl() const override { return decl; }
  NullablePtr<AbstractStorageDecl>
  getEnclosingAbstractStorageDecl() const override {
    return decl;
  }
  bool isThisAnAbstractStorageDecl() const override { return true; }
};

class AbstractStmtScope : public ASTScopeImpl {
public:
  virtual Stmt *getStmt() const = 0;
  NullablePtr<const void> addressForPrinting() const override {
    return getStmt();
  }
  SourceRange getChildlessSourceRange() const override;
};

class LabeledConditionalStmtScope : public AbstractStmtScope {
public:
  Stmt *getStmt() const override;
  virtual LabeledConditionalStmt *getLabeledConditionalStmt() const = 0;

  /// If a condition is present, create the martuska.
  /// Return the lookupParent for the use scope.
  ASTScopeImpl *createCondScopes();

protected:
  ASTScopeImpl *createCondScopes(ScopeCreator &);
  virtual Stmt *getStmtAfterTheConditions() const = 0;
};

class IfStmtScope : public LabeledConditionalStmtScope {
public:
  IfStmt *const stmt;
  IfStmtScope(IfStmt *e) : stmt(e) {}
  virtual ~IfStmtScope() {}

  void expandMe(ScopeCreator &) override;
  std::string getClassName() const override;
  LabeledConditionalStmt *getLabeledConditionalStmt() const override;

protected:
  Stmt *getStmtAfterTheConditions() const override;
};

class WhileStmtScope : public LabeledConditionalStmtScope {
public:
  WhileStmt *const stmt;
  WhileStmtScope(WhileStmt *e) : stmt(e) {}
  virtual ~WhileStmtScope() {}

  void expandMe(ScopeCreator &) override;
  std::string getClassName() const override;
  LabeledConditionalStmt *getLabeledConditionalStmt() const override;

protected:
  Stmt *getStmtAfterTheConditions() const override;
};

class GuardStmtScope : public LabeledConditionalStmtScope {
public:
  GuardStmt *const stmt;
  GuardStmtScope(GuardStmt *e) : stmt(e) {}
  virtual ~GuardStmtScope() {}

  void expandMe(ScopeCreator &) override;
  std::string getClassName() const override;
  LabeledConditionalStmt *getLabeledConditionalStmt() const override;

protected:
  Stmt *getStmtAfterTheConditions() const override;
};

class RepeatWhileScope : public AbstractStmtScope {
public:
  RepeatWhileStmt *const stmt;
  RepeatWhileScope(RepeatWhileStmt *e) : stmt(e) {}
  virtual ~RepeatWhileScope() {}

  void expandMe(ScopeCreator &) override;
  std::string getClassName() const override;
  Stmt *getStmt() const override { return stmt; }
};

class DoCatchStmtScope : public AbstractStmtScope {
public:
  DoCatchStmt *const stmt;
  DoCatchStmtScope(DoCatchStmt *e) : stmt(e) {}
  virtual ~DoCatchStmtScope() {}

  void expandMe(ScopeCreator &) override;
  std::string getClassName() const override;
  Stmt *getStmt() const override { return stmt; }
};

class SwitchStmtScope : public AbstractStmtScope {
public:
  SwitchStmt *const stmt;
  SwitchStmtScope(SwitchStmt *e) : stmt(e) {}
  virtual ~SwitchStmtScope() {}

  void expandMe(ScopeCreator &) override;
  std::string getClassName() const override;
  Stmt *getStmt() const override { return stmt; }
};

class ForEachStmtScope : public AbstractStmtScope {
public:
  ForEachStmt *const stmt;
  ForEachStmtScope(ForEachStmt *e) : stmt(e) {}
  virtual ~ForEachStmtScope() {}

  void expandMe(ScopeCreator &) override;
  std::string getClassName() const override;
  Stmt *getStmt() const override { return stmt; }
};

class ForEachPatternScope : public AbstractStmtScope {
public:
  ForEachStmt *const stmt;
  ForEachPatternScope(ForEachStmt *e) : stmt(e) {}
  virtual ~ForEachPatternScope() {}

  void expandMe(ScopeCreator &) override;
  std::string getClassName() const override;
  Stmt *getStmt() const override { return stmt; }
  SourceRange getChildlessSourceRange() const override;

protected:
  bool lookupLocalBindings(Optional<bool>, DeclConsumer) const override;
};

class CatchStmtScope : public AbstractStmtScope {
public:
  CatchStmt *const stmt;
  CatchStmtScope(CatchStmt *e) : stmt(e) {}
  virtual ~CatchStmtScope() {}

  void expandMe(ScopeCreator &) override;
  std::string getClassName() const override;
  SourceRange getChildlessSourceRange() const override;
  Stmt *getStmt() const override { return stmt; }

protected:
  bool lookupLocalBindings(Optional<bool>,
                           ASTScopeImpl::DeclConsumer) const override;
};

class CaseStmtScope : public AbstractStmtScope {
public:
  CaseStmt *const stmt;
  CaseStmtScope(CaseStmt *e) : stmt(e) {}
  virtual ~CaseStmtScope() {}

  void expandMe(ScopeCreator &) override;
  std::string getClassName() const override;
  SourceRange getChildlessSourceRange() const override;
  Stmt *getStmt() const override { return stmt; }

protected:
  bool lookupLocalBindings(Optional<bool>,
                           ASTScopeImpl::DeclConsumer) const override;
};

class BraceStmtScope : public AbstractStmtScope {
public:
  BraceStmt *const stmt;
  BraceStmtScope(BraceStmt *e) : stmt(e) {}
  virtual ~BraceStmtScope() {}

  void expandMe(ScopeCreator &) override;
  std::string getClassName() const override;
  SourceRange getChildlessSourceRange() const override;
  virtual NullablePtr<DeclContext> getDeclContext() const override;

  NullablePtr<ClosureExpr> parentClosureIfAny() const; // public??
  Stmt *getStmt() const override { return stmt; }

protected:
  bool lookupLocalBindings(Optional<bool>, DeclConsumer) const override;
};
} // namespace ast_scope
} // namespace swift

#endif // SWIFT_AST_AST_SCOPE_H
