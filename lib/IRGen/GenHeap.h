//===--- GenHeap.h - Heap-object layout and management ----------*- C++ -*-===//
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
// operations on heap objects and their metadata.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IRGEN_GENHEAP_H
#define SWIFT_IRGEN_GENHEAP_H

#include "NecessaryBindings.h"
#include "StructLayout.h"

namespace llvm {
  class Constant;
  template <class T> class SmallVectorImpl;
}

namespace swift {
namespace irgen {
  class Address;
  
/// A heap layout is the result of laying out a complete structure for
/// heap-allocation.
class HeapLayout : public StructLayout {
  SmallVector<SILType, 8> ElementTypes;
  
public:
  HeapLayout(IRGenModule &IGM, LayoutStrategy strategy,
             ArrayRef<SILType> elementTypes,
             ArrayRef<const TypeInfo *> elementTypeInfos,
             llvm::StructType *typeToFill = 0);

  /// Get the types of the elements.
  ArrayRef<SILType> getElementTypes() const {
    return ElementTypes;
  }
  
  /// Build a size function for this layout.
  llvm::Constant *createSizeFn(IRGenModule &IGM) const;

  /// As a convenience, build a metadata object with internal linkage
  /// consisting solely of the standard heap metadata.
  llvm::Constant *getPrivateMetadata(IRGenModule &IGM) const;
};

/// Emit a heap object deallocation.
void emitDeallocateHeapObject(IRGenFunction &IGF,
                              llvm::Value *object,
                              llvm::Value *size,
                              llvm::Value *alignMask);

} // end namespace irgen
} // end namespace swift

#endif
