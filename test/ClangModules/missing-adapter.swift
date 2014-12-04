// RUN: rm -rf %t
// RUN: mkdir -p %t

// RUN: %swift -emit-module %S/Inputs/adapter.swift -sdk %S/Inputs -I %S/Inputs/custom-modules -module-name ClangModuleWithAdapter -o %t
// RUN: %swift %s -target x86_64-apple-macosx10.9 -sdk %S/Inputs -I %S/Inputs/custom-modules -I %t -parse
// RUN: %swift %s -target x86_64-apple-macosx10.9 -I %t -parse -show-diagnostics-after-fatal -verify

// When run without the underlying SDK, we should get an error here.
import ClangModuleWithAdapter // expected-error{{cannot load underlying module for 'ClangModuleWithAdapter'}}
