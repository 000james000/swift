//===--- Metadata.h - Swift Language ABI Metadata Support -------*- C++ -*-===//
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
// Swift ABI for generating and uniquing metadata.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_RUNTIME_METADATA_H
#define SWIFT_RUNTIME_METADATA_H

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include "swift/ABI/MetadataValues.h"

namespace swift {

struct HeapObject;
struct Metadata;

/// Storage for an arbitrary value.  In C/C++ terms, this is an
/// 'object', because it is rooted in memory.
///
/// The context dictates what type is actually stored in this object,
/// and so this type is intentionally incomplete.
///
/// An object can be in one of two states:
///  - An uninitialized object has a completely unspecified state.
///  - An initialized object holds a valid value of the type.
struct OpaqueValue;

/// A fixed-size buffer for local values.  It is capable of owning
/// (possibly in side-allocated memory) the storage necessary
/// to hold a value of an arbitrary type.  Because it is fixed-size,
/// it can be allocated in places that must be agnostic to the
/// actual type: for example, within objects of existential type,
/// or for local variables in generic functions.
///
/// The context dictates its type, which ultimately means providing
/// access to a value witness table by which the value can be
/// accessed and manipulated.
///
/// A buffer can directly store three pointers and is pointer-aligned.
/// Three pointers is a sweet spot for Swift, because it means we can
/// store a structure containing a pointer, a size, and an owning
/// object, which is a common pattern in code due to ARC.  In a GC
/// environment, this could be reduced to two pointers without much loss.
///
/// A buffer can be in one of three states:
///  - An unallocated buffer has a completely unspecified state.
///  - An allocated buffer has been initialized so that it
///    owns unintialized value storage for the stored type.
///  - An initialized buffer is an allocated buffer whose value
///    storage has been initialized.
struct ValueBuffer {
  void *PrivateData[3];
};

struct ValueWitnessTable;

/// Types stored in the value-witness table.
class ValueWitnessFlags {
  typedef size_t int_type;
  enum : int_type {
    AlignmentMask = 0x0000FFFF,
    IsNonPOD =      0x00010000,
    IsNonInline =   0x00020000,
    HasExtraInhabitants = 0x00040000,
    // Everything else is reserved.
  };
  int_type Data;

  constexpr ValueWitnessFlags(int_type data) : Data(data) {}
public:
  constexpr ValueWitnessFlags() : Data(0) {}

  /// The required alignment of the first byte of an object of this
  /// type, expressed as a mask of the low bits that must not be set
  /// in the pointer.
  ///
  /// This representation can be easily converted to the 'alignof'
  /// result by merely adding 1, but it is more directly useful for
  /// performing dynamic structure layouts, and it grants an
  /// additional bit of precision in a compact field without needing
  /// to switch to an exponent representation.
  ///
  /// For example, if the type needs to be 8-byte aligned, the
  /// appropriate alignment mask should be 0x7.
  size_t getAlignmentMask() const {
    return (Data & AlignmentMask);
  }
  constexpr ValueWitnessFlags withAlignmentMask(size_t alignMask) const {
    return ValueWitnessFlags((Data & ~AlignmentMask) | alignMask);
  }

  size_t getAlignment() const { return getAlignmentMask() + 1; }
  constexpr ValueWitnessFlags withAlignment(size_t alignment) const {
    return withAlignmentMask(alignment - 1);
  }

  /// True if the type requires out-of-line allocation of its storage.
  bool isInlineStorage() const { return !(Data & IsNonInline); }
  constexpr ValueWitnessFlags withInlineStorage(bool isInline) const {
    return ValueWitnessFlags((Data & ~IsNonInline) |
                               (isInline ? 0 : IsNonInline));
  }

  /// True if values of this type can be copied with memcpy and
  /// destroyed with a no-op.
  ///
  /// Unlike C++, non-POD types in Swift are still required to be
  /// address-invariant, so a value can always be "moved" from place
  /// to place with a memcpy.
  bool isPOD() const { return !(Data & IsNonPOD); }
  constexpr ValueWitnessFlags withPOD(bool isPOD) const {
    return ValueWitnessFlags((Data & ~IsNonPOD) |
                               (isPOD ? 0 : IsNonPOD));
  }
  
  /// True if this type's binary representation has extra inhabitants, that is,
  /// bit patterns that do not form valid values of the type.
  ///
  /// If true, then the extra inhabitant value witness table entries are
  /// available in this type's value witness table.
  bool hasExtraInhabitants() const { return Data & HasExtraInhabitants; }
  constexpr ValueWitnessFlags
  withExtraInhabitants(bool hasExtraInhabitants) const {
    return ValueWitnessFlags((Data & ~HasExtraInhabitants) |
                               (hasExtraInhabitants ? HasExtraInhabitants : 0));
  }
};
  
class ExtraInhabitantFlags {
  typedef size_t int_type;
  enum : int_type {
    NumExtraInhabitantsMask = 0x7FFFFFFFU,
  };
  int_type Data;
  
  constexpr ExtraInhabitantFlags(int_type data) : Data(data) {}

public:
  constexpr ExtraInhabitantFlags() : Data(0) {}
  
  /// The number of extra inhabitants in the type's representation.
  int getNumExtraInhabitants() const { return Data & NumExtraInhabitantsMask; }
  
  constexpr ExtraInhabitantFlags
  withNumExtraInhabitants(unsigned numExtraInhabitants) const {
    return ExtraInhabitantFlags((Data & ~NumExtraInhabitantsMask) |
                                  numExtraInhabitants);
  }
};

namespace value_witness_types {

/// Given an initialized buffer, destroy its value and deallocate
/// the buffer.  This can be decomposed as:
///
///   self->destroy(self->projectBuffer(buffer), self);
///   self->deallocateBuffer(buffer), self);
///
/// Preconditions:
///   'buffer' is an initialized buffer
/// Postconditions:
///   'buffer' is an unallocated buffer
typedef void destroyBuffer(ValueBuffer *buffer, const Metadata *self);

/// Given an unallocated buffer, initialize it as a copy of the
/// object in the source buffer.  This can be decomposed as:
///
///   self->initalizeBufferWithCopy(dest, self->projectBuffer(src), self)
///
/// This operation does not need to be safe aginst 'dest' and 'src' aliasing.
/// 
/// Preconditions:
///   'dest' is an unallocated buffer
/// Postconditions:
///   'dest' is an initialized buffer
/// Invariants:
///   'src' is an initialized buffer
typedef OpaqueValue *initializeBufferWithCopyOfBuffer(ValueBuffer *dest,
                                                      ValueBuffer *src,
                                                      const Metadata *self);

/// Given an allocated or initialized buffer, derive a pointer to
/// the object.
/// 
/// Invariants:
///   'buffer' is an allocated or initialized buffer
typedef OpaqueValue *projectBuffer(ValueBuffer *buffer,
                                   const Metadata *self);

/// Given an allocated buffer, deallocate the object.
///
/// Preconditions:
///   'buffer' is an allocated buffer
/// Postconditions:
///   'buffer' is an unallocated buffer
typedef void deallocateBuffer(ValueBuffer *buffer,
                              const Metadata *self);

/// Given an initialized object, destroy it.
///
/// Preconditions:
///   'object' is an initialized object
/// Postconditions:
///   'object' is an uninitialized object
typedef void destroy(OpaqueValue *object,
                     const Metadata *self);

/// Given an uninitialized buffer and an initialized object, allocate
/// storage in the buffer and copy the value there.
///
/// Returns the dest object.
///
/// Preconditions:
///   'dest' is an uninitialized buffer
/// Postconditions:
///   'dest' is an initialized buffer
/// Invariants:
///   'src' is an initialized object
typedef OpaqueValue *initializeBufferWithCopy(ValueBuffer *dest,
                                              OpaqueValue *src,
                                              const Metadata *self);

/// Given an uninitialized object and an initialized object, copy
/// the value.
///
/// This operation does not need to be safe aginst 'dest' and 'src' aliasing.
/// 
/// Returns the dest object.
///
/// Preconditions:
///   'dest' is an uninitialized object
/// Postconditions:
///   'dest' is an initialized object
/// Invariants:
///   'src' is an initialized object
typedef OpaqueValue *initializeWithCopy(OpaqueValue *dest,
                                        OpaqueValue *src,
                                        const Metadata *self);

/// Given two initialized objects, copy the value from one to the
/// other.
///
/// This operation must be safe aginst 'dest' and 'src' aliasing.
/// 
/// Returns the dest object.
///
/// Invariants:
///   'dest' is an initialized object
///   'src' is an initialized object
typedef OpaqueValue *assignWithCopy(OpaqueValue *dest,
                                    OpaqueValue *src,
                                    const Metadata *self);

/// Given an uninitialized buffer and an initialized object, move
/// the value from the object to the buffer, leaving the source object
/// uninitialized.
///
/// This operation does not need to be safe aginst 'dest' and 'src' aliasing.
/// 
/// Returns the dest object.
///
/// Preconditions:
///   'dest' is an uninitialized buffer
///   'src' is an initialized object
/// Postconditions:
///   'dest' is an initialized buffer
///   'src' is an uninitialized object
typedef OpaqueValue *initializeBufferWithTake(ValueBuffer *dest,
                                              OpaqueValue *src,
                                              const Metadata *self);

/// Given an uninitialized object and an initialized object, move
/// the value from one to the other, leaving the source object
/// uninitialized.
///
/// Guaranteed to be equivalent to a memcpy of self->size bytes.
/// There is no need for a initializeBufferWithTakeOfBuffer, because that
/// can simply be a pointer-aligned memcpy of sizeof(ValueBuffer)
/// bytes.
///
/// This operation does not need to be safe aginst 'dest' and 'src' aliasing.
/// 
/// Returns the dest object.
///
/// Preconditions:
///   'dest' is an uninitialized object
///   'src' is an initialized object
/// Postconditions:
///   'dest' is an initialized object
///   'src' is an uninitialized object
typedef OpaqueValue *initializeWithTake(OpaqueValue *dest,
                                        OpaqueValue *src,
                                        const Metadata *self);

/// Given an initialized object and an initialized object, move
/// the value from one to the other, leaving the source object
/// uninitialized.
///
/// This operation does not need to be safe aginst 'dest' and 'src' aliasing.
/// Therefore this can be decomposed as:
///
///   self->destroy(dest, self);
///   self->initializeWithTake(dest, src, self);
///
/// Returns the dest object.
///
/// Preconditions:
///   'src' is an initialized object
/// Postconditions:
///   'src' is an uninitialized object
/// Invariants:
///   'dest' is an initialized object
typedef OpaqueValue *assignWithTake(OpaqueValue *dest,
                                    OpaqueValue *src,
                                    const Metadata *self);

/// Given an uninitialized buffer, allocate an object.
///
/// Returns the uninitialized object.
///
/// Preconditions:
///   'buffer' is an uninitialized buffer
/// Postconditions:
///   'buffer' is an allocated buffer
typedef OpaqueValue *allocateBuffer(ValueBuffer *buffer,
                                    const Metadata *self);

  
/// Given an initialized object, return the metadata pointer for its dynamic
/// type.
///
/// Preconditions:
///   'src' is an initialized object
typedef const Metadata *typeOf(OpaqueValue *src,
                               const Metadata *self);
  
/// The number of bytes required to store an object of this type.
/// This value may be zero.  This value is not necessarily a
/// multiple of the alignment.
typedef size_t size;

/// Flags which apply to the type here.
typedef ValueWitnessFlags flags;

/// When allocating an array of objects of this type, the number of bytes
/// between array elements.  This value may be zero.  This value is always
/// a multiple of the alignment.
typedef size_t stride;
  
/// Store an extra inhabitant, named by a unique positive or zero index,
/// into the given uninitialized storage for the type.
typedef void storeExtraInhabitant(OpaqueValue *dest,
                                  int index,
                                  const Metadata *self);
  
/// Get the extra inhabitant index for the bit pattern stored at the given
/// address, or return -1 if there is a valid value at the address.
typedef int getExtraInhabitantIndex(const OpaqueValue *src,
                                    const Metadata *self);
  
/// Flags which describe extra inhabitants.
typedef ExtraInhabitantFlags extraInhabitantFlags;

} // end namespace value_witness_types

/// A standard routine, suitable for placement in the value witness
/// table, for copying an opaque POD object.
extern "C" OpaqueValue *swift_copyPOD(OpaqueValue *dest,
                                      OpaqueValue *src,
                                      const Metadata *self);

#define FOR_ALL_FUNCTION_VALUE_WITNESSES(MACRO) \
  MACRO(destroyBuffer) \
  MACRO(initializeBufferWithCopyOfBuffer) \
  MACRO(projectBuffer) \
  MACRO(deallocateBuffer) \
  MACRO(destroy) \
  MACRO(initializeBufferWithCopy) \
  MACRO(initializeWithCopy) \
  MACRO(assignWithCopy) \
  MACRO(initializeBufferWithTake) \
  MACRO(initializeWithTake) \
  MACRO(assignWithTake) \
  MACRO(allocateBuffer) \
  MACRO(typeOf)

/// A value-witness table.  A value witness table is built around
/// the requirements of some specific type.  The information in
/// a value-witness table is intended to be sufficient to lay out
/// and manipulate values of an arbitrary type.
struct ValueWitnessTable {
  // For the meaning of all of these witnesses, consult the comments
  // on their associated typedefs, above.

#define DECLARE_WITNESS(NAME) \
  value_witness_types::NAME *NAME;
  FOR_ALL_FUNCTION_VALUE_WITNESSES(DECLARE_WITNESS)
#undef DECLARE_WITNESS

  value_witness_types::size size;
  value_witness_types::flags flags;
  value_witness_types::stride stride;

  /// Would values of a type with the given layout requirements be
  /// allocated inline?
  static bool isValueInline(size_t size, size_t alignment) {
    return (size <= sizeof(ValueBuffer) &&
            alignment <= alignof(ValueBuffer));
  }

  /// Are values of this type allocated inline?
  bool isValueInline() const {
    return flags.isInlineStorage();
  }

  /// Is this type POD?
  bool isPOD() const {
    return flags.isPOD();
  }

  /// Return the size of this type.  Unlike in C, this has not been
  /// padded up to the alignment; that value is maintained as
  /// 'stride'.
  size_t getSize() const {
    return size;
  }

  /// Return the stride of this type.  This is the size rounded up to
  /// be a multiple of the alignment.
  size_t getStride() const {
    return stride;
  }

  /// Return the alignment required by this type, in bytes.
  size_t getAlignment() const {
    return flags.getAlignment();
  }

  /// The alignment mask of this type.  An offset may be rounded up to
  /// the required alignment by adding this mask and masking by its
  /// bit-negation.
  ///
  /// For example, if the type needs to be 8-byte aligned, the value
  /// of this witness is 0x7.
  size_t getAlignmentMask() const {
    return flags.getAlignmentMask();
  }
  
  /// The number of extra inhabitants, that is, bit patterns that do not form
  /// valid values of the type, in this type's binary representation.
  unsigned getNumExtraInhabitants() const;
};
  
/// A value-witness table with extra inhabitants entry points.
/// These entry points are available only if the HasExtraInhabitants flag bit is
/// set in the 'flags' field.
struct ExtraInhabitantsValueWitnessTable : ValueWitnessTable {
  value_witness_types::storeExtraInhabitant *storeExtraInhabitant;
  value_witness_types::getExtraInhabitantIndex *getExtraInhabitantIndex;
  value_witness_types::extraInhabitantFlags extraInhabitantFlags;
  
  constexpr ExtraInhabitantsValueWitnessTable()
    : ValueWitnessTable{}, storeExtraInhabitant(nullptr),
      getExtraInhabitantIndex(nullptr), extraInhabitantFlags() {}
  constexpr ExtraInhabitantsValueWitnessTable(const ValueWitnessTable &base,
                            value_witness_types::storeExtraInhabitant *sei,
                            value_witness_types::getExtraInhabitantIndex *geii,
                            value_witness_types::extraInhabitantFlags eif)
    : ValueWitnessTable(base), storeExtraInhabitant(sei),
      getExtraInhabitantIndex(geii), extraInhabitantFlags(eif) {}
};
  
inline unsigned ValueWitnessTable::getNumExtraInhabitants() const {
  // If the table does not have extra inhabitant witnesses, then there are zero.
  if (!flags.hasExtraInhabitants())
    return 0;
  return static_cast<const ExtraInhabitantsValueWitnessTable &>(*this)
    .extraInhabitantFlags
    .getNumExtraInhabitants();
}

// Standard value-witness tables.

// The "Int" tables are used for arbitrary POD data with the matching
// size/alignment characteristics.
extern "C" const ValueWitnessTable _TWVBi8_;      // Builtin.Int8
extern "C" const ValueWitnessTable _TWVBi16_;     // Builtin.Int16
extern "C" const ValueWitnessTable _TWVBi32_;     // Builtin.Int32
extern "C" const ValueWitnessTable _TWVBi64_;     // Builtin.Int64

// The object-pointer table can be used for arbitrary Swift refcounted
// pointer types.
extern "C" const ValueWitnessTable _TWVBo;        // Builtin.ObjectPointer

// The ObjC-pointer table can be used for arbitrary ObjC pointer types.
extern "C" const ValueWitnessTable _TWVBO;        // Builtin.ObjCPointer

// The () -> () table can be used for arbitrary function types.
extern "C" const ValueWitnessTable _TWVFT_T_;     // () -> ()

// The () table can be used for arbitrary empty types.
extern "C" const ValueWitnessTable _TWVT_;        // ()
  
/// Return the value witnesses for unmanaged pointers.
static inline const ValueWitnessTable &getUnmanagedPointerValueWitnesses() {
#ifdef __LP64__
  return _TWVBi64_;
#else
  return _TWVBi32_;
#endif
}

/// The header before a metadata object which appears on all type
/// metadata.  Note that heap metadata are not necessarily type
/// metadata, even for objects of a heap type: for example, objects of
/// Objective-C type possess a form of heap metadata (an Objective-C
/// Class pointer), but this metadata lacks the type metadata header.
/// This case can be distinguished using the isTypeMetadata() flag
/// on ClassMetadata.
struct TypeMetadataHeader {
  /// A pointer to the value-witnesses for this type.  This is only
  /// present for type metadata.
  const ValueWitnessTable *ValueWitnesses;
};

/// A "full" metadata pointer is simply an adjusted address point on a
/// metadata object; it points to the beginning of the metadata's
/// allocation, rather than to the canonical address point of the
/// metadata object.
template <class T> struct FullMetadata : T::HeaderType, T {
  typedef typename T::HeaderType HeaderType;

  FullMetadata() = default;
  constexpr FullMetadata(const HeaderType &header, const T &metadata)
    : HeaderType(header), T(metadata) {}
};

/// Given a canonical metadata pointer, produce the adjusted metadata pointer.
template <class T>
static inline FullMetadata<T> *asFullMetadata(T *metadata) {
  return (FullMetadata<T>*) (((typename T::HeaderType*) metadata) - 1);
}
template <class T>
static inline const FullMetadata<T> *asFullMetadata(const T *metadata) {
  return asFullMetadata(const_cast<T*>(metadata));
}

// std::result_of is busted in Xcode 5. This is a simplified reimplementation
// that isn't SFINAE-safe.
namespace {
  template<typename T> struct _ResultOf;
  
  template<typename R, typename...A>
  struct _ResultOf<R(A...)> {
    using type = R;
  };
}
  
/// The common structure of all type metadata.
struct Metadata {
  constexpr Metadata() : Kind(MetadataKind::Class) {}
  constexpr Metadata(MetadataKind Kind) : Kind(Kind) {}
  
  /// The basic header type.
  typedef TypeMetadataHeader HeaderType;

private:
  /// The kind. Only valid for non-class metadata; getKind() must be used to get
  /// the kind value.
  MetadataKind Kind;
public:
  /// Get the metadata kind.
  MetadataKind getKind() const {
    if (Kind > MetadataKind::MetadataKind_Last)
      return MetadataKind::Class;
    return Kind;
  }
  
  /// Set the metadata kind.
  void setKind(MetadataKind kind) {
    Kind = kind;
  }

  /// Is this metadata for a class type?
  bool isClassType() const {
    return Kind > MetadataKind::MetadataKind_Last
      || Kind == MetadataKind::Class;
  }

  const ValueWitnessTable *getValueWitnesses() const {
    return asFullMetadata(this)->ValueWitnesses;
  }

  void setValueWitnesses(const ValueWitnessTable *table) {
    asFullMetadata(this)->ValueWitnesses = table;
  }
  
  // Define forwarders for value witnesses. These invoke this metadata's value
  // witness table with itself as the 'self' parameter.
  #define FORWARD_WITNESS(WITNESS)                                         \
    template<typename...A>                                                 \
    _ResultOf<value_witness_types::WITNESS>::type                          \
    vw_##WITNESS(A &&...args) const {                                      \
      return getValueWitnesses()->WITNESS(std::forward<A>(args)..., this); \
    }
  FOR_ALL_FUNCTION_VALUE_WITNESSES(FORWARD_WITNESS)
  #undef FORWARD_WITNESS
  
protected:
  friend struct OpaqueMetadata;
  
  /// Metadata should not be publicly copied or moved.
  constexpr Metadata(const Metadata &) = default;
  Metadata &operator=(const Metadata &) = default;
  constexpr Metadata(Metadata &&) = default;
  Metadata &operator=(Metadata &&) = default;
};

/// The common structure of opaque metadata.  Adds nothing.
struct OpaqueMetadata {
  typedef TypeMetadataHeader HeaderType;

  // We have to represent this as a member so we can list-initialize it.
  Metadata base;
};

// Standard POD opaque metadata.
// The "Int" metadata are used for arbitrary POD data with the
// matching characteristics.
typedef FullMetadata<OpaqueMetadata> FullOpaqueMetadata;
extern "C" const FullOpaqueMetadata _TMdBi8_;      // Builtin.Int8
extern "C" const FullOpaqueMetadata _TMdBi16_;     // Builtin.Int16
extern "C" const FullOpaqueMetadata _TMdBi32_;     // Builtin.Int32
extern "C" const FullOpaqueMetadata _TMdBi64_;     // Builtin.Int64
extern "C" const FullOpaqueMetadata _TMdBo;        // Builtin.ObjectPointer
extern "C" const FullOpaqueMetadata _TMdBO;        // Builtin.ObjCPointer
  
// FIXME: The compiler should generate this.
extern "C" const FullOpaqueMetadata _TMdSb;        // swift.Bool

/// The prefix on a heap metadata.
struct HeapMetadataHeaderPrefix {
  /// Destroy the object, returning the allocated size of the object
  /// or 0 if the object shouldn't be deallocated.
  void (*destroy)(HeapObject *);
};

/// The header present on all heap metadata.
struct HeapMetadataHeader : HeapMetadataHeaderPrefix, TypeMetadataHeader {
  constexpr HeapMetadataHeader(const HeapMetadataHeaderPrefix &heapPrefix,
                               const TypeMetadataHeader &typePrefix)
    : HeapMetadataHeaderPrefix(heapPrefix), TypeMetadataHeader(typePrefix) {}
};

/// The common structure of all metadata for heap-allocated types.  A
/// pointer to one of these can be retrieved by loading the 'isa'
/// field of any heap object, whether it was managed by Swift or by
/// Objective-C.  However, when loading from an Objective-C object,
/// this metadata may not have the heap-metadata header, and it may
/// not be the Swift type metadata for the object's dynamic type.
struct HeapMetadata : Metadata {
  typedef HeapMetadataHeader HeaderType;

  HeapMetadata() = default;
  constexpr HeapMetadata(const Metadata &base) : Metadata(base) {}
};

/// Header for a generic parameter descriptor. This is a variable-sized
/// structure that describes how to find and parse a generic parameter vector
/// within
struct GenericParameterDescriptor {
  /// The offset of the descriptor in the metadata record. If NumParams is zero,
  /// this value is meaningless.
  uintptr_t Offset;
  /// The number of type parameters. A value of zero means there is no generic
  /// parameter vector.
  uintptr_t NumParams;
  
  /// True if the nominal type has generic parameters.
  bool hasGenericParams() const { return NumParams > 0; }
  
  /// A type parameter.
  struct Parameter {
    /// The number of protocol witness tables required by this type parameter.
    uintptr_t NumWitnessTables;
    
    // TODO: This is the bare minimum to be able to parse an opaque generic
    // parameter vector. Should we include additional info, such as the
    // required protocols?
  };

  /// The parameter descriptors are in a tail-emplaced array of NumParams
  /// elements.
  Parameter Parameters[1];
};
  
struct ClassTypeDescriptor;
struct StructTypeDescriptor;
struct EnumTypeDescriptor;

/// Common information about all nominal types. For generic types, this
/// descriptor is shared for all instantiations of the generic type.
struct NominalTypeDescriptor {
  /// The kind of nominal type descriptor.
  NominalTypeKind Kind;
  /// The mangled name of the nominal type, with no generic parameters.
  const char *Name;
  
  /// The following fields are kind-dependent.
  union {
    /// Information about class types.
    struct {
      /// The number of stored properties in the class, not including its
      /// superclasses. If there is a field offset vector, this is its length.
      uintptr_t NumFields;
      /// The offset of the field offset vector for this class's stored
      /// properties in its metadata, if any. 0 means there is no field offset
      /// vector.
      uintptr_t FieldOffsetVectorOffset;
      
      /// True if metadata records for this type have a field offset vector for
      /// its stored properties.
      bool hasFieldOffsetVector() const { return FieldOffsetVectorOffset != 0; }
      
      /// The field names. A doubly-null-terminated list of strings, whose
      /// length and order is consistent with that of the field offset vector.
      const char *FieldNames;
    } Class;
    
    /// Information about struct types.
    struct {
      /// The number of stored properties in the class, not including its
      /// superclasses. If there is a field offset vector, this is its length.
      uintptr_t NumFields;
      /// The offset of the field offset vector for this class's stored
      /// properties in its metadata, if any. 0 means there is no field offset
      /// vector.
      uintptr_t FieldOffsetVectorOffset;
      
      /// True if metadata records for this type have a field offset vector for
      /// its stored properties.
      bool hasFieldOffsetVector() const { return FieldOffsetVectorOffset != 0; }
      
      /// The field names. A doubly-null-terminated list of strings, whose
      /// length and order is consistent with that of the field offset vector.
      const char *FieldNames;
    } Struct;
    
    /// Information about enum types.
    struct {
      /// The number of non-empty cases in the enum.
      uintptr_t NumNonEmptyCases;
      /// The number of empty cases in the enum.
      uintptr_t NumEmptyCases;
      /// The names of the cases. A doubly-null-terminated list of strings,
      /// whose length is NumNonEmptyCases + NumEmptyCases. Cases are named in
      /// tag order, non-empty cases first, followed by empty cases.
      const char *CaseNames;
    } Enum;
  };
  
  /// The generic parameter descriptor header. This describes how to find and
  /// parse the generic parameter vector in metadata records for this nominal
  /// type.
  GenericParameterDescriptor GenericParams;
  
  // NOTE: GenericParams ends with a tail-allocated array, so it cannot be
  // followed by additional fields.
};

/// The structure of all class metadata.  This structure is embedded
/// directly within the class's heap metadata structure and therefore
/// cannot be extended without an ABI break.
///
/// Note that the layout of this type is compatible with the layout of
/// an Objective-C class.
struct ClassMetadata : public HeapMetadata {
  ClassMetadata() = default;
  constexpr ClassMetadata(const HeapMetadata &base,
                          const ClassMetadata *superClass,
                          uintptr_t data,
                          const NominalTypeDescriptor *description,
                          uintptr_t size, uintptr_t alignMask)
    : HeapMetadata(base), SuperClass(superClass),
      CacheData{nullptr, nullptr}, Data(data),
      Description(description),
      InstanceSize(size), InstanceAlignMask(alignMask) {}

  /// The metadata for the super class.  This is null for the root class.
  const ClassMetadata *SuperClass;

  /// The cache data is used for certain dynamic lookups; it is owned
  /// by the runtime and generally needs to interoperate with
  /// Objective-C's use.
  void *CacheData[2];

  /// The data pointer is used for out-of-line metadata and is
  /// generally opaque, except that the compiler sets the low bit in
  /// order to indicate that this is a Swift metatype and therefore
  /// that the type metadata header is present.
  uintptr_t Data;

  /// Is this object a valid swift type metadata?
  bool isTypeMetadata() const {
    return Data & 1;
  }

  /// An out-of-line Swift-specific description of the type.
  const NominalTypeDescriptor *Description;
  
  /// The size and alignment mask of instances of this type.
  uintptr_t InstanceSize, InstanceAlignMask;

  // After this come the class members, laid out as follows:
  //   - class members for the superclass (recursively)
  //   - metadata reference for the parent, if applicable
  //   - generic parameters for this class
  //   - class variables (if we choose to support these)
  //   - "tabulated" virtual methods
};

/// The structure of metadata for heap-allocated local variables.
/// This is non-type metadata.
///
/// It would be nice for tools to be able to dynamically discover the
/// type of a heap-allocated local variable.  This should not require
/// us to aggressively produce metadata for the type, though.  The
/// obvious solution is to simply place the mangling of the type after
/// the variable metadata.
///
/// One complication is that, in generic code, we don't want something
/// as low-priority (sorry!) as the convenience of tools to force us
/// to generate per-instantiation metadata for capturing variables.
/// In these cases, the heap-destructor function will be using
/// information stored in the allocated object (rather than in
/// metadata) to actually do the work of destruction, but even then,
/// that information needn't be metadata for the actual variable type;
/// consider the case of local variable of type (T, Int).
///
/// Anyway, that's all something to consider later.
struct HeapLocalVariableMetadata : public HeapMetadata {
  // No extra fields for now.
};

/// The structure of metadata for heap-allocated arrays.
/// This is non-type metadata.
///
/// The comments on HeapLocalVariableMetadata about tools wanting type
/// discovery apply equally here.
struct HeapArrayMetadata : public HeapMetadata {
  // No extra fields for now.
};

/// The structure of wrapper metadata for Objective-C classes.  This
/// is used as a type metadata pointer when the actual class isn't
/// Swift-compiled.
struct ObjCClassWrapperMetadata : public Metadata {
  const ClassMetadata *Class;
};

/// The structure of type metadata for structs.
struct StructMetadata : public Metadata {
  /// An out-of-line description of the type.
  const NominalTypeDescriptor *Description;

  /// The parent type of this member type, or null if this is not a
  /// member type.
  const Metadata *Parent;

  // This is followed by the generics information, if this type is generic.
};

/// The structure of function type metadata.
struct FunctionTypeMetadata : public Metadata {
  /// The type metadata for the argument type.
  const Metadata *ArgumentType;

  /// The type metadata for the result type.
  const Metadata *ResultType;
};

/// The structure of metadata for metatypes.
struct MetatypeMetadata : public Metadata {
  /// The type metadata for the element.
  const Metadata *InstanceType;
};

/// The structure of tuple type metadata.
struct TupleTypeMetadata : public Metadata {

  TupleTypeMetadata() = default;
  constexpr TupleTypeMetadata(const Metadata &base,
                              size_t numElements,
                              const char *labels)
    : Metadata(base), NumElements(numElements), Labels(labels) {}

  /// The number of elements.
  size_t NumElements;

  /// The labels string;  see swift_getTupleTypeMetadata.
  const char *Labels;

  struct Element {
    /// The type of the element.
    const Metadata *Type;

    /// The offset of the tuple element within the tuple.
    size_t Offset;

    OpaqueValue *findIn(OpaqueValue *tuple) const {
      return (OpaqueValue*) (((char*) tuple) + Offset);
    }
  };

  Element *getElements() {
    return reinterpret_cast<Element*>(this+1);
  }
  const Element *getElements() const {
    return reinterpret_cast<const Element *>(this+1);
  }
};
  
/// The standard metadata for the empty tuple type.
extern "C" const FullMetadata<TupleTypeMetadata> _TMdT_;

struct ProtocolDescriptor;
  
/// An array of protocol descriptors with a header and tail-allocated elements.
struct ProtocolDescriptorList {
  uintptr_t NumProtocols;

  const ProtocolDescriptor **getProtocols() {
    return reinterpret_cast<const ProtocolDescriptor **>(this + 1);
  }
  
  const ProtocolDescriptor * const *getProtocols() const {
    return reinterpret_cast<const ProtocolDescriptor * const *>(this + 1);
  }
  
  const ProtocolDescriptor *operator[](size_t i) const {
    return getProtocols()[i];
  }
  
  const ProtocolDescriptor *&operator[](size_t i) {
    return getProtocols()[i];
  }

  constexpr ProtocolDescriptorList() : NumProtocols(0) {}
  
protected:
  constexpr ProtocolDescriptorList(uintptr_t NumProtocols)
    : NumProtocols(NumProtocols) {}
};
  
/// A literal class for creating constant protocol descriptors in the runtime.
template<uintptr_t NUM_PROTOCOLS>
struct LiteralProtocolDescriptorList : ProtocolDescriptorList {
  const ProtocolDescriptorList *Protocols[NUM_PROTOCOLS];
  
  template<typename...DescriptorPointers>
  constexpr LiteralProtocolDescriptorList(DescriptorPointers...elements)
    : ProtocolDescriptorList(NUM_PROTOCOLS), Protocols{elements...}
  {}
};
  
/// Flag that indicates whether an existential type is class-constrained or not.
enum class ProtocolClassConstraint : bool {
  /// The protocol is class-constrained, so only class types can conform to it.
  Class = false,
  /// Any type can conform to the protocol.
  Any = true,
};
  
/// Flags for protocol descriptors.
class ProtocolDescriptorFlags {
  typedef uint32_t int_type;
  enum : int_type {
    IsSwift           = 1U <<  0U,
    ClassConstraint   = 1U <<  1U,
    NeedsWitnessTable = 1U <<  2U,
    /// Reserved by the ObjC runtime.
    _ObjC_FixedUp     = 1U << 31U,
  };

  int_type Data;
  
  constexpr ProtocolDescriptorFlags(int_type Data) : Data(Data) {}
public:
  constexpr ProtocolDescriptorFlags() : Data(0) {}
  constexpr ProtocolDescriptorFlags withSwift(bool s) const {
    return ProtocolDescriptorFlags((Data & ~IsSwift) | (s ? IsSwift : 0));
  }
  constexpr ProtocolDescriptorFlags withClassConstraint(
                                              ProtocolClassConstraint c) const {
    return ProtocolDescriptorFlags((Data & ~ClassConstraint)
                                     | (bool(c) ? ClassConstraint : 0));
  }
  constexpr ProtocolDescriptorFlags withNeedsWitnessTable(bool n) const {
    return ProtocolDescriptorFlags((Data & ~NeedsWitnessTable)
                                     | (n ? NeedsWitnessTable : 0));
  }
  
  /// Was the protocol defined in Swift?
  bool isSwift() const { return Data & IsSwift; }
  /// Is the protocol class-constrained?
  ProtocolClassConstraint getClassConstraint() const {
    return ProtocolClassConstraint(bool(Data & ClassConstraint));
  }
  /// Does the protocol require a witness table for method dispatch?
  bool needsWitnessTable() const { return Data & NeedsWitnessTable; }
};
  
/// A protocol descriptor. This is not type metadata, but is referenced by
/// existential type metadata records to describe a protocol constraint.
/// Its layout is compatible with the Objective-C runtime's 'protocol_t' record
/// layout.
struct ProtocolDescriptor {
  /// Unused by the Swift runtime.
  const void *_ObjC_Isa;
  
  /// The mangled name of the protocol.
  const char *Name;
  
  /// The list of protocols this protocol refines.
  const ProtocolDescriptorList *InheritedProtocols;
  
  /// Unused by the Swift runtime.
  const void *_ObjC_InstanceMethods, *_ObjC_ClassMethods,
             *_ObjC_OptionalInstanceMethods, *_ObjC_OptionalClassMethods,
             *_ObjC_InstanceProperties;
  
  /// Size of the descriptor record.
  uint32_t DescriptorSize;
  
  /// Additional flags.
  ProtocolDescriptorFlags Flags;
  
  constexpr ProtocolDescriptor(const char *Name,
                               const ProtocolDescriptorList *Inherited,
                               ProtocolDescriptorFlags Flags)
    : _ObjC_Isa(nullptr), Name(Name), InheritedProtocols(Inherited),
      _ObjC_InstanceMethods(nullptr), _ObjC_ClassMethods(nullptr),
      _ObjC_OptionalInstanceMethods(nullptr),
      _ObjC_OptionalClassMethods(nullptr),
      _ObjC_InstanceProperties(nullptr),
      DescriptorSize(sizeof(ProtocolDescriptor)),
      Flags(Flags)
  {}
};
  
/// Flags in an existential type metadata record.
class ExistentialTypeFlags {
  typedef size_t int_type;
  enum : int_type {
    NumWitnessTablesMask  = 0x7FFFFFFFU,
    ClassConstraintMask = 0x80000000U,
  };
  int_type Data;

  constexpr ExistentialTypeFlags(int_type Data) : Data(Data) {}
public:
  constexpr ExistentialTypeFlags() : Data(0) {}
  constexpr ExistentialTypeFlags withNumWitnessTables(unsigned numTables) const {
    return ExistentialTypeFlags((Data & ~NumWitnessTablesMask) | numTables);
  }
  constexpr ExistentialTypeFlags
  withClassConstraint(ProtocolClassConstraint c) const {
    return ExistentialTypeFlags((Data & ~ClassConstraintMask)
                                  | (bool(c) ? ClassConstraintMask : 0));
  }
  
  unsigned getNumWitnessTables() const {
    return Data & NumWitnessTablesMask;
  }
  
  ProtocolClassConstraint getClassConstraint() const {
    return ProtocolClassConstraint(bool(Data & ClassConstraintMask));
  }
};

/// The structure of existential type metadata.
struct ExistentialTypeMetadata : public Metadata {
  /// The number of witness tables and class-constrained-ness of the type.
  ExistentialTypeFlags Flags;
  /// The protocol constraints.
  ProtocolDescriptorList Protocols;
  
  /// NB: Protocols has a tail-emplaced array; additional fields cannot follow.
  
  constexpr ExistentialTypeMetadata()
    : Metadata{MetadataKind::Existential},
      Flags(ExistentialTypeFlags()), Protocols() {}
};

/// \brief The header in front of a generic metadata template.
///
/// This is optimized so that the code generation pattern
/// requires the minimal number of independent arguments.
/// For example, we want to be able to allocate a generic class
/// Dictionary<T,U> like so:
///   extern GenericMetadata Dictionary_metadata_header;
///   void *arguments[] = { typeid(T), typeid(U) };
///   void *metadata = swift_getGenericMetadata(&Dictionary_metadata_header,
///                                             &arguments);
///   void *object = swift_allocObject(metadata);
///
/// Note that the metadata header is *not* const data; it includes 8
/// pointers worth of implementation-private data.
///
/// Both the metadata header and the arguments buffer are guaranteed
/// to be pointer-aligned.
struct GenericMetadata {
  /// The fill function. Receives a pointer to the instantiated metadata and
  /// the argument pointer passed to swift_getGenericMetadata.
  void (*FillFunction)(void *metadata, const void *arguments);
  
  /// The size of the template in bytes.
  uint32_t MetadataSize;

  /// The number of generic arguments that we need to unique on,
  /// in words.  The first 'NumArguments * sizeof(void*)' bytes of
  /// the arguments buffer are the key. There may be additional private-contract
  /// data used by FillFunction not used for uniquing.
  uint16_t NumKeyArguments;

  /// The offset of the address point in the template in bytes.
  uint16_t AddressPoint;

  /// Data that the runtime can use for its own purposes.  It is guaranteed
  /// to be zero-filled by the compiler.
  void *PrivateData[8];

  // Here there is a variably-sized field:
  // char alignas(void*) MetadataTemplate[MetadataSize];

  /// Return the starting address of the metadata template data.
  const void *getMetadataTemplate() const {
    return reinterpret_cast<const void *>(this + 1);
  }
};

/// \brief Fetch a uniqued metadata object for a generic nominal type.
///
/// The basic algorithm for fetching a metadata object is:
///   func swift_getGenericMetadata(header, arguments) {
///     if (metadata = getExistingMetadata(&header.PrivateData,
///                                        arguments[0..header.NumArguments]))
///       return metadata
///     metadata = malloc(header.MetadataSize)
///     memcpy(metadata, header.MetadataTemplate, header.MetadataSize)
///     for (i in 0..header.NumFillInstructions)
///       metadata[header.FillInstructions[i].ToIndex]
///         = arguments[header.FillInstructions[i].FromIndex]
///     setExistingMetadata(&header.PrivateData,
///                         arguments[0..header.NumArguments],
///                         metadata)
///     return metadata
///   }
extern "C" const Metadata *
swift_getGenericMetadata(GenericMetadata *pattern,
                         const void *arguments);

/// \brief Fetch a uniqued metadata for a function type.
extern "C" const FunctionTypeMetadata *
swift_getFunctionTypeMetadata(const Metadata *argMetadata,
                              const Metadata *resultMetadata);

/// \brief Fetch a uniqued type metadata for an ObjC class.
extern "C" const Metadata *
swift_getObjCClassMetadata(const ClassMetadata *theClass);

/// \brief Fetch a uniqued metadata for a tuple type.
///
/// The labels argument is null if and only if there are no element
/// labels in the tuple.  Otherwise, it is a null-terminated
/// concatenation of space-terminated NFC-normalized UTF-8 strings,
/// assumed to point to constant global memory.
///
/// That is, for the tuple type (a : Int, Int, c : Int), this
/// argument should be:
///   "a  c \0"
///
/// This representation allows label strings to be efficiently
/// (1) uniqued within a linkage unit and (2) compared with strcmp.
/// In other words, it's optimized for code size and uniquing
/// efficiency, not for the convenience of actually consuming
/// these strings.
///
/// \param elements - potentially invalid if numElements is zero;
///   otherwise, an array of metadata pointers.
/// \param labels - the labels string
/// \param proposedWitnesses - an optional proposed set of value witnesses.
///   This is useful when working with a non-dependent tuple type
///   where the entrypoint is just being used to unique the metadata.
extern "C" const TupleTypeMetadata *
swift_getTupleTypeMetadata(size_t numElements,
                           const Metadata * const *elements,
                           const char *labels,
                           const ValueWitnessTable *proposedWitnesses);

extern "C" const TupleTypeMetadata *
swift_getTupleTypeMetadata2(const Metadata *elt0, const Metadata *elt1,
                            const char *labels,
                            const ValueWitnessTable *proposedWitnesses);
extern "C" const TupleTypeMetadata *
swift_getTupleTypeMetadata3(const Metadata *elt0, const Metadata *elt1,
                            const Metadata *elt2, const char *labels,
                            const ValueWitnessTable *proposedWitnesses);

/// Initialize the value witness table and struct field offset vector for a
/// struct, using the "Universal" layout strategy.
extern "C" void swift_initStructMetadata_UniversalStrategy(size_t numFields,
                                         const Metadata * const *fieldTypes,
                                         size_t *fieldOffsets,
                                         ValueWitnessTable *vwtable);

/// Initialize the field offset vector for a dependent-layout class, using the
/// "Universal" layout strategy.
extern "C" void swift_initClassMetadata_UniversalStrategy(ClassMetadata *self,
                                            const ClassMetadata *super,
                                            size_t numFields,
                                            const Metadata * const *fieldTypes,
                                            size_t *fieldOffsets);
  
/// \brief Fetch a uniqued metadata for a metatype type.
extern "C" const MetatypeMetadata *
swift_getMetatypeMetadata(const Metadata *instanceType);

/// \brief Fetch a uniqued metadata for an existential type. The array
/// referenced by \c protocols will be sorted in-place.
extern "C" const ExistentialTypeMetadata *
swift_getExistentialMetadata(size_t numProtocols,
                             const ProtocolDescriptor **protocols);
  
/// \brief Checked dynamic cast to a class type.
///
/// \param object The object to cast.
/// \param targetType The type to which we are casting, which is known to be
/// a class type.
///
/// \returns the object if the cast succeeds, or null otherwise.
extern "C" const void *
swift_dynamicCastClass(const void *object, const ClassMetadata *targetType);

/// \brief Unconditional, checked dynamic cast to a class type.
///
/// Aborts if the object isn't of the target type.
///
/// \param object The object to cast.
/// \param targetType The type to which we are casting, which is known to be
/// a class type.
///
/// \returns the object.
extern "C" const void *
swift_dynamicCastClassUnconditional(const void *object,
                                    const ClassMetadata *targetType);

/// \brief Checked Objective-C-style dynamic cast to a class type.
///
/// \param object The object to cast, or nil.
/// \param targetType The type to which we are casting, which is known to be
/// a class type.
///
/// \returns the object if the cast succeeds, or null otherwise.
extern "C" const void *
swift_dynamicCastObjCClass(const void *object, const ClassMetadata *targetType);

/// \brief Unconditional, checked, Objective-C-style dynamic cast to a class
/// type.
///
/// Aborts if the object isn't of the target type.
/// Note that unlike swift_dynamicCastClassUnconditional, this does not abort
/// if the object is 'nil'.
///
/// \param object The object to cast, or nil.
/// \param targetType The type to which we are casting, which is known to be
/// a class type.
///
/// \returns the object.
extern "C" const void *
swift_dynamicCastObjCClassUnconditional(const void *object,
                                        const ClassMetadata *targetType);

/// \brief Checked dynamic cast of a class instance pointer to the given type.
///
/// \param object The class instance to cast.
///
/// \param targetType The type to which we are casting, which may be either a
/// class type or a wrapped Objective-C class type.
///
/// \returns the object, or null if it doesn't have the given target type.
extern "C" const void *
swift_dynamicCast(const void *object, const Metadata *targetType);

/// \brief Unconditional checked dynamic cast of a class instance pointer to
/// the given type.
///
/// Aborts if the object isn't of the target type.
///
/// \param object The class instance to cast.
///
/// \param targetType The type to which we are casting, which may be either a
/// class type or a wrapped Objective-C class type.
///
/// \returns the object.
extern "C" const void *
swift_dynamicCastUnconditional(const void *object,
                               const Metadata *targetType);

/// \brief Checked dynamic cast of an opaque value to the given type.
///
/// \param value Pointer to the value to cast.
///
/// \param sourceType The original static type of the value.
///
/// \param targetType The type to which we are casting, which may be any Swift
/// type metadata pointer.
extern "C" const OpaqueValue *
swift_dynamicCastIndirect(const OpaqueValue *value,
                          const Metadata *sourceType,
                          const Metadata *targetType);

/// \brief Unconditional checked dynamic cast of an opaque value to the given
/// type.
///
/// \param value Pointer to the value to cast.
///
/// \param sourceType The original static type of the value.
///
/// \param targetType The type to which we are casting, which may be any Swift
/// type metadata pointer.
extern "C" const OpaqueValue *
swift_dynamicCastIndirectUnconditional(const OpaqueValue *value,
                                       const Metadata *sourceType,
                                       const Metadata *targetType);

/// \brief Standard 'typeof' value witness for types with static metatypes.
///
/// \param obj  A pointer to the object. Ignored.
/// \param self The type metadata for the object.
///
/// \returns self.
extern "C" const Metadata *
swift_staticTypeof(OpaqueValue *obj, const Metadata *self);

/// \brief Standard 'typeof' value witness for heap object references.
///
/// \param obj  A pointer to the object reference.
/// \param self The static type metadata for the object. Ignored.
///
/// \returns The dynamic type metadata for the object.
extern "C" const Metadata *
swift_objectTypeof(OpaqueValue *obj, const Metadata *self);

/// \brief Standard 'typeof' value witness for ObjC object references.
///
/// \param obj  A pointer to the object reference.
/// \param self The static type metadata for the object. Ignored.
///
/// \returns The dynamic type metadata for the object.
extern "C" const Metadata *
swift_objcTypeof(OpaqueValue *obj, const Metadata *self);
  
} // end namespace swift

#endif /* SWIFT_RUNTIME_METADATA_H */
