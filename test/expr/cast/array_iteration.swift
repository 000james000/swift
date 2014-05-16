// RUN: %swift -parse %s -verify

class View {
	var subviews: Array<AnyObject>! = []
}

var rootView = View()
var v = [View(), View()]
rootView.subviews = v

rootView.subviews as View[]

for view in (rootView.subviews as View[])! {
	println("found subview")
}

(rootView.subviews!) as View[]

(rootView.subviews) as View[]

var ao: AnyObject[] = []
ao as View[] // works


var b = Array<(String, Int)>()

for x in b {
  println("hi")
}

var c : Array<(String, Int)>! = Array()

for x in c {
  println("hi")
}

var d : Array<(String, Int)>? = Array()

for x in d! {
	println("hi")
}