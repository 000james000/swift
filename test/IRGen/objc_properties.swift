// RUN: rm -rf %t/clang-module-cache
// RUN: %swift -target x86_64-apple-macosx10.9 %s -emit-ir | FileCheck %s

@objc class SomeObject {
  var readonly : SomeObject {
    get {
      return self
    }
  }

  var readwrite : SomeObject {
    get {
      return bareIvar
    }
    set {
      bareIvar = newValue
    }
  }

  var bareIvar : SomeObject

  init() { 
    bareIvar = SomeObject()
  }
}

extension SomeObject {
  var extensionProperty : SomeObject {
    get {
      return self
    }
    set {
      bareIvar = self
    }
  }
}

// <rdar://problem/16952186> Crash with @lazy in @objc class
@objc
class LazyPropertyCrash  {
  @lazy var applicationFilesDirectory: LazyPropertyCrash = LazyPropertyCrash()
}

// <rdar://16909436>
@objc class Tree {
  weak var parent: Tree?
}


// CHECK: [[READONLY_NAME:@.*]] = private unnamed_addr constant [9 x i8] c"readonly\00"
// CHECK: [[READONLY_ATTRS:@.*]] = private unnamed_addr constant [7 x i8] c"T@,N,R\00"

// CHECK: [[GETTER_SIGNATURE:@.*]] = private unnamed_addr constant [8 x i8] c"@16@0:8\00"

// CHECK: [[READWRITE_NAME:@.*]] = private unnamed_addr constant [10 x i8] c"readwrite\00"
// CHECK: [[READWRITE_ATTRS:@.*]] = private unnamed_addr constant [7 x i8] c"T@,N,&\00"

// CHECK: [[SETTER_SIGNATURE:@.*]] = private unnamed_addr constant [11 x i8] c"v24@0:8@16\00"

// CHECK: [[BAREIVAR_NAME:@.*]] = private unnamed_addr constant [9 x i8] c"bareIvar\00"
// CHECK: [[BAREIVAR_ATTRS:@.*]] = private unnamed_addr constant [17 x i8] c"T@,N,&,VbareIvar\00"

// CHECK: @_INSTANCE_METHODS__TtC15objc_properties10SomeObject = private constant { {{.*}}] } {
// CHECK:   i32 24,
// CHECK:   i32 6,
// CHECK:   [6 x { i8*, i8*, i8* }] [{
// CHECK:     i8* getelementptr inbounds ([9 x i8]* @"\01L_selector_data(readonly)", i64 0, i64 0),
// CHECK:     i8* getelementptr inbounds ([8 x i8]* [[GETTER_SIGNATURE]], i64 0, i64 0),
// CHECK:     i8* bitcast ([[OPAQUE0:%.*]]* ([[OPAQUE1:%.*]]*, i8*)* @_TToFC15objc_properties10SomeObjectg8readonlyS0_ to i8*)
// CHECK:   }, {
// CHECK:     i8* getelementptr inbounds ([10 x i8]* @"\01L_selector_data(readwrite)", i64 0, i64 0),
// CHECK:     i8* getelementptr inbounds ([8 x i8]* [[GETTER_SIGNATURE]], i64 0, i64 0),
// CHECK:     i8* bitcast ([[OPAQUE0]]* ([[OPAQUE1]]*, i8*)* @_TToFC15objc_properties10SomeObjectg9readwriteS0_ to i8*)
// CHECK:   }, {
// CHECK:     i8* getelementptr inbounds ([14 x i8]* @"\01L_selector_data(setReadwrite:)", i64 0, i64 0),
// CHECK:     i8* getelementptr inbounds ([11 x i8]* [[SETTER_SIGNATURE]], i64 0, i64 0),
// CHECK:     i8* bitcast (void ([[OPAQUE3:%.*]]*, i8*, [[OPAQUE4:%.*]]*)* @_TToFC15objc_properties10SomeObjects9readwriteS0_ to i8*)
// CHECK:   }, {
// CHECK:     i8* getelementptr inbounds ([9 x i8]* @"\01L_selector_data(bareIvar)", i64 0, i64 0),
// CHECK:     i8* getelementptr inbounds ([8 x i8]* [[GETTER_SIGNATURE]], i64 0, i64 0),
// CHECK:     i8* bitcast ([[OPAQUE0]]* ([[OPAQUE1]]*, i8*)* @_TToFC15objc_properties10SomeObjectg8bareIvarS0_ to i8*)
// CHECK:   }, {
// CHECK:     i8* getelementptr inbounds ([13 x i8]* @"\01L_selector_data(setBareIvar:)", i64 0, i64 0),
// CHECK:     i8* getelementptr inbounds ([11 x i8]* [[SETTER_SIGNATURE]], i64 0, i64 0),
// CHECK:     i8* bitcast (void ([[OPAQUE3]]*, i8*, [[OPAQUE4]]*)* @_TToFC15objc_properties10SomeObjects8bareIvarS0_ to i8*)
// CHECK:   }, {
// CHECK:     i8* getelementptr inbounds ([5 x i8]* @"\01L_selector_data(init)", i64 0, i64 0), 
// CHECK:     i8* getelementptr inbounds ([8 x i8]* @2, i64 0, i64 0), 
// CHECK:     i8* bitcast ([[OPAQUE5:%.*]]* ([[OPAQUE6:%.*]]*, i8*)* @_TToFC15objc_properties10SomeObjectcfMS0_FT_S0_ to i8*)
// CHECK:   }]
// CHECK: }, section "__DATA, __objc_const", align 8

// CHECK: @_PROPERTIES__TtC15objc_properties10SomeObject = private constant { {{.*}}] } {
// CHECK:   i32 16,
// CHECK:   i32 3,
// CHECK:   [3 x { i8*, i8* }] [{
// CHECK:     i8* getelementptr inbounds ([9 x i8]* [[READONLY_NAME]], i64 0, i64 0),
// CHECK:     i8* getelementptr inbounds ([7 x i8]* [[READONLY_ATTRS]], i64 0, i64 0)
// CHECK:   }, {
// CHECK:     i8* getelementptr inbounds ([10 x i8]* [[READWRITE_NAME]], i64 0, i64 0),
// CHECK:     i8* getelementptr inbounds ([7 x i8]* [[READWRITE_ATTRS]], i64 0, i64 0)
// CHECK:   }, {
// CHECK:     i8* getelementptr inbounds ([9 x i8]* [[BAREIVAR_NAME]], i64 0, i64 0),
// CHECK:     i8* getelementptr inbounds ([17 x i8]* [[BAREIVAR_ATTRS]], i64 0, i64 0)
// CHECK:   }]
// CHECK: }, section "__DATA, __objc_const", align 8

// CHECK: [[EXTENSIONPROPERTY_NAME:@.*]] = private unnamed_addr constant [18 x i8] c"extensionProperty\00"

// CHECK: @"_CATEGORY_INSTANCE_METHODS__TtC15objc_properties10SomeObject_$_objc_properties" = private constant { {{.*}}] } {
// CHECK:   i32 24,
// CHECK:   i32 2,
// CHECK:   [2 x { i8*, i8*, i8* }] [{
// CHECK:     { i8* getelementptr inbounds ([18 x i8]* @"\01L_selector_data(extensionProperty)", i64 0, i64 0),
// CHECK:     i8* getelementptr inbounds ([8 x i8]* [[GETTER_SIGNATURE]], i64 0, i64 0),
// CHECK:     i8* bitcast ([[OPAQUE0]]* ([[OPAQUE1]]*, i8*)* @_TToFC15objc_properties10SomeObjectg17extensionPropertyS0_ to i8*)
// CHECK:   }, {
// CHECK:     i8* getelementptr inbounds ([22 x i8]* @"\01L_selector_data(setExtensionProperty:)", i64 0, i64 0),
// CHECK:     i8* getelementptr inbounds ([11 x i8]* [[SETTER_SIGNATURE]], i64 0, i64 0),
// CHECK:     i8* bitcast (void ([[OPAQUE3]]*, i8*, [[OPAQUE4]]*)* @_TToFC15objc_properties10SomeObjects17extensionPropertyS0_ to i8*)
// CHECK:   }]
// CHECK: }, section "__DATA, __objc_const", align 8

// CHECK: @"_CATEGORY_PROPERTIES__TtC15objc_properties10SomeObject_$_objc_properties" = private constant { {{.*}}] } {
// CHECK:   i32 16,
// CHECK:   i32 1,
// CHECK:   [1 x { i8*, i8* }] [{
// CHECK:     i8* getelementptr inbounds ([18 x i8]* [[EXTENSIONPROPERTY_NAME]], i64 0, i64 0),
// CHECK:     i8* getelementptr inbounds ([7 x i8]* [[READWRITE_ATTRS]], i64 0, i64 0)
// CHECK:   }]
// CHECK: }, section "__DATA, __objc_const", align 8

// CHECK: @_INSTANCE_METHODS__TtC15objc_properties4Tree =
// CHECK:    i8* getelementptr inbounds ([7 x i8]* @"\01L_selector_data(parent)", i64 0, i64 0),
// CHECK:    i8* getelementptr inbounds ([8 x i8]* [[GETTER_SIGNATURE]], i64 0, i64 0),
// CHECK:    i8* bitcast (%2* (%2*, i8*)* @_TToFC15objc_properties4Treeg6parentXwGSqS0__ to i8*)
// CHECK:    i8* getelementptr inbounds ([11 x i8]* @"\01L_selector_data(setParent:)", i64 0, i64 0),
// CHECK:    i8* getelementptr inbounds ([11 x i8]* [[SETTER_SIGNATURE]], i64 0, i64 0),
// CHECK:    i8* bitcast (void (%2*, i8*, %2*)* @_TToFC15objc_properties4Trees6parentXwGSqS0__ to i8*)
