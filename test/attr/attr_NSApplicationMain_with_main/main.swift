// expected-note{{top-level code defined in this source file}}
// RUN: rm -rf %t/clang-module-cache
// RUN: %swift %clang-importer-sdk -parse -verify -module-cache-path %t/clang-module-cache %s %S/delegate.swift
// XFAIL: linux

hi()
