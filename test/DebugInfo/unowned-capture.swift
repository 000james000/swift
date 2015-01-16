// RUN: %swift -primary-file %s -emit-ir -g -o - | FileCheck %s
class Foo
{
  func DefinesClosure (a_string : String) -> () -> String
  {
    // Verify that we only emit the implicit argument,
    // and not the unowned local copy of self.
    //
    // CHECK-NOT: [ DW_TAG_auto_variable ] [self]
    // CHECK: [ DW_TAG_arg_variable ] [self]
    // CHECK-NOT: [ DW_TAG_auto_variable ] [self]
    return { [unowned self] in
             var tmp_string = a_string
             return tmp_string
           }
  }
}
