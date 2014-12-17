/// a ==> bad ==> c ==> d | b --> bad --> e ==> f

// RUN: rm -rf %t && cp -r %S/Inputs/fail-chained/ %t
// RUN: touch -t 201401240005 %t/*

// RUN: cd %t && %swiftc_driver -c -driver-use-frontend-path %S/Inputs/update-dependencies.py -output-file-map %t/output.json -incremental ./a.swift ./b.swift ./c.swift ./d.swift ./e.swift ./f.swift ./bad.swift -module-name main -j1 -v 2>&1 | FileCheck -check-prefix=CHECK-FIRST %s

// CHECK-FIRST: Handled a.swift
// CHECK-FIRST: Handled b.swift
// CHECK-FIRST: Handled c.swift
// CHECK-FIRST: Handled d.swift
// CHECK-FIRST: Handled e.swift
// CHECK-FIRST: Handled f.swift
// CHECK-FIRST: Handled bad.swift

// RUN: rm %t/a.o
// RUN: cd %t && not %swiftc_driver -c -driver-use-frontend-path %S/Inputs/update-dependencies-bad.py -output-file-map %t/output.json -incremental ./a.swift ./b.swift ./c.swift ./d.swift ./e.swift ./f.swift ./bad.swift -module-name main -j1 -v > %t/a.txt 2>&1
// RUN: FileCheck -check-prefix=CHECK-A %s < %t/a.txt
// RUN: FileCheck -check-prefix=NEGATIVE-A %s < %t/a.txt
// RUN: FileCheck -check-prefix=CHECK-RECORD-A %s < %t/main~buildrecord.swiftdeps

// CHECK-A: Handled a.swift
// CHECK-A: Handled bad.swift
// NEGATIVE-A-NOT: Handled b.swift
// NEGATIVE-A-NOT: Handled c.swift
// NEGATIVE-A-NOT: Handled d.swift
// NEGATIVE-A-NOT: Handled e.swift
// NEGATIVE-A-NOT: Handled f.swift

// CHECK-RECORD-A-DAG: - "./a.swift"
// CHECK-RECORD-A-DAG: - "./b.swift"
// CHECK-RECORD-A-DAG: - !dirty "./c.swift"
// CHECK-RECORD-A-DAG: - !dirty "./d.swift"
// CHECK-RECORD-A-DAG: - !private "./e.swift"
// CHECK-RECORD-A-DAG: - "./f.swift"
// CHECK-RECORD-A-DAG: - !dirty "./bad.swift"


// RUN: rm -rf %t && cp -r %S/Inputs/fail-chained/ %t
// RUN: touch -t 201401240005 %t/*

// RUN: cd %t && %swiftc_driver -c -driver-use-frontend-path %S/Inputs/update-dependencies.py -output-file-map %t/output.json -incremental ./a.swift ./b.swift ./c.swift ./d.swift ./e.swift ./f.swift ./bad.swift -module-name main -j1 -v 2>&1 | FileCheck -check-prefix=CHECK-FIRST %s

// RUN: rm %t/b.o
// RUN: cd %t && not %swiftc_driver -c -driver-use-frontend-path %S/Inputs/update-dependencies-bad.py -output-file-map %t/output.json -incremental ./a.swift ./b.swift ./c.swift ./d.swift ./e.swift ./f.swift ./bad.swift -module-name main -j1 -v > %t/b.txt 2>&1
// RUN: FileCheck -check-prefix=CHECK-B %s < %t/b.txt
// RUN: FileCheck -check-prefix=NEGATIVE-B %s < %t/b.txt
// RUN: FileCheck -check-prefix=CHECK-RECORD-B %s < %t/main~buildrecord.swiftdeps

// CHECK-B: Handled b.swift
// CHECK-B: Handled bad.swift
// NEGATIVE-B-NOT: Handled a.swift
// NEGATIVE-B-NOT: Handled c.swift
// NEGATIVE-B-NOT: Handled d.swift
// NEGATIVE-B-NOT: Handled e.swift
// NEGATIVE-B-NOT: Handled f.swift

// CHECK-RECORD-B-DAG: - "./a.swift"
// CHECK-RECORD-B-DAG: - "./b.swift"
// CHECK-RECORD-B-DAG: - "./c.swift"
// CHECK-RECORD-B-DAG: - "./d.swift"
// CHECK-RECORD-B-DAG: - "./e.swift"
// CHECK-RECORD-B-DAG: - "./f.swift"
// CHECK-RECORD-B-DAG: - !private "./bad.swift"


// RUN: rm -rf %t && cp -r %S/Inputs/fail-chained/ %t
// RUN: touch -t 201401240005 %t/*

// RUN: cd %t && %swiftc_driver -c -driver-use-frontend-path %S/Inputs/update-dependencies.py -output-file-map %t/output.json -incremental ./a.swift ./b.swift ./c.swift ./d.swift ./e.swift ./f.swift ./bad.swift -module-name main -j1 -v 2>&1 | FileCheck -check-prefix=CHECK-FIRST %s

// RUN: rm %t/bad.o
// RUN: cd %t && not %swiftc_driver -c -driver-use-frontend-path %S/Inputs/update-dependencies-bad.py -output-file-map %t/output.json -incremental ./a.swift ./b.swift ./c.swift ./d.swift ./e.swift ./f.swift ./bad.swift -module-name main -j1 -v > %t/b.txt 2>&1
// RUN: FileCheck -check-prefix=CHECK-BAD %s < %t/b.txt
// RUN: FileCheck -check-prefix=NEGATIVE-BAD %s < %t/b.txt
// RUN: FileCheck -check-prefix=CHECK-RECORD-BAD %s < %t/main~buildrecord.swiftdeps

// CHECK-BAD: Handled bad.swift
// NEGATIVE-BAD-NOT: Handled a.swift
// NEGATIVE-BAD-NOT: Handled b.swift
// NEGATIVE-BAD-NOT: Handled c.swift
// NEGATIVE-BAD-NOT: Handled d.swift
// NEGATIVE-BAD-NOT: Handled e.swift
// NEGATIVE-BAD-NOT: Handled f.swift

// CHECK-RECORD-BAD-DAG: - "./a.swift"
// CHECK-RECORD-BAD-DAG: - "./b.swift"
// CHECK-RECORD-BAD-DAG: - !dirty "./c.swift"
// CHECK-RECORD-BAD-DAG: - !dirty "./d.swift"
// CHECK-RECORD-BAD-DAG: - !private "./e.swift"
// CHECK-RECORD-BAD-DAG: - "./f.swift"
// CHECK-RECORD-BAD-DAG: - "./bad.swift"
