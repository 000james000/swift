// RUN: %swift -parse -enable-source-import -primary-file %s %S/Inputs/multi-file-with-main/main.swift -module-name=MultiFile -sdk "" -verify

func testOperator() {
  // FIXME: Lousy error message.
  let x: Int = 1 +++ "abc" // expected-error {{binary operator '+++' cannot be applied operands of type 'Int' and 'String'}}
  println(x)
}
