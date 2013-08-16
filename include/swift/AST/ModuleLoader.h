//===--- ModuleLoader.h - Module Loader Interface ----------- -*- C++ -*- -===//
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
// This file implements an abstract interface for loading modules.
//
//===----------------------------------------------------------------------===//
#ifndef SWIFT_AST_MODULE_LOADER_H
#define SWIFT_AST_MODULE_LOADER_H

#include "swift/AST/Identifier.h"
#include "swift/AST/Module.h"
#include "swift/AST/Type.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/SourceLoc.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
namespace swift {

class Module;
class NominalTypeDecl;

enum class KnownProtocolKind : uint8_t;
  
/// \brief Abstract interface that loads named modules into the AST.
class ModuleLoader : public llvm::RefCountedBaseVPTR {
public:
  virtual ~ModuleLoader();

  /// \brief Import a module with the given module path.
  ///
  /// \param importLoc The location of the 'import' keyword.
  ///
  /// \param path A sequence of (identifier, location) pairs that denote
  /// the dotted module name to load, e.g., AppKit.NSWindow.
  ///
  /// \returns the module referenced, if it could be loaded. Otherwise,
  /// emits a diagnostic and returns NULL.
  virtual
  Module *loadModule(SourceLoc importLoc,
                     ArrayRef<std::pair<Identifier, SourceLoc>> path) = 0;

  /// \brief Look for declarations associated with the given name.
  ///
  /// \param module The module to search.
  ///
  /// \param accessPath The access path used to refer to the name within this
  /// (top-level) module.
  ///
  /// \param name The name we're searching for.
  ///
  /// \param lookupKind Whether we're performing qualified vs. unqualified
  /// lookup.
  ///
  /// \param result Will be populated with the results of name lookup.
  virtual void lookupValue(Module *module,
                           Module::AccessPathTy accessPath, Identifier name,
                           NLKind lookupKind,
                           SmallVectorImpl<ValueDecl*> &result) { }
  
  /// \brief Load extensions to the given nominal type.
  ///
  /// \param nominal The nominal type whose extensions should be loaded.
  ///
  /// \param previousGeneration The previous generation number. The AST already
  /// contains extensions loaded from any generation up to and including this
  /// one.
  virtual void loadExtensions(NominalTypeDecl *nominal,
                              unsigned previousGeneration) { };
  
  /// \brief Load decls that provide conformances to the given compiler-known
  /// protocol.
  ///
  /// \param kind The known protocol whose decls should be loaded.
  ///
  /// \param previousGeneration The previous generation number. The AST already
  /// contains decls conforming to this protocol loaded from any generation up
  /// to and including this one.
  virtual void loadDeclsConformingTo(KnownProtocolKind kind,
                                     unsigned previousGeneration) { };

  /// \brief Look for members of the given type.
  ///
  /// \param module The module to search.
  ///
  /// \param base The type into which we will look to find members.
  ///
  /// \param name The name of the members we are looking for.
  ///
  /// \param result Will be populated with the results of name lookup.
  virtual void lookupMembers(Module *module, Type base, Identifier name,
                             SmallVectorImpl<ValueDecl*> &result) { }

  /// \brief Look for a declaration of the given operator.
  ///
  /// \returns The operator decl, or null if this module does not define the
  /// operator in question.
  virtual OperatorDecl *lookupOperator(Module *module, Identifier name,
                                       DeclKind fixity) {
    return nullptr;
  }

  /// \brief Look for modules re-exported by the given module.
  virtual void
  getReexportedModules(const Module *module,
                       SmallVectorImpl<Module::ImportedModule> &exports) { }

  /// \brief Look for all visible top-level decls in the module.
  virtual void lookupVisibleDecls(const Module *module,
                                  Module::AccessPathTy accessPath,
                                  VisibleDeclConsumer &consumer,
                                  NLKind lookupKind) { }

  /// \brief Look for all class members.
  ///
  /// This is used for id-style lookup.
  virtual void lookupClassMembers(const Module *module,
                                  Module::AccessPathTy accessPath,
                                  VisibleDeclConsumer &consumer) { }
};

}


#endif
