//===--- ClangImporter.cpp - Import Clang Modules --------------*- C++ -*--===//
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
// This file implements support for loading Clang modules into Swift.
//
//===----------------------------------------------------------------------===//
#ifndef SWIFT_CLANG_IMPORTER_H
#define SWIFT_CLANG_IMPORTER_H

#include "swift/AST/ClangModuleLoader.h"

namespace clang {
  class ASTContext;
  class Module;
  class TargetInfo;
  class VisibleDeclConsumer;
}

namespace swift {

class ASTContext;
class ClangModuleUnit;
class ClangNode;
class LazyResolver;
class Module;
class NominalTypeDecl;
class VisibleDeclConsumer;
class ClangImporterOptions;
enum class SelectorSplitKind;

/// \brief Class that imports Clang modules into Swift, mapping directly
/// from Clang ASTs over to Swift ASTs.
class ClangImporter final : public ClangModuleLoader {
  friend class ClangModuleUnit;

public:
  class Implementation;

private:
  Implementation &Impl;

  ClangImporter(ASTContext &ctx, const ClangImporterOptions &clangImporterOpts);

public:
  /// \brief Create a new Clang importer that can import a suitable Clang
  /// module into the given ASTContext.
  ///
  /// \param ctx The ASTContext into which the module will be imported.
  /// The ASTContext's SearchPathOptions will be used for the Clang importer.
  ///
  /// \param targetTriple The target triple to use for the import.
  ///
  /// \param clangImporterOpts The options to use for the Clang importer.
  ///
  /// \returns a new Clang module importer, or null (with a diagnostic) if
  /// an error occurred.
  static ClangImporter *create(ASTContext &ctx, StringRef targetTriple,
                               const ClangImporterOptions &clangImporterOpts);

  ClangImporter(const ClangImporter &) = delete;
  ClangImporter(ClangImporter &&) = delete;
  ClangImporter &operator=(const ClangImporter &) = delete;
  ClangImporter &operator=(ClangImporter &&) = delete;

  ~ClangImporter();

  /// \brief Import a module with the given module path.
  ///
  /// Clang modules will be imported using the Objective-C ARC dialect,
  /// with all warnings disabled.
  ///
  /// \param importLoc The location of the 'import' keyword.
  ///
  /// \param path A sequence of (identifier, location) pairs that denote
  /// the dotted module name to load, e.g., AppKit.NSWindow.
  ///
  /// \returns the module referenced, if it could be loaded. Otherwise,
  /// emits a diagnostic and returns NULL.
  virtual Module *loadModule(SourceLoc importLoc,
                             ArrayRef<std::pair<Identifier, SourceLoc>> path)
                                                                      override;

  /// \brief Look for declarations associated with the given name.
  ///
  /// \param name The name we're searching for.
  void lookupValue(Identifier name, VisibleDeclConsumer &consumer);

  /// \brief Look for visible declarations in the Clang translation unit and
  /// import them as Swift decls.
  ///
  /// \param Consumer The VisibleDeclConsumer that will be fed decls as they
  /// are found and imported.
  void lookupVisibleDecls(VisibleDeclConsumer &Consumer) const;

  /// \brief Load extensions to the given nominal type.
  ///
  /// \param nominal The nominal type whose extensions should be loaded.
  ///
  /// \param previousGeneration The previous generation number. The AST already
  /// contains extensions loaded from any generation up to and including this
  /// one.
  virtual void loadExtensions(NominalTypeDecl *nominal,
                              unsigned previousGeneration) override;

  const clang::Module *getClangOwningModule(ClangNode Node) const;

  void verifyAllModules() override;

  void setTypeResolver(LazyResolver &resolver);
  void clearTypeResolver();

  clang::TargetInfo &getTargetInfo() const;
  clang::ASTContext &getClangASTContext() const override;
  clang::Preprocessor &getClangPreprocessor() const override;
  std::string getClangModuleHash() const;
};

typedef decltype(&ClangImporter::create) ClangImporterCtorTy;
ClangImporterCtorTy getClangImporterCtor();

}

#endif
