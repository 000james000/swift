// RUN: %target-parse-verify-swift -enable-experimental-availability-checking

// REQUIRES: OS=macosx

@availability(OSX, introduced=10.9)
var globalAvailableOn10_9: Int = 9

@availability(OSX, introduced=10.10)
var globalAvailableOn10_10: Int = 10

@availability(OSX, introduced=10.11)
var globalAvailableOn10_11: Int = 11

// Top level should reflect the minimum deployment target.
let ignored1: Int = globalAvailableOn10_9

let ignored2: Int = globalAvailableOn10_10 // expected-error {{'globalAvailableOn10_10' is only available on OS X 10.10 or newer}}
    // expected-note@-1 {{guard with version check}}

let ignored3: Int = globalAvailableOn10_11 // expected-error {{'globalAvailableOn10_11' is only available on OS X 10.11 or newer}}
    // expected-note@-1 {{guard with version check}}

// Functions without annotations should reflect the minimum deployment target.
func functionWithoutAvailability() {
	let _: Int = globalAvailableOn10_9
	
	let _: Int = globalAvailableOn10_10 // expected-error {{'globalAvailableOn10_10' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing global function}}
      // expected-note@-2 {{guard with version check}}

	let _: Int = globalAvailableOn10_11 // expected-error {{'globalAvailableOn10_11' is only available on OS X 10.11 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing global function}}
      // expected-note@-2 {{guard with version check}}
}

// Functions with annotations should refine their bodies.
@availability(OSX, introduced=10.10)
func functionAvailableOn10_10() {
	let _: Int = globalAvailableOn10_9
 	let _: Int = globalAvailableOn10_10
 	
 	// Nested functions should get their own refinement context.
 	@availability(OSX, introduced=10.11)
 	func innerFunctionAvailableOn10_11() {
 		let _: Int = globalAvailableOn10_9
 		let _: Int = globalAvailableOn10_10
 		let _: Int = globalAvailableOn10_11
 	}
 	
 	let _: Int = globalAvailableOn10_11 // expected-error {{'globalAvailableOn10_11' is only available on OS X 10.11 or newer}}
      // expected-note@-1 {{guard with version check}}
}

if #os(OSX >= 10.10) {
  let _: Int = globalAvailableOn10_10
  let _: Int = globalAvailableOn10_11 // expected-error {{'globalAvailableOn10_11' is only available on OS X 10.11 or newer}}
      // expected-note@-1 {{guard with version check}}
}

if #os(OSX >= 10.10) {
  let _: Int = globalAvailableOn10_10
  let _: Int = globalAvailableOn10_11 // expected-error {{'globalAvailableOn10_11' is only available on OS X 10.11 or newer}}
      // expected-note@-1 {{guard with version check}}
} else {
  let _: Int = globalAvailableOn10_9
  let _: Int = globalAvailableOn10_10 // expected-error {{'globalAvailableOn10_10' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{guard with version check}}
}

@availability(OSX, introduced=10.10)
var globalAvailableOnOSX10_10AndiOS8_0: Int = 10

if #os(OSX >= 10.10, iOS >= 8.0) {
  let _: Int = globalAvailableOnOSX10_10AndiOS8_0
}

if #os(OSX >= 10.10, OSX >= 10.11) {  // expected-error {{conditions for 'OSX' already specified for this query}}
}

if #os(iOS >= 9.0) {  // expected-error {{condition required for target platform 'OSX'}}
  let _: Int = globalAvailableOnOSX10_10AndiOS8_0 // expected-error {{'globalAvailableOnOSX10_10AndiOS8_0' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{guard with version check}}
}

// Multiple unavailable references in a single statement

let ignored4: (Int, Int) = (globalAvailableOn10_10, globalAvailableOn10_11) // expected-error {{'globalAvailableOn10_10' is only available on OS X 10.10 or newer}}  expected-error {{'globalAvailableOn10_11' is only available on OS X 10.11 or newer}}
    // expected-note@-1 2{{guard with version check}}

// Global functions

@availability(OSX, introduced=10.9)
func funcAvailableOn10_9() {}

@availability(OSX, introduced=10.10)
func funcAvailableOn10_10() {}

funcAvailableOn10_9()

let ignored5 = funcAvailableOn10_10 // expected-error {{'funcAvailableOn10_10()' is only available on OS X 10.10 or newer}}
    // expected-note@-1 {{guard with version check}}

funcAvailableOn10_10() // expected-error {{'funcAvailableOn10_10()' is only available on OS X 10.10 or newer}}
    // expected-note@-1 {{guard with version check}}

if #os(OSX >= 10.10) {
  funcAvailableOn10_10()
}

// Overloaded global functions
@availability(OSX, introduced=10.9)
func overloadedFunction() {}

@availability(OSX, introduced=10.10)
func overloadedFunction(on1010: Int) {}

overloadedFunction()
overloadedFunction(0) // expected-error {{'overloadedFunction' is only available on OS X 10.10 or newer}}
    // expected-note@-1 {{guard with version check}}

// Unavailable methods

class ClassWithUnavailableMethod {
  @availability(OSX, introduced=10.9)
  func methAvailableOn10_9() {}
  
  @availability(OSX, introduced=10.10)
  func methAvailableOn10_10() {}
  
  @availability(OSX, introduced=10.10)
  class func classMethAvailableOn10_10() {}
  
  func someOtherMethod() {
    methAvailableOn10_9()
    methAvailableOn10_10() // expected-error {{'methAvailableOn10_10()' is only available on OS X 10.10 or newer}}
        // expected-note@-1 {{add @availability attribute to enclosing class}}
        // expected-note@-2 {{add @availability attribute to enclosing instance method}}
        // expected-note@-3 {{guard with version check}}
  }
}

func callUnavailableMethods(o: ClassWithUnavailableMethod) {
  let m10_9 = o.methAvailableOn10_9
  m10_9()
  
  let m10_10 = o.methAvailableOn10_10 // expected-error {{'methAvailableOn10_10()' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing global function}}
      // expected-note@-2 {{guard with version check}}

  m10_10()
  
  o.methAvailableOn10_9()
  o.methAvailableOn10_10() // expected-error {{'methAvailableOn10_10()' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing global function}}
      // expected-note@-2 {{guard with version check}}
}

func callUnavailableMethodsViaIUO(o: ClassWithUnavailableMethod!) {
  let m10_9 = o.methAvailableOn10_9
  m10_9()
  
  let m10_10 = o.methAvailableOn10_10 // expected-error {{'methAvailableOn10_10()' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing global function}}
      // expected-note@-2 {{guard with version check}}

  m10_10()
  
  o.methAvailableOn10_9()
  o.methAvailableOn10_10() // expected-error {{'methAvailableOn10_10()' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing global function}}
      // expected-note@-2 {{guard with version check}}
}

func callUnavailableClassMethod() {
  ClassWithUnavailableMethod.classMethAvailableOn10_10() // expected-error {{'classMethAvailableOn10_10()' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing global function}}
      // expected-note@-2 {{guard with version check}}
  
  let m10_10 = ClassWithUnavailableMethod.classMethAvailableOn10_10 // expected-error {{'classMethAvailableOn10_10()' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing global function}}
      // expected-note@-2 {{guard with version check}}

  m10_10()
}

class SubClassWithUnavailableMethod : ClassWithUnavailableMethod {
  func someMethod() {
    methAvailableOn10_9()
    methAvailableOn10_10() // expected-error {{'methAvailableOn10_10()' is only available on OS X 10.10 or newer}}
        // expected-note@-1 {{add @availability attribute to enclosing class}}
        // expected-note@-2 {{add @availability attribute to enclosing instance method}}
        // expected-note@-3 {{guard with version check}}
  }
}

class SubClassOverridingUnavailableMethod : ClassWithUnavailableMethod {

  override func methAvailableOn10_10() {
    methAvailableOn10_9()
    super.methAvailableOn10_10() // expected-error {{'methAvailableOn10_10()' is only available on OS X 10.10 or newer}}
        // expected-note@-1 {{add @availability attribute to enclosing class}}
        // expected-note@-2 {{add @availability attribute to enclosing instance method}}
        // expected-note@-3 {{guard with version check}}
    
    let m10_9 = super.methAvailableOn10_9
    m10_9()
    
    let m10_10 = super.methAvailableOn10_10 // expected-error {{'methAvailableOn10_10()' is only available on OS X 10.10 or newer}}
        // expected-note@-1 {{add @availability attribute to enclosing class}}
        // expected-note@-2 {{add @availability attribute to enclosing instance method}}
        // expected-note@-3 {{guard with version check}}
    m10_10()
  }
  
  func someMethod() {
    methAvailableOn10_9()
    // Calling our override should be fine
    methAvailableOn10_10()
  }
}

class ClassWithUnavailableOverloadedMethod {
  @availability(OSX, introduced=10.9)
  func overloadedMethod() {}

  @availability(OSX, introduced=10.10)
  func overloadedMethod(on1010: Int) {}
}

func callUnavailableOverloadedMethod(o: ClassWithUnavailableOverloadedMethod) {
  o.overloadedMethod()
  o.overloadedMethod(0) // expected-error {{'overloadedMethod' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing global function}}
      // expected-note@-2 {{guard with version check}}
}

// Initializers

class ClassWithUnavailableInitializer {
  @availability(OSX, introduced=10.9)
  required init() {  }
  
  @availability(OSX, introduced=10.10)
  required init(_ val: Int) {  }
  
  convenience init(s: String) {
    self.init(5) // expected-error {{'init' is only available on OS X 10.10 or newer}}
        // expected-note@-1 {{add @availability attribute to enclosing class}}
        // expected-note@-2 {{add @availability attribute to enclosing initializer}}
        // expected-note@-3 {{guard with version check}}
  }
  
  @availability(OSX, introduced=10.10)
  convenience init(onlyOn1010: String) {
    self.init(5)
  }
}

func callUnavailableInitializer() {
  ClassWithUnavailableInitializer()
  ClassWithUnavailableInitializer(5) // expected-error {{'init' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing global function}}
      // expected-note@-2 {{guard with version check}}
  
  let i = ClassWithUnavailableInitializer.self 
  i()
  i(5) // expected-error {{'init' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing global function}}
      // expected-note@-2 {{guard with version check}}
}

class SuperWithWithUnavailableInitializer {
  @availability(OSX, introduced=10.9)
  init() {  }
  
  @availability(OSX, introduced=10.10)
  init(_ val: Int) {  }
}

class SubOfClassWithUnavailableInitializer : SuperWithWithUnavailableInitializer {
  override init(_ val: Int) {
    super.init(5) // expected-error {{'init' is only available on OS X 10.10 or newer}}
        // expected-note@-1 {{add @availability attribute to enclosing class}}
        // expected-note@-2 {{add @availability attribute to enclosing initializer}}
        // expected-note@-3 {{guard with version check}}
  }
  
  override init() {
    super.init()
  }
  
  @availability(OSX, introduced=10.10)
  init(on1010: Int) {
    super.init(22)
  }
}

// Properties

class ClassWithUnavailableProperties {
  @availability(OSX, introduced=10.9)
  var availableOn10_9Stored: Int = 9
  
  @availability(OSX, introduced=10.10)
  var availableOn10_10Stored : Int = 10

  @availability(OSX, introduced=10.9)
  var availableOn10_9Computed: Int {
    get {
      let _: Int = availableOn10_10Stored // expected-error {{'availableOn10_10Stored' is only available on OS X 10.10 or newer}}
          // expected-note@-1 {{add @availability attribute to enclosing class}}
          // expected-note@-2 {{guard with version check}}
      
      if #os(OSX >= 10.10) {
        let _: Int = availableOn10_10Stored
      }
      
      return availableOn10_9Stored
    }
    set(newVal) {
      availableOn10_9Stored = newVal
    }
  }
  
  @availability(OSX, introduced=10.10)
  var availableOn10_10Computed: Int {
    get {
      return availableOn10_10Stored
    }
    set(newVal) {
      availableOn10_10Stored = newVal
    }
  }
  
  var propWithSetterOnlyAvailableOn10_10 : Int {
    get {
      funcAvailableOn10_10() // expected-error {{'funcAvailableOn10_10()' is only available on OS X 10.10 or newer}}
          // expected-note@-1 {{add @availability attribute to enclosing class}}
          // expected-note@-2 {{add @availability attribute to enclosing var}}
          // expected-note@-3 {{guard with version check}}
      return 0
    }
    @availability(OSX, introduced=10.10)
    set(newVal) {
    funcAvailableOn10_10()
    }
  }
  
  var propWithGetterOnlyAvailableOn10_10 : Int {
    @availability(OSX, introduced=10.10)
    get {
      funcAvailableOn10_10()
      return 0
    }
    set(newVal) {
      funcAvailableOn10_10() // expected-error {{'funcAvailableOn10_10()' is only available on OS X 10.10 or newer}}
          // expected-note@-1 {{add @availability attribute to enclosing class}}
          // expected-note@-2 {{add @availability attribute to enclosing var}}
          // expected-note@-3 {{guard with version check}}
    }
  }
  
  var propWithGetterAndSetterOnlyAvailableOn10_10 : Int {
    @availability(OSX, introduced=10.10)
    get {
      return 0
    }
    @availability(OSX, introduced=10.10)
    set(newVal) {
    }
  }
  
  var propWithSetterOnlyAvailableOn10_10ForNestedMemberRef : ClassWithUnavailableProperties {
    get {
      return ClassWithUnavailableProperties()
    }
    @availability(OSX, introduced=10.10)
    set(newVal) {
    }
  }
  
  var propWithGetterOnlyAvailableOn10_10ForNestedMemberRef : ClassWithUnavailableProperties {
    @availability(OSX, introduced=10.10)
    get {
      return ClassWithUnavailableProperties()
    }
    set(newVal) {
    }
  }
}

func accessUnavailableProperties(o: ClassWithUnavailableProperties) {
  // Stored properties
  let _: Int = o.availableOn10_9Stored
  let _: Int = o.availableOn10_10Stored // expected-error {{'availableOn10_10Stored' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing global function}}
      // expected-note@-2 {{guard with version check}}
  
  o.availableOn10_9Stored = 9
  o.availableOn10_10Stored = 10 // expected-error {{'availableOn10_10Stored' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing global function}}
      // expected-note@-2 {{guard with version check}}

  // Computed Properties
  let _: Int = o.availableOn10_9Computed
  let _: Int = o.availableOn10_10Computed // expected-error {{'availableOn10_10Computed' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing global function}}
      // expected-note@-2 {{guard with version check}}
  
  o.availableOn10_9Computed = 9
  o.availableOn10_10Computed = 10 // expected-error {{'availableOn10_10Computed' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing global function}}
      // expected-note@-2 {{guard with version check}}
  
  // Getter allowed on 10.9 but setter is not
  let _: Int = o.propWithSetterOnlyAvailableOn10_10
  o.propWithSetterOnlyAvailableOn10_10 = 5 // expected-error {{setter for 'propWithSetterOnlyAvailableOn10_10' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing global function}}
      // expected-note@-2 {{guard with version check}}
  
  if #os(OSX >= 10.10) {
    // Setter is allowed on 10.10 and greater
    o.propWithSetterOnlyAvailableOn10_10 = 5
  }
  
  // Setter allowed on 10.9 but getter is not
  o.propWithGetterOnlyAvailableOn10_10 = 5
  let _: Int = o.propWithGetterOnlyAvailableOn10_10 // expected-error {{getter for 'propWithGetterOnlyAvailableOn10_10' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing global function}}
      // expected-note@-2 {{guard with version check}}

  if #os(OSX >= 10.10) {
    // Getter is allowed on 10.10 and greater
    let _: Int = o.propWithGetterOnlyAvailableOn10_10
  }
  
  // Tests for nested member refs
  
  // Both getters are potentially unavailable.
  let _: Int = o.propWithGetterOnlyAvailableOn10_10ForNestedMemberRef.propWithGetterOnlyAvailableOn10_10 // expected-error {{getter for 'propWithGetterOnlyAvailableOn10_10ForNestedMemberRef' is only available on OS X 10.10 or newer}} expected-error {{getter for 'propWithGetterOnlyAvailableOn10_10' is only available on OS X 10.10 or newer}}
      // expected-note@-1 2{{add @availability attribute to enclosing global function}}
      // expected-note@-2 2{{guard with version check}}

  // Nested getter is potentially unavailable, outer getter is available
  let _: Int = o.propWithGetterOnlyAvailableOn10_10ForNestedMemberRef.propWithSetterOnlyAvailableOn10_10 // expected-error {{getter for 'propWithGetterOnlyAvailableOn10_10ForNestedMemberRef' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing global function}}
      // expected-note@-2 {{guard with version check}}

  // Nested getter is available, outer getter is potentially unavailable
  let _:Int = o.propWithSetterOnlyAvailableOn10_10ForNestedMemberRef.propWithGetterOnlyAvailableOn10_10 // expected-error {{getter for 'propWithGetterOnlyAvailableOn10_10' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing global function}}
      // expected-note@-2 {{guard with version check}}

  // Both getters are always available.
  let _: Int = o.propWithSetterOnlyAvailableOn10_10ForNestedMemberRef.propWithSetterOnlyAvailableOn10_10
  
  
  // Nesting in source of assignment
  var v: Int
  
  v = o.propWithGetterOnlyAvailableOn10_10 // expected-error {{getter for 'propWithGetterOnlyAvailableOn10_10' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing global function}}
      // expected-note@-2 {{guard with version check}}

  v = (o.propWithGetterOnlyAvailableOn10_10) // expected-error {{getter for 'propWithGetterOnlyAvailableOn10_10' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing global function}}
      // expected-note@-2 {{guard with version check}}
  
  // Inout requires access to both getter and setter
  
  func takesInout(inout i : Int) { }
  
  takesInout(&o.propWithGetterOnlyAvailableOn10_10) // expected-error {{cannot pass as inout because getter for 'propWithGetterOnlyAvailableOn10_10' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing global function}}
      // expected-note@-2 {{guard with version check}}

  takesInout(&o.propWithSetterOnlyAvailableOn10_10) // expected-error {{cannot pass as inout because setter for 'propWithSetterOnlyAvailableOn10_10' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing global function}}
      // expected-note@-2 {{guard with version check}}
  
  takesInout(&o.propWithGetterAndSetterOnlyAvailableOn10_10) // expected-error {{cannot pass as inout because getter for 'propWithGetterAndSetterOnlyAvailableOn10_10' is only available on OS X 10.10 or newer}} expected-error {{cannot pass as inout because setter for 'propWithGetterAndSetterOnlyAvailableOn10_10' is only available on OS X 10.10 or newer}}
      // expected-note@-1 2{{add @availability attribute to enclosing global function}}
      // expected-note@-2 2{{guard with version check}}

  takesInout(&o.availableOn10_9Computed)
  takesInout(&o.propWithGetterOnlyAvailableOn10_10ForNestedMemberRef.availableOn10_9Computed) // expected-error {{getter for 'propWithGetterOnlyAvailableOn10_10ForNestedMemberRef' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing global function}}
      // expected-note@-2 {{guard with version check}}
}

// Enums

@availability(OSX, introduced=10.10)
enum EnumIntroducedOn10_10 {
 case Element
}

@availability(OSX, introduced=10.11)
enum EnumIntroducedOn10_11 {
 case Element
}

@availability(OSX, introduced=10.10)
enum CompassPoint {
  case North
  case South
  case East

  @availability(OSX, introduced=10.11)
  case West

  case WithAvailableByEnumPayload(p : EnumIntroducedOn10_10)

  @availability(OSX, introduced=10.11)
  case WithAvailableByEnumElementPayload(p : EnumIntroducedOn10_11)

  @availability(OSX, introduced=10.11)
  case WithAvailableByEnumElementPayload1(p : EnumIntroducedOn10_11), WithAvailableByEnumElementPayload2(p : EnumIntroducedOn10_11)

  case WithUnavailablePayload(p : EnumIntroducedOn10_11) // expected-error {{'EnumIntroducedOn10_11' is only available on OS X 10.11 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing case}}

    case WithUnavailablePayload1(p : EnumIntroducedOn10_11), WithUnavailablePayload2(p : EnumIntroducedOn10_11) // expected-error 2{{'EnumIntroducedOn10_11' is only available on OS X 10.11 or newer}}
      // expected-note@-1 2{{add @availability attribute to enclosing case}}
}

@availability(OSX, introduced=10.11)
func functionTakingEnumIntroducedOn10_11(e: EnumIntroducedOn10_11) { }

func useEnums() {
  let _: CompassPoint = .North // expected-error {{'CompassPoint' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing global function}}
      // expected-note@-2 {{guard with version check}}

  if #os(OSX >= 10.10) {
    let _: CompassPoint = .North

    let _: CompassPoint = .West // expected-error {{'West' is only available on OS X 10.11 or newer}}
        // expected-note@-1 {{add @availability attribute to enclosing global function}}
        // expected-note@-2 {{guard with version check}}

  }

  if #os(OSX >= 10.11) {
    let _: CompassPoint = .West
  }

  // Pattern matching on an enum element does not require it to be definitely available
  if #os(OSX >= 10.10) {
    let point: CompassPoint = .North
    switch (point) {
      case .North, .South, .East:
        println("NSE")
      case .West: // We do not expect an error here
        print("W")

      case .WithAvailableByEnumElementPayload(let p):
        println("WithAvailableByEnumElementPayload")

        // For the moment, we do not incorporate enum element availability into 
        // TRC construction. Perhaps we should?
        functionTakingEnumIntroducedOn10_11(p)  // expected-error {{'functionTakingEnumIntroducedOn10_11' is only available on OS X 10.11 or newer}}
          // expected-note@-1 {{add @availability attribute to enclosing global function}}
          // expected-note@-2 {{guard with version check}}
    }
  }
}

// Classes

@availability(OSX, introduced=10.9)
class ClassAvailableOn10_9 {
  func someMethod() {}
  class func someClassMethod() {}
  var someProp : Int = 22
}

@availability(OSX, introduced=10.10)
class ClassAvailableOn10_10 { // expected-note {{enclosing scope here}}
  func someMethod() {}
  class func someClassMethod() {
    let _ = ClassAvailableOn10_10()
  }
  var someProp : Int = 22

  @availability(OSX, introduced=10.9) // expected-error {{declaration cannot be more available than enclosing scope}}
  func someMethodAvailableOn10_9() { }

  @availability(OSX, introduced=10.11)
  var propWithGetter: Int { // expected-note{{enclosing scope here}}
    @availability(OSX, introduced=10.10) // expected-error {{declaration cannot be more available than enclosing scope}}
    get { return 0 }
  }
}

func classAvailability() {
  ClassAvailableOn10_9.someClassMethod()
  ClassAvailableOn10_10.someClassMethod() // expected-error {{'ClassAvailableOn10_10' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing global function}}
      // expected-note@-2 {{guard with version check}}

  ClassAvailableOn10_9.self
  ClassAvailableOn10_10.self // expected-error {{'ClassAvailableOn10_10' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing global function}}
      // expected-note@-2 {{guard with version check}}
  
  let o10_9 = ClassAvailableOn10_9()
  let o10_10 = ClassAvailableOn10_10() // expected-error {{'ClassAvailableOn10_10' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing global function}}
      // expected-note@-2 {{guard with version check}}
  
  o10_9.someMethod()
  o10_10.someMethod()
  
  let _ = o10_9.someProp
  let _ = o10_10.someProp 
}

func castingUnavailableClass(o : AnyObject) {
  let _ = o as! ClassAvailableOn10_10 // expected-error {{'ClassAvailableOn10_10' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing global function}}
      // expected-note@-2 {{guard with version check}}

  let _ = o as? ClassAvailableOn10_10 // expected-error {{'ClassAvailableOn10_10' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing global function}}
      // expected-note@-2 {{guard with version check}}

  let _ = o is ClassAvailableOn10_10 // expected-error {{'ClassAvailableOn10_10' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing global function}}
      // expected-note@-2 {{guard with version check}}
}

protocol Createable {
  init()
}

@availability(OSX, introduced=10.10)
class ClassAvailableOn10_10_Createable : Createable { 
  required init() {}
}

func create<T : Createable>() -> T {
  return T()
}

class ClassWithGenericTypeParameter<T> { }

class ClassWithTwoGenericTypeParameter<T, S> { }

func classViaTypeParameter() {
  let _ : ClassAvailableOn10_10_Createable = // expected-error {{'ClassAvailableOn10_10_Createable' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing global function}}
      // expected-note@-2 {{guard with version check}}
      create()
      
  let _ = create() as
      ClassAvailableOn10_10_Createable // expected-error {{'ClassAvailableOn10_10_Createable' is only available on OS X 10.10 or newer}}
          // expected-note@-1 {{add @availability attribute to enclosing global function}}
          // expected-note@-2 {{guard with version check}}

  let _ = [ClassAvailableOn10_10]() // expected-error {{'ClassAvailableOn10_10' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing global function}}
      // expected-note@-2 {{guard with version check}}

  let _: ClassWithGenericTypeParameter<ClassAvailableOn10_10> = ClassWithGenericTypeParameter() // expected-error {{'ClassAvailableOn10_10' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing global function}}
      // expected-note@-2 {{guard with version check}}

  let _: ClassWithTwoGenericTypeParameter<ClassAvailableOn10_10, String> = ClassWithTwoGenericTypeParameter() // expected-error {{'ClassAvailableOn10_10' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing global function}}
      // expected-note@-2 {{guard with version check}}

  let _: ClassWithTwoGenericTypeParameter<String, ClassAvailableOn10_10> = ClassWithTwoGenericTypeParameter() // expected-error {{'ClassAvailableOn10_10' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing global function}}
      // expected-note@-2 {{guard with version check}}

  let _: ClassWithTwoGenericTypeParameter<ClassAvailableOn10_10, ClassAvailableOn10_10> = ClassWithTwoGenericTypeParameter() // expected-error 2{{'ClassAvailableOn10_10' is only available on OS X 10.10 or newer}}
      // expected-note@-1 2{{add @availability attribute to enclosing global function}}
      // expected-note@-2 2{{guard with version check}}

  let _: ClassAvailableOn10_10? = nil // expected-error {{'ClassAvailableOn10_10' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing global function}}
      // expected-note@-2 {{guard with version check}}

}

// Unavailable class used in declarations

class ClassWithDeclarationsOfUnavailableClasses {

  @availability(OSX, introduced=10.10)
  init() {
    unavailablePropertyOfUnavailableType = ClassAvailableOn10_10()
    unavailablePropertyOfUnavailableType = ClassAvailableOn10_10()
  }

  var propertyOfUnavailableType: ClassAvailableOn10_10 // expected-error {{'ClassAvailableOn10_10' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing class}}
      // expected-note@-2 {{add @availability attribute to enclosing var}}
  
  @availability(OSX, introduced=10.10)
  var unavailablePropertyOfUnavailableType: ClassAvailableOn10_10
  
  @availability(OSX, introduced=10.10)
  var unavailablePropertyOfUnavailableTypeWithInitializer: ClassAvailableOn10_10 = ClassAvailableOn10_10() 
  
  func methodWithUnavailableParameterType(o : ClassAvailableOn10_10) { // expected-error {{'ClassAvailableOn10_10' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing class}}
      // expected-note@-2 {{add @availability attribute to enclosing instance method}}

  }
  
  @availability(OSX, introduced=10.10)
  func unavailableMethodWithUnavailableParameterType(o : ClassAvailableOn10_10) {
  }
  
  func methodWithUnavailableReturnType() -> ClassAvailableOn10_10  { // expected-error {{'ClassAvailableOn10_10' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing class}}
      // expected-note@-2 {{add @availability attribute to enclosing instance method}}

    return ClassAvailableOn10_10() // expected-error {{'ClassAvailableOn10_10' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing class}}
      // expected-note@-2 {{add @availability attribute to enclosing instance method}}
      // expected-note@-3 {{guard with version check}}
  }
  
  @availability(OSX, introduced=10.10)
  func unavailableMethodWithUnavailableReturnType() -> ClassAvailableOn10_10  {
    return ClassAvailableOn10_10()
  }

  func methodWithUnavailableLocalDeclaration() {
    let o : ClassAvailableOn10_10 = methodWithUnavailableReturnType() // expected-error {{'ClassAvailableOn10_10' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing class}}
      // expected-note@-2 {{add @availability attribute to enclosing instance method}}
      // expected-note@-3 {{guard with version check}}
  }
  
  @availability(OSX, introduced=10.10)
  func unavailableMethodWithUnavailableLocalDeclaration() {
    let o : ClassAvailableOn10_10 = methodWithUnavailableReturnType()
  }
}

class ClassExtendingUnavailableClass : ClassAvailableOn10_10 { // expected-error {{'ClassAvailableOn10_10' is only available on OS X 10.10 or newer}}
    // expected-note@-1 {{add @availability attribute to enclosing class}}

}

@availability(OSX, introduced=10.10)
class UnavailableClassExtendingUnavailableClass : ClassAvailableOn10_10 {
}

// Method availability is contravariant

class SuperWithAlwaysAvailableMembers {
  func shouldAlwaysBeAvailableMethod() { // expected-note {{overridden declaration is here}}
  }
  
  var shouldAlwaysBeAvailableProperty: Int { // expected-note {{overridden declaration is here}}
    get { return 9 }
    set(newVal) {}
  }
}

class SubWithLimitedMemberAvailability : SuperWithAlwaysAvailableMembers {
  @availability(OSX, introduced=10.10)
  override func shouldAlwaysBeAvailableMethod() { // expected-error {{overriding 'shouldAlwaysBeAvailableMethod' must be as available as declaration it overrides}}
  }
  
  @availability(OSX, introduced=10.10)
  override var shouldAlwaysBeAvailableProperty: Int { // expected-error {{overriding 'shouldAlwaysBeAvailableProperty' must be as available as declaration it overrides}}
    get { return 10 }
    set(newVal) {}
  }
}

class SuperWithLimitedMemberAvailability {
  @availability(OSX, introduced=10.10)
  func someMethod() {
  }
  
  @availability(OSX, introduced=10.10)
  var someProperty: Int {
    get { return 10 }
    set(newVal) {}
  }
}

class SubWithLargerMemberAvailability : SuperWithLimitedMemberAvailability {
  @availability(OSX, introduced=10.9)
  override func someMethod() {
    super.someMethod() // expected-error {{'someMethod()' is only available on OS X 10.10 or newer}}
        // expected-note@-1 {{add @availability attribute to enclosing class}}
        // expected-note@-2 {{guard with version check}}
    
    if #os(OSX >= 10.10) {
      super.someMethod()
    }
  }
  
  @availability(OSX, introduced=10.9)
  override var someProperty: Int {
    get { 
      let _ = super.someProperty // expected-error {{'someProperty' is only available on OS X 10.10 or newer}}
          // expected-note@-1 {{add @availability attribute to enclosing class}}
          // expected-note@-2 {{guard with version check}}
      
      if #os(OSX >= 10.10) {
        let _ = super.someProperty
      }
      
      return 9
      }
    set(newVal) {}
  }
}

// Inheritance and availability

@availability(OSX, introduced=10.10)
protocol ProtocolAvailableOn10_10 {
}

@availability(OSX, introduced=10.9)
class SubclassAvailableOn10_9OfClassAvailableOn10_10 : ClassAvailableOn10_10 { // expected-error {{'ClassAvailableOn10_10' is only available on OS X 10.10 or newer}}
}

@availability(OSX, introduced=10.9)
class ClassAvailableOn10_9AdoptingProtocolAvailableOn10_10 : ProtocolAvailableOn10_10 { // expected-error {{'ProtocolAvailableOn10_10' is only available on OS X 10.10 or newer}}
}

// Extensions

extension ClassAvailableOn10_10 { } // expected-error {{'ClassAvailableOn10_10' is only available on OS X 10.10 or newer}}
    // expected-note@-1 {{add @availability attribute to enclosing extension}}

@availability(OSX, introduced=10.10)
extension ClassAvailableOn10_10 {
  func m() {
    let _ = globalAvailableOn10_10
    let _ = globalAvailableOn10_11 // expected-error {{'globalAvailableOn10_11' is only available on OS X 10.11 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing instance method}}
      // expected-note@-2 {{guard with version check}}
  }
}

class ClassToExtend { }

@availability(OSX, introduced=10.10)
extension ClassToExtend {

  func extensionMethod() { }

  @availability(OSX, introduced=10.11)
  func extensionMethod10_11() { }

  class ExtensionClass { }

  // We rely on not allowing nesting of extensions, so test to make sure
  // this emits an error.
  extension ClassToExtend { } // expected-error {{declaration is only valid at file scope}}
}


@availability(OSX, introduced=10.10)
extension ClassToExtend { // expected-note {{enclosing scope here}}
  @availability(OSX, introduced=10.9) // expected-error {{declaration cannot be more available than enclosing scope}}
  func extensionMethod10_9() { }
}

func useUnavailableExtension() {
  let o = ClassToExtend()

  o.extensionMethod() // expected-error {{'extensionMethod()' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing global function}}
      // expected-note@-2 {{guard with version check}}

  let _ = ClassToExtend.ExtensionClass() // expected-error {{'ExtensionClass' is only available on OS X 10.10 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing global function}}
      // expected-note@-2 {{guard with version check}}

  o.extensionMethod10_11() // expected-error {{'extensionMethod10_11()' is only available on OS X 10.11 or newer}}
      // expected-note@-1 {{add @availability attribute to enclosing global function}}
      // expected-note@-2 {{guard with version check}}
}

// Useless #os(...) checks

func functionWithDefaultAvailabilityAndUselessCheck() {
// Default availability reflects minimum deployment: 10.9 and up

  if #os(OSX >= 10.9) { // expected-warning {{unnecessary check for 'OSX'; minimum deployment target ensures guard will always be true}}
    let _ = globalAvailableOn10_9
  }
  
  if #os(OSX >= 10.10) { // expected-note {{enclosing scope here}}
    let _ = globalAvailableOn10_10
    
    if #os(OSX >= 10.10) { // expected-warning {{unnecessary check for 'OSX'; enclosing scope ensures guard will always be true}}
      let _ = globalAvailableOn10_10 
    }
  }
}

@availability(OSX, introduced=10.10)
func functionWithSpecifiedAvailabilityAndUselessCheck() { // expected-note 2{{enclosing scope here}}
  if #os(OSX >= 10.9) { // expected-warning {{unnecessary check for 'OSX'; enclosing scope ensures guard will always be true}}
    let _ = globalAvailableOn10_9
  }
  
  if #os(OSX >= 10.10) { // expected-warning {{unnecessary check for 'OSX'; enclosing scope ensures guard will always be true}}
    let _ = globalAvailableOn10_10 
  }
}

// #os(...) outside if statement guards

let _ = #os(OSX >= 10.10) // expected-error {{check can only be used as guard of if statement}}

// For the moment, we don't allow #os() in IfExprs.
(#os(OSX >= 10.10) ? 1 : 0) // expected-error {{check can only be used as guard of if statement}}

if #os(OSX >= 10.10) && #os(OSX >= 10.11) { // expected-error 2{{check can only be used as guard of if statement}}
}


// Tests for Fix-It replacement text
// The whitespace in the replacement text is particularly important here -- it reflects the level
// of indentation for the added if #os() or @availability attribute. Note that, for the moment, we hard
// code *added* indentation in Fix-Its as 4 spaces (that is, when indenting in a Fix-It, we
// take whatever indentation was there before and add 4 spaces to it).

functionAvailableOn10_10()
    // expected-error@-1 {{'functionAvailableOn10_10()' is only available on OS X 10.10 or newer}}
    // expected-note@-2 {{guard with version check}} {{1-27=if #os(OSX >= 10.10) {\n    functionAvailableOn10_10()\n} else {\n    // Fallback on earlier versions\n}}}

let declForFixitAtTopLevel: ClassAvailableOn10_10? = nil
      // expected-error@-1 {{'ClassAvailableOn10_10' is only available on OS X 10.10 or newer}}
      // expected-note@-2 {{guard with version check}} {{1-57=if #os(OSX >= 10.10) {\n    let declForFixitAtTopLevel: ClassAvailableOn10_10? = nil\n} else {\n    // Fallback on earlier versions\n}}}

func fixitForReferenceInGlobalFunction() {
  functionAvailableOn10_10()
      // expected-error@-1 {{'functionAvailableOn10_10()' is only available on OS X 10.10 or newer}}
      // expected-note@-2 {{guard with version check}} {{3-29=if #os(OSX >= 10.10) {\n      functionAvailableOn10_10()\n  } else {\n      // Fallback on earlier versions\n  }}}
      // expected-note@-3 {{add @availability attribute to enclosing global function}} {{1-1=@availability(OSX, introduced=10.10)\n}}
}

public func fixitForReferenceInGlobalFunctionWithDeclModifier() {
  functionAvailableOn10_10()
      // expected-error@-1 {{'functionAvailableOn10_10()' is only available on OS X 10.10 or newer}}
      // expected-note@-2 {{guard with version check}} {{3-29=if #os(OSX >= 10.10) {\n      functionAvailableOn10_10()\n  } else {\n      // Fallback on earlier versions\n  }}}
      // expected-note@-3 {{add @availability attribute to enclosing global function}} {{1-1=@availability(OSX, introduced=10.10)\n}}
}

@noreturn
func fixitForReferenceInGlobalFunctionWithAttribute() {
  functionAvailableOn10_10()
    // expected-error@-1 {{'functionAvailableOn10_10()' is only available on OS X 10.10 or newer}}
    // expected-note@-2 {{guard with version check}} {{3-29=if #os(OSX >= 10.10) {\n      functionAvailableOn10_10()\n  } else {\n      // Fallback on earlier versions\n  }}}
    // expected-note@-3 {{add @availability attribute to enclosing global function}} {{1-1=@availability(OSX, introduced=10.10)\n}}
}

func takesAutoclosure(@autoclosure c : () -> ()) {
}

class ClassForFixit {
  func fixitForReferenceInMethod() {
    functionAvailableOn10_10()
        // expected-error@-1 {{'functionAvailableOn10_10()' is only available on OS X 10.10 or newer}}
        // expected-note@-2 {{guard with version check}} {{5-31=if #os(OSX >= 10.10) {\n        functionAvailableOn10_10()\n    } else {\n        // Fallback on earlier versions\n    }}}
        // expected-note@-3 {{add @availability attribute to enclosing instance method}} {{3-3=@availability(OSX, introduced=10.10)\n  }}
        // expected-note@-4 {{add @availability attribute to enclosing class}} {{1-1=@availability(OSX, introduced=10.10)\n}}
  }

  func fixitForReferenceNestedInMethod() {
    func inner() {
      functionAvailableOn10_10()
          // expected-error@-1 {{'functionAvailableOn10_10()' is only available on OS X 10.10 or newer}}
          // expected-note@-2 {{guard with version check}} {{7-33=if #os(OSX >= 10.10) {\n          functionAvailableOn10_10()\n      } else {\n          // Fallback on earlier versions\n      }}}
          // expected-note@-3 {{add @availability attribute to enclosing instance method}} {{3-3=@availability(OSX, introduced=10.10)\n  }}
          // expected-note@-4 {{add @availability attribute to enclosing class}} {{1-1=@availability(OSX, introduced=10.10)\n}}
    }

    let _: () -> () = { () in
      functionAvailableOn10_10()
          // expected-error@-1 {{'functionAvailableOn10_10()' is only available on OS X 10.10 or newer}}
          // expected-note@-2 {{guard with version check}} {{7-33=if #os(OSX >= 10.10) {\n          functionAvailableOn10_10()\n      } else {\n          // Fallback on earlier versions\n      }}}
          // expected-note@-3 {{add @availability attribute to enclosing instance method}} {{3-3=@availability(OSX, introduced=10.10)\n  }}
          // expected-note@-4 {{add @availability attribute to enclosing class}} {{1-1=@availability(OSX, introduced=10.10)\n}}
    }

    takesAutoclosure(functionAvailableOn10_10())
          // expected-error@-1 {{'functionAvailableOn10_10()' is only available on OS X 10.10 or newer}}
          // expected-note@-2 {{guard with version check}} {{5-49=if #os(OSX >= 10.10) {\n        takesAutoclosure(functionAvailableOn10_10())\n    } else {\n        // Fallback on earlier versions\n    }}}
          // expected-note@-3 {{add @availability attribute to enclosing instance method}} {{3-3=@availability(OSX, introduced=10.10)\n  }}
          // expected-note@-4 {{add @availability attribute to enclosing class}} {{1-1=@availability(OSX, introduced=10.10)\n}}
  }

  var fixitForReferenceInPropertyAccessor: Int {
    get {
      functionAvailableOn10_10()
        // expected-error@-1 {{'functionAvailableOn10_10()' is only available on OS X 10.10 or newer}}
        // expected-note@-2 {{guard with version check}} {{7-33=if #os(OSX >= 10.10) {\n          functionAvailableOn10_10()\n      } else {\n          // Fallback on earlier versions\n      }}}
        // expected-note@-3 {{add @availability attribute to enclosing var}} {{3-3=@availability(OSX, introduced=10.10)\n  }}
        // expected-note@-4 {{add @availability attribute to enclosing class}} {{1-1=@availability(OSX, introduced=10.10)\n}}

      return 5
    }
  }

  var fixitForReferenceInPropertyAccessorType: ClassAvailableOn10_10? = nil
      // expected-error@-1 {{'ClassAvailableOn10_10' is only available on OS X 10.10 or newer}}
      // expected-note@-2 {{add @availability attribute to enclosing var}} {{3-3=@availability(OSX, introduced=10.10)\n  }}
      // expected-note@-3 {{add @availability attribute to enclosing class}} {{1-1=@availability(OSX, introduced=10.10)\n}}

  var fixitForReferenceInPropertyAccessorTypeMultiple: ClassAvailableOn10_10? = nil, other: Int = 7
      // expected-error@-1 {{'ClassAvailableOn10_10' is only available on OS X 10.10 or newer}}
      // expected-note@-2 {{add @availability attribute to enclosing var}} {{3-3=@availability(OSX, introduced=10.10)\n  }}
      // expected-note@-3 {{add @availability attribute to enclosing class}} {{1-1=@availability(OSX, introduced=10.10)\n}}

  func fixitForRefInGuardOfIf() {
    if (globalAvailableOn10_10 > 1066) {
      let _ = 5
      let _ = 6
    }
        // expected-error@-4 {{'globalAvailableOn10_10' is only available on OS X 10.10 or newer}}
        // expected-note@-5 {{guard with version check}} {{5-6=if #os(OSX >= 10.10) {\n        if (globalAvailableOn10_10 > 1066) {\n          let _ = 5\n          let _ = 6\n        }\n    } else {\n        // Fallback on earlier versions\n    }}}
        // expected-note@-6 {{add @availability attribute to enclosing instance method}} {{3-3=@availability(OSX, introduced=10.10)\n  }}
        // expected-note@-7 {{add @availability attribute to enclosing class}} {{1-1=@availability(OSX, introduced=10.10)\n}}
  }
}

extension ClassToExtend {
  func fixitForReferenceInExtensionMethod() {
    functionAvailableOn10_10()
        // expected-error@-1 {{'functionAvailableOn10_10()' is only available on OS X 10.10 or newer}}
        // expected-note@-2 {{guard with version check}} {{5-31=if #os(OSX >= 10.10) {\n        functionAvailableOn10_10()\n    } else {\n        // Fallback on earlier versions\n    }}}
        // expected-note@-3 {{add @availability attribute to enclosing instance method}} {{3-3=@availability(OSX, introduced=10.10)\n  }}
        // expected-note@-4 {{add @availability attribute to enclosing extension}} {{1-1=@availability(OSX, introduced=10.10)\n}}
  }
}

enum EnumForFixit {
  case CaseWithUnavailablePayload(p: ClassAvailableOn10_10)
      // expected-error@-1 {{'ClassAvailableOn10_10' is only available on OS X 10.10 or newer}}
      // expected-note@-2 {{add @availability attribute to enclosing case}} {{3-3=@availability(OSX, introduced=10.10)\n  }}
      // expected-note@-3 {{add @availability attribute to enclosing enum}} {{1-1=@availability(OSX, introduced=10.10)\n}}

  case CaseWithUnavailablePayload2(p: ClassAvailableOn10_10), WithoutPayload
      // expected-error@-1 {{'ClassAvailableOn10_10' is only available on OS X 10.10 or newer}}
      // expected-note@-2 {{add @availability attribute to enclosing case}} {{3-3=@availability(OSX, introduced=10.10)\n  }}
      // expected-note@-3 {{add @availability attribute to enclosing enum}} {{1-1=@availability(OSX, introduced=10.10)\n}}
}
