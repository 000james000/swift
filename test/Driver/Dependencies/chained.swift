// RUN: rm -rf %t && cp -r %S/Inputs/chained/ %t
// RUN: touch -t 201401240005 %t/*

// RUN: cd %t && %swiftc_driver -c -driver-use-frontend-path %S/Inputs/update-dependencies.py -output-file-map %t/output.json -incremental ./main.swift ./other.swift ./yet-another.swift -module-name main -j1 -v 2>&1 | FileCheck -check-prefix=CHECK-FIRST %s

// CHECK-FIRST: Handled main.swift
// CHECK-FIRST: Handled other.swift

// RUN: cd %t && %swiftc_driver -c -driver-use-frontend-path %S/Inputs/update-dependencies.py -output-file-map %t/output.json -incremental ./main.swift ./other.swift ./yet-another.swift -module-name main -j1 -v 2>&1 | FileCheck -check-prefix=CHECK-SECOND %s

// CHECK-SECOND-NOT: Handled

// RUN: rm %t/other.o
// RUN: cd %t && %swiftc_driver -c -driver-use-frontend-path %S/Inputs/update-dependencies.py -output-file-map %t/output.json -incremental ./main.swift ./other.swift ./yet-another.swift -module-name main -j1 -v 2>&1 | FileCheck -check-prefix=CHECK-THIRD %s

// CHECK-THIRD: Handled other.swift
// CHECK-THIRD-DAG: Handled main.swift
// CHECK-THIRD-DAG: Handled yet-another.swift

// RUN: rm %t/other.o
// RUN: cd %t && %swiftc_driver -c -driver-use-frontend-path %S/Inputs/update-dependencies.py -output-file-map %t/output.json -incremental ./other.swift ./main.swift ./yet-another.swift -module-name main -j1 -v 2>&1 | FileCheck -check-prefix=CHECK-THIRD %s

// RUN: rm %t/other.o
// RUN: cd %t && %swiftc_driver -c -driver-use-frontend-path %S/Inputs/update-dependencies.py -output-file-map %t/output.json -incremental ./yet-another.swift ./other.swift ./main.swift -module-name main -j1 -v 2>&1 | FileCheck -check-prefix=CHECK-THIRD %s

// RUN: rm %t/other.o
// RUN: cd %t && %swiftc_driver -c -driver-use-frontend-path %S/Inputs/update-dependencies.py -output-file-map %t/output.json -incremental ./other.swift ./yet-another.swift ./main.swift -module-name main -j1 -v 2>&1 | FileCheck -check-prefix=CHECK-THIRD %s
