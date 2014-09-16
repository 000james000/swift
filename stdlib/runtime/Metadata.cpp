//===--- Metadata.cpp - Swift Language ABI Metdata Support ----------------===//
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
// Implementations of the metadata ABI functions.
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/MathExtras.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/Range.h"
#include "swift/Runtime/HeapObject.h"
#include "swift/Runtime/Metadata.h"
#include "swift/Strings.h"
#include <algorithm>
#include <new>
#include <mutex>
#include <cctype>
#include <dispatch/dispatch.h>
#include <pthread.h>
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Hashing.h"
#include "ExistentialMetadataImpl.h"
#include "Debug.h"

#ifndef SWIFT_DEBUG_RUNTIME
#define SWIFT_DEBUG_RUNTIME 0
#endif

using namespace swift;
using namespace metadataimpl;

static void *permanentAlloc(size_t size) {
  return malloc(size);
}

namespace {
  template <class Entry> class MetadataCache;

  template <class Impl>
  struct CacheEntryHeader {
    /// LLDB walks this list.
    const Impl *Next;
  };

  /// A CRTP class for defining entries in a metadata cache.
  template <class Impl, class Header = CacheEntryHeader<Impl> >
  class alignas(void*) CacheEntry : public Header {

    CacheEntry(const CacheEntry &other) = delete;
    void operator=(const CacheEntry &other) = delete;

    Impl *asImpl() { return static_cast<Impl*>(this); }
    const Impl *asImpl() const { return static_cast<const Impl*>(this); }

  protected:
    CacheEntry() = default;

  public:
    static Impl *allocate(const void * const *arguments,
                          size_t numArguments, size_t payloadSize) {
      void *buffer = permanentAlloc(sizeof(Impl)  +
                                    numArguments * sizeof(void*) +
                                    payloadSize);
      void *resultPtr = (char*)buffer + numArguments * sizeof(void*);
      auto result = new (resultPtr) Impl(numArguments);

      // Copy the arguments into the right place for the key.
      memcpy(buffer, arguments,
             numArguments * sizeof(void*));

      return result;
    }

    void **getArgumentsBuffer() {
      return reinterpret_cast<void**>(this) - asImpl()->getNumArguments();
    }
    void * const *getArgumentsBuffer() const {
      return reinterpret_cast<void * const *>(this)
               - asImpl()->getNumArguments();
    }

    template <class T> T *getData() {
      return reinterpret_cast<T *>(asImpl() + 1);
    }
    template <class T> const T *getData() const {
      return const_cast<CacheEntry*>(this)->getData<T>();
    }
    
    static const Impl *fromArgumentsBuffer(const void * const *argsBuffer,
                                           unsigned numArguments) {
      return reinterpret_cast<const Impl *>(argsBuffer + numArguments);
    }
  };
  
  // A wrapper around a pointer to a metadata cache entry that provides
  // DenseMap semantics that compare values in the key vector for the metadata
  // instance.
  //
  // This is stored as a pointer to the arguments buffer, so that we can save
  // an offset while looking for the matching argument given a key.
  template<class Entry>
  class EntryRef {
    const void * const *args;
    unsigned length;
    
    EntryRef(const void * const *args, unsigned length)
    : args(args), length(length)
    {}

    friend struct llvm::DenseMapInfo<EntryRef>;
  public:
    static EntryRef forEntry(const Entry *e, unsigned numArguments) {
      return EntryRef(e->getArgumentsBuffer(), numArguments);
    }
    
    static EntryRef forArguments(const void * const *args,
                                 unsigned numArguments) {
      return EntryRef(args, numArguments);
    }
    
    const Entry *getEntry() const {
      return Entry::fromArgumentsBuffer(args, length);
    }
    
    const void * const *begin() const { return args; }
    const void * const *end() const { return args + length; }
    unsigned size() const { return length; }
  };
}

namespace llvm {
  template<class Entry>
  struct DenseMapInfo<EntryRef<Entry>> {
    static inline EntryRef<Entry> getEmptyKey() {
      // {nullptr, 0} is a legitimate "no arguments" representation.
      return {(const void * const *)UINTPTR_MAX, 1};
    }
    
    static inline EntryRef<Entry> getTombstoneKey() {
      return {(const void * const *)UINTPTR_MAX, 2};
    }
    
    static inline unsigned getHashValue(EntryRef<Entry> val) {
      llvm::hash_code hash
        = llvm::hash_combine_range(val.begin(), val.end());
      return (unsigned)hash;
    }
    
    static inline bool isEqual(EntryRef<Entry> a, EntryRef<Entry> b) {
      unsigned asize = a.size(), bsize = b.size();
      if (asize != bsize)
        return false;
      auto abegin = a.begin(), bbegin = b.begin();
      if (abegin == (const void * const *)UINTPTR_MAX
          || bbegin == (const void * const *)UINTPTR_MAX)
        return abegin == bbegin;
      for (unsigned i = 0; i < asize; ++i) {
        if (abegin[i] != bbegin[i])
          return false;
      }
      return true;
    }
  };
}

namespace {
  struct MetadataCacheLock {
    std::mutex Mutex;
    std::condition_variable Queue;
  };

  /// The implementation of a metadata cache.  Note that all-zero must
  /// be a valid state for the cache.
  template <class Entry> class MetadataCache {
    /// Thread safety
    MetadataCacheLock *Lock;

    /// The head of a linked list connecting all the metadata cache entries.
    /// TODO: Remove this when LLDB is able to understand the final data
    /// structure for the metadata cache.
    const Entry *Head;

    enum class EntryState : uint8_t { Complete, Building, BuildingWithWaiters };
    
    /// The lookup table for cached entries.
    ///
    /// The EntryRef may be to temporary memory lacking a full backing
    /// Entry unless the value is Complete.  However, if there is an
    /// entry with a non-Complete value, there will eventually be a
    /// notification to the lock's queue.
    ///
    /// TODO: Consider a more tuned hashtable implementation.
    llvm::DenseMap<EntryRef<Entry>, EntryState> Entries;

  public:
    MetadataCache() : Lock(new MetadataCacheLock()) {
    }
    ~MetadataCache() { delete Lock; }

    /// Caches are not copyable.
    MetadataCache(const MetadataCache &other) = delete;
    MetadataCache &operator=(const MetadataCache &other) = delete;

    /// Try to find an existing entry in this cache.  If this returns
    /// null, it is the caller's responsibility to eventually call add.
    const Entry *find(const void * const *arguments, size_t numArguments) {
      std::unique_lock<std::mutex> lockGuard(Lock->Mutex);

      auto key = EntryRef<Entry>::forArguments(arguments, numArguments);

      // Try to insert 'false' as the map value.  Note that this is
      // inserting
      auto found = Entries.insert({key, EntryState::Building});

      // If that succeeded, we're in charge of creating the entry now.
      if (found.second) return nullptr;

      // If it failed, there's an existing entry.
      auto it = found.first;

      // Wait until the entry's state goes to Complete.
      while (it->second != EntryState::Complete) {
        // Make sure the adder knows to notify us.
        it->second = EntryState::BuildingWithWaiters;

        // Wait.
        Lock->Queue.wait(lockGuard);

        // Don't trust the existing iterator.
        it = Entries.find(key);
        assert(it != Entries.end());

        // We need to check again because (1) wait() is allowed to
        // return spuriously and (2) we share one condition variable
        // for all the entries.
      }

      return it->first.getEntry();
    }

    /// Add the given entry to the cache, taking responsibility for
    /// it.  Returns the entry that should be used, which might not be
    /// the same as the argument if we lost a race to instantiate it.
    /// Regardless, the argument should be considered potentially
    /// invalid after this call.
    const Entry *add(Entry *entry) {
      // Grab the lock.
      std::unique_lock<std::mutex> lockGuard(Lock->Mutex);

      // Maintain the linked list.
      // TODO: Remove this when LLDB is able to understand the final data
      // structure for the metadata cache.
      entry->Next = Head;
      Head = entry;

      // Find the existing entry, which should always exist.
      auto key = EntryRef<Entry>::forEntry(entry, entry->getNumArguments());
      auto it = Entries.find(key);
      assert(it != Entries.end());

      // The existing key is a reference to the (probably stack-based)
      // arguments array, so overwrite it.  Maps don't normally allow
      // their keys to be overwritten, and doing so isn't officially
      // allowed, but supposedly it is unofficially guaranteed to
      // work, at least with the standard containers.
      const_cast<EntryRef<Entry>&>(it->first) = key;

      assert(it->second != EntryState::Complete);
      bool shouldNotify = (it->second == EntryState::BuildingWithWaiters);
      it->second = EntryState::Complete;

      // Drop the lock before notifying the queue.
      lockGuard.unlock();

      // Notify anybody who was waiting for us (or really, anybody who
      // was waiting on the queue at all).
      if (shouldNotify)
        Lock->Queue.notify_all();

      return entry;
    }
  };

  /// A template for lazily-constructed, zero-initialized global objects.
  template <class T> class Lazy {
    T Value;
    dispatch_once_t OnceToken;

  public:
    T &get() {
      dispatch_once_f(&OnceToken, this, lazyInitCallback);
      return Value;
    }

  private:
    static void lazyInitCallback(void *argument) {
      auto self = reinterpret_cast<Lazy*>(argument);
      ::new (&self->Value) T();
    }
  };
}

namespace {
  struct GenericCacheEntry;

  // The cache entries in a generic cache are laid out like this:
  struct GenericCacheEntryHeader : CacheEntry<GenericCacheEntry> {
    const Metadata *Value;
    size_t NumArguments;
  };

  struct GenericCacheEntry
      : CacheEntry<GenericCacheEntry, GenericCacheEntryHeader> {
    GenericCacheEntry(unsigned numArguments) {
      NumArguments = numArguments;
    }

    size_t getNumArguments() const { return NumArguments; }

    static GenericCacheEntry *getFromMetadata(GenericMetadata *pattern,
                                              Metadata *metadata) {
      char *bytes = (char*) metadata;
      if (auto classType = dyn_cast<ClassMetadata>(metadata)) {
        assert(classType->isTypeMetadata());
        bytes -= classType->getClassAddressPoint();
      } else {
        bytes -= pattern->AddressPoint;
      }
      bytes -= sizeof(GenericCacheEntry);
      return reinterpret_cast<GenericCacheEntry*>(bytes);
    }
  };
}

using GenericMetadataCache = MetadataCache<GenericCacheEntry>;
using LazyGenericMetadataCache = Lazy<GenericMetadataCache>;

/// Fetch the metadata cache for a generic metadata structure.
static GenericMetadataCache &getCache(GenericMetadata *metadata) {
  // Keep this assert even if you change the representation above.
  static_assert(sizeof(LazyGenericMetadataCache) <=
                sizeof(GenericMetadata::PrivateData),
                "metadata cache is larger than the allowed space");

  auto lazyCache =
    reinterpret_cast<LazyGenericMetadataCache*>(metadata->PrivateData);
  return lazyCache->get();
}

ClassMetadata *
swift::swift_allocateGenericClassMetadata(GenericMetadata *pattern,
                                          const void *arguments,
                                          ClassMetadata *superclass) {
  void * const *argumentsAsArray = reinterpret_cast<void * const *>(arguments);
  size_t numGenericArguments = pattern->NumKeyArguments;

  // Right now, we only worry about there being a difference in prefix matter.
  size_t metadataSize = pattern->MetadataSize;
  size_t prefixSize = pattern->AddressPoint;
  size_t extraPrefixSize = 0;
  if (superclass && superclass->isTypeMetadata()) {
    if (superclass->getClassAddressPoint() > prefixSize) {
      extraPrefixSize = (superclass->getClassAddressPoint() - prefixSize);
      prefixSize += extraPrefixSize;
      metadataSize += extraPrefixSize;
    }
  }
  assert(metadataSize == pattern->MetadataSize + extraPrefixSize);
  assert(prefixSize == pattern->AddressPoint + extraPrefixSize);

  char *bytes = GenericCacheEntry::allocate(argumentsAsArray,
                                            numGenericArguments,
                                            metadataSize)->getData<char>();

  // Copy any extra prefix bytes in from the superclass.
  if (extraPrefixSize) {
    memcpy(bytes, (const char*) superclass - prefixSize, extraPrefixSize);
    bytes += extraPrefixSize;
  }

  // Copy in the metadata template.
  memcpy(bytes, pattern->getMetadataTemplate(), pattern->MetadataSize);

  // Okay, move to the address point.
  bytes += pattern->AddressPoint;
  ClassMetadata *metadata = reinterpret_cast<ClassMetadata*>(bytes);
  assert(metadata->isTypeMetadata());

  // Overwrite the superclass field.
  metadata->SuperClass = superclass;

  // Adjust the class object extents.
  if (extraPrefixSize) {
    metadata->setClassSize(metadata->getClassSize() + extraPrefixSize);
    metadata->setClassAddressPoint(prefixSize);
  }
  assert(metadata->getClassAddressPoint() == prefixSize);

  return metadata;
}

Metadata *
swift::swift_allocateGenericValueMetadata(GenericMetadata *pattern,
                                          const void *arguments) {
  void * const *argumentsAsArray = reinterpret_cast<void * const *>(arguments);
  size_t numGenericArguments = pattern->NumKeyArguments;

  char *bytes =
    GenericCacheEntry::allocate(argumentsAsArray, numGenericArguments,
                                pattern->MetadataSize)->getData<char>();

  // Copy in the metadata template.
  memcpy(bytes, pattern->getMetadataTemplate(), pattern->MetadataSize);

  // Okay, move to the address point.
  bytes += pattern->AddressPoint;
  Metadata *metadata = reinterpret_cast<Metadata*>(bytes);
  return metadata;
}

static const Metadata *
instantiateGenericMetadata(GenericMetadata *pattern, const void *arguments) {
  // Create the metadata.
  Metadata *metadata = pattern->CreateFunction(pattern, arguments);

  // The metadata is now valid.  Add to the cache list.
  auto entry = GenericCacheEntry::getFromMetadata(pattern, metadata);
  entry->Value = metadata;

  auto canonFullMetadata = getCache(pattern).add(entry)->Value;
  return canonFullMetadata;
}

/// The primary entrypoint.
const Metadata *
swift::swift_getGenericMetadata(GenericMetadata *pattern,
                                const void *arguments) {
  auto genericArgs = (const void * const *) arguments;
  size_t numGenericArgs = pattern->NumKeyArguments;

#if SWIFT_DEBUG_RUNTIME
  printf("swift_getGenericMetadata(%p):\n", pattern);
  for (unsigned i = 0; i != numGenericArgs; ++i) {
    printf("  %p\n", genericArgs[i]);
  }
#endif

  if (auto entry = getCache(pattern).find(genericArgs, numGenericArgs)) {
#if SWIFT_DEBUG_RUNTIME
    printf("found in cache!\n");
#endif
    auto metadata = entry->Value;
#if SWIFT_DEBUG_RUNTIME
    printf(" -> %p\n", metadata);
#endif
    return metadata;
  }


  // Otherwise, instantiate a new one.
#if SWIFT_DEBUG_RUNTIME
  printf("not found in cache!\n");
#endif
  auto metadata = instantiateGenericMetadata(pattern, arguments);
#if SWIFT_DEBUG_RUNTIME
  printf(" -> %p\n", metadata);
#endif

  return metadata;
}

/// Fast entry points.
const Metadata *
swift::swift_getGenericMetadata1(GenericMetadata *pattern, const void*argument){
  return swift_getGenericMetadata(pattern, &argument);
}

const Metadata *
swift::swift_getGenericMetadata2(GenericMetadata *pattern,
                                 const void *arg0, const void *arg1) {
  const void *args[] = {arg0, arg1};
  return swift_getGenericMetadata(pattern, args);
}

const Metadata *
swift::swift_getGenericMetadata3(GenericMetadata *pattern,
                                 const void *arg0,
                                 const void *arg1,
                                 const void *arg2) {
  const void *args[] = {arg0, arg1, arg2};
  return swift_getGenericMetadata(pattern, args);
}

const Metadata *
swift::swift_getGenericMetadata4(GenericMetadata *pattern,
                                 const void *arg0,
                                 const void *arg1,
                                 const void *arg2,
                                 const void *arg3) {
  const void *args[] = {arg0, arg1, arg2, arg3};
  return swift_getGenericMetadata(pattern, args);
}

namespace {
  class ObjCClassCacheEntry : public CacheEntry<ObjCClassCacheEntry> {
    FullMetadata<ObjCClassWrapperMetadata> Metadata;

  public:
    ObjCClassCacheEntry(size_t numArguments) {}

    static constexpr size_t getNumArguments() {
      return 1;
    }

    FullMetadata<ObjCClassWrapperMetadata> *getData() {
      return &Metadata;
    }
    const FullMetadata<ObjCClassWrapperMetadata> *getData() const {
      return &Metadata;
    }
  };
}

/// The uniquing structure for ObjC class-wrapper metadata.
static MetadataCache<ObjCClassCacheEntry> ObjCClassWrappers;

const Metadata *
swift::swift_getObjCClassMetadata(const ClassMetadata *theClass) {
  // If the class pointer is valid as metadata, no translation is required.
  if (theClass->isTypeMetadata()) {
    return theClass;
  }

  // Look for an existing entry.
  const size_t numGenericArgs = 1;
  const void *args[] = { theClass };
  if (auto entry = ObjCClassWrappers.find(args, numGenericArgs)) {
    return entry->getData();
  }

  auto entry = ObjCClassCacheEntry::allocate(args, numGenericArgs, 0);

  auto metadata = entry->getData();
  metadata->setKind(MetadataKind::ObjCClassWrapper);
  metadata->ValueWitnesses = &_TWVBO;
  metadata->Class = theClass;

  return ObjCClassWrappers.add(entry)->getData();
}

namespace {
  class FunctionCacheEntry : public CacheEntry<FunctionCacheEntry> {
    FullMetadata<FunctionTypeMetadata> Metadata;

  public:
    FunctionCacheEntry(size_t numArguments) {}

    static constexpr size_t getNumArguments() {
      return 2;
    }    

    FullMetadata<FunctionTypeMetadata> *getData() {
      return &Metadata;
    }
    const FullMetadata<FunctionTypeMetadata> *getData() const {
      return &Metadata;
    }
  };
}

/// The uniquing structure for function type metadata.
namespace {
  MetadataCache<FunctionCacheEntry> FunctionTypes;
  MetadataCache<FunctionCacheEntry> BlockTypes;
  
  const FunctionTypeMetadata *
  _getFunctionTypeMetadata(const Metadata *argMetadata,
                           const Metadata *resultMetadata,
                           MetadataKind Kind,
                           MetadataCache<FunctionCacheEntry> &Cache,
                           const ValueWitnessTable &ValueWitnesses) {
    const size_t numGenericArgs = 2;

    typedef FullMetadata<FunctionTypeMetadata> FullFunctionTypeMetadata;

    const void *args[] = { argMetadata, resultMetadata };
    if (auto entry = Cache.find(args, numGenericArgs)) {
      return entry->getData();
    }

    auto entry = FunctionCacheEntry::allocate(args, numGenericArgs, 0);

    auto metadata = entry->getData();
    metadata->setKind(Kind);
    metadata->ValueWitnesses = &ValueWitnesses;
    metadata->ArgumentType = argMetadata;
    metadata->ResultType = resultMetadata;

    return Cache.add(entry)->getData();
  }
}

const FunctionTypeMetadata *
swift::swift_getFunctionTypeMetadata(const Metadata *argMetadata,
                                     const Metadata *resultMetadata) {
  return _getFunctionTypeMetadata(argMetadata, resultMetadata,
                                  MetadataKind::Function,
                                  FunctionTypes,
                                  _TWVFT_T_);
}

const FunctionTypeMetadata *
swift::swift_getBlockTypeMetadata(const Metadata *argMetadata,
                                  const Metadata *resultMetadata) {
  return _getFunctionTypeMetadata(argMetadata, resultMetadata,
                                  MetadataKind::Block,
                                  BlockTypes,
                                  _TWVBO);
}

/*** Tuples ****************************************************************/

namespace {
  class TupleCacheEntry;
  struct TupleCacheEntryHeader : CacheEntryHeader<TupleCacheEntry> {
    size_t NumArguments;
  };
  class TupleCacheEntry
    : public CacheEntry<TupleCacheEntry, TupleCacheEntryHeader> {
  public:
    // NOTE: if you change the layout of this type, you'll also need
    // to update tuple_getValueWitnesses().
    ExtraInhabitantsValueWitnessTable Witnesses;
    FullMetadata<TupleTypeMetadata> Metadata;

    TupleCacheEntry(size_t numArguments) {
      NumArguments = numArguments;
    }
    
    size_t getNumArguments() const {
      return Metadata.NumElements;
    }

    FullMetadata<TupleTypeMetadata> *getData() {
      return &Metadata;
    }
    const FullMetadata<TupleTypeMetadata> *getData() const {
      return &Metadata;
    }
  };
}

/// The uniquing structure for tuple type metadata.
static MetadataCache<TupleCacheEntry> TupleTypes;

/// Given a metatype pointer, produce the value-witness table for it.
/// This is equivalent to metatype->ValueWitnesses but more efficient.
static const ValueWitnessTable *tuple_getValueWitnesses(const Metadata *metatype) {
  return ((const ExtraInhabitantsValueWitnessTable*) asFullMetadata(metatype)) - 1;
}

/// Generic tuple value witness for 'projectBuffer'.
template <bool IsPOD, bool IsInline>
static OpaqueValue *tuple_projectBuffer(ValueBuffer *buffer,
                                        const Metadata *metatype) {
  assert(IsPOD == tuple_getValueWitnesses(metatype)->isPOD());
  assert(IsInline == tuple_getValueWitnesses(metatype)->isValueInline());

  if (IsInline)
    return reinterpret_cast<OpaqueValue*>(buffer);
  else
    return *reinterpret_cast<OpaqueValue**>(buffer);
}

/// Generic tuple value witness for 'allocateBuffer'
template <bool IsPOD, bool IsInline>
static OpaqueValue *tuple_allocateBuffer(ValueBuffer *buffer,
                                         const Metadata *metatype) {
  assert(IsPOD == tuple_getValueWitnesses(metatype)->isPOD());
  assert(IsInline == tuple_getValueWitnesses(metatype)->isValueInline());

  if (IsInline)
    return reinterpret_cast<OpaqueValue*>(buffer);

  auto wtable = tuple_getValueWitnesses(metatype);
  auto value = (OpaqueValue*) swift_slowAlloc(wtable->size,
                                              wtable->getAlignmentMask());

  *reinterpret_cast<OpaqueValue**>(buffer) = value;
  return value;
}

/// Generic tuple value witness for 'deallocateBuffer'.
template <bool IsPOD, bool IsInline>
static void tuple_deallocateBuffer(ValueBuffer *buffer,
                                   const Metadata *metatype) {
  assert(IsPOD == tuple_getValueWitnesses(metatype)->isPOD());
  assert(IsInline == tuple_getValueWitnesses(metatype)->isValueInline());

  if (IsInline)
    return;

  auto wtable = tuple_getValueWitnesses(metatype);
  auto value = *reinterpret_cast<OpaqueValue**>(buffer);
  swift_slowDealloc(value, wtable->size, wtable->getAlignmentMask());
}

/// Generic tuple value witness for 'destroy'.
template <bool IsPOD, bool IsInline>
static void tuple_destroy(OpaqueValue *tuple, const Metadata *_metadata) {
  auto &metadata = *(const TupleTypeMetadata*) _metadata;
  assert(IsPOD == tuple_getValueWitnesses(&metadata)->isPOD());
  assert(IsInline == tuple_getValueWitnesses(&metadata)->isValueInline());

  if (IsPOD) return;

  for (size_t i = 0, e = metadata.NumElements; i != e; ++i) {
    auto &eltInfo = metadata.getElements()[i];
    OpaqueValue *elt = eltInfo.findIn(tuple);
    auto eltWitnesses = eltInfo.Type->getValueWitnesses();
    eltWitnesses->destroy(elt, eltInfo.Type);
  }
}

/// Generic tuple value witness for 'destroyArray'.
template <bool IsPOD, bool IsInline>
static void tuple_destroyArray(OpaqueValue *array, size_t n,
                               const Metadata *_metadata) {
  auto &metadata = *(const TupleTypeMetadata*) _metadata;
  assert(IsPOD == tuple_getValueWitnesses(&metadata)->isPOD());
  assert(IsInline == tuple_getValueWitnesses(&metadata)->isValueInline());
  
  if (IsPOD) return;
  
  size_t stride = tuple_getValueWitnesses(&metadata)->stride;
  char *bytes = (char*)array;
  
  while (n--) {
    tuple_destroy<IsPOD, IsInline>((OpaqueValue*)bytes, _metadata);
    bytes += stride;
  }
}

/// Generic tuple value witness for 'destroyBuffer'.
template <bool IsPOD, bool IsInline>
static void tuple_destroyBuffer(ValueBuffer *buffer, const Metadata *metatype) {
  assert(IsPOD == tuple_getValueWitnesses(metatype)->isPOD());
  assert(IsInline == tuple_getValueWitnesses(metatype)->isValueInline());

  auto tuple = tuple_projectBuffer<IsPOD, IsInline>(buffer, metatype);
  tuple_destroy<IsPOD, IsInline>(tuple, metatype);
  tuple_deallocateBuffer<IsPOD, IsInline>(buffer, metatype);
}

// The operation doesn't have to be initializeWithCopy, but they all
// have basically the same type.
typedef value_witness_types::initializeWithCopy *
  ValueWitnessTable::*forEachOperation;

/// Perform an operation for each field of two tuples.
static OpaqueValue *tuple_forEachField(OpaqueValue *destTuple,
                                       OpaqueValue *srcTuple,
                                       const Metadata *_metatype,
                                       forEachOperation member) {
  auto &metatype = *(const TupleTypeMetadata*) _metatype;
  for (size_t i = 0, e = metatype.NumElements; i != e; ++i) {
    auto &eltInfo = metatype.getElements()[i];
    auto eltValueWitnesses = eltInfo.Type->getValueWitnesses();

    OpaqueValue *destElt = eltInfo.findIn(destTuple);
    OpaqueValue *srcElt = eltInfo.findIn(srcTuple);
    (eltValueWitnesses->*member)(destElt, srcElt, eltInfo.Type);
  }

  return destTuple;
}

/// Perform a naive memcpy of src into dest.
static OpaqueValue *tuple_memcpy(OpaqueValue *dest,
                                 OpaqueValue *src,
                                 const Metadata *metatype) {
  assert(metatype->getValueWitnesses()->isPOD());
  return (OpaqueValue*)
    memcpy(dest, src, metatype->getValueWitnesses()->getSize());
}
/// Perform a naive memcpy of n tuples from src into dest.
static OpaqueValue *tuple_memcpy_array(OpaqueValue *dest,
                                       OpaqueValue *src,
                                       size_t n,
                                       const Metadata *metatype) {
  assert(metatype->getValueWitnesses()->isPOD());
  return (OpaqueValue*)
    memcpy(dest, src, metatype->getValueWitnesses()->stride * n);
}
/// Perform a naive memmove of n tuples from src into dest.
static OpaqueValue *tuple_memmove_array(OpaqueValue *dest,
                                        OpaqueValue *src,
                                        size_t n,
                                        const Metadata *metatype) {
  assert(metatype->getValueWitnesses()->isPOD());
  return (OpaqueValue*)
    memmove(dest, src, metatype->getValueWitnesses()->stride * n);
}

/// Generic tuple value witness for 'initializeWithCopy'.
template <bool IsPOD, bool IsInline>
static OpaqueValue *tuple_initializeWithCopy(OpaqueValue *dest,
                                             OpaqueValue *src,
                                             const Metadata *metatype) {
  assert(IsPOD == tuple_getValueWitnesses(metatype)->isPOD());
  assert(IsInline == tuple_getValueWitnesses(metatype)->isValueInline());

  if (IsPOD) return tuple_memcpy(dest, src, metatype);
  return tuple_forEachField(dest, src, metatype,
                            &ValueWitnessTable::initializeWithCopy);
}

/// Generic tuple value witness for 'initializeArrayWithCopy'.
template <bool IsPOD, bool IsInline>
static OpaqueValue *tuple_initializeArrayWithCopy(OpaqueValue *dest,
                                                  OpaqueValue *src,
                                                  size_t n,
                                                  const Metadata *metatype) {
  assert(IsPOD == tuple_getValueWitnesses(metatype)->isPOD());
  assert(IsInline == tuple_getValueWitnesses(metatype)->isValueInline());

  if (IsPOD) return tuple_memcpy_array(dest, src, n, metatype);
  
  char *destBytes = (char*)dest;
  char *srcBytes = (char*)src;
  size_t stride = tuple_getValueWitnesses(metatype)->stride;
  
  while (n--) {
    tuple_initializeWithCopy<IsPOD, IsInline>((OpaqueValue*)destBytes,
                                              (OpaqueValue*)srcBytes,
                                              metatype);
    destBytes += stride; srcBytes += stride;
  }
  
  return dest;
}

/// Generic tuple value witness for 'initializeWithTake'.
template <bool IsPOD, bool IsInline>
static OpaqueValue *tuple_initializeWithTake(OpaqueValue *dest,
                                             OpaqueValue *src,
                                             const Metadata *metatype) {
  assert(IsPOD == tuple_getValueWitnesses(metatype)->isPOD());
  assert(IsInline == tuple_getValueWitnesses(metatype)->isValueInline());

  if (IsPOD) return tuple_memcpy(dest, src, metatype);
  return tuple_forEachField(dest, src, metatype,
                            &ValueWitnessTable::initializeWithTake);
}

/// Generic tuple value witness for 'initializeArrayWithTakeFrontToBack'.
template <bool IsPOD, bool IsInline>
static OpaqueValue *tuple_initializeArrayWithTakeFrontToBack(
                                             OpaqueValue *dest,
                                             OpaqueValue *src,
                                             size_t n,
                                             const Metadata *metatype) {
  assert(IsPOD == tuple_getValueWitnesses(metatype)->isPOD());
  assert(IsInline == tuple_getValueWitnesses(metatype)->isValueInline());

  if (IsPOD) return tuple_memmove_array(dest, src, n, metatype);
  
  char *destBytes = (char*)dest;
  char *srcBytes = (char*)src;
  size_t stride = tuple_getValueWitnesses(metatype)->stride;
  
  while (n--) {
    tuple_initializeWithTake<IsPOD, IsInline>((OpaqueValue*)destBytes,
                                              (OpaqueValue*)srcBytes,
                                              metatype);
    destBytes += stride; srcBytes += stride;
  }
  
  return dest;
}

/// Generic tuple value witness for 'initializeArrayWithTakeBackToFront'.
template <bool IsPOD, bool IsInline>
static OpaqueValue *tuple_initializeArrayWithTakeBackToFront(
                                             OpaqueValue *dest,
                                             OpaqueValue *src,
                                             size_t n,
                                             const Metadata *metatype) {
  assert(IsPOD == tuple_getValueWitnesses(metatype)->isPOD());
  assert(IsInline == tuple_getValueWitnesses(metatype)->isValueInline());

  if (IsPOD) return tuple_memmove_array(dest, src, n, metatype);
  
  size_t stride = tuple_getValueWitnesses(metatype)->stride;
  char *destBytes = (char*)dest + n * stride;
  char *srcBytes = (char*)src + n * stride;
  
  while (n--) {
    destBytes -= stride; srcBytes -= stride;
    tuple_initializeWithTake<IsPOD, IsInline>((OpaqueValue*)destBytes,
                                              (OpaqueValue*)srcBytes,
                                              metatype);
  }
  
  return dest;
}

/// Generic tuple value witness for 'assignWithCopy'.
template <bool IsPOD, bool IsInline>
static OpaqueValue *tuple_assignWithCopy(OpaqueValue *dest,
                                         OpaqueValue *src,
                                         const Metadata *metatype) {
  assert(IsPOD == tuple_getValueWitnesses(metatype)->isPOD());
  assert(IsInline == tuple_getValueWitnesses(metatype)->isValueInline());

  if (IsPOD) return tuple_memcpy(dest, src, metatype);
  return tuple_forEachField(dest, src, metatype,
                            &ValueWitnessTable::assignWithCopy);
}

/// Generic tuple value witness for 'assignWithTake'.
template <bool IsPOD, bool IsInline>
static OpaqueValue *tuple_assignWithTake(OpaqueValue *dest,
                                         OpaqueValue *src,
                                         const Metadata *metatype) {
  if (IsPOD) return tuple_memcpy(dest, src, metatype);
  return tuple_forEachField(dest, src, metatype,
                            &ValueWitnessTable::assignWithTake);
}

/// Generic tuple value witness for 'initializeBufferWithCopy'.
template <bool IsPOD, bool IsInline>
static OpaqueValue *tuple_initializeBufferWithCopy(ValueBuffer *dest,
                                                   OpaqueValue *src,
                                                   const Metadata *metatype) {
  assert(IsPOD == tuple_getValueWitnesses(metatype)->isPOD());
  assert(IsInline == tuple_getValueWitnesses(metatype)->isValueInline());

  return tuple_initializeWithCopy<IsPOD, IsInline>(
                        tuple_allocateBuffer<IsPOD, IsInline>(dest, metatype),
                        src,
                        metatype);
}

/// Generic tuple value witness for 'initializeBufferWithTake'.
template <bool IsPOD, bool IsInline>
static OpaqueValue *tuple_initializeBufferWithTake(ValueBuffer *dest,
                                                   OpaqueValue *src,
                                                   const Metadata *metatype) {
  assert(IsPOD == tuple_getValueWitnesses(metatype)->isPOD());
  assert(IsInline == tuple_getValueWitnesses(metatype)->isValueInline());

  return tuple_initializeWithTake<IsPOD, IsInline>(
                        tuple_allocateBuffer<IsPOD, IsInline>(dest, metatype),
                        src,
                        metatype);
}

/// Generic tuple value witness for 'initializeBufferWithCopyOfBuffer'.
template <bool IsPOD, bool IsInline>
static OpaqueValue *tuple_initializeBufferWithCopyOfBuffer(ValueBuffer *dest,
                                                           ValueBuffer *src,
                                                     const Metadata *metatype) {
  assert(IsPOD == tuple_getValueWitnesses(metatype)->isPOD());
  assert(IsInline == tuple_getValueWitnesses(metatype)->isValueInline());

  return tuple_initializeBufferWithCopy<IsPOD, IsInline>(
                            dest,
                            tuple_projectBuffer<IsPOD, IsInline>(src, metatype),
                            metatype);
}

/// Generic tuple value witness for 'initializeBufferWithTakeOfBuffer'.
template <bool IsPOD, bool IsInline>
static OpaqueValue *tuple_initializeBufferWithTakeOfBuffer(ValueBuffer *dest,
                                                           ValueBuffer *src,
                                                     const Metadata *metatype) {
  assert(IsPOD == tuple_getValueWitnesses(metatype)->isPOD());
  assert(IsInline == tuple_getValueWitnesses(metatype)->isValueInline());

  if (IsInline) {
    return tuple_initializeWithTake<IsPOD, IsInline>(
                      tuple_projectBuffer<IsPOD, IsInline>(dest, metatype),
                      tuple_projectBuffer<IsPOD, IsInline>(src, metatype),
                      metatype);
  } else {
    dest->PrivateData[0] = src->PrivateData[0];
    return (OpaqueValue*) dest->PrivateData[0];
  }
}

static void tuple_storeExtraInhabitant(OpaqueValue *tuple,
                                       int index,
                                       const Metadata *_metatype) {
  auto &metatype = *(const TupleTypeMetadata*) _metatype;
  auto &eltInfo = metatype.getElements()[0];

  assert(eltInfo.Offset == 0);
  OpaqueValue *elt = tuple;

  eltInfo.Type->vw_storeExtraInhabitant(elt, index);
}

static int tuple_getExtraInhabitantIndex(const OpaqueValue *tuple,
                                         const Metadata *_metatype) {
  auto &metatype = *(const TupleTypeMetadata*) _metatype;
  auto &eltInfo = metatype.getElements()[0];

  assert(eltInfo.Offset == 0);
  const OpaqueValue *elt = tuple;

  return eltInfo.Type->vw_getExtraInhabitantIndex(elt);
}

/// Various standard witness table for tuples.
static const ValueWitnessTable tuple_witnesses_pod_inline = {
#define TUPLE_WITNESS(NAME) &tuple_##NAME<true, true>,
  FOR_ALL_FUNCTION_VALUE_WITNESSES(TUPLE_WITNESS)
#undef TUPLE_WITNESS
  0,
  ValueWitnessFlags(),
  0
};
static const ValueWitnessTable tuple_witnesses_nonpod_inline = {
#define TUPLE_WITNESS(NAME) &tuple_##NAME<false, true>,
  FOR_ALL_FUNCTION_VALUE_WITNESSES(TUPLE_WITNESS)
#undef TUPLE_WITNESS
  0,
  ValueWitnessFlags(),
  0
};
static const ValueWitnessTable tuple_witnesses_pod_noninline = {
#define TUPLE_WITNESS(NAME) &tuple_##NAME<true, false>,
  FOR_ALL_FUNCTION_VALUE_WITNESSES(TUPLE_WITNESS)
#undef TUPLE_WITNESS
  0,
  ValueWitnessFlags(),
  0
};
static const ValueWitnessTable tuple_witnesses_nonpod_noninline = {
#define TUPLE_WITNESS(NAME) &tuple_##NAME<false, false>,
  FOR_ALL_FUNCTION_VALUE_WITNESSES(TUPLE_WITNESS)
#undef TUPLE_WITNESS
  0,
  ValueWitnessFlags(),
  0
};

namespace {
struct BasicLayout {
  size_t size;
  ValueWitnessFlags flags;
  size_t stride;
  
  static constexpr BasicLayout initialForValueType() {
    return {0, ValueWitnessFlags().withAlignment(1).withPOD(true), 0};
  }
  
  static constexpr BasicLayout initialForHeapObject() {
    return {sizeof(HeapObject),
            ValueWitnessFlags().withAlignment(alignof(HeapObject)),
            sizeof(HeapObject)};
  }
};

static size_t roundUpToAlignMask(size_t size, size_t alignMask) {
  return (size + alignMask) & ~alignMask;
}
  
/// Perform basic sequential layout given a vector of metadata pointers,
/// calling a functor with the offset of each field, and returning the
/// final layout characteristics of the type.
/// FUNCTOR should have signature:
///   void (size_t index, const Metadata *type, size_t offset)
template<typename FUNCTOR>
void performBasicLayout(BasicLayout &layout,
                        const Metadata * const *elements,
                        size_t numElements,
                        FUNCTOR &&f) {
  size_t size = layout.size;
  size_t alignMask = layout.flags.getAlignmentMask();
  bool isPOD = layout.flags.isPOD();
  bool isBitwiseTakable = layout.flags.isBitwiseTakable();
  for (unsigned i = 0; i != numElements; ++i) {
    auto elt = elements[i];
    
    // Lay out this element.
    auto eltVWT = elt->getValueWitnesses();
    size = roundUpToAlignMask(size, eltVWT->getAlignmentMask());

    // Report this record to the functor.
    f(i, elt, size);
    
    // Update the size and alignment of the aggregate..
    size += eltVWT->size;
    alignMask = std::max(alignMask, eltVWT->getAlignmentMask());
    if (!eltVWT->isPOD()) isPOD = false;
    if (!eltVWT->isBitwiseTakable()) isBitwiseTakable = false;
  }
  bool isInline = ValueWitnessTable::isValueInline(size, alignMask + 1);
  
  layout.size = size;
  layout.flags = ValueWitnessFlags().withAlignmentMask(alignMask)
                                    .withPOD(isPOD)
                                    .withBitwiseTakable(isBitwiseTakable)
                                    .withInlineStorage(isInline);
  layout.stride = roundUpToAlignMask(size, alignMask);
}
} // end anonymous namespace

const TupleTypeMetadata *
swift::swift_getTupleTypeMetadata(size_t numElements,
                                  const Metadata * const *elements,
                                  const char *labels,
                                  const ValueWitnessTable *proposedWitnesses) {
#if SWIFT_DEBUG_RUNTIME
  printf("looking up tuple type metadata\n");
  for (unsigned i = 0; i < numElements; ++i)
    printf("  %p\n", elements[0]);
#endif

  // FIXME: include labels when uniquing!
  auto genericArgs = (const void * const *) elements;
  if (auto entry = TupleTypes.find(genericArgs, numElements)) {
#if SWIFT_DEBUG_RUNTIME
    printf("found in cache! %p\n", entry->getData());
#endif
    return entry->getData();
  }
  
#if SWIFT_DEBUG_RUNTIME
  printf("not found in cache!\n");
#endif

  // We might reasonably get called by generic code, like a demangler
  // that produces type objects.  As long as we sink this below the
  // fast-path map lookup, it doesn't really cost us anything.
  if (numElements == 0) return &_TMdT_;

  typedef TupleTypeMetadata::Element Element;

  // Allocate the tuple cache entry, which includes space for both the
  // metadata and a value-witness table.
  auto entry = TupleCacheEntry::allocate(genericArgs, numElements,
                                         numElements * sizeof(Element));

  auto witnesses = &entry->Witnesses;

  auto metadata = entry->getData();
  metadata->setKind(MetadataKind::Tuple);
  metadata->ValueWitnesses = witnesses;
  metadata->NumElements = numElements;
  metadata->Labels = labels;

  // Perform basic layout on the tuple.
  auto layout = BasicLayout::initialForValueType();
  performBasicLayout(layout, elements, numElements,
    [&](size_t i, const Metadata *elt, size_t offset) {
      metadata->getElements()[i].Type = elt;
      metadata->getElements()[i].Offset = offset;
    });
  
  witnesses->size = layout.size;
  witnesses->flags = layout.flags;
  witnesses->stride = layout.stride;

  // Copy the function witnesses in, either from the proposed
  // witnesses or from the standard table.
  if (!proposedWitnesses) {
    // For a tuple with a single element, just use the witnesses for
    // the element type.
    if (numElements == 1) {
      proposedWitnesses = elements[0]->getValueWitnesses();

    // Otherwise, use generic witnesses (when we can't pattern-match
    // into something better).
    } else if (layout.flags.isInlineStorage()
               && layout.flags.isPOD()) {
      if (layout.size == 8) proposedWitnesses = &_TWVBi64_;
      else if (layout.size == 4) proposedWitnesses = &_TWVBi32_;
      else if (layout.size == 2) proposedWitnesses = &_TWVBi16_;
      else if (layout.size == 1) proposedWitnesses = &_TWVBi8_;
      else proposedWitnesses = &tuple_witnesses_pod_inline;
    } else if (layout.flags.isInlineStorage()
               && !layout.flags.isPOD()) {
      proposedWitnesses = &tuple_witnesses_nonpod_inline;
    } else if (!layout.flags.isInlineStorage()
               && layout.flags.isPOD()) {
      proposedWitnesses = &tuple_witnesses_pod_noninline;
    } else {
      assert(!layout.flags.isInlineStorage()
             && !layout.flags.isPOD());
      proposedWitnesses = &tuple_witnesses_nonpod_noninline;
    }
  }
#define ASSIGN_TUPLE_WITNESS(NAME) \
  witnesses->NAME = proposedWitnesses->NAME;
  FOR_ALL_FUNCTION_VALUE_WITNESSES(ASSIGN_TUPLE_WITNESS)
#undef ASSIGN_TUPLE_WITNESS

  // We have extra inhabitants if the first element does.
  // FIXME: generalize this.
  if (auto firstEltEIVWT = dyn_cast<ExtraInhabitantsValueWitnessTable>(
                                          elements[0]->getValueWitnesses())) {
    witnesses->flags = witnesses->flags.withExtraInhabitants(true);
    witnesses->extraInhabitantFlags = firstEltEIVWT->extraInhabitantFlags;
    witnesses->storeExtraInhabitant = tuple_storeExtraInhabitant;
    witnesses->getExtraInhabitantIndex = tuple_getExtraInhabitantIndex;
  }

  auto finalMetadata = TupleTypes.add(entry)->getData();
#if SWIFT_DEBUG_RUNTIME
  printf(" -> %p\n", finalMetadata);
#endif
  return finalMetadata;
}

const TupleTypeMetadata *
swift::swift_getTupleTypeMetadata2(const Metadata *elt0, const Metadata *elt1,
                                   const char *labels,
                                   const ValueWitnessTable *proposedWitnesses) {
  const Metadata *elts[] = { elt0, elt1 };
  return swift_getTupleTypeMetadata(2, elts, labels, proposedWitnesses);
}

const TupleTypeMetadata *
swift::swift_getTupleTypeMetadata3(const Metadata *elt0, const Metadata *elt1,
                                   const Metadata *elt2,
                                   const char *labels,
                                   const ValueWitnessTable *proposedWitnesses) {
  const Metadata *elts[] = { elt0, elt1, elt2 };
  return swift_getTupleTypeMetadata(3, elts, labels, proposedWitnesses);
}

/*** Structs ***************************************************************/

/// Initialize the value witness table and struct field offset vector for a
/// struct, using the "Universal" layout strategy.
void swift::swift_initStructMetadata_UniversalStrategy(size_t numFields,
                                     const Metadata * const *fieldTypes,
                                     size_t *fieldOffsets,
                                     ValueWitnessTable *vwtable) {
  auto layout = BasicLayout::initialForValueType();
  performBasicLayout(layout, fieldTypes, numFields,
    [&](size_t i, const Metadata *fieldType, size_t offset) {
      fieldOffsets[i] = offset;
    });
  
  vwtable->size = layout.size;
  vwtable->flags = layout.flags;
  vwtable->stride = layout.stride;

  // We have extra inhabitants if the first element does.
  // FIXME: generalize this.
  if (auto firstFieldVWT = dyn_cast<ExtraInhabitantsValueWitnessTable>(
                                        fieldTypes[0]->getValueWitnesses())) {
    vwtable->flags = vwtable->flags.withExtraInhabitants(true);
    auto xiVWT = cast<ExtraInhabitantsValueWitnessTable>(vwtable);
    xiVWT->extraInhabitantFlags = firstFieldVWT->extraInhabitantFlags;

    // The compiler should already have initialized these.
    assert(xiVWT->storeExtraInhabitant);
    assert(xiVWT->getExtraInhabitantIndex);
  }
}

/*** Classes ***************************************************************/

namespace {
  /// The structure of ObjC class ivars as emitted by compilers.
  struct ClassIvarEntry {
    size_t *Offset;
    const char *Name;
    const char *Type;
    uint32_t Log2Alignment;
    uint32_t Size;
  };

  /// The structure of ObjC class ivar lists as emitted by compilers.
  struct ClassIvarList {
    uint32_t EntrySize;
    uint32_t Count;

    ClassIvarEntry *getIvars() {
      return reinterpret_cast<ClassIvarEntry*>(this+1);
    }
    const ClassIvarEntry *getIvars() const {
      return reinterpret_cast<const ClassIvarEntry*>(this+1);
    }
  };

  /// The structure of ObjC class rodata as emitted by compilers.
  struct ClassROData {
    uint32_t Flags;
    uint32_t InstanceStart;
    uint32_t InstanceSize;
#ifdef __LP64__
    uint32_t Reserved;
#endif
    const uint8_t *IvarLayout;
    const char *Name;
    const void *MethodList;
    const void *ProtocolList;
    ClassIvarList *IvarList;
    const uint8_t *WeakIvarLayout;
    const void *PropertyList;
  };
}

static uint32_t getLog2AlignmentFromMask(size_t alignMask) {
  assert(((alignMask + 1) & alignMask) == 0 &&
         "not an alignment mask!");

  uint32_t log2 = 0;
  while ((1 << log2) != (alignMask + 1))
    log2++;
  return log2;
}

/// Initialize the field offset vector for a dependent-layout class, using the
/// "Universal" layout strategy.
void swift::swift_initClassMetadata_UniversalStrategy(ClassMetadata *self,
                                            const ClassMetadata *super,
                                            size_t numFields,
                                      const ClassFieldLayout *fieldLayouts,
                                            size_t *fieldOffsets) {
  // Start layout by appending to a standard heap object header.
  size_t size, alignMask;

  // If we have a superclass, start from its size and alignment instead.
  if (super) {
    // This is straightforward if the superclass is Swift.
    if (super->isTypeMetadata()) {
      size = super->getInstanceSize();
      alignMask = super->getInstanceAlignMask();

    // If it's Objective-C, we need to clone the ivar descriptors.
    // The data pointer will still be the value we set up according
    // to the compiler ABI.
    } else {
      ClassROData *rodata = (ClassROData*) (self->Data & ~uintptr_t(1));

      // Do layout starting from our notion of where the superclass starts.
      size = rodata->InstanceStart;
      alignMask = 0xF; // malloc alignment guarantee

      if (numFields) {
        // Clone the ivar list.
        const ClassIvarList *dependentIvars = rodata->IvarList;
        assert(dependentIvars->Count == numFields);
        assert(dependentIvars->EntrySize == sizeof(ClassIvarEntry));

        auto ivarListSize = sizeof(ClassIvarList) +
                            numFields * sizeof(ClassIvarEntry);
        auto ivars = (ClassIvarList*) permanentAlloc(ivarListSize);
        memcpy(ivars, dependentIvars, ivarListSize);
        rodata->IvarList = ivars;

        for (unsigned i = 0; i != numFields; ++i) {
          ClassIvarEntry &ivar = ivars->getIvars()[i];

          // The offset variable for the ivar is the respective entry in
          // the field-offset vector.
          ivar.Offset = &fieldOffsets[i];

          // If the ivar's size doesn't match the field layout we
          // computed, overwrite it and give it better type information.
          if (ivar.Size != fieldLayouts[i].Size) {
            ivar.Size = fieldLayouts[i].Size;
            ivar.Type = nullptr;
            ivar.Log2Alignment =
              getLog2AlignmentFromMask(fieldLayouts[i].AlignMask);
          }
        }
      }
    }

  // If we don't have a formal superclass, start with the basic heap header.
  } else {
    auto heapLayout = BasicLayout::initialForHeapObject();
    size = heapLayout.size;
    alignMask = heapLayout.flags.getAlignmentMask();
  }

  for (unsigned i = 0; i != numFields; ++i) {
    auto offset = roundUpToAlignMask(size, fieldLayouts[i].AlignMask);
    fieldOffsets[i] = offset;
    size = offset + fieldLayouts[i].Size;
    alignMask = std::max(alignMask, fieldLayouts[i].AlignMask);
  }
  
  // Save the final size and alignment into the metadata record.
  assert(self->isTypeMetadata());
  self->setInstanceSize(size);
  self->setInstanceAlignMask(alignMask);
}

/*** Metatypes *************************************************************/

namespace {
  class MetatypeCacheEntry : public CacheEntry<MetatypeCacheEntry> {
    FullMetadata<MetatypeMetadata> Metadata;

  public:
    MetatypeCacheEntry(size_t numArguments) {}

    static constexpr size_t getNumArguments() {
      return 1;
    }

    FullMetadata<MetatypeMetadata> *getData() {
      return &Metadata;
    }
    const FullMetadata<MetatypeMetadata> *getData() const {
      return &Metadata;
    }
  };
}

/// The uniquing structure for metatype type metadata.
static MetadataCache<MetatypeCacheEntry> MetatypeTypes;

/// \brief Find the appropriate value witness table for the given type.
static const ValueWitnessTable *
getMetatypeValueWitnesses(const Metadata *instanceType) {
  // The following metatypes have non-trivial representation
  // in the concrete:
  //   - class types
  //   - metatypes of types that require value witnesses

  // For class types, return the unmanaged-pointer witnesses.
  if (instanceType->isClassType())
    return &getUnmanagedPointerPointerValueWitnesses();

  // Metatypes preserve the triviality of their instance type.
  if (instanceType->getKind() == MetadataKind::Metatype)
    return instanceType->getValueWitnesses();

  // Everything else is trivial and can use the empty-tuple metadata.
  return &_TWVT_;
}

/// \brief Fetch a uniqued metadata for a metatype type.
extern "C" const MetatypeMetadata *
swift::swift_getMetatypeMetadata(const Metadata *instanceMetadata) {
  const size_t numGenericArgs = 1;

  const void *args[] = { instanceMetadata };
  if (auto entry = MetatypeTypes.find(args, numGenericArgs)) {
    return entry->getData();
  }

  auto entry = MetatypeCacheEntry::allocate(args, numGenericArgs, 0);

  auto metadata = entry->getData();
  metadata->setKind(MetadataKind::Metatype);
  metadata->ValueWitnesses = getMetatypeValueWitnesses(instanceMetadata);
  metadata->InstanceType = instanceMetadata;

  return MetatypeTypes.add(entry)->getData();
}

/*** Existential Metatypes *************************************************/

namespace {
  class ExistentialMetatypeCacheEntry :
      public CacheEntry<ExistentialMetatypeCacheEntry> {
    FullMetadata<ExistentialMetatypeMetadata> Metadata;

  public:
    ExistentialMetatypeCacheEntry(size_t numArguments) {}

    static constexpr size_t getNumArguments() {
      return 1;
    }

    FullMetadata<ExistentialMetatypeMetadata> *getData() {
      return &Metadata;
    }
    const FullMetadata<ExistentialMetatypeMetadata> *getData() const {
      return &Metadata;
    }
  };
}

/// The uniquing structure for existential metatype type metadata.
static MetadataCache<ExistentialMetatypeCacheEntry> ExistentialMetatypeTypes;

/// \brief Find the appropriate value witness table for the given type.
static const ValueWitnessTable *
getExistentialMetatypeValueWitnesses(unsigned numWitnessTables) {
  // FIXME
  return &getUnmanagedPointerPointerValueWitnesses();
}

/// \brief Fetch a uniqued metadata for a metatype type.
extern "C" const ExistentialMetatypeMetadata *
swift::swift_getExistentialMetatypeMetadata(const Metadata *instanceMetadata) {
  const size_t numGenericArgs = 1;

  const void *args[] = { instanceMetadata };
  if (auto entry = ExistentialMetatypeTypes.find(args, numGenericArgs)) {
    return entry->getData();
  }

  auto entry = ExistentialMetatypeCacheEntry::allocate(args, numGenericArgs, 0);

  // FIXME: the value witnesses should probably account for room for
  // protocol witness tables

  ExistentialTypeFlags flags;
  if (instanceMetadata->getKind() == MetadataKind::Existential) {
    flags = static_cast<const ExistentialTypeMetadata*>(instanceMetadata)->Flags;
  } else {
    assert(instanceMetadata->getKind() == MetadataKind::ExistentialMetatype);
    flags = static_cast<const ExistentialMetatypeMetadata*>(instanceMetadata)->Flags;
  }

  auto metadata = entry->getData();
  metadata->setKind(MetadataKind::ExistentialMetatype);
  metadata->ValueWitnesses = getExistentialMetatypeValueWitnesses(flags.getNumWitnessTables());
  metadata->InstanceType = instanceMetadata;
  metadata->Flags = flags;

  return ExistentialMetatypeTypes.add(entry)->getData();
}

/*** Existential types ********************************************************/

namespace {
  class ExistentialCacheEntry : public CacheEntry<ExistentialCacheEntry> {
  public:
    FullMetadata<ExistentialTypeMetadata> Metadata;

    ExistentialCacheEntry(size_t numArguments) {
      Metadata.Protocols.NumProtocols = numArguments;
    }

    size_t getNumArguments() const {
      return Metadata.Protocols.NumProtocols;
    }

    FullMetadata<ExistentialTypeMetadata> *getData() {
      return &Metadata;
    }
    const FullMetadata<ExistentialTypeMetadata> *getData() const {
      return &Metadata;
    }
  };
}

/// The uniquing structure for existential type metadata.
static MetadataCache<ExistentialCacheEntry> ExistentialTypes;

static const ValueWitnessTable OpaqueExistentialValueWitnesses_0 =
  ValueWitnessTableForBox<OpaqueExistentialBox<0>>::table;
static const ValueWitnessTable OpaqueExistentialValueWitnesses_1 =
  ValueWitnessTableForBox<OpaqueExistentialBox<1>>::table;

static llvm::DenseMap<unsigned, const ValueWitnessTable*>
  OpaqueExistentialValueWitnessTables;

/// Instantiate a value witness table for an opaque existential container with
/// the given number of witness table pointers.
static const ValueWitnessTable *
getOpaqueExistentialValueWitnesses(unsigned numWitnessTables) {
  // We pre-allocate a couple of important cases.
  if (numWitnessTables == 0)
    return &OpaqueExistentialValueWitnesses_0;
  if (numWitnessTables == 1)
    return &OpaqueExistentialValueWitnesses_1;

  // FIXME: make thread-safe

  auto found = OpaqueExistentialValueWitnessTables.find(numWitnessTables);
  if (found != OpaqueExistentialValueWitnessTables.end())
    return found->second;
  
  using Box = NonFixedOpaqueExistentialBox;
  using Witnesses = NonFixedValueWitnesses<Box, /*known allocated*/ true>;
  static_assert(!Witnesses::hasExtraInhabitants, "no extra inhabitants");
  
  auto *vwt = new ValueWitnessTable;
#define STORE_VAR_OPAQUE_EXISTENTIAL_WITNESS(WITNESS) \
  vwt->WITNESS = Witnesses::WITNESS;
  FOR_ALL_FUNCTION_VALUE_WITNESSES(STORE_VAR_OPAQUE_EXISTENTIAL_WITNESS)
#undef STORE_VAR_OPAQUE_EXISTENTIAL_WITNESS
  
  vwt->size = Box::Container::getSize(numWitnessTables);
  vwt->flags = ValueWitnessFlags()
    .withAlignment(Box::Container::getAlignment(numWitnessTables))
    .withPOD(false)
    .withBitwiseTakable(false)
    .withInlineStorage(false)
    .withExtraInhabitants(false);
  vwt->stride = Box::Container::getStride(numWitnessTables);

  OpaqueExistentialValueWitnessTables.insert({numWitnessTables, vwt});
  
  return vwt;
}

static const ExtraInhabitantsValueWitnessTable ClassExistentialValueWitnesses_1 =
  ValueWitnessTableForBox<ClassExistentialBox<1>>::table;
static const ExtraInhabitantsValueWitnessTable ClassExistentialValueWitnesses_2 =
  ValueWitnessTableForBox<ClassExistentialBox<2>>::table;

static llvm::DenseMap<unsigned, const ExtraInhabitantsValueWitnessTable*>
  ClassExistentialValueWitnessTables;

/// Instantiate a value witness table for a class-constrained existential
/// container with the given number of witness table pointers.
static const ExtraInhabitantsValueWitnessTable *
getClassExistentialValueWitnesses(unsigned numWitnessTables) {
  if (numWitnessTables == 0)
    return &_TWVBO;
  if (numWitnessTables == 1)
    return &ClassExistentialValueWitnesses_1;
  if (numWitnessTables == 2)
    return &ClassExistentialValueWitnesses_2;

  static_assert(3 * sizeof(void*) >= sizeof(ValueBuffer),
                "not handling all possible inline-storage class existentials!");

  auto found = ClassExistentialValueWitnessTables.find(numWitnessTables);
  if (found != ClassExistentialValueWitnessTables.end())
    return found->second;
  
  using Box = NonFixedClassExistentialBox;
  using Witnesses = NonFixedValueWitnesses<Box, /*known allocated*/ true>;
  
  auto *vwt = new ExtraInhabitantsValueWitnessTable;
#define STORE_VAR_CLASS_EXISTENTIAL_WITNESS(WITNESS) \
  vwt->WITNESS = Witnesses::WITNESS;
  FOR_ALL_FUNCTION_VALUE_WITNESSES(STORE_VAR_CLASS_EXISTENTIAL_WITNESS)
  STORE_VAR_CLASS_EXISTENTIAL_WITNESS(storeExtraInhabitant)
  STORE_VAR_CLASS_EXISTENTIAL_WITNESS(getExtraInhabitantIndex)
#undef STORE_VAR_CLASS_EXISTENTIAL_WITNESS
  
  vwt->size = Box::Container::getSize(numWitnessTables);
  vwt->flags = ValueWitnessFlags()
    .withAlignment(Box::Container::getAlignment(numWitnessTables))
    .withPOD(false)
    .withBitwiseTakable(true)
    .withInlineStorage(false)
    .withExtraInhabitants(true);
  vwt->stride = Box::Container::getStride(numWitnessTables);
  vwt->extraInhabitantFlags = ExtraInhabitantFlags()
    .withNumExtraInhabitants(Witnesses::numExtraInhabitants);
  
  ClassExistentialValueWitnessTables.insert({numWitnessTables, vwt});
  
  return vwt;
}

/// Get the value witness table for an existential type, first trying to use a
/// shared specialized table for common cases.
static const ValueWitnessTable *
getExistentialValueWitnesses(ProtocolClassConstraint classConstraint,
                             unsigned numWitnessTables) {
  switch (classConstraint) {
  case ProtocolClassConstraint::Class:
    return getClassExistentialValueWitnesses(numWitnessTables);
  case ProtocolClassConstraint::Any:
    return getOpaqueExistentialValueWitnesses(numWitnessTables);
  }
}

const OpaqueValue *
ExistentialTypeMetadata::projectValue(const OpaqueValue *container) const {
  // The layout of the container depends on whether it's class-constrained.
  if (Flags.getClassConstraint() == ProtocolClassConstraint::Class) {
    auto classContainer =
      reinterpret_cast<const ClassExistentialContainer*>(container);
    return reinterpret_cast<const OpaqueValue *>(&classContainer->Value);
  } else {
    auto opaqueContainer =
      reinterpret_cast<const OpaqueExistentialContainer*>(container);
    return opaqueContainer->Type->vw_projectBuffer(
                         const_cast<ValueBuffer*>(&opaqueContainer->Buffer));
  }  
}

const Metadata *
ExistentialTypeMetadata::getDynamicType(const OpaqueValue *container) const {
  // The layout of the container depends on whether it's class-constrained.
  if (isClassBounded()) {
    auto classContainer =
      reinterpret_cast<const ClassExistentialContainer*>(container);
    void *obj = classContainer->Value;
    return swift_getObjectType(reinterpret_cast<HeapObject*>(obj));
  } else {
    auto opaqueContainer =
      reinterpret_cast<const OpaqueExistentialContainer*>(container);
    return opaqueContainer->Type;
  }
}

const void * const *
ExistentialTypeMetadata::getWitnessTable(const OpaqueValue *container,
                                         unsigned i) const {
  assert(i < Flags.getNumWitnessTables());

  // The layout of the container depends on whether it's class-constrained.
  const void * const * witnessTables;
  if (isClassBounded()) {
    auto classContainer =
      reinterpret_cast<const ClassExistentialContainer*>(container);
    witnessTables = classContainer->getWitnessTables();
  } else {
    auto opaqueContainer =
      reinterpret_cast<const OpaqueExistentialContainer*>(container);
    witnessTables = opaqueContainer->getWitnessTables();
  }

  // The return type here describes extra structure for the protocol
  // witness table for some reason.  We should probaby have a nominal
  // type for these, just for type safety reasons.
  return reinterpret_cast<const void * const *>(witnessTables[i]);
}

/// \brief Fetch a uniqued metadata for an existential type. The array
/// referenced by \c protocols will be sorted in-place.
const ExistentialTypeMetadata *
swift::swift_getExistentialTypeMetadata(size_t numProtocols,
                                        const ProtocolDescriptor **protocols) {
  // Sort the protocol set.
  std::sort(protocols, protocols + numProtocols);
  
  // Calculate the class constraint and number of witness tables for the
  // protocol set.
  unsigned numWitnessTables = 0;
  ProtocolClassConstraint classConstraint = ProtocolClassConstraint::Any;
  for (auto p : make_range(protocols, protocols + numProtocols)) {
    if (p->Flags.needsWitnessTable()) {
      ++numWitnessTables;
    }
    if (p->Flags.getClassConstraint() == ProtocolClassConstraint::Class)
      classConstraint = ProtocolClassConstraint::Class;
  }
  
  auto protocolArgs = reinterpret_cast<const void * const *>(protocols);
  
  if (auto entry = ExistentialTypes.find(protocolArgs, numProtocols)) {
    return entry->getData();
  }
  
  auto entry = ExistentialCacheEntry::allocate(protocolArgs, numProtocols,
                             sizeof(const ProtocolDescriptor *) * numProtocols);
  auto metadata = entry->getData();
  metadata->setKind(MetadataKind::Existential);
  metadata->ValueWitnesses = getExistentialValueWitnesses(classConstraint,
                                                          numWitnessTables);
  metadata->Flags = ExistentialTypeFlags()
    .withNumWitnessTables(numWitnessTables)
    .withClassConstraint(classConstraint);
  metadata->Protocols.NumProtocols = numProtocols;
  for (size_t i = 0; i < numProtocols; ++i)
    metadata->Protocols[i] = protocols[i];
  
  return ExistentialTypes.add(entry)->getData();
}

/// \brief Perform a copy-assignment from one existential container to another.
/// Both containers must be of the same existential type representable with no
/// witness tables.
OpaqueValue *swift::swift_assignExistentialWithCopy0(OpaqueValue *dest,
                                                     const OpaqueValue *src,
                                                     const Metadata *type) {
  using Witnesses = ValueWitnesses<OpaqueExistentialBox<0>>;
  return Witnesses::assignWithCopy(dest, const_cast<OpaqueValue*>(src), type);
}

/// \brief Perform a copy-assignment from one existential container to another.
/// Both containers must be of the same existential type representable with one
/// witness table.
OpaqueValue *swift::swift_assignExistentialWithCopy1(OpaqueValue *dest,
                                                     const OpaqueValue *src,
                                                     const Metadata *type) {
  using Witnesses = ValueWitnesses<OpaqueExistentialBox<1>>;
  return Witnesses::assignWithCopy(dest, const_cast<OpaqueValue*>(src), type);
}

/// \brief Perform a copy-assignment from one existential container to another.
/// Both containers must be of the same existential type representable with the
/// same number of witness tables.
OpaqueValue *swift::swift_assignExistentialWithCopy(OpaqueValue *dest,
                                                    const OpaqueValue *src,
                                                    const Metadata *type) {
  assert(!type->getValueWitnesses()->isValueInline());
  using Witnesses = NonFixedValueWitnesses<NonFixedOpaqueExistentialBox,
                                           /*known allocated*/ true>;
  return Witnesses::assignWithCopy(dest, const_cast<OpaqueValue*>(src), type);
}

/*** Foreign types *********************************************************/

namespace {
  /// A string whose data is globally-allocated.
  struct GlobalString {
    StringRef Data;
    /*implicit*/ GlobalString(StringRef data) : Data(data) {}
  };
}

template <>
struct llvm::DenseMapInfo<GlobalString> {
  static GlobalString getEmptyKey() {
    return StringRef((const char*) 0, 0);
  }
  static GlobalString getTombstoneKey() {
    return StringRef((const char*) 1, 0);
  }
  static unsigned getHashValue(const GlobalString &val) {
    // llvm::hash_value(StringRef) is, unfortunately, defined out of
    // line in a library we otherwise would not need to link against.
    return llvm::hash_combine_range(val.Data.begin(), val.Data.end());
  }
  static bool isEqual(const GlobalString &lhs, const GlobalString &rhs) {
    return lhs.Data == rhs.Data;
  }
};

// We use a DenseMap over what are essentially StringRefs instead of a
// StringMap because we don't need to actually copy the string.
static llvm::DenseMap<GlobalString, const ForeignTypeMetadata *> ForeignTypes;

const ForeignTypeMetadata *
swift::swift_getForeignTypeMetadata(ForeignTypeMetadata *nonUnique) {
  // Fast path: check the invasive cache.
  if (nonUnique->Unique) return nonUnique->Unique;

  // Okay, insert a new row.
  // FIXME: locking!
  auto insertResult = ForeignTypes.insert({GlobalString(nonUnique->Name),
                                           nonUnique});
  auto uniqueMetadata = insertResult.first->second;

  // If the insertion created a new entry, set up the metadata we were
  // passed as the insertion result.
  if (insertResult.second) {
    // Call the initialization callback if present.
    if (nonUnique->hasInitializationFunction())
      nonUnique->getInitializationFunction()(nonUnique);
  }

  // Remember the unique result in the invasive cache.  We don't want
  // to do this until after the initialization completes; otherwise,
  // it will be possible for code to fast-path through this function
  // too soon.
  nonUnique->Unique = uniqueMetadata;
  return uniqueMetadata;
}

/*** Other metadata routines ***********************************************/

const NominalTypeDescriptor *
Metadata::getNominalTypeDescriptor() const {
  switch (getKind()) {
  case MetadataKind::Class: {
    const ClassMetadata *cls = static_cast<const ClassMetadata *>(this);
    assert(cls->isTypeMetadata());
    if (cls->isArtificialSubclass())
      return nullptr;
    return cls->getDescription();
  }
  case MetadataKind::Struct:
  case MetadataKind::Enum:
    return static_cast<const StructMetadata *>(this)->Description;
  case MetadataKind::ForeignClass:
  case MetadataKind::Opaque:
  case MetadataKind::Tuple:
  case MetadataKind::Function:
  case MetadataKind::Block:
  case MetadataKind::PolyFunction:
  case MetadataKind::Existential:
  case MetadataKind::ExistentialMetatype:
  case MetadataKind::Metatype:
  case MetadataKind::ObjCClassWrapper:
  case MetadataKind::HeapArray:
  case MetadataKind::HeapLocalVariable:
    return nullptr;
  }
}

/// Scan and return a single run-length encoded identifier.
/// Returns a malloc-allocated string, or nullptr on failure.
/// mangled is advanced past the end of the scanned token.
static char *scanIdentifier(const char *&mangled)
{
  const char *original = mangled;

  {
    if (*mangled == '0') goto fail;  // length may not be zero
    
    size_t length = 0;
    while (isdigit(*mangled)) {
      size_t oldlength = length;
      length *= 10;
      length += *mangled++ - '0';
      if (length <= oldlength) goto fail;  // integer overflow
    }
    
    if (length == 0) goto fail;
    if (length > strlen(mangled)) goto fail;
    
    char *result = strndup(mangled, length);
    assert(result);
    mangled += length;
    return result;
  }

fail:
  mangled = original;  // rewind
  return nullptr;
}


/// \brief Demangle a mangled class name into module+class.
/// Returns true if the name was successfully decoded.
/// On success, *outModule and *outClass must be freed with free().
/// FIXME: this should be replaced by a real demangler
bool swift::swift_demangleSimpleClass(const char *mangledName, 
                                      char **outModule, char **outClass) {
  char *moduleName = nullptr;
  char *className = nullptr;

  {  
    // Prefix for a mangled class
    const char *m = mangledName;
    if (0 != strncmp(m, "_TtC", 4)) 
      goto fail;
    m += 4;
    
    // Module name
    if (strncmp(m, "Ss", 2) == 0) {
      moduleName = strdup(swift::STDLIB_NAME);
      assert(moduleName);
      m += 2;
    } else {
      moduleName = scanIdentifier(m);
      if (!moduleName) 
        goto fail;
    }
    
    // Class name
    className = scanIdentifier(m);
    if (!className) 
      goto fail;
    
    // Nothing else
    if (strlen(m)) 
      goto fail;
    
    *outModule = moduleName;
    *outClass = className;
    return true;
  }

fail:
  if (moduleName) free(moduleName);
  if (className) free(className);
  *outModule = nullptr;
  *outClass = nullptr;
  return false;
}

namespace llvm {
namespace hashing {
namespace detail {
  // An extern variable expected by LLVM's hashing templates. We don't link any
  // LLVM libs into the runtime, so define this here.
  size_t fixed_seed_override = 0;
}
}
}

