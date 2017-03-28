// RUN: not %target-swift-frontend -c -parse-as-library -module-name test -validate-tbd-against-ir %s 2>&1 | %FileCheck %s

// FIXME: TBDGen is incorrect:
// CHECK: symbol '_T04test10PropertiesC16privateVarGetSetSifgWo' (witness table offset for test.Properties.privateVarGetSet.getter : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test11PrivateInitCACycfc' (test.PrivateInit.init () -> test.PrivateInit) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test14PrivateMethodsCfD' (test.PrivateMethods.__deallocating_deinit) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test14PrivateNothingCfd' (test.PrivateNothing.deinit) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test7MethodsC13privateMethodyyF' (test.Methods.privateMethod () -> ()) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test14PrivateMethodsCACycfCWo' (witness table offset for test.PrivateMethods.__allocating_init () -> test.PrivateMethods) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test14PrivateStaticsCfd' (test.PrivateStatics.deinit) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test11PrivateInitCACSi8private__tcfCWo' (witness table offset for test.PrivateInit.__allocating_init (private_ : Swift.Int) -> test.PrivateInit) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test10PropertiesCACycfc' (test.Properties.init () -> test.Properties) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test14PrivateMethodsCfd' (test.PrivateMethods.deinit) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test14PrivateNothingCACycfc' (test.PrivateNothing.init () -> test.PrivateNothing) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test10PropertiesC15publicVarGetSetSifsWo' (witness table offset for test.Properties.publicVarGetSet.setter : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test14PrivateStaticsCACycfc' (test.PrivateStatics.init () -> test.PrivateStatics) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test10PropertiesC13privateVarGetSifgWo' (witness table offset for test.Properties.privateVarGet.getter : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test10PropertiesC10privateVarSifmWo' (witness table offset for test.Properties.privateVar.materializeForSet : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test14PrivateNothingCACycfCWo' (witness table offset for test.PrivateNothing.__allocating_init () -> test.PrivateNothing) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test17PrivatePropertiesC10privateVarSifmWo' (witness table offset for test.PrivateProperties.privateVar.materializeForSet : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test10PropertiesC13privateVarGetSifg' (test.Properties.privateVarGet.getter : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test7NothingCMm' (metaclass for test.Nothing) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test7StaticsCMm' (metaclass for test.Statics) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test10PropertiesC16privateVarGetSetSifg' (test.Properties.privateVarGetSet.getter : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test10PropertiesC9publicVarSivfi' (test.Properties.(publicVar : Swift.Int).(variable initialization expression)) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test4InitCfd' (test.Init.deinit) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test10PropertiesC16privateVarGetSetSifm' (test.Properties.privateVarGetSet.materializeForSet : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test10PropertiesC16privateVarGetSetSifs' (test.Properties.privateVarGetSet.setter : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test17PrivatePropertiesCfD' (test.PrivateProperties.__deallocating_deinit) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test4InitCACSi7public__tcfCWo' (witness table offset for test.Init.__allocating_init (public_ : Swift.Int) -> test.Init) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test7MethodsCMm' (metaclass for test.Methods) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test10PropertiesCfd' (test.Properties.deinit) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test17PrivatePropertiesCfd' (test.PrivateProperties.deinit) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test11PrivateInitCACycfCWo' (witness table offset for test.PrivateInit.__allocating_init () -> test.PrivateInit) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test17PrivatePropertiesC13privateVarGetSifg' (test.PrivateProperties.privateVarGet.getter : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test17PrivatePropertiesC16privateVarGetSetSifsWo' (witness table offset for test.PrivateProperties.privateVarGetSet.setter : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test10PropertiesC12publicVarGetSifgWo' (witness table offset for test.Properties.publicVarGet.getter : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test14PrivateMethodsC13privateMethodyyF' (test.PrivateMethods.privateMethod () -> ()) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test10PropertiesC10privateVarSivWvd' (direct field offset for test.Properties.privateVar : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test17PrivatePropertiesC10privateVarSivWvd' (direct field offset for test.PrivateProperties.privateVar : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test10PropertiesC15publicVarGetSetSifmWo' (witness table offset for test.Properties.publicVarGetSet.materializeForSet : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test10PropertiesC10privateVarSifgWo' (witness table offset for test.Properties.privateVar.getter : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test17PrivatePropertiesC10privateVarSifgWo' (witness table offset for test.PrivateProperties.privateVar.getter : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test7MethodsCACycfCWo' (witness table offset for test.Methods.__allocating_init () -> test.Methods) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test14PrivateMethodsCACycfc' (test.PrivateMethods.init () -> test.PrivateMethods) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test7StaticsC9publicVarSivZ' (static test.Statics.publicVar : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test11PrivateInitCfD' (test.PrivateInit.__deallocating_deinit) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test10PropertiesC9publicLetSivWvd' (direct field offset for test.Properties.publicLet : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test14PrivateNothingCMm' (metaclass for test.PrivateNothing) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test7NothingCACycfCWo' (witness table offset for test.Nothing.__allocating_init () -> test.Nothing) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test11PrivateInitCfd' (test.PrivateInit.deinit) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test14PrivateStaticsCMm' (metaclass for test.PrivateStatics) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test7MethodsC13privateMethodyyFWo' (witness table offset for test.Methods.privateMethod () -> ()) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test10PropertiesC9publicVarSifsWo' (witness table offset for test.Properties.publicVar.setter : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test17PrivatePropertiesC13privateVarGetSifgWo' (witness table offset for test.PrivateProperties.privateVarGet.getter : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test14PrivateMethodsCMm' (metaclass for test.PrivateMethods) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test4InitCACycfc' (test.Init.init () -> test.Init) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test4InitCACSi8private__tcfc' (test.Init.init (private_ : Swift.Int) -> test.Init) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test17PrivatePropertiesC16privateVarGetSetSifmWo' (witness table offset for test.PrivateProperties.privateVarGetSet.materializeForSet : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test17PrivatePropertiesC10privateVarSifg' (test.PrivateProperties.privateVar.getter : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test10PropertiesC16privateVarGetSetSifsWo' (witness table offset for test.Properties.privateVarGetSet.setter : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test17PrivatePropertiesC10privateVarSifm' (test.PrivateProperties.privateVar.materializeForSet : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test17PrivatePropertiesC10privateVarSifs' (test.PrivateProperties.privateVar.setter : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test7NothingCACycfc' (test.Nothing.init () -> test.Nothing) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test10PropertiesC15publicVarGetSetSifgWo' (witness table offset for test.Properties.publicVarGetSet.getter : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test17PrivatePropertiesCACycfc' (test.PrivateProperties.init () -> test.PrivateProperties) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test7StaticsCACycfc' (test.Statics.init () -> test.Statics) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test4InitCMm' (metaclass for test.Init) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test7StaticsC9publicVarSifau' (test.Statics.publicVar.unsafeMutableAddressor : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test14PrivateStaticsCACycfCWo' (witness table offset for test.PrivateStatics.__allocating_init () -> test.PrivateStatics) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test4InitCACycfCWo' (witness table offset for test.Init.__allocating_init () -> test.Init) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test10PropertiesC16privateVarGetSetSifmytfU_' (test.Properties.(privateVarGetSet.materializeForSet : Swift.Int).(closure #1)) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test4InitCACSi8private__tcfCWo' (witness table offset for test.Init.__allocating_init (private_ : Swift.Int) -> test.Init) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test10PropertiesCMm' (metaclass for test.Properties) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test17PrivatePropertiesCMm' (metaclass for test.PrivateProperties) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test7MethodsC12publicMethodyyFWo' (witness table offset for test.Methods.publicMethod () -> ()) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test17PrivatePropertiesC16privateVarGetSetSifg' (test.PrivateProperties.privateVarGetSet.getter : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test17PrivatePropertiesC16privateVarGetSetSifm' (test.PrivateProperties.privateVarGetSet.materializeForSet : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test4InitCACSi7public__tcfc' (test.Init.init (public_ : Swift.Int) -> test.Init) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test17PrivatePropertiesC16privateVarGetSetSifs' (test.PrivateProperties.privateVarGetSet.setter : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test17PrivatePropertiesCACycfCWo' (witness table offset for test.PrivateProperties.__allocating_init () -> test.PrivateProperties) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test10PropertiesC9publicVarSifmWo' (witness table offset for test.Properties.publicVar.materializeForSet : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test17PrivatePropertiesC16privateVarGetSetSifgWo' (witness table offset for test.PrivateProperties.privateVarGetSet.getter : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test7NothingCfd' (test.Nothing.deinit) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test10PropertiesC16privateVarGetSetSifmWo' (witness table offset for test.Properties.privateVarGetSet.materializeForSet : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test7StaticsCfd' (test.Statics.deinit) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test10PropertiesC10privateVarSifg' (test.Properties.privateVar.getter : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test17PrivatePropertiesC16privateVarGetSetSifmytfU_' (test.PrivateProperties.(privateVarGetSet.materializeForSet : Swift.Int).(closure #1)) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test10PropertiesC10privateVarSifm' (test.Properties.privateVar.materializeForSet : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test7StaticsC10privateVarSivZ' (static test.Statics.privateVar : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test11PrivateInitCACSi8private__tcfc' (test.PrivateInit.init (private_ : Swift.Int) -> test.PrivateInit) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test10PropertiesC10privateVarSifs' (test.Properties.privateVar.setter : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test10PropertiesC10privateLetSivWvd' (direct field offset for test.Properties.privateLet : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test7MethodsCfd' (test.Methods.deinit) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test17PrivatePropertiesC10privateLetSivWvd' (direct field offset for test.PrivateProperties.privateLet : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test11PrivateInitCMm' (metaclass for test.PrivateInit) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test10PropertiesC10privateVarSifsWo' (witness table offset for test.Properties.privateVar.setter : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test17PrivatePropertiesC10privateVarSifsWo' (witness table offset for test.PrivateProperties.privateVar.setter : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test14PrivateMethodsC13privateMethodyyFWo' (witness table offset for test.PrivateMethods.privateMethod () -> ()) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test7MethodsCACycfc' (test.Methods.init () -> test.Methods) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test14PrivateStaticsC10privateLetSivZ' (static test.PrivateStatics.privateLet : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test14PrivateStaticsC10privateVarSivZ' (static test.PrivateStatics.privateVar : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test10PropertiesC9publicLetSivfi' (test.Properties.(publicLet : Swift.Int).(variable initialization expression)) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test10PropertiesC9publicVarSivWvd' (direct field offset for test.Properties.publicVar : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test10PropertiesCACycfCWo' (witness table offset for test.Properties.__allocating_init () -> test.Properties) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test10PropertiesC9publicVarSifgWo' (witness table offset for test.Properties.publicVar.getter : Swift.Int) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test7StaticsCACycfCWo' (witness table offset for test.Statics.__allocating_init () -> test.Statics) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test14PrivateNothingCfD' (test.PrivateNothing.__deallocating_deinit) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test14PrivateStaticsCfD' (test.PrivateStatics.__deallocating_deinit) is in generated IR file, but not in TBD file
// CHECK: symbol '_T04test10PropertiesC10privateLetSifau' (test.Properties.privateLet.unsafeMutableAddressor : Swift.Int) is in TBD file, but not in generated IR
// CHECK: symbol '_T04test7StaticsC10privateLetSifau' (test.Statics.privateLet.unsafeMutableAddressor : Swift.Int) is in TBD file, but not in generated IR
// CHECK: symbol '_T04test10PropertiesC9publicLetSiv' (test.Properties.publicLet : Swift.Int) is in TBD file, but not in generated IR
// CHECK: symbol '_T04test7StaticsC9publicVarSifmZ' (static test.Statics.publicVar.materializeForSet : Swift.Int) is in TBD file, but not in generated IR
// CHECK: symbol '_T04test10PropertiesC9publicLetSifau' (test.Properties.publicLet.unsafeMutableAddressor : Swift.Int) is in TBD file, but not in generated IR
// CHECK: symbol '_T04test10PropertiesC9publicLetSifg' (test.Properties.publicLet.getter : Swift.Int) is in TBD file, but not in generated IR
// CHECK: symbol '_T04test7StaticsC15publicVarGetSetSifmZ' (static test.Statics.publicVarGetSet.materializeForSet : Swift.Int) is in TBD file, but not in generated IR
// CHECK: symbol '_T04test10PropertiesC15publicVarGetSetSifm' (test.Properties.publicVarGetSet.materializeForSet : Swift.Int) is in TBD file, but not in generated IR
// CHECK: symbol '_T04test10PropertiesC9publicVarSifg' (test.Properties.publicVar.getter : Swift.Int) is in TBD file, but not in generated IR
// CHECK: symbol '_T04test10PropertiesC10privateLetSiv' (test.Properties.privateLet : Swift.Int) is in TBD file, but not in generated IR
// CHECK: symbol '_T04test10PropertiesC9publicVarSifs' (test.Properties.publicVar.setter : Swift.Int) is in TBD file, but not in generated IR
// CHECK: symbol '_T04test10PropertiesC9publicVarSifm' (test.Properties.publicVar.materializeForSet : Swift.Int) is in TBD file, but not in generated IR
// CHECK: symbol '_T04test7StaticsC9publicLetSifgZ' (static test.Statics.publicLet.getter : Swift.Int) is in TBD file, but not in generated IR
// CHECK: symbol '_T04test7StaticsC9publicVarSifgZ' (static test.Statics.publicVar.getter : Swift.Int) is in TBD file, but not in generated IR
// CHECK: symbol '_T04test7StaticsC9publicVarSifsZ' (static test.Statics.publicVar.setter : Swift.Int) is in TBD file, but not in generated IR

public class Nothing {}

public class Init {
    public init() {}
    public init(public_: Int) {}
    
    init(private_: Int) {}

    deinit {}
}

public class Methods {
    public init() {}
    public func publicMethod() {}
    func privateMethod() {}
}

public class Properties {
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

public class Statics {
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


class PrivateNothing {}

class PrivateInit {
    init() {}
    init(private_: Int) {}
}

class PrivateMethods {
    init() {}
    func privateMethod() {}
}

class PrivateProperties {
    let privateLet: Int = 0

    var privateVar: Int = 0

    var privateVarGet: Int { return 0 }

    var privateVarGetSet: Int {
        get { return 0 }
        set {}
    }
}

class PrivateStatics {
    static func privateStaticFunc() {}

    static let privateLet: Int = 0

    static var privateVar: Int = 0

    static var privateVarGet: Int { return 0 }

    static var privateVarGetSet: Int {
        get { return 0 }
        set {}
    }
}

