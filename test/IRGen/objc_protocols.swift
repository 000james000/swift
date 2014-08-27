// RUN: rm -rf %t/clang-module-cache
// RUN: %swift -target x86_64-apple-macosx10.9 -module-cache-path %t/clang-module-cache -sdk %S/Inputs -I=%S/Inputs -enable-source-import %s -emit-ir | FileCheck %s

import gizmo
import objc_protocols_Bas

// -- Protocol "Frungible" inherits only objc protocols and should have no
//    out-of-line inherited witnesses in its witness table.
// CHECK: [[ZIM_FRUNGIBLE_WITNESS:@_TWPC14objc_protocols3ZimS_9Frungible]] = constant [1 x i8*] [
// CHECK:    i8* bitcast (void (%C14objc_protocols3Zim*, %swift.type*)* @_TTWC14objc_protocols3ZimS_9FrungibleFS1_6frungeUS1___fQPS1_FT_T_ to i8*)
// CHECK: ]

protocol Ansible {
  func anse()
}

class Foo : NSRuncing, NSFunging, Ansible {
  func runce() {}
  func funge() {}
  func foo() {}
  func anse() {}
}
// CHECK: @_INSTANCE_METHODS__TtC14objc_protocols3Foo = private constant { i32, i32, [3 x { i8*, i8*, i8* }] } {
// CHECK:   i32 24, i32 3,
// CHECK:   [3 x { i8*, i8*, i8* }] [
// CHECK:     { i8*, i8*, i8* } { i8* getelementptr inbounds ([6 x i8]* @"\01L_selector_data(runce)", i64 0, i64 0), i8* getelementptr inbounds ([8 x i8]* [[ENC:@[0-9]+]], i64 0, i64 0), i8* bitcast (void (i8*, i8*)* @_TToFC14objc_protocols3Foo5runcefS0_FT_T_ to i8*) },
// CHECK:     { i8*, i8*, i8* } { i8* getelementptr inbounds ([6 x i8]* @"\01L_selector_data(funge)", i64 0, i64 0), i8* getelementptr inbounds ([8 x i8]* [[ENC]], i64 0, i64 0), i8* bitcast (void (i8*, i8*)* @_TToFC14objc_protocols3Foo5fungefS0_FT_T_ to i8*) },
// CHECK:     { i8*, i8*, i8* } { i8* getelementptr inbounds ([4 x i8]* @"\01L_selector_data(foo)", i64 0, i64 0), i8* getelementptr inbounds ([8 x i8]* [[ENC]], i64 0, i64 0), i8* bitcast (void (i8*, i8*)* @_TToFC14objc_protocols3Foo3foofS0_FT_T_ to i8*)
// CHECK:   }]
// CHECK: }, section "__DATA, __objc_const", align 8

class Bar {
  func bar() {}
}

// -- Bar does not directly have objc methods...
// CHECK-NOT: @_INSTANCE_METHODS_Bar

extension Bar : NSRuncing, NSFunging {
  func runce() {}
  func funge() {}
  func foo() {}

  func notObjC() {}
}

// -- ...but the ObjC protocol conformances on its extension add some
// CHECK: @"_CATEGORY_INSTANCE_METHODS__TtC14objc_protocols3Bar_$_objc_protocols" = private constant { i32, i32, [3 x { i8*, i8*, i8* }] } {
// CHECK:   i32 24, i32 3,
// CHECK:   [3 x { i8*, i8*, i8* }] [
// CHECK:     { i8*, i8*, i8* } { i8* getelementptr inbounds ([6 x i8]* @"\01L_selector_data(runce)", i64 0, i64 0), i8* getelementptr inbounds ([8 x i8]* [[ENC]], i64 0, i64 0), i8* bitcast (void (i8*, i8*)* @_TToFC14objc_protocols3Bar5runcefS0_FT_T_ to i8*) },
// CHECK:     { i8*, i8*, i8* } { i8* getelementptr inbounds ([6 x i8]* @"\01L_selector_data(funge)", i64 0, i64 0), i8* getelementptr inbounds ([8 x i8]* [[ENC]], i64 0, i64 0), i8* bitcast (void (i8*, i8*)* @_TToFC14objc_protocols3Bar5fungefS0_FT_T_ to i8*) },
// CHECK:     { i8*, i8*, i8* } { i8* getelementptr inbounds ([4 x i8]* @"\01L_selector_data(foo)", i64 0, i64 0), i8* getelementptr inbounds ([8 x i8]* [[ENC]], i64 0, i64 0), i8* bitcast (void (i8*, i8*)* @_TToFC14objc_protocols3Bar3foofS0_FT_T_ to i8*) }
// CHECK:   ]
// CHECK: }, section "__DATA, __objc_const", align 8

// class Bas from objc_protocols_Bas module
extension Bas : NSRuncing {
  // -- The runce() implementation comes from the original definition.
  public
  func foo() {}
}

// CHECK: @"_CATEGORY_INSTANCE_METHODS__TtC18objc_protocols_Bas3Bas_$_objc_protocols" = private constant { i32, i32, [1 x { i8*, i8*, i8* }] } {
// CHECK:   i32 24, i32 1,
// CHECK;   [1 x { i8*, i8*, i8* }] [
// CHECK:     { i8*, i8*, i8* } { i8* getelementptr inbounds ([4 x i8]* @"\01L_selector_data(foo)", i64 0, i64 0), i8* getelementptr inbounds ([8 x i8]* [[ENC]], i64 0, i64 0), i8* bitcast (void (i8*, i8*)* @_TToFE14objc_protocolsC18objc_protocols_Bas3Bas3foofS1_FT_T_ to i8*) }
// CHECK:   ]
// CHECK: }, section "__DATA, __objc_const", align 8

// -- Swift protocol refinement of ObjC protocols.
protocol Frungible : NSRuncing, NSFunging {
  func frunge()
}

class Zim : Frungible {
  func runce() {}
  func funge() {}
  func foo() {}

  func frunge() {}
}

// CHECK: @_INSTANCE_METHODS__TtC14objc_protocols3Zim = private constant { i32, i32, [3 x { i8*, i8*, i8* }] } {
// CHECK:   i32 24, i32 3,
// CHECK:   [3 x { i8*, i8*, i8* }] [
// CHECK:     { i8*, i8*, i8* } { i8* getelementptr inbounds ([6 x i8]* @"\01L_selector_data(runce)", i64 0, i64 0), i8* getelementptr inbounds ([8 x i8]* [[ENC]], i64 0, i64 0), i8* bitcast (void (i8*, i8*)* @_TToFC14objc_protocols3Zim5runcefS0_FT_T_ to i8*) },
// CHECK:     { i8*, i8*, i8* } { i8* getelementptr inbounds ([6 x i8]* @"\01L_selector_data(funge)", i64 0, i64 0), i8* getelementptr inbounds ([8 x i8]* [[ENC]], i64 0, i64 0), i8* bitcast (void (i8*, i8*)* @_TToFC14objc_protocols3Zim5fungefS0_FT_T_ to i8*) },
// CHECK:     { i8*, i8*, i8* } { i8* getelementptr inbounds ([4 x i8]* @"\01L_selector_data(foo)", i64 0, i64 0), i8* getelementptr inbounds ([8 x i8]* [[ENC]], i64 0, i64 0), i8* bitcast (void (i8*, i8*)* @_TToFC14objc_protocols3Zim3foofS0_FT_T_ to i8*) }
// CHECK:   ]
// CHECK: }, section "__DATA, __objc_const", align 8

// class Zang from objc_protocols_Bas module
extension Zang : Frungible {
  public
  func runce() {}
  // funge() implementation from original definition of Zang
  public
  func foo() {}

  func frunge() {}
}

// CHECK: @"_CATEGORY_INSTANCE_METHODS__TtC18objc_protocols_Bas4Zang_$_objc_protocols" = private constant { i32, i32, [2 x { i8*, i8*, i8* }] } {
// CHECK:   i32 24, i32 2,
// CHECK:   [2 x { i8*, i8*, i8* }] [
// CHECK:     { i8*, i8*, i8* } { i8* getelementptr inbounds ([6 x i8]* @"\01L_selector_data(runce)", i64 0, i64 0), i8* getelementptr inbounds ([8 x i8]* [[ENC]], i64 0, i64 0), i8* bitcast (void (i8*, i8*)* @_TToFE14objc_protocolsC18objc_protocols_Bas4Zang5runcefS1_FT_T_ to i8*) },
// CHECK:     { i8*, i8*, i8* } { i8* getelementptr inbounds ([4 x i8]* @"\01L_selector_data(foo)", i64 0, i64 0), i8* getelementptr inbounds ([8 x i8]* [[ENC]], i64 0, i64 0), i8* bitcast (void (i8*, i8*)* @_TToFE14objc_protocolsC18objc_protocols_Bas4Zang3foofS1_FT_T_ to i8*) }
// CHECK:   ]
// CHECK: }, section "__DATA, __objc_const", align 8

// -- Force generation of witness for Zim.
// CHECK: define { %objc_object*, i8** } @_TF14objc_protocols22mixed_heritage_erasure{{.*}}
func mixed_heritage_erasure(x: Zim) -> Frungible {
  return x
  // CHECK: [[T0:%.*]] = insertvalue { %objc_object*, i8** } undef, %objc_object* {{%.*}}, 0
  // CHECK: insertvalue { %objc_object*, i8** } [[T0]], i8** getelementptr inbounds ([1 x i8*]* [[ZIM_FRUNGIBLE_WITNESS]], i32 0, i32 0), 1
}

// CHECK-LABEL: define void @_TF14objc_protocols12objc_generic{{.*}}(%objc_object*, %swift.type* %T) {
func objc_generic<T : NSRuncing>(x: T) {
  x.runce()
  // CHECK: [[SELECTOR:%.*]] = load i8** @"\01L_selector(runce)", align 8
  // CHECK: bitcast %objc_object* %0 to [[OBJTYPE:.*]]*
  // CHECK: call void bitcast (void ()* @objc_msgSend to void ([[OBJTYPE]]*, i8*)*)([[OBJTYPE]]* {{%.*}}, i8* [[SELECTOR]])
}

// CHECK-LABEL: define void @_TF14objc_protocols17call_objc_generic{{.*}}(%objc_object*, %swift.type* %T) {
// CHECK:         call void @_TF14objc_protocols12objc_generic{{.*}}(%objc_object* %0, %swift.type* %T)
func call_objc_generic<T : NSRuncing>(x: T) {
  objc_generic(x)
}

// CHECK-LABEL: define void @_TF14objc_protocols13objc_protocol{{.*}}(%objc_object*) {
func objc_protocol(x: NSRuncing) {
  x.runce()
  // CHECK: [[SELECTOR:%.*]] = load i8** @"\01L_selector(runce)", align 8
  // CHECK: bitcast %objc_object* %0 to [[OBJTYPE:.*]]*
  // CHECK: call void bitcast (void ()* @objc_msgSend to void ([[OBJTYPE]]*, i8*)*)([[OBJTYPE]]* {{%.*}}, i8* [[SELECTOR]])
}

// CHECK: define %objc_object* @_TF14objc_protocols12objc_erasure{{.*}}(%CSo7NSSpoon*) {
func objc_erasure(x: NSSpoon) -> NSRuncing {
  return x
  // CHECK: [[RES:%.*]] = bitcast %CSo7NSSpoon* {{%.*}} to %objc_object*
  // CHECK: ret %objc_object* [[RES]]
}

// CHECK: define void @_TF14objc_protocols25objc_protocol_composition{{.*}}(%objc_object*)
func objc_protocol_composition(x: protocol<NSRuncing, NSFunging>) {
  x.runce()
  // CHECK: [[RUNCE:%.*]] = load i8** @"\01L_selector(runce)", align 8
  // CHECK: bitcast %objc_object* %0 to [[OBJTYPE:.*]]*
  // CHECK: call void bitcast (void ()* @objc_msgSend to void ([[OBJTYPE]]*, i8*)*)([[OBJTYPE]]* {{%.*}}, i8* [[RUNCE]])
  x.funge()
  // CHECK: [[FUNGE:%.*]] = load i8** @"\01L_selector(funge)", align 8
  // CHECK: call void bitcast (void ()* @objc_msgSend to void ([[OBJTYPE]]*, i8*)*)([[OBJTYPE]]* {{%.*}}, i8* [[FUNGE]])
}

// CHECK: define void @_TF14objc_protocols31objc_swift_protocol_composition{{.*}}(%objc_object*, i8**)
func objc_swift_protocol_composition
(x:protocol<NSRuncing, Ansible, NSFunging>) {
  x.runce()
  // CHECK: [[RUNCE:%.*]] = load i8** @"\01L_selector(runce)", align 8
  // CHECK: bitcast %objc_object* %0 to [[OBJTYPE:.*]]*
  // CHECK: call void bitcast (void ()* @objc_msgSend to void ([[OBJTYPE]]*, i8*)*)([[OBJTYPE]]* {{%.*}}, i8* [[RUNCE]])
  /* TODO: Abstraction difference from ObjC protocol composition to 
   * opaque protocol
  x.anse()
   */
  x.funge()
  // CHECK: [[FUNGE:%.*]] = load i8** @"\01L_selector(funge)", align 8
  // CHECK: call void bitcast (void ()* @objc_msgSend to void ([[OBJTYPE]]*, i8*)*)([[OBJTYPE]]* {{%.*}}, i8* [[FUNGE]])
}

// TODO: Mixed class-bounded/fully general protocol compositions.

