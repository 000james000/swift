//===--- Lazy.h - A lazily-initialized object -----------------------------===//
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

#ifndef SWIFT_RUNTIME_LAZY_H
#define SWIFT_RUNTIME_LAZY_H

#ifdef __APPLE__
#include <dispatch/dispatch.h>
#else
#include <mutex>
#endif
#include "swift/Basic/type_traits.h"

namespace swift {

/// A template for lazily-constructed, zero-initialized, leaked-on-exit
/// global objects.
template <class T> class Lazy {
  typename std::aligned_storage<sizeof(T), alignof(T)>::type Value;
  
#ifdef __APPLE__
  dispatch_once_t OnceToken;
#else
  std::once_flag OnceToken;
#endif

public:
  T &get();
  
  /// Get the value, assuming it must have already been initialized by this
  /// point.
  T &unsafeGetAlreadyInitialized() {
    return *reinterpret_cast<T*>(&Value);
  }
  
  constexpr Lazy() = default;

  T *operator->() { return &get(); }
  T &operator*() { return get(); }

private:
  static void lazyInitCallback(void *Argument) {
    auto self = reinterpret_cast<Lazy *>(Argument);
    ::new (&self->unsafeGetAlreadyInitialized()) T();
  }
  
  Lazy(const Lazy &) = delete;
  Lazy &operator=(const Lazy &) = delete;
};

template<typename T>
inline T& Lazy<T>::get() {
  static_assert(std::is_literal_type<Lazy<T>>::value,
                "Lazy<T> must be a literal type");

#ifdef __APPLE__
  dispatch_once_f(&OnceToken, this, lazyInitCallback);
#else
  std::call_once(OnceToken, lazyInitCallback, this);
#endif
  return unsafeGetAlreadyInitialized();
}

} // namespace swift

#endif // SWIFT_RUNTIME_LAZY_H

