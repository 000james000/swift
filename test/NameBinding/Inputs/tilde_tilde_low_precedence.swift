// Part of operators.swift multi-file test.

operator infix ~~ {
  associativity none
  precedence 5
}

public func ~~(x: Int, y: Int) -> Bool {
  return x < y
}

operator infix ~~~ {
  associativity none
  precedence 5
}
