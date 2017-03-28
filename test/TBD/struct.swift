// RUN: not %target-swift-frontend -c -parse-as-library -module-name test -validate-tbd-against-ir %s 2>&1 | %FileCheck %s

// FIXME: TBDGen is incorrect:
// CHECK: symbol '_T04test10PropertiesV9publicVarSivfi' (test.Properties.(publicVar : Swift.Int).(variable initialization expression)) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test7StaticsV9publicLetSivZ' (static test.Statics.publicLet : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test7StaticsV9publicVarSivZ' (static test.Statics.publicVar : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test7StaticsV9publicLetSifau' (test.Statics.publicLet.unsafeMutableAddressor : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test10PropertiesV9publicLetSivfi' (test.Properties.(publicLet : Swift.Int).(variable initialization expression)) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test7StaticsV10privateLetSivZ' (static test.Statics.privateLet : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test7StaticsV10privateVarSivZ' (static test.Statics.privateVar : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test7StaticsV9publicVarSifau' (test.Statics.publicVar.unsafeMutableAddressor : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test10PropertiesV15publicVarGetSetSifm' (test.Properties.publicVarGetSet.materializeForSet : Swift.Int) is in TBD file, but not in generated IR
// CHECK: symbol '_T04test7StaticsV9publicLetSifgZ' (static test.Statics.publicLet.getter : Swift.Int) is in TBD file, but not in generated IR
// CHECK: symbol '_T04test10PropertiesV9publicLetSifg' (test.Properties.publicLet.getter : Swift.Int) is in TBD file, but not in generated IR
// CHECK: symbol '_T04test7StaticsV15publicVarGetSetSifmZ' (static test.Statics.publicVarGetSet.materializeForSet : Swift.Int) is in TBD file, but not in generated IR
// CHECK: symbol '_T04test7StaticsV9publicVarSifgZ' (static test.Statics.publicVar.getter : Swift.Int) is in TBD file, but not in generated IR
// CHECK: symbol '_T04test10PropertiesV9publicVarSifg' (test.Properties.publicVar.getter : Swift.Int) is in TBD file, but not in generated IR
// CHECK: symbol '_T04test10PropertiesV9publicVarSifm' (test.Properties.publicVar.materializeForSet : Swift.Int) is in TBD file, but not in generated IR
// CHECK: symbol '_T04test7StaticsV9publicVarSifsZ' (static test.Statics.publicVar.setter : Swift.Int) is in TBD file, but not in generated IR
// CHECK: symbol '_T04test10PropertiesV9publicVarSifs' (test.Properties.publicVar.setter : Swift.Int) is in TBD file, but not in generated IR
// CHECK: symbol '_T04test7StaticsV9publicVarSifmZ' (static test.Statics.publicVar.materializeForSet : Swift.Int) is in TBD file, but not in generated IR

public struct Nothing {}

public struct Init {
    public init() {}
    public init(public_: Int) {}
    
    init(private_: Int) {}
}

public struct Methods {
    public init() {}
    public func publicMethod() {}
    func privateMethod() {}
}

public struct Properties {
    public let publicLet: Int = 0
    let privateLet: Int = 0

    public var publicVar: Int = 0
    var privateVar: Int = 0

    public var publicVarGet: Int { return 0 }
    var privateVarGet: Int { return 0 }

    public var publicVarGetSet: Int {
        get { return 0 }
        set {}
    }
    var privateVarGetSet: Int {
        get { return 0 }
        set {}
    }
}

public struct Statics {
    public static func publicStaticFunc() {}
    static func privateStaticFunc() {}

    public static let publicLet: Int = 0
    static let privateLet: Int = 0

    public static var publicVar: Int = 0
    static var privateVar: Int = 0

    public static var publicVarGet: Int { return 0 }
    static var privateVarGet: Int { return 0 }

    public static var publicVarGetSet: Int {
        get { return 0 }
        set {}
    }
    static var privateVarGetSet: Int {
        get { return 0 }
        set {}
    }
}


struct PrivateNothing {}

struct PrivateInit {
    init() {}
    init(private_: Int) {}
}

struct PrivateMethods {
    init() {}
    func privateMethod() {}
}

struct PrivateProperties {
    let privateLet: Int = 0

    var privateVar: Int = 0

    var privateVarGet: Int { return 0 }

    var privateVarGetSet: Int {
        get { return 0 }
        set {}
    }
}

struct PrivateStatics {
    static func privateStaticFunc() {}

    static let privateLet: Int = 0

    static var privateVar: Int = 0

    static var privateVarGet: Int { return 0 }

    static var privateVarGetSet: Int {
        get { return 0 }
        set {}
    }
}

