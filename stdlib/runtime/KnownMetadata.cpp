//===--- KnownMetadata.cpp - Swift Language ABI Known Metadata Objects ----===//
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
// Definitions of some builtin metadata objects.
//
//===----------------------------------------------------------------------===//

#include "swift/Runtime/Metadata.h"
#include "swift/Runtime/Alloc.h"
#include <cstring>
#include <climits>

using namespace swift;

/// Copy a value from one object to another based on the size in the
/// given type metadata.
OpaqueValue *swift::swift_copyPOD(OpaqueValue *dest, OpaqueValue *src,
                                  const Metadata *type) {
  return (OpaqueValue*) memcpy(dest, src, type->getValueWitnesses()->size);
}

/// A function which helpfully does nothing.
static void doNothing(void *ptr, const void *self) {}

/// A projectBuffer implementation which just reinterprets the buffer.
static OpaqueValue *projectBuffer(ValueBuffer *dest, const Metadata *self) {
  return reinterpret_cast<OpaqueValue*>(dest);
}

/// A function which does a naive copy.
template <class T> static T *copy(T *dest, T *src, const Metadata *self) {
  *dest = *src;
  return dest;
}

/// A function which returns back a static metatype.
const Metadata *swift::swift_staticTypeof(OpaqueValue *src,
                                          const Metadata *self) {
  return self;
}

// Work around a Xcode 4.5 bug (rdar://12288058) by explicitly
// instantiating this function template at the types we'll need.
#define INSTANTIATE(TYPE) \
  template TYPE *copy<TYPE>(TYPE*, TYPE*, const Metadata*);
INSTANTIATE(bool);
INSTANTIATE(uint8_t);
INSTANTIATE(uint16_t);
INSTANTIATE(uint32_t);
INSTANTIATE(uint64_t);
INSTANTIATE(uintptr_t);
#undef INSTANTIATE

#define POD_VALUE_WITNESS_TABLE(TYPE, SIZE) { \
  (value_witness_types::destroyBuffer*) &doNothing,                     \
  (value_witness_types::initializeBufferWithCopyOfBuffer*) &copy<TYPE>, \
  (value_witness_types::projectBuffer*) &projectBuffer,                 \
  (value_witness_types::deallocateBuffer*) &doNothing,                  \
  (value_witness_types::destroy*) &doNothing,                           \
  (value_witness_types::initializeBufferWithCopy*) &copy<TYPE>,         \
  (value_witness_types::initializeWithCopy*) &copy<TYPE>,               \
  (value_witness_types::assignWithCopy*) &copy<TYPE>,                   \
  (value_witness_types::initializeBufferWithTake*) &copy<TYPE>,         \
  (value_witness_types::initializeWithTake*) &copy<TYPE>,               \
  (value_witness_types::assignWithTake*) &copy<TYPE>,                   \
  (value_witness_types::allocateBuffer*) &projectBuffer,                \
  (value_witness_types::typeOf*) &swift_staticTypeof,                   \
  (value_witness_types::size) (SIZE),                                   \
  ValueWitnessFlags().withAlignment(SIZE).withPOD(true)                 \
                     .withInlineStorage(true),                          \
  (value_witness_types::stride) (SIZE)                                  \
}

const ValueWitnessTable swift::_TWVBi8_ = POD_VALUE_WITNESS_TABLE(uint8_t, 1);
const ValueWitnessTable swift::_TWVBi16_ = POD_VALUE_WITNESS_TABLE(uint16_t, 2);
const ValueWitnessTable swift::_TWVBi32_ = POD_VALUE_WITNESS_TABLE(uint32_t, 4);
const ValueWitnessTable swift::_TWVBi64_ = POD_VALUE_WITNESS_TABLE(uint64_t, 8);

/// A function to initialize a buffer/variable by retaining the given
/// pointer and then assigning it.
static HeapObject **initWithRetain(HeapObject **dest,
                                   HeapObject **src,
                                   const Metadata *self) {
  *dest = swift_retain(*src);
  return dest;
}

/// A function to destroy a buffer/variable by releasing the value in it.
static void destroyWithRelease(HeapObject **var, const Metadata *self) {
  swift_release(*var);
}

/// A function to assign to a variable by copying from an existing one.
static HeapObject **assignWithRetain(HeapObject **dest,
                                     HeapObject **src,
                                     const Metadata *self) {
  HeapObject *newValue = swift_retain(*src);
  swift_release(*dest);
  *dest = newValue;
  return dest;
}

/// A function to assign to a variable by taking from an existing one.
static HeapObject **assignWithoutRetain(HeapObject **dest,
                                        HeapObject **src,
                                        const Metadata *self) {
  HeapObject *newValue = *src;
  swift_release(*dest);
  *dest = newValue;
  return dest;
}

/// A function to get the dynamic class type of a Swift heap object.
const Metadata *swift::swift_objectTypeof(OpaqueValue *obj,
                                          const Metadata *self) {
  auto *heapRef = *reinterpret_cast<HeapObject**>(obj);
  auto *classMetadata = static_cast<const ClassMetadata*>(heapRef->metadata);
  // If the heap metadata for the object is not a type, give up and return the
  // static type.
  if (!classMetadata->isTypeMetadata())
    return self;
  return classMetadata;
}

// The extra inhabitants and spare bits of heap object pointers.
// These must align with the values in IRGen's SwiftTargetInfo.cpp.
#if defined(__x86_64__)

# ifdef __APPLE__
// Darwin reserves the low 4GB of address space.
static const uintptr_t LeastValidPointerValue = 4ULL*1024ULL*1024ULL*1024ULL;
# else
// Assume only the null 4K page is reserved.
static const uintptr_t LeastValidPointerValue = 4096U;
# endif

// Only the bottom 47 bits are used, and heap objects are eight-byte-aligned.
static const uintptr_t SwiftSpareBitsMask   = 0xFFFF800000000007ULL;
// Objective-C reserves the high and low bits for tagged pointers.
static const uintptr_t ObjCSpareBitsMask    = 0x8FFF800000000006ULL;
static const uintptr_t ObjCReservedBitsMask = 0x8000000000000001ULL;

// Number of low bits reserved by Objective-C.
static const unsigned ObjCReservedLowBits = 1U;

#elif defined(__arm64__)

// Darwin reserves the low 4GB of address space.
static const uintptr_t LeastValidPointerValue = 4ULL*1024ULL*1024ULL*1024ULL;

// TBI guarantees the top byte of pointers is unused.
// Heap objects are eight-byte aligned.
static const uintptr_t SwiftSpareBitsMask   = 0xFF00000000000007ULL;
// Objective-C reserves the high and low bits for tagged pointers.
static const uintptr_t ObjCSpareBitsMask    = 0x8F00000000000006ULL;
static const uintptr_t ObjCReservedBitsMask = 0x8000000000000001ULL;

// Number of low bits reserved by Objective-C.
static const unsigned ObjCReservedLowBits = 1U;

#else

// Assume only 0 is an invalid pointer.
static const uintptr_t LeastValidPointerValue = 1U;
// Make no assumptions about spare bits.
static const uintptr_t SwiftSpareBitsMask = 0U;
static const uintptr_t ObjCSpareBitsMask = 0U;
static const uintptr_t ObjCReservedBitsMask = 0U;
static const unsigned ObjCReservedLowBits = 0U;

#endif

/// Store an invalid pointer value as an extra inhabitant of a heap object.
static void storeHeapObjectExtraInhabitant(HeapObject **dest,
                                           int index,
                                           const Metadata *self) {
  // This must be consistent with the storeHeapObjectExtraInhabitant
  // implementation in IRGen's GenType.cpp.
  
  // FIXME: We could use high spare bits to produce extra inhabitants, but we
  // probably won't need to.
  *dest = (HeapObject*)((uintptr_t)index << ObjCReservedLowBits);
}

/// Return the extra inhabitant index for an invalid pointer value, or -1 if
/// the pointer is valid.
static int getHeapObjectExtraInhabitantIndex(HeapObject * const* src,
                                             const Metadata *self) {
  // This must be consistent with the getHeapObjectExtraInhabitant
  // implementation in IRGen's GenType.cpp.

  uintptr_t val = (uintptr_t)*src;

  // Return -1 for valid pointers.
  // FIXME: We could use high spare bits to produce extra inhabitants, but we
  // probably won't need to.
  if (val >= LeastValidPointerValue)
    return -1;
  
  // Return -1 for ObjC tagged pointers.
  // FIXME: This check is unnecessary for known-Swift types.
  if (val & ObjCReservedBitsMask)
    return -1;
  
  return (int)(val >> ObjCReservedLowBits);
}

static constexpr unsigned getHeapObjectExtraInhabitantCount() {
  // This must be consistent with the getHeapObjectExtraInhabitantCount
  // implementation in IRGen's GenType.cpp.
  
  // The runtime needs no more than INT_MAX inhabitants.
  return (LeastValidPointerValue >> ObjCReservedLowBits) > INT_MAX
    ? (unsigned)INT_MAX
    : (unsigned)(LeastValidPointerValue >> ObjCReservedLowBits);
}

/// The basic value-witness table for Swift object pointers.
const ExtraInhabitantsValueWitnessTable swift::_TWVBo = {
  {
    (value_witness_types::destroyBuffer*) &destroyWithRelease,
    (value_witness_types::initializeBufferWithCopyOfBuffer*) &initWithRetain,
    (value_witness_types::projectBuffer*) &projectBuffer,
    (value_witness_types::deallocateBuffer*) &doNothing,
    (value_witness_types::destroy*) &destroyWithRelease,
    (value_witness_types::initializeBufferWithCopy*) &initWithRetain,
    (value_witness_types::initializeWithCopy*) &initWithRetain,
    (value_witness_types::assignWithCopy*) &assignWithRetain,
    (value_witness_types::initializeBufferWithTake*) &copy<uintptr_t>,
    (value_witness_types::initializeWithTake*) &copy<uintptr_t>,
    (value_witness_types::assignWithTake*) &assignWithoutRetain,
    (value_witness_types::allocateBuffer*) &projectBuffer,
    (value_witness_types::typeOf*) &swift_objectTypeof,
    (value_witness_types::size) sizeof(void*),
    ValueWitnessFlags().withAlignment(alignof(void*))
                       .withPOD(false)
                       .withExtraInhabitants(true)
                       .withInlineStorage(true),
    (value_witness_types::stride) sizeof(void*),
  },
  (value_witness_types::storeExtraInhabitant*) &storeHeapObjectExtraInhabitant,
  (value_witness_types::getExtraInhabitantIndex*) &getHeapObjectExtraInhabitantIndex,
  ExtraInhabitantFlags()
    .withNumExtraInhabitants(getHeapObjectExtraInhabitantCount())
};

/*** Objective-C pointers ****************************************************/

// This section can reasonably be suppressed in builds that don't
// need to support Objective-C.

// ARC entrypoints.
extern "C" void *objc_retain(void *);
extern "C" void objc_release(void *);

// ObjC runtime entrypoints.
extern "C" const void *object_getClass(void *);

/// A function to initialize a buffer/variable by retaining the given
/// pointer and then assigning it.
static void **initWithObjCRetain(void **dest, void **src,
                                 const Metadata *self) {
  *dest = objc_retain(*src);
  return dest;
}

/// A function to destroy a buffer/variable by releasing the value in it.
static void destroyWithObjCRelease(void **var, const Metadata *self) {
  objc_release(*var);
}

/// A function to assign to a variable by copying from an existing one.
static void **assignWithObjCRetain(void **dest, void **src,
                                   const Metadata *self) {
  void *newValue = objc_retain(*src);
  objc_release(*dest);
  *dest = newValue;
  return dest;
}

/// A function to assign to a variable by taking from an existing one.
static void **assignWithoutObjCRetain(void **dest, void **src,
                                      const Metadata *self) {
  void *newValue = *src;
  objc_release(*dest);
  *dest = newValue;
  return dest;
}

/// A function to get the Swift type metadata wrapper for an ObjC object's
/// dynamic type.

const Metadata *swift::swift_objcTypeof(OpaqueValue *src, const Metadata *self)
{
  auto object = *reinterpret_cast<HeapObject**>(src);
  return swift_unknownTypeOf(object);
}

const Metadata *swift::swift_unknownTypeOf(HeapObject *object)
{
  auto theClass = object_getClass(object);
  auto classAsMetadata = reinterpret_cast<const ClassMetadata*>(theClass);
  if (classAsMetadata->isTypeMetadata()) return classAsMetadata;
  
  return swift_getObjCClassMetadata(classAsMetadata);
}

/// The basic value-witness table for ObjC object pointers.
const ExtraInhabitantsValueWitnessTable swift::_TWVBO = {
  {
    (value_witness_types::destroyBuffer*) &destroyWithObjCRelease,
    (value_witness_types::initializeBufferWithCopyOfBuffer*) &initWithObjCRetain,
    (value_witness_types::projectBuffer*) &projectBuffer,
    (value_witness_types::deallocateBuffer*) &doNothing,
    (value_witness_types::destroy*) &destroyWithObjCRelease,
    (value_witness_types::initializeBufferWithCopy*) &initWithObjCRetain,
    (value_witness_types::initializeWithCopy*) &initWithObjCRetain,
    (value_witness_types::assignWithCopy*) &assignWithObjCRetain,
    (value_witness_types::initializeBufferWithTake*) &copy<uintptr_t>,
    (value_witness_types::initializeWithTake*) &copy<uintptr_t>,
    (value_witness_types::assignWithTake*) &assignWithoutObjCRetain,
    (value_witness_types::allocateBuffer*) &projectBuffer,
    (value_witness_types::typeOf*) &swift_objcTypeof,
    (value_witness_types::size) sizeof(void*),
    ValueWitnessFlags().withAlignment(alignof(void*)).withPOD(false)
                       .withInlineStorage(true)
                       .withExtraInhabitants(true),
    (value_witness_types::stride) sizeof(void*)
  },
  (value_witness_types::storeExtraInhabitant*) &storeHeapObjectExtraInhabitant,
  (value_witness_types::getExtraInhabitantIndex*) &getHeapObjectExtraInhabitantIndex,
  ExtraInhabitantFlags()
    .withNumExtraInhabitants(getHeapObjectExtraInhabitantCount())
};

/*** Functions ***************************************************************/

namespace {
  struct Function {
    void *FnPtr;
    HeapObject *Data;
  };
}

// Assert what we consider to be a reasonable property of ValueBuffer.
static_assert(sizeof(Function) <= sizeof(ValueBuffer),
              "function values don't fit inline in a value buffer");

static void function_destroy(Function *fn, const Metadata *self) {
  swift_release(fn->Data);
}

static Function *function_initWithRetain(Function *dest, Function *src,
                                         const Metadata *self) {
  dest->FnPtr = src->FnPtr;
  dest->Data = swift_retain(src->Data);
  return dest;
}

static Function *function_initWithoutRetain(Function *dest, Function *src,
                                            const Metadata *self) {
  dest->FnPtr = src->FnPtr;
  dest->Data = src->Data;
  return dest;
}

static Function *function_assignWithRetain(Function *dest, Function *src,
                                           const Metadata *self) {
  dest->FnPtr = src->FnPtr;
  if (dest->Data != src->Data) {
    HeapObject *oldData = dest->Data;
    dest->Data = swift_retain(src->Data);
    swift_release(oldData);
  }
  return dest;
}

static Function *function_assignWithoutRetain(Function *dest, Function *src,
                                              const Metadata *self) {
  dest->FnPtr = src->FnPtr;
  HeapObject *oldData = dest->Data;
  dest->Data = src->Data;
  swift_release(oldData);
  return dest;
}

/// The basic value-witness table for function types.
const ValueWitnessTable swift::_TWVFT_T_ = {
  (value_witness_types::destroyBuffer*) &function_destroy,
  (value_witness_types::initializeBufferWithCopyOfBuffer*) &function_initWithRetain,
  (value_witness_types::projectBuffer*) &projectBuffer,
  (value_witness_types::deallocateBuffer*) &doNothing,
  (value_witness_types::destroy*) &function_destroy,
  (value_witness_types::initializeBufferWithCopy*) &function_initWithRetain,
  (value_witness_types::initializeWithCopy*) &function_initWithRetain,
  (value_witness_types::assignWithCopy*) &function_assignWithRetain,
  (value_witness_types::initializeBufferWithTake*) &function_initWithoutRetain,
  (value_witness_types::initializeWithTake*) &function_initWithoutRetain,
  (value_witness_types::assignWithTake*) &function_assignWithoutRetain,
  (value_witness_types::allocateBuffer*) &projectBuffer,
  (value_witness_types::typeOf*) &swift_staticTypeof,
  (value_witness_types::size) sizeof(Function),
  ValueWitnessFlags().withAlignment(alignof(Function)).withPOD(false)
                     .withInlineStorage(true),
  (value_witness_types::stride) sizeof(Function)
};

/*** Empty tuples ************************************************************/

// A function which does nothing and returns its first argument.
static void *doNothing3(void *dest, void *src, void *self) {
  return dest;
}

/// The basic value-witness table for empty types.
const ValueWitnessTable swift::_TWVT_ = {
  (value_witness_types::destroyBuffer*) &doNothing,
  (value_witness_types::initializeBufferWithCopyOfBuffer*) &doNothing3,
  (value_witness_types::projectBuffer*) &projectBuffer,
  (value_witness_types::deallocateBuffer*) &doNothing,
  (value_witness_types::destroy*) &doNothing,
  (value_witness_types::initializeBufferWithCopy*) &doNothing3,
  (value_witness_types::initializeWithCopy*) &doNothing3,
  (value_witness_types::assignWithCopy*) &doNothing3,
  (value_witness_types::initializeBufferWithTake*) &doNothing3,
  (value_witness_types::initializeWithTake*) &doNothing3,
  (value_witness_types::assignWithTake*) &doNothing3,
  (value_witness_types::allocateBuffer*) &projectBuffer,
  (value_witness_types::typeOf*) &swift_staticTypeof,
  (value_witness_types::size) 0,
  ValueWitnessFlags().withAlignment(1).withPOD(true).withInlineStorage(true),
  (value_witness_types::stride) 0
};

/*** Known metadata **********************************************************/

// Define some builtin opaque metadata.
#define OPAQUE_METADATA(TYPE) \
  const FullOpaqueMetadata swift::_TMd##TYPE = { \
    { &_TWV##TYPE },                             \
    { { MetadataKind::Opaque } }                 \
  };
OPAQUE_METADATA(Bi8_)
OPAQUE_METADATA(Bi16_)
OPAQUE_METADATA(Bi32_)
OPAQUE_METADATA(Bi64_)
OPAQUE_METADATA(Bo)
OPAQUE_METADATA(BO)

/// The standard metadata for the empty tuple.
const FullMetadata<TupleTypeMetadata> swift::_TMdT_ = {
  { &_TWVT_ },                 // ValueWitnesses
  {
    { MetadataKind::Tuple },   // Kind
    0,                         // NumElements
    nullptr                    // Labels
  }
};
