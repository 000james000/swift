// FIXME: rdar://problem/19648117 Needs splitting objc parts out
// XFAIL: linux

// RUN: echo '#include "header-to-print.h"' > %t.m
// RUN: %target-swift-ide-test -source-filename %s -print-header -header-to-print %S/Inputs/print_clang_header/header-to-print.h -print-regular-comments --cc-args -Xclang -triple -Xclang %target-triple -isysroot %clang-importer-sdk-path -fsyntax-only %t.m -I %S/Inputs/print_clang_header > %t.txt
// RUN: diff -u %S/Inputs/print_clang_header/header-to-print.h.printed.txt %t.txt

// RUN: echo '#include <Foo/header-to-print.h>' > %t.framework.m
// RUN: sed -e "s:INPUT_DIR:%S/Inputs/print_clang_header:g" -e "s:OUT_DIR:%t:g" %S/Inputs/print_clang_header/Foo-vfsoverlay.yaml > %t.yaml
// RUN: %target-swift-ide-test -source-filename %s -print-header -header-to-print %S/Inputs/print_clang_header/header-to-print.h -print-regular-comments --cc-args -Xclang -triple -Xclang %target-triple -isysroot %clang-importer-sdk-path -fsyntax-only %t.framework.m -F %t -ivfsoverlay %t.yaml -Xclang -fmodule-implementation-of -Xclang Foo > %t.framework.txt
// RUN: diff -u %S/Inputs/print_clang_header/header-to-print.h.printed.txt %t.framework.txt
