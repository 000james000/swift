// RUN: rm -rf %t && mkdir -p %t

// RUN: %swiftc_driver -driver-print-jobs -target x86_64-apple-macosx10.9 %s 2>&1 > %t.simple.txt
// RUN: FileCheck %s < %t.simple.txt

// RUN: %swiftc_driver -driver-print-jobs -target x86_64-apple-macosx10.9 %s -sdk %S/../Inputs/clang-importer-sdk -Xfrontend -foo -Xfrontend -bar -Xllvm -baz -Xcc -garply -F /path/to/frameworks -F /path/to/more/frameworks -I /path/to/headers -I path/to/more/headers -module-cache-path /tmp/modules -emit-reference-dependencies 2>&1 > %t.complex.txt
// RUN: FileCheck %s < %t.complex.txt
// RUN: FileCheck -check-prefix COMPLEX %s < %t.complex.txt

// RUN: %swiftc_driver -driver-print-jobs -emit-silgen -target x86_64-apple-macosx10.9 %s 2>&1 > %t.silgen.txt
// RUN: FileCheck %s < %t.silgen.txt
// RUN: FileCheck -check-prefix SILGEN %s < %t.silgen.txt

// RUN: %swiftc_driver -driver-print-jobs -emit-sil -target x86_64-apple-macosx10.9 %s 2>&1 > %t.sil.txt
// RUN: FileCheck %s < %t.sil.txt
// RUN: FileCheck -check-prefix SIL %s < %t.sil.txt

// RUN: %swiftc_driver -driver-print-jobs -emit-ir -target x86_64-apple-macosx10.9 %s 2>&1 > %t.ir.txt
// RUN: FileCheck %s < %t.ir.txt
// RUN: FileCheck -check-prefix IR %s < %t.ir.txt

// RUN: %swiftc_driver -driver-print-jobs -emit-bc -target x86_64-apple-macosx10.9 %s 2>&1 > %t.bc.txt
// RUN: FileCheck %s < %t.bc.txt
// RUN: FileCheck -check-prefix BC %s < %t.bc.txt

// RUN: %swiftc_driver -driver-print-jobs -S -target x86_64-apple-macosx10.9 %s 2>&1 > %t.s.txt
// RUN: FileCheck %s < %t.s.txt
// RUN: FileCheck -check-prefix ASM %s < %t.s.txt

// RUN: %swiftc_driver -driver-print-jobs -c -target x86_64-apple-macosx10.9 %s 2>&1 > %t.c.txt
// RUN: FileCheck %s < %t.c.txt
// RUN: FileCheck -check-prefix OBJ %s < %t.c.txt

// RUN: not %swiftc_driver -driver-print-jobs -c -target x86_64-apple-macosx10.9 %s %s 2>&1 | FileCheck -check-prefix DUPLICATE-NAME %s
// RUN: cp %s %t
// RUN: not %swiftc_driver -driver-print-jobs -c -target x86_64-apple-macosx10.9 %s %t/driver-compile.swift 2>&1 | FileCheck -check-prefix DUPLICATE-NAME %s

// REQUIRES: X86


// CHECK: bin/swift
// CHECK: Driver/driver-compile.swift
// CHECK: -o

// COMPLEX: bin/swift
// COMPLEX: -c
// COMPLEX: Driver/driver-compile.swift
// COMPLEX-DAG: -sdk {{.*}}/Inputs/clang-importer-sdk
// COMPLEX-DAG: -foo -bar
// COMPLEX-DAG: -Xllvm -baz
// COMPLEX-DAG: -Xcc -garply
// COMPLEX-DAG: -F /path/to/frameworks -F /path/to/more/frameworks
// COMPLEX-DAG: -I /path/to/headers -I path/to/more/headers
// COMPLEX-DAG: -module-cache-path /tmp/modules
// COMPLEX-DAG: -emit-reference-dependencies-path {{(.*/)?driver-compile[^ /]+}}.swiftdeps
// COMPLEX: -o {{.+}}.o


// SILGEN: bin/swift
// SILGEN: -emit-silgen
// SILGEN: -o -

// SIL: bin/swift
// SIL: -emit-sil{{ }}
// SIL: -o -

// IR: bin/swift
// IR: -emit-ir
// IR: -o -

// BC: bin/swift
// BC: -emit-bc
// BC: -o {{[^-]}}

// ASM: bin/swift
// ASM: -S{{ }}
// ASM: -o -

// OBJ: bin/swift
// OBJ: -c{{ }}
// OBJ: -o {{[^-]}}

// DUPLICATE-NAME: error: filename "driver-compile.swift" used twice: '{{.*}}test/Driver/driver-compile.swift' and '{{.*}}driver-compile.swift'
// DUPLICATE-NAME: note: filenames are used to distinguish private declarations with the same name
