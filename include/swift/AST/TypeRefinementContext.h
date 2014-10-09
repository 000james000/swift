//===--- TypeRefinementContext.h - Swift Refinement Context -----*- C++ -*-===//
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
// This file defines the TypeRefinementContext class.  A TypeRefinementContext
// is the semantic construct that refines a type within its lexical scope.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_TYPEREFINEMENTCONTEXT_H
#define SWIFT_TYPEREFINEMENTCONTEXT_H

#include "swift/AST/Identifier.h"
#include "swift/AST/Availability.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/SourceLoc.h"
#include "swift/Basic/STLExtras.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/Support/ErrorHandling.h"

namespace swift {
  class Decl;
  class IfStmt;
  class SourceFile;
  class Stmt;

/// Represents a lexical context in which types are refined. For now,
/// types are refined solely for API availability checking, based on
/// the operating system versions that the refined context may execute
/// upon.
///
/// These refinement contexts form a lexical tree parallel to the AST but much
/// more sparse: we only introduce refinement contexts when there is something
/// to refine.
class TypeRefinementContext {

  /// Describes the reason a type refinement context was introduced.
  enum class Reason {
    /// The root refinement context.
    Root,

    /// The context was introduced by a declaration (e.g., the body of a
    /// function declaration or the contents of a class declaration).
    Decl,

    /// The context was introduced for the Then branch of an IfStmt.
    IfStmtThenBranch
  };

  using IntroNode = llvm::PointerUnion3<SourceFile *, Decl *, IfStmt *>;

  /// The AST node that introduced this context.
  IntroNode Node;

  SourceRange SrcRange;

  VersionRange PotentialVersions;

  std::vector<TypeRefinementContext *> Children;

  TypeRefinementContext(ASTContext &Ctx, IntroNode Node,
                        TypeRefinementContext *Parent, SourceRange SrcRange,
                        const VersionRange &Versions);

public:
  
  /// Create the root refinement context for the given SourceFile.
  static TypeRefinementContext *createRoot(ASTContext &Ctx, SourceFile *SF,
                                           const VersionRange &Versions);

  /// Create a refinement context for the given declaration.
  static TypeRefinementContext *createForDecl(ASTContext &Ctx, Decl *D,
                                              TypeRefinementContext *Parent,
                                              const VersionRange &Versions,
                                              SourceRange SrcRange);
  
  /// Create a refinement context for the Then branch of the given IfStmt.
  static TypeRefinementContext *
  createForIfStmtThen(ASTContext &Ctx, IfStmt *S, TypeRefinementContext *Parent,
                      const VersionRange &Versions);
  
  /// Returns the reason this context was introduced.
  Reason getReason() const {
    if (Node.is<Decl *>()) {
      return Reason::Decl;
    } else if (Node.is<IfStmt *>()) {
      // We will need an additional bit to discriminate when we add
      // refinement contexts for Else branches.
      return Reason::IfStmtThenBranch;
    } else if (Node.is<SourceFile *>()) {
      return Reason::Root;
    }
    llvm_unreachable("Unhandled introduction node");
  }
  
  /// Returns the AST node that introduced this refinement context. Note that
  /// this node may be different than the refined range. For example, a
  /// refinement context covering an IfStmt Then branch will have the
  /// IfStmt as the introduction node (and its reason as IfStmtThenBranch)
  /// but its source range will cover the Then branch.
  IntroNode getIntroductionNode() const { return Node; }
  
  /// Returns the source range on which this context refines types.
  SourceRange getSourceRange() const { return SrcRange; }

  /// Returns a version range representing the range of operating system
  /// versions on which the code contained in this context may run.
  const VersionRange &getPotentialVersions() const { return PotentialVersions; }

  /// Adds a child refinement context.
  void addChild(TypeRefinementContext *Child) {
    assert(Child->getSourceRange().isValid());
    Children.push_back(Child);
  }

  /// Returns the inner-most TypeRefinementContext descendent of this context
  /// for the given source location.
  TypeRefinementContext *findMostRefinedSubContext(SourceLoc Loc,
                                                   SourceManager &SM);

  // Only allow allocation of TypeRefinementContext using the allocator in
  // ASTContext.
  void *operator new(size_t Bytes, ASTContext &C,
                     unsigned Alignment = alignof(TypeRefinementContext));
};

} // end namespace swift

#endif
