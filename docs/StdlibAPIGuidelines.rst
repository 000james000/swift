:orphan:

.. default-role:: code

=======================================
Swift Standard Library API Design Guide
=======================================

.. Note:: This guide documents *current practice* in the Swift
          standard library as of April 2015.  API conventions are
          expected to evolve in the near future to better harmonize
          with Cocoa.

The current Swift Standard Library API conventions start with the
Cocoa guidelines as discussed on these two wiki pages: [`API
Guidelines <http://cocoa.apple.com/cgi-bin/wiki.pl?API_Guidelines>`_,
`Properties <http://cocoa.apple.com/cgi-bin/wiki.pl?Properties>`_],
and in this `WWDC Presentation
<http://cocoa.apple.com/CocoaAPIDesign.pdf>`_.  Below, we list where
and how the standard library's API conventions differ from those of
Cocoa

Differences
===========

Points in this section clash in one way or other with the Cocoa
guidelines.

* **Arguments to unary functions**, methods, and initializers
  typically don't get a label, nor are they typically referenced by the
  suffix of a function or method name::

    alligators.insert(fred)

    if alligators.contains(george) { 
      return
    }

  we add a preposition to the end of a function name if the 
  role of the first argument would otherwise be unclear:

  ..parsed-literal::

    // origin of measurement is aPosition
    aPosition.distance\ **To**\ (otherPosition)

    // we're not "indexing x"
    if let position = aSet.index\ **Of**\ (x) { ... } 

* Labels are used on initial arguments to denote special cases:
  
  .. parsed-literal::

    // Normal case: result has same value as argument (traps on overflow)
    Int(aUInt)                           

    // Reinterprets all bits, so the meaning of the result is different
    Int(**bitPattern**: aUInt)               

    // Reinterprets bits that fit, losing information in some cases.
    Int32(**truncatingBitPattern**: anInt64) 

* Argument labels are chosen to clarify the *role* of an argument,
  rather than its type:

  .. parsed-literal::

    x.replaceRange(r, **with:** someElements)

    p.initializeFrom(q, **count:** n)
  
* Second and later arguments are labeled except in cases where there's
  no way to distinguish the arguments' roles::

    swap(&a, &b)                                                    // OK

    let topOfPicture = min(topOfSquare, topOfTriangle, topOfCircle) // OK
    
* We don't use namespace prefixes such as “`NS`”, relying instead on
  the language's own facilities.

* Names of types, protocols and enum cases are UpperCamelCase.
  Everything else is lowerCamelCase. When an initialism appears,
  it is **uniformly upper- or lower-cased to fit the pattern**:

  .. parsed-literal::

     let v: String.\ **UTF**\ 16View = s.\ **utf**\ 16

* Protocol names end in `Type`, `able`, or `ible`.  Other type names do not.

Additional Conventions
======================

* We document the complexity of operations using big-O notation.

* Properties are O(1) to read and write.

* We prefer methods and properties to free functions.  Free functions
  are used when there's no obvious `self` (e.g. `min(x, y, z)`), when
  the function is an unconstrained generic (e.g. `swap(&a, &b)`), or
  when function syntax is part of the domain notation (e.g. `sin(x)`).

* Type conversions use initialization syntax whenever possible, with
  the source of the conversion being the first argument::

    let s0 = String(anInt)            // yes
    let s1 = String(anInt, radix: 2)  // yes
    let s1 = anInt.toString()         // no

  The exception is when the type conversion is part of a protocol,
  because Swift doesn't allow a protocol to put requirements on
  other types::

    protocol IntConvertible {
      func toInt() -> Int      // OK, or
      var asInt: Int {get}     // OK, or
      var intValue: Int {get}  // OK
    }

* Even unlabelled parameter names should be meaningful as they'll be
  referred to in comments and visible in “generated headers”
  (cmd-click in Xcode):

  .. parsed-literal::

    /// Reserve enough space to store \`\ **minimumCapacity**\ \` elements.
    ///
    /// PostCondition: \`\ capacity >= **minimumCapacity**\ \` and the array has
    /// mutable contiguous storage.
    ///
    /// Complexity: O(\`count\`)
    mutating func reserveCapacity(**minimumCapacity**: Int)
    
* Type parameter names of generic types describe the role of the 
  parameter, e.g.
  
  .. parsed-literal::

     struct Array<**Element**> { // *not* Array<**T**>

Acceptable Short or Non-Descriptive Names
-----------------------------------------

* Type parameter names of generic functions may be single characters:

  .. parsed-literal::

    func swap<**T**>(inout lhs: T, inout rhs: T)

* `lhs` and `rhs` are acceptable names for binary operator or
  symmetric binary function arguments.

* `self_` is an acceptable name for unary operator arguments or the
  first argument of binary assignment operators.

* `body` is an acceptable name for a trailing closure argument when
  the resulting construct is supposed to act like a language extension
  and is likely to have side-effects::

    func map<U>(transformation: T->U) -> [U] // not this one

    func each<S: SequenceType>(s: S, body: (S.Generator.Element)->())

Prefixes and Suffixes
---------------------

* `Any` is used as a prefix to denote “type erasure,”
  e.g. `AnySequence<T>` wraps any sequence with element type `T`,
  conforms to `SequenceType` itself, and forwards all operations to the
  wrapped sequence.  The specific type of the wrapped sequence is
  erased.

* `Custom` is used as a prefix for special protocols that will always
  be dynamically checked for at runtime and don't make good generic
  constraints, e.g. `CustomStringConvertible`.

* `InPlace` is used as a suffix to denote the mutating member of a
  pair of related methods:

  .. parsed-literal::

    extension Set {
      func union(other: Set) -> Set
      mutating func union\ **InPlace**\ (other: Set)
    }

* `with` is used as a prefix to denote a function that executes a
  closure within a context, such as a guaranteed lifetime:

  .. parsed-literal::

     s.\ **with**\ CString {
       let fd = fopen($0)
       ...
     } // don't use that pointer after the closing brace

* `Pointer` is used as a suffix to denote a non-class type that acts
  like a reference, c.f. `ManagedBufferPointer`

* `unsafe` or `Unsafe` is *always* used as a prefix when a function or
  type allows the user to violate memory or type safety, except on
  methods of types whose names begin with `Unsafe`, where the type
  name is assumed to convey that.

* `C` is used as a prefix to denote types corresponding to C language
  types, e.g. `CChar`.
