//===--- ClassMetadataLayout.h - CRTP for class metadata --------*- C++ -*-===//
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
// A CRTP helper class for class metadata.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IRGEN_CLASSMETADATALAYOUT_H
#define SWIFT_IRGEN_CLASSMETADATALAYOUT_H

#include "swift/SIL/SILDeclRef.h"
#include "IRGen.h"
#include "MetadataLayout.h"

namespace swift {
namespace irgen {

class IRGenModule;

/// The number of fields in a FullHeapMetadata object.
const unsigned NumHeapMetadataFields = 3;

/// Does the given class method require a different dispatch-table
/// entry from from all of the methods it overrides?  The restrictions
/// on overriding generally prevent this, but it can happen when a
/// class overrides a method from a generic class.
bool doesMethodRequireOverrideEntry(IRGenModule &IGM, AbstractFunctionDecl *fn,
                                    ResilienceExpansion explosionLevel,
                                    unsigned uncurryLevel);

/// A CRTP class for laying out class metadata.  Note that this does
/// *not* handle the metadata template stuff.
template <class Impl> class ClassMetadataLayout : public MetadataLayout<Impl> {
  typedef MetadataLayout<Impl> super;

protected:
  using super::IGM;
  using super::asImpl;

  /// The most-derived class.
  ClassDecl *const Target;

  /// Is the object layout globally resilient at this point?
  bool IsObjectResilient = false;

  /// Is the metadata layout globally resilient at this point?
  bool IsMetadataResilient = false;

  ClassMetadataLayout(IRGenModule &IGM, ClassDecl *target)
    : super(IGM), Target(target) {}

public:
  void layout() {
    // HeapMetadata header.
    asImpl().addDestructorFunction();

    // Metadata header.
    super::layout();

    // ClassMetadata header.  In ObjCInterop mode, this must be
    // layout-compatible with an Objective-C class.  The superclass
    // pointer is useful regardless of mode, but the rest of the data
    // isn't necessary.
    asImpl().addSuperClass();
    if (IGM.ObjCInterop) {
      asImpl().addClassCacheData();
      asImpl().addClassDataPointer();
    }

    asImpl().addInstanceSize();
    asImpl().addInstanceAlignMask();
    asImpl().addNominalTypeDescriptor();
    
    // Class members.
    addClassMembers(Target);
  }

private:
  /// Add fields associated with the given class and its bases.
  void addClassMembers(ClassDecl *theClass) {
    // Add any fields associated with the superclass.
    // NB: We don't apply superclass substitutions to members because we want
    // consistent metadata layout between generic superclasses and concrete
    // subclasses.
    if (Type superclass = theClass->getSuperclass()) {
      addClassMembers(superclass->getClassOrBoundGenericClass());
    }

    // Add a reference to the parent class, if applicable.
    if (theClass->getDeclContext()->isTypeContext()) {
      asImpl().addParentMetadataRef(theClass);
    }

    // Add space for the generic parameters, if applicable.
    // Note that we only add references for the immediate parameters;
    // parameters for the parent context are handled by the parent.
    if (auto generics = theClass->getGenericParams()) {
      addGenericClassFields(theClass, *generics);
    }

    // If there exists a potential context from which the class is
    // resilient, subsequent fields will require indirect offsets.
    if (IGM.isResilient(theClass, ResilienceScope::Universal)) {
      IsObjectResilient = true;
      IsMetadataResilient = true;
    }
    
    // Add entries for the methods.
    for (auto member : theClass->getMembers()) {
      // If this is a non-overriding @final member, we don't need table entries.
      if (auto *VD = dyn_cast<ValueDecl>(member))
        if (VD->isFinal() && VD->getOverriddenDecl() == nullptr)
          continue;
      
      // Add entries for methods.
      if (auto fn = dyn_cast<FuncDecl>(member)) {
        // Ignore accessors.  These get added when their AbstractStorageDecl is
        // visited.
        if (fn->isAccessor())
          continue;
        addMethodEntries(fn);
      } else if (auto ctor = dyn_cast<ConstructorDecl>(member)) {
        // Add entries for constructors.
        addMethodEntries(ctor);
      } else if (auto *asd = dyn_cast<AbstractStorageDecl>(member)) {
        // FIXME: Stored properties shouldn't be represented this way.
        if (!asd->hasAccessorFunctions()) continue;

        addMethodEntries(asd->getGetter());
        if (auto *setter = asd->getSetter())
          addMethodEntries(setter);
      }
    }
    
    // Add field offsets.
    for (auto member : theClass->getMembers()) {
      if (auto field = dyn_cast<VarDecl>(member))
        if (field->hasStorage())
          updateForFieldSize(field);
    }

    asImpl().noteStartOfFieldOffsets(theClass);
    for (auto member : theClass->getMembers()) {
      if (auto field = dyn_cast<VarDecl>(member))
        if (field->hasStorage())
          addFieldEntries(field);
    }
    asImpl().noteEndOfFieldOffsets(theClass);
  }
  
  /// Notes the beginning of the field offset vector for a particular ancestor
  /// of a generic-layout class.
  void noteStartOfFieldOffsets(ClassDecl *whichClass) {}

  /// Notes the end of the field offset vector for a particular ancestor
  /// of a generic-layout class.
  void noteEndOfFieldOffsets(ClassDecl *whichClass) {}

private:
  /// Add fields related to the generics of this class declaration.
  /// TODO: don't add new fields that are implied by the superclass.
  /// fields.  e.g., if B<T> extends A<T>, the witness for T in A's
  /// section should be enough.
  void addGenericClassFields(ClassDecl *theClass,
                             const GenericParamList &generics) {
    asImpl().addGenericFields(generics, theClass);
  }

  void addFieldEntries(VarDecl *field) {
    asImpl().addFieldOffset(field);
  }

  void updateForFieldSize(VarDecl *field) {
    assert(field->hasStorage());

    // Update the class layout based on abstract, globally-known
    // characteristics of the type.
    SILType fieldType = IGM.getLoweredType(AbstractionPattern(field->getType()),
                                           field->getType());
    switch (IGM.classifyTypeSize(fieldType, ResilienceScope::Universal)) {
    case ObjectSize::Fixed:
      return;
    case ObjectSize::Resilient:
      IsObjectResilient = true;
      return;
    case ObjectSize::Dependent:
      IsObjectResilient = true;
      return;
    }
    llvm_unreachable("invalid type size classification");
  }

  void addMethodEntries(AbstractFunctionDecl *fn) {
    // If the method does not have a vtable entry, don't add any.
    if (!hasKnownVTableEntry(IGM, fn))
      return;
    
    // TODO: consider emitting at different explosion levels and
    // uncurryings.
    auto explosionLevel = ResilienceExpansion::Minimal;
    unsigned uncurryLevel = fn->getNaturalArgumentCount() - 1;
    
    if (isa<FuncDecl>(fn))
      maybeAddMethod(fn, SILDeclRef::Kind::Func, explosionLevel, uncurryLevel);
    else {
      auto ctor = cast<ConstructorDecl>(fn);
      if (ctor->isRequired())
        maybeAddMethod(fn, SILDeclRef::Kind::Allocator, explosionLevel, 
                       uncurryLevel);
      maybeAddMethod(fn, SILDeclRef::Kind::Initializer, explosionLevel, 
                     uncurryLevel);      
    }
  }

  void maybeAddMethod(AbstractFunctionDecl *fn,
                      SILDeclRef::Kind kind,
                      ResilienceExpansion explosionLevel,
                      unsigned uncurryLevel) {
    // If the method overrides something, we don't need a new entry.
    if (fn->getOverriddenDecl()) {
      // Except we do if it differs by abstraction from all the
      // methods it overrides.
      if (!doesMethodRequireOverrideEntry(IGM, fn, explosionLevel,
                                          uncurryLevel))
        return;
    }

    // Both static and non-static functions go in the metadata.
    asImpl().addMethod(SILDeclRef(fn, kind, explosionLevel, uncurryLevel));
  }
};

/// An "implementation" of ClassMetadataLayout that just scans through
/// the metadata layout, maintaining the offset of the next field.
template <class Impl>
class ClassMetadataScanner : public ClassMetadataLayout<Impl> {
  typedef ClassMetadataLayout<Impl> super;
protected:
  Size NextOffset = Size(0);

  ClassMetadataScanner(IRGenModule &IGM, ClassDecl *target)
    : super(IGM, target) {}

public:
  void addMetadataFlags() { addPointer(); }
  void addNominalTypeDescriptor() { addPointer(); }
  void addValueWitnessTable() { addPointer(); }
  void addDestructorFunction() { addPointer(); }
  void addParentMetadataRef(ClassDecl *forClass) { addPointer(); }
  void addSuperClass() { addPointer(); }
  void addInstanceSize() { addInt32(); }
  void addInstanceAlignMask() { addInt32(); }
  void addClassCacheData() { addPointer(); addPointer(); }
  void addClassDataPointer() { addPointer(); }
  void addMethod(SILDeclRef fn) { addPointer(); }
  void addFieldOffset(VarDecl *var) { addPointer(); }
  void addGenericArgument(ArchetypeType *argument, ClassDecl *forClass) {
    addPointer();
  }
  void addGenericWitnessTable(ArchetypeType *argument,
                              ProtocolDecl *protocol,
                              ClassDecl *forClass) {
    addPointer();
  }

private:
  // Our layout here assumes that there will never be unclaimed space
  // in the metadata.
  void addPointer() {
    NextOffset += super::IGM.getPointerSize();
  }
  void addInt32() {
    NextOffset += Size(4);
  }
};

} // end namespace irgen
} // end namespace swift

#endif
