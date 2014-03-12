//===--- Alloc.h - Swift Language Allocation ABI ---------------*- C++ -*--===//
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
// Swift Allocation ABI
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_RUNTIME_ALLOC_H
#define SWIFT_RUNTIME_ALLOC_H

#include <cstddef>
#include <cstdint>
#include "swift/Runtime/Config.h"
#include "swift/Runtime/FastEntryPoints.h"

// Bring in the definition of HeapObject 
#include "../../../stdlib/shims/HeapObject.h"

namespace swift {

struct Metadata;
struct HeapMetadata;
struct OpaqueValue;

/// Allocates a new heap object.  The returned memory is
/// uninitialized outside of the heap-object header.  The object
/// has an initial retain count of 1, and its metadata is set to
/// the given value.
///
/// At some point "soon after return", it will become an
/// invariant that metadata->getSize(returnValue) will equal
/// requiredSize.
///
/// Either aborts or throws a swift exception if the allocation fails.
///
/// \param requiredSize - the required size of the allocation,
///   including the header
/// \param requiredAlignmentMask - the required alignment of the allocation;
///   always one less than a power of 2 that's at least alignof(void*)
/// \return never null
///
/// POSSIBILITIES: The argument order is fair game.  It may be useful
/// to have a variant which guarantees zero-initialized memory.
extern "C" HeapObject *swift_allocObject(HeapMetadata const *metadata,
                                         size_t requiredSize,
                                         size_t requiredAlignmentMask);
  
/// The structure returned by swift_allocPOD and swift_allocBox.
struct BoxPair {
  /// The pointer to the heap object.
  HeapObject *heapObject;
  
  /// The pointer to the value inside the box.
  OpaqueValue *value;
  
  // FIXME: rdar://16257592 arm codegen does't call swift_allocBox correctly.
  // Structs are returned indirectly on these platforms, but we want to return
  // in registers, so cram the result into an unsigned long long.
  // Use an enum class with implicit conversions so we don't dirty C callers
  // too much.
#if __arm__ || __i386__
  enum class Return : unsigned long long {};
  
  operator Return() const {
    union {
      BoxPair value;
      Return mangled;
    } reinterpret = {*this};
    
    return reinterpret.mangled;
  }
  
  BoxPair() = default;
  BoxPair(HeapObject *h, OpaqueValue *v)
    : heapObject(h), value(v) {}
  
  /*implicit*/ BoxPair(Return r) {
    union {
      Return mangled;
      BoxPair value;
    } reinterpret = {r};
    
    *this = reinterpret.value;
  }
#else
  using Return = BoxPair;
#endif
};

/// Allocates a heap object with POD value semantics. The returned memory is
/// uninitialized outside of the heap object header. The object has an
/// initial retain count of 1, and its metadata is set to a predefined
/// POD heap metadata for which destruction is a no-op.
///
/// \param dataSize           The size of the data area for the allocation.
///                           Excludes the heap metadata header.
/// \param dataAlignmentMask  The alignment of the data area.
///
/// \returns a BoxPair in which the heapObject field points to the newly-created
///          HeapObject and the value field points to the data area inside the
///          allocation. The value pointer will have the alignment specified
///          by the dataAlignmentMask and point to dataSize bytes of memory.
extern "C" BoxPair::Return
swift_allocPOD(size_t dataSize, size_t dataAlignmentMask);

/// Deallocates a heap object known to have been allocated by swift_allocPOD and
/// to have no remaining owners.
extern "C" void swift_deallocPOD(HeapObject *obj);
  
/// Allocates a heap object that can contain a value of the given type.
/// Returns a Box structure containing a HeapObject* pointer to the
/// allocated object, and a pointer to the value inside the heap object.
/// The value pointer points to an uninitialized buffer of size and alignment
/// appropriate to store a value of the given type.
/// The heap object has an initial retain count of 1, and its metadata is set
/// such that destroying the heap object destroys the contained value.
extern "C" BoxPair::Return swift_allocBox(Metadata const *type);

// Allocate plain old memory, this is the generalized entry point
//
// The default API will wait for available memory and return zero filled.
//
// The "try" flag tells the runtime to not wait for memory
// The "raw" flag allocates uninitialized memory.
// When neither flag is needed, pass zero.
//
// If alignment is needed, then please round up to the desired alignment.
// For example, a 12 byte allocation with 8 byte alignment becomes 16.
#define SWIFT_TRYALLOC 0x0001
#define SWIFT_RAWALLOC 0x0002
extern "C" void *swift_slowAlloc(size_t bytes, uintptr_t flags);

// These exist as fast entry points for the above slow API.
//
// When the compiler knows that the bytes to be allocated are constant and the
// value is <= 4KB then the compiler precomputes an offset that the runtime uses
// to quickly allocate/free from a per-thread cache.
//
// The algorithm is like so:
//
// if (!__builtin_constant_p(bytes) || (bytes > 0x1000)) {
//   return swift_slowAlloc(bytes, 0);
// }
// if (bytes == 0) {
//   tinyIndex = 0;
// } else {
//   --bytes;
// #ifdef __LP64__
//   if      (bytes < 0x80)   { idx = (bytes >> 3);        }
//   else if (bytes < 0x100)  { idx = (bytes >> 4) + 0x8;  }
//   else if (bytes < 0x200)  { idx = (bytes >> 5) + 0x10; }
//   else if (bytes < 0x400)  { idx = (bytes >> 6) + 0x18; }
//   else if (bytes < 0x800)  { idx = (bytes >> 7) + 0x20; }
//   else if (bytes < 0x1000) { idx = (bytes >> 8) + 0x28; }
// #else
//   if      (bytes < 0x40)   { idx = (bytes >> 2);        }
//   else if (bytes < 0x80)   { idx = (bytes >> 3) + 0x8;  }
//   else if (bytes < 0x100)  { idx = (bytes >> 4) + 0x10; }
//   else if (bytes < 0x200)  { idx = (bytes >> 5) + 0x18; }
//   else if (bytes < 0x400)  { idx = (bytes >> 6) + 0x20; }
//   else if (bytes < 0x800)  { idx = (bytes >> 7) + 0x28; }
//   else if (bytes < 0x1000) { idx = (bytes >> 8) + 0x30; }
// #endif
//   else                     { __builtin_trap();          }
// }
// return swift_alloc(tinyIndex);
typedef unsigned long AllocIndex;

extern "C" void *swift_rawAlloc(AllocIndex idx);
extern "C" void *swift_tryRawAlloc(AllocIndex idx);

// If bytes is knowable but is large OR if bytes is not knowable,
// then use the slow entry point and pass zero:
extern "C" void swift_slowDealloc(void *ptr, size_t bytes);

// If the caller cannot promise to zero the object during destruction,
// then call these corresponding APIs:
extern "C" void swift_rawDealloc(void *ptr, AllocIndex idx);
extern "C" void swift_slowRawDealloc(void *ptr, size_t bytes);

/// Atomically increments the retain count of an object.
///
/// \param object - may be null, in which case this is a no-op
/// \return its argument value exactly
///
/// POSSIBILITIES: We may end up wanting a bunch of different variants:
///  - the general version which correctly handles null values, swift
///     objects, and ObjC objects
///    - a variant that assumes that its operand is a swift object
///      - a variant that can safely use non-atomic operations
///      - maybe a variant that can assume a non-null object
/// It may also prove worthwhile to have this use a custom CC
/// which preserves a larger set of registers.
extern "C" HeapObject *swift_retain(HeapObject *object);
extern "C" void swift_retain_noresult(HeapObject *object);

static inline HeapObject *_swift_retain(HeapObject *object) {
  if (object) {
    object->refCount += RC_INTERVAL;
  }
  return object;
}


/// Atomically decrements the retain count of an object.  If the
/// retain count reaches zero, the object is destroyed as follows:
///
///   size_t allocSize = object->metadata->destroy(object);
///   if (allocSize) swift_deallocObject(object, allocSize);
///
/// \param object - may be null, in which case this is a no-op
///
/// POSSIBILITIES: We may end up wanting a bunch of different variants:
///  - the general version which correctly handles null values, swift
///     objects, and ObjC objects
///    - a variant that assumes that its operand is a swift object
///      - a variant that can safely use non-atomic operations
///      - maybe a variant that can assume a non-null object
/// It's unlikely that a custom CC would be beneficial here.
extern "C" void swift_release(HeapObject *object);

/// Deallocate the given memory; it was returned by swift_allocObject
/// but is otherwise in an unknown state.
///
/// \param object - never null
/// \param allocatedSize - the allocated size of the object from the
///   program's perspective, i.e. the value
///
/// POSSIBILITIES: It may be useful to have a variant which
/// requires the object to have been fully zeroed from offsets
/// sizeof(SwiftHeapObject) to allocatedSize.
extern "C" void swift_deallocObject(HeapObject *object, size_t allocatedSize);

/// Deallocate the given memory allocated by swift_allocBox; it was returned
/// by swift_allocBox but is otherwise in an unknown state. The given Metadata
/// pointer must be the same metadata pointer that was passed to swift_allocBox
/// when the memory was allocated.
extern "C" void swift_deallocBox(HeapObject *object, Metadata const *type);
  
/// RAII object that wraps a Swift heap object and releases it upon
/// destruction.
class SwiftRAII {
  HeapObject *object;

public:
  SwiftRAII(HeapObject *obj, bool AlreadyRetained) : object(obj) {
    if (!AlreadyRetained)
      swift_retain(obj);
  }

  ~SwiftRAII() {
    if (object)
      swift_release(object);
  }

  SwiftRAII(const SwiftRAII &other) : object(swift_retain(*other)) {
    ;
  }
  SwiftRAII(SwiftRAII &&other) : object(*other) {
    other.object = nullptr;
  }
  SwiftRAII &operator=(const SwiftRAII &other) {
    if (object)
      swift_release(object);
    object = swift_retain(*other);
    return *this;
  }
  SwiftRAII &operator=(SwiftRAII &&other) {
    if (object)
      swift_release(object);
    object = *other;
    other.object = nullptr;
    return *this;
  }

  HeapObject *operator *() const { return object; }
};

/// Increment the weak retain count.
extern "C" void swift_weakRetain(HeapObject *value);

/// Decrement the weak retain count.
extern "C" void swift_weakRelease(HeapObject *value);

/// Increment the strong retain count of an object which may have been
/// deallocated.
extern "C" void swift_retainUnowned(HeapObject *value);

/// A weak reference value object.  This is ABI.
struct WeakReference {
  HeapObject *Value;
};

/// Initialize a weak reference.
///
/// \param ref - never null
/// \param value - can be null
extern "C" void swift_weakInit(WeakReference *ref, HeapObject *value);

/// Assign a new value to a weak reference.
///
/// \param ref - never null
/// \param value - can be null
extern "C" void swift_weakAssign(WeakReference *ref, HeapObject *value);

/// Load a value from a weak reference.  If the current value is a
/// non-null object that has begun deallocation, returns null;
/// otherwise, retains the object before returning.
///
/// \param ref - never null
/// \return can be null
extern "C" HeapObject *swift_weakLoadStrong(WeakReference *ref);

/// Load a value from a weak reference as if by swift_weakLoadStrong,
/// but leaving the reference in an uninitialized state.
///
/// \param ref - never null
/// \return can be null
extern "C" HeapObject *swift_weakTakeStrong(WeakReference *ref);

/// Destroy a weak reference.
///
/// \param ref - never null, but can refer to a null object
extern "C" void swift_weakDestroy(WeakReference *ref);

/// Copy initialize a weak reference.
///
/// \param dest - never null, but can refer to a null object
/// \param src - never null, but can refer to a null object
extern "C" void swift_weakCopyInit(WeakReference *dest, WeakReference *src);

/// Take initialize a weak reference.
///
/// \param dest - never null, but can refer to a null object
/// \param src - never null, but can refer to a null object
extern "C" void swift_weakTakeInit(WeakReference *dest, WeakReference *src);

/// Copy assign a weak reference.
///
/// \param dest - never null, but can refer to a null object
/// \param src - never null, but can refer to a null object
extern "C" void swift_weakCopyAssign(WeakReference *dest, WeakReference *src);

/// Take assign a weak reference.
///
/// \param dest - never null, but can refer to a null object
/// \param src - never null, but can refer to a null object
extern "C" void swift_weakTakeAssign(WeakReference *dest, WeakReference *src);

#if SWIFT_OBJC_INTEROP

/// Increment the strong retain count of an object which might not be a native
/// Swift object.
extern "C" void *swift_unknownRetain(void *value);
  
/// Decrement the strong retain count of an object which might not be a native
/// Swift object.
extern "C" void swift_unknownRelease(void *value);
  
/// Increment the strong retain count of an object which may have been
/// deallocated and which might not be a native Swift object.
extern "C" void swift_unknownRetainUnowned(void *value);

/// Increment the weak-reference count of an object that might not be
/// a native Swift object.
extern "C" void swift_unknownWeakRetain(void *value);

/// Decrement the weak-reference count of an object that might not be
/// a native Swift object.
extern "C" void swift_unknownWeakRelease(void *value);

/// Initialize a weak reference.
///
/// \param ref - never null
/// \param value - not necessarily a native Swift object; can be null
extern "C" void swift_unknownWeakInit(WeakReference *ref, void *value);

/// Assign a new value to a weak reference.
///
/// \param ref - never null
/// \param value - not necessarily a native Swift object; can be null
extern "C" void swift_unknownWeakAssign(WeakReference *ref, void *value);

/// Load a value from a weak reference, much like swift_weakLoadStrong
/// but without requiring the variable to refer to a native Swift object.
///
/// \param ref - never null
/// \return can be null
extern "C" void *swift_unknownWeakLoadStrong(WeakReference *ref);

/// Load a value from a weak reference as if by
/// swift_unknownWeakLoadStrong, but leaving the reference in an
/// uninitialized state.
///
/// \param ref - never null
/// \return can be null
extern "C" void *swift_unknownWeakTakeStrong(WeakReference *ref);

/// Destroy a weak reference variable that might not refer to a native
/// Swift object.
extern "C" void swift_unknownWeakDestroy(WeakReference *object);

/// Copy-initialize a weak reference variable from one that might not
/// refer to a native Swift object.
extern "C" void swift_unknownWeakCopyInit(WeakReference *dest, WeakReference *src);

/// Take-initialize a weak reference variable from one that might not
/// refer to a native Swift object.
extern "C" void swift_unknownWeakTakeInit(WeakReference *dest, WeakReference *src);

/// Copy-assign a weak reference variable from another when either
/// or both variables might not refer to a native Swift object.
extern "C" void swift_unknownWeakCopyAssign(WeakReference *dest, WeakReference *src);

/// Take-assign a weak reference variable from another when either
/// or both variables might not refer to a native Swift object.
extern "C" void swift_unknownWeakTakeAssign(WeakReference *dest, WeakReference *src);

#endif

} // end namespace swift

#endif /* SWIFT_RUNTIME_ALLOC_H */
