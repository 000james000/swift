// High-level tests that DI accepts and rejects failure from failable
// initializers properly.

// RUN: %swift -emit-sil -verify %s

// For value types, we can handle failure at any point, using DI's established
// analysis for partial struct and tuple values.

struct Struct {
  let x, y: Int 

  init() { x = 0; y = 0 }

  init?(failBeforeInitialization: ()) {
    return nil
  }

  init?(failAfterPartialInitialization: ()) {
    x = 0
    return nil
  }

  init?(failAfterFullInitialization: ()) {
    x = 0
    y = 0
    return nil
  }

  init?(failAfterWholeObjectInitializationByAssignment: ()) {
    self = Struct()
    return nil
  }

  init?(failAfterWholeObjectInitializationByDelegation: ()) {
    self.init()
    return nil
  }
}

// For classes, we cannot yet support failure with a partially initialized
// object.
// TODO: We ought to be able to for native Swift classes.

class RootClass {
  let x, y: Int

  init() { x = 0; y = 0 }

  convenience init?(failBeforeDelegation: Bool) {
    if failBeforeDelegation { return nil } // TODO: e/xpected-error
    self.init()
  }

  convenience init?(failAfterDelegation: ()) {
    self.init()
    return nil // OK
  }

  init?(failBeforeInitialization: ()) {
    return nil // expected-error{{properties of a class instance must be initialized before returning nil}}
  }

  init?(failAfterPartialInitialization: ()) {
    x = 0
    return nil // expected-error{{properties of a class instance must be initialized before returning nil}}
  }

  init?(failAfterFullInitialization: ()) {
    x = 0
    y = 0
    return nil // OK
  }

  convenience init?(failBeforeFailableDelegation: Bool) {
    if failBeforeFailableDelegation { return nil } // TODO: e/xpected-error
    self.init(failBeforeInitialization: ())
  }

  convenience init?(failAfterFailableDelegation: ()) {
    self.init(failBeforeInitialization: ())
    return nil // OK
  }
}

class SubClass: RootClass {
  let z: Int

  override init() {
    z = 0
    super.init()
  }

  override init?(failBeforeInitialization: ()) {
    return nil // TODO: e/xpected-error
  }

  init?(failBeforeSuperInitialization: ()) {
    z = 0
    return nil // TODO: e/xpected-error
  }

  override init?(failAfterFullInitialization: ()) {
    z = 0
    super.init()
    return nil // OK
  }

  init?(failBeforeFailableSuperInit: Bool) {
    z = 0
    if failBeforeFailableSuperInit { return nil } // TODO: e/xpected-error
    super.init(failBeforeInitialization: ())
  }

  init?(failAfterFailableSuperInit: Bool) {
    z = 0
    super.init(failBeforeInitialization: ())
    return nil // OK
  }

  convenience init?(failBeforeDelegation: Bool) {
    if failBeforeDelegation { return nil } // TODO: e/xpected-error
    self.init()
  }

  convenience init?(failAfterDelegation: ()) {
    self.init()
    return nil // OK
  }

  convenience init?(failBeforeFailableDelegation: Bool) {
    if failBeforeFailableDelegation { return nil } // TODO: e/xpected-error
    self.init(failBeforeInitialization: ())
  }

  convenience init?(failAfterFailableDelegation: ()) {
    self.init(failBeforeInitialization: ())
    return nil // OK
  }
}
