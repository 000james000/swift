//===--- LValue.h - Logical LValue Representation ---------------*- C++ -*-===//
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
// A storage structure for keeping track of logical lvalues during SILGen.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_LOWERING_LVALUE_H
#define SWIFT_LOWERING_LVALUE_H

#include "SILGen.h"
#include "swift/Basic/DiverseList.h"

namespace swift {
namespace Lowering {
  class SILGenFunction;
  class ManagedValue;

class PhysicalPathComponent;
class LogicalPathComponent;

/// An l-value path component represents a chunk of the access path to
/// an object.  Path components may be either "physical" or "logical".
/// A physical path involves elementary address manipulations; these
/// address manipulations may be in some way dynamic, but they are
/// ultimately just pointer arithmetic.  A logical path requires
/// getter/setter logic.
///
/// This divide between physical/logical is closely related to the
/// fragile/resilient split, with two primary differences:
///   - Any sort of implementation can be fragile.  For example, a
///     computed field can still be fragile, meaning that it is known
///     to be implemented with a getter/setter.  The known
///     implementation must be a direct offset in order to qualify as
///     physical.
///   - A path component's implementation can be resilient and yet
///     still qualify for physical access if we are in a privileged
///     component.
class PathComponent {
  friend class LValue;
  unsigned AllocatedSize : 31;
  const unsigned IsPhysical : 1;

  // This anchor method serves three purposes: it aligns the class to
  // a pointer boundary, it makes the class a primary base so that
  // subclasses will be at offset zero, and it anchors the v-table
  // to a specific file.
  virtual void _anchor();

protected:
  PathComponent(bool isPhysical)
    : IsPhysical(isPhysical) {}

  virtual ~PathComponent() {}

public:
  /// Returns sizeof(the final type), plus any extra storage required.
  size_t allocated_size() const { return AllocatedSize; }

  /// Is this component physical or logical?  If physical, this will
  /// be a subclass of PhysicalPathComponent.  If logical, this will
  /// be a subclass of LogicalPathComponent.
  bool isPhysical() const { return IsPhysical; }
  bool isLogical() const { return !IsPhysical; }

  // These are implemented inline after the respective class declarations.

  PhysicalPathComponent &asPhysical();
  const PhysicalPathComponent &asPhysical() const;

  LogicalPathComponent &asLogical();
  const LogicalPathComponent &asLogical() const;
  
  /// Returns the type of the value addressed by the component.
  virtual Type getObjectType() const = 0;
};

/// An abstract class for "physical" path components, i.e. path
/// components that can be accessed as address manipulations.  See the
/// comment for PathComponent for more information.
class PhysicalPathComponent : public PathComponent {
  virtual void _anchor();

protected:
  PhysicalPathComponent() : PathComponent(true) {}

public:
  virtual SILValue offset(SILGenFunction &gen,
                       SILLocation loc,
                       SILValue base) const = 0;
};

inline PhysicalPathComponent &PathComponent::asPhysical() {
  assert(isPhysical());
  return static_cast<PhysicalPathComponent&>(*this);
}
inline const PhysicalPathComponent &PathComponent::asPhysical() const {
  assert(isPhysical());
  return static_cast<const PhysicalPathComponent&>(*this);
}

/// An abstract class for "logical" path components, i.e. path
/// components that require getter/setter methods to access.  See the
/// comment for PathComponent for more information.
class LogicalPathComponent : public PathComponent {
  virtual void _anchor();

protected:
  LogicalPathComponent() : PathComponent(false) {}

public:
  /// Clone the path component onto the heap.
  virtual std::unique_ptr<LogicalPathComponent>
  clone(SILGenFunction &gen) const = 0;
  
  /// True if the property is settable.
  virtual bool isSettable() const = 0;
  
  /// Set the property.
  virtual void set(SILGenFunction &gen, SILLocation loc,
                   RValue &&rvalue, SILValue base) const = 0;

  /// Get the property.
  virtual ManagedValue get(SILGenFunction &gen, SILLocation loc,
                           SILValue base, SGFContext c) const = 0;
  
  /// Get the property, materialize a temporary lvalue for it, and if
  /// we're in a writeback scope, register a writeback.
  Materialize getMaterialized(SILGenFunction &gen, SILLocation loc,
                              SILValue base) const;
};

inline LogicalPathComponent &PathComponent::asLogical() {
  assert(isLogical());
  return static_cast<LogicalPathComponent&>(*this);
}
inline const LogicalPathComponent &PathComponent::asLogical() const {
  assert(isLogical());
  return static_cast<const LogicalPathComponent&>(*this);
}

/// An lvalue represents a reference to storage holding a value
/// of a type, as opposed to an rvalue, which is an actual value
/// of the type.
class LValue {
  DiverseList<PathComponent, 128> Path;
  Type ObjectType;

public:
  LValue() = default;
  LValue(const LValue &other) = default;
  LValue(LValue &&other) = default;

  bool isValid() const { return !Path.empty(); }

  /// Is this lvalue purely physical?
  bool isPhysical() const {
    assert(isValid());
    for (auto &component : Path)
      if (!component.isPhysical())
        return false;
    return true;
  }

  /// Add a new component at the end of the access path of this lvalue.
  template <class T, class... A> T &add(A &&...args) {
    T &component = Path.add<T>(std::forward<A>(args)...);
    component.AllocatedSize = sizeof(T);
    assert(component.allocated_size() == sizeof(T));
    ObjectType = component.getObjectType();
    return component;
  }

  template <class T, class... A> T &addWithExtra(A &&...args) {
    size_t extraSize = T::extra_storage_size(std::forward<A>(args)...);
    T &component = Path.addWithExtra<T>(extraSize, std::forward<A>(args)...);
    component.AllocatedSize = sizeof(T) + extraSize;
    assert(component.allocated_size() == sizeof(T) + extraSize);
    ObjectType = component.getObjectType();
    return component;
  }

  typedef DiverseListImpl<PathComponent>::iterator iterator;
  typedef DiverseListImpl<PathComponent>::const_iterator const_iterator;

  iterator begin() { return Path.begin(); }
  iterator end() { return Path.end(); }
  const_iterator begin() const { return Path.begin(); }
  const_iterator end() const { return Path.end(); }
  
  /// Returns the type of the value addressed by the LValue.
  Type getObjectType() const { return ObjectType; }
};
  
/// RAII object to enable writebacks for logical lvalues evaluated within the
/// scope, which will be applied when the object goes out of scope.
class WritebackScope {
  SILGenFunction *gen;
  bool wasInWritebackScope;
  size_t savedDepth;
public:
  WritebackScope(SILGenFunction &gen);
  ~WritebackScope();
  
  WritebackScope(const WritebackScope &) = delete;
  WritebackScope &operator=(const WritebackScope &) = delete;
  
  WritebackScope(WritebackScope &&o);
  WritebackScope &operator=(WritebackScope &&o);
};
  
/// RAII object to disable writebacks for logical lvalues evaluated within the
/// scope. Used for LoadExprs.
class DisableWritebackScope {
  SILGenFunction &gen;
  bool wasInWritebackScope;
public:
  DisableWritebackScope(SILGenFunction &gen)
    : gen(gen), wasInWritebackScope(gen.InWritebackScope)
  {
    gen.InWritebackScope = false;
  }
  
  ~DisableWritebackScope() {
    gen.InWritebackScope = wasInWritebackScope;
  }
};

} // end namespace Lowering
} // end namespace swift

#endif
