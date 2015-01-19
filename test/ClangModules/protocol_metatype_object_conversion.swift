// RUN: %target-swift-frontend %clang-importer-sdk -parse -verify %s -disable-objc-attr-requires-foundation-module

import ObjectiveC

@objc protocol ObjCProto {}
@objc protocol ObjCProto2 {}
protocol NonObjCProto {}
typealias TwoObjCProtos = protocol<ObjCProto, ObjCProto2>

func takesProtocol(x: Protocol) {}

takesProtocol(ObjCProto.self)
takesProtocol(ObjCProto2.self)
takesProtocol(NonObjCProto.self) // expected-error{{cannot invoke 'takesProtocol' with an argument list of type '(NonObjCProto.Protocol)'}} expected-note{{expected an argument list of type '(Protocol)'}}
takesProtocol(TwoObjCProtos.self) // expected-error{{cannot invoke 'takesProtocol' with an argument list of type '(TwoObjCProtos.Protocol)'}} expected-note {{expected an argument list of type '(Protocol)'}}
