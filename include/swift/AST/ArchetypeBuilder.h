//===--- ArchetypeBuilder.h - Generic Archetype Builder -------------------===//
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
// Support for collecting a set of generic requirements, both explicitly stated
// and inferred, and computing the archetypes and required witness tables from
// those requirements.
//
//===----------------------------------------------------------------------===//

#include "swift/Basic/LLVM.h"
#include "swift/Basic/Optional.h"
#include "llvm/ADT/ArrayRef.h"
#include <functional>
#include <memory>

namespace swift {

class AbstractTypeParamDecl;
class ArchetypeType;
class Pattern;
class ProtocolDecl;
class Requirement;
class SourceLoc;
class Type;
class TypeRepr;
class ASTContext;
class DiagnosticEngine;

/// \brief Collects a set of requirements of generic parameters, both explicitly
/// stated and inferred, and determines the set of archetypes for each of
/// the generic parameters.
class ArchetypeBuilder {
  struct PotentialArchetype;
  class InferRequirementsWalker;
  friend class InferRequirementsWalker;

  ASTContext &Context;
  DiagnosticEngine &Diags;
  struct Implementation;
  std::unique_ptr<Implementation> Impl;

  ArchetypeBuilder(const ArchetypeBuilder &) = delete;
  ArchetypeBuilder &operator=(const ArchetypeBuilder &) = delete;

  /// \brief Resolve the given type to the potential archetype it names.
  ///
  /// This routine will synthesize nested types as required to refer to a
  /// potential archetype, even in cases where no requirement specifies the
  /// requirement for such an archetype. FIXME: The failure to include such a
  /// requirement will be diagnosed at some point later (when the types in the
  /// signature are fully resolved).
  ///
  /// For any type that cannot refer to an archetype, this routine returns null.
  PotentialArchetype *resolveType(Type type);

  /// \brief Add a new conformance requirement specifying that the given
  /// potential archetype conforms to the given protocol.
  bool addConformanceRequirement(PotentialArchetype *T,
                                 ProtocolDecl *Proto);

  /// \brief Add a new superclass requirement specifying that the given
  /// potential archetype has the given type as an ancestor.
  bool addSuperclassRequirement(PotentialArchetype *T, SourceLoc ColonLoc,
                                Type Superclass);

  /// \brief Add a new same-type requirement specifying that the given potential
  /// archetypes should map to the equivalent archetype.
  bool addSameTypeRequirement(PotentialArchetype *T1,
                              SourceLoc EqualLoc,
                              PotentialArchetype *T2);

public:
  ArchetypeBuilder(ASTContext &Context, DiagnosticEngine &Diags);

  /// Construct a new archtype builder.
  ///
  /// \param Context The ASTContext in which the builder will create archetypes.
  ///
  /// \param Diags The diagnostics entity to use.
  ///
  /// \param getInheritedProtocols A function that determines the set of
  /// protocols inherited from the given protocol. This produces the final
  /// results of ProtocolDecl::getProtocols().
  ///
  /// \param getConformsTo A function that determines the set of protocols
  /// to which the given type parameter conforms. The produces the final
  /// results of AbstractTypeParamDecl::getProtocols() for an associated type.
  ArchetypeBuilder(
    ASTContext &Context, DiagnosticEngine &Diags,
    std::function<ArrayRef<ProtocolDecl *>(ProtocolDecl *)>
      getInheritedProtocols,
    std::function<ArrayRef<ProtocolDecl *>(AbstractTypeParamDecl *)>
      getConformsTo);
  ArchetypeBuilder(ArchetypeBuilder &&);
  ~ArchetypeBuilder();

  /// \brief Add a new generic parameter for which there may be requirements.
  ///
  /// \returns true if an error occurred, false otherwise.
  bool addGenericParameter(AbstractTypeParamDecl *GenericParam,
                           Optional<unsigned> Index = Nothing);

  /// \brief Add a new requirement.
  ///
  /// \returns true if this requirement makes the set of requirements
  /// inconsistent, in which case a diagnostic will have been issued.
  bool addRequirement(const Requirement &Req);

  /// \brief Add a new, implicit conformance requirement for one of the
  /// parameters.
  bool addImplicitConformance(AbstractTypeParamDecl *Param,
                              ProtocolDecl *Proto);

  /// Infer requirements from the given type representation, recursively.
  ///
  /// This routine infers requirements from a type that occurs within the
  /// signature of a generic function. For example, given:
  ///
  /// \code
  /// func f<K, V>(dict : Dictionary<K, V>) { ... }
  /// \endcode
  ///
  /// where \c Dictionary requires that its key type be \c Hashable,
  /// the requirement \c K : Hashable is inferred from the parameter type,
  /// because the type \c Dictionary<K,V> cannot be formed without it.
  ///
  /// \returns true if an error occurred, false otherwise.
  bool inferRequirements(TypeRepr *type);

  /// Infer requirements from the given pattern, recursively.
  ///
  /// This routine infers requirements from a type that occurs within the
  /// signature of a generic function. For example, given:
  ///
  /// \code
  /// func f<K, V>(dict : Dictionary<K, V>) { ... }
  /// \endcode
  ///
  /// where \c Dictionary requires that its key type be \c Hashable,
  /// the requirement \c K : Hashable is inferred from the parameter type,
  /// because the type \c Dictionary<K,V> cannot be formed without it.
  ///
  /// \returns true if an error occurred, false otherwise.
  bool inferRequirements(Pattern *pattern);

  /// \brief Assign archetypes to each of the generic parameters and all
  /// of their associated types, recursively.
  ///
  /// This operation should only be performed after all generic parameters and
  /// requirements have been added to the builder. It is non-reversible.
  void assignArchetypes();

  /// \brief Retrieve the archetype that corresponds to the given generic
  /// parameter.
  ArchetypeType *getArchetype(AbstractTypeParamDecl *GenericParam) const;

  /// \brief Retrieve the array of all of the archetypes produced during
  /// archetype assignment. The 'primary' archetypes will occur first in this
  /// list.
  ArrayRef<ArchetypeType *> getAllArchetypes();

  // FIXME: Infer requirements from signatures
  // FIXME: Compute the set of 'extra' witness tables needed to express this
  // requirement set.

  /// \brief Dump all of the requirements, both specified and inferred.
  void dump();
};

} // end namespace swift
