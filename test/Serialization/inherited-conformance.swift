// RUN: rm -rf %t
// RUN: mkdir %t
// RUN: %target-swift-frontend -emit-module -o %t -module-name Base %S/Inputs/inherited-conformance-base.swift
// RUN: %target-swift-frontend -emit-module -o %t -module-name User -I %t %S/Inputs/inherited-conformance-user.swift
// RUN: %target-swift-frontend -parse -I %t %s

import User

var test = User.OneToAThousand()
println(test[test.startIndex])
