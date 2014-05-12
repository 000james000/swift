// RUN: %swift -parse %s -verify

func takeIntToInt(f: (Int) -> Int) { }
func takeIntIntToInt(f: (Int, Int) -> Int) { }

// Simple closures with anonymous arguments
func simple() {
  takeIntToInt({$0 + 1})
  takeIntIntToInt({$0 + $1 + 1})
}

// Anonymous arguments with inference
func myMap<T, U>(array: T[], f: (T) -> U) -> U[] {}

func testMap(array: Int[]) {
  var farray = myMap(array, { Float($0) })
  var f : Float = farray[0]
  var farray2 = myMap(array, { (x : Int) in Float(x) })
  farray = farray2
}
