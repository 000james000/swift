class SomeClass {
  class NestedClass {}

  static func staticFunc1() -> Int {}
  static var staticVar1: Int
}
struct SomeStruct {
  init() {}
  init(a: Int) {}
}
enum SomeEnum {
  case Foo
}
protocol SomeProtocol {
  typealias Foo
}
protocol SomeExistential {
}
class SomeProtocolImpl : SomeProtocol {}
typealias SomeTypealias = Swift.Int
var someGlobal: Int
func someFunc() {}

