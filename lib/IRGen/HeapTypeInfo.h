//===--- HeapTypeInfo.h - Utilities for reference-counted types -*- C++ -*-===//
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
// This file defines some routines that are useful for emitting
// types that are single, reference-counted pointers.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IRGEN_HEAPTYPEINFO_H
#define SWIFT_IRGEN_HEAPTYPEINFO_H

#include "llvm/IR/DerivedTypes.h"
#include "ReferenceTypeInfo.h"
#include "ScalarTypeInfo.h"

namespace swift {
namespace irgen {

/// HeapTypeInfo - A type designed for use implementing a type
/// which consists solely of something reference-counted.
///
/// Subclasses should implement the following method, returning true
/// if it's known to be OK to use Swift reference-counting on values
/// of this type:
///   bool hasSwiftRefcount() const;
template <class Impl>
class HeapTypeInfo : public SingleScalarTypeInfo<Impl, ReferenceTypeInfo> {
  typedef SingleScalarTypeInfo<Impl, ReferenceTypeInfo> super;
protected:
  using super::asDerived;
public:
  HeapTypeInfo(llvm::PointerType *storage, Size size, Alignment align)
    : super(storage, size, align) {}

  bool isSingleRetainablePointer(ResilienceScope scope) const {
    return asDerived().hasSwiftRefcount();
  }

  static const bool IsScalarPOD = false;

  void emitScalarRelease(IRGenFunction &IGF, llvm::Value *value) const {
    if (asDerived().hasSwiftRefcount()) {
      IGF.emitRelease(value);
    } else {
      IGF.emitObjCRelease(value);
    }
  }

  void emitScalarRetain(IRGenFunction &IGF, llvm::Value *value) const {
    if (asDerived().hasSwiftRefcount()) {
      IGF.emitRetainCall(value);
    } else {
      IGF.emitObjCRetainCall(value);
    }
  }

  void emitScalarWeakRelease(IRGenFunction &IGF, llvm::Value *value) const {
    if (asDerived().hasSwiftRefcount()) {
      IGF.emitWeakRelease(value);
    } else {
      IGF.emitUnknownWeakRelease(value);
    }
  }

  void emitScalarWeakRetain(IRGenFunction &IGF, llvm::Value *value) const {
    if (asDerived().hasSwiftRefcount()) {
      IGF.emitWeakRetain(value);
    } else {
      IGF.emitUnknownWeakRetain(value);
    }
  }

  void retain(IRGenFunction &IGF, Explosion &e) const override {
    llvm::Value *value = e.claimNext();
    asDerived().emitScalarRetain(IGF, value);
  }

  void release(IRGenFunction &IGF, Explosion &e) const override {
    llvm::Value *value = e.claimNext();
    asDerived().emitScalarRelease(IGF, value);
  }

  void weakRetain(IRGenFunction &IGF, Explosion &e) const override {
    llvm::Value *value = e.claimNext();
    asDerived().emitScalarWeakRetain(IGF, value);
  }

  void weakRelease(IRGenFunction &IGF, Explosion &e) const override {
    llvm::Value *value = e.claimNext();
    asDerived().emitScalarWeakRelease(IGF, value);
  }

  const WeakTypeInfo *createWeakStorageType(TypeConverter &TC) const {
    if (asDerived().hasSwiftRefcount()) {
      return TC.createSwiftWeakStorageType(this->getStorageType());
    } else {
      return TC.createUnknownWeakStorageType(this->getStorageType());
    }
  }

  const UnownedTypeInfo *createUnownedStorageType(TypeConverter &TC) const {
    if (asDerived().hasSwiftRefcount()) {
      return TC.createSwiftUnownedStorageType(this->getStorageType());
    } else {
      return TC.createUnknownUnownedStorageType(this->getStorageType());
    }
  }
};

}
}

#endif


