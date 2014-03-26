//===--- ModuleInterfacePrinting.h - Routines to print module interface ---===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IDE_MODULE_INTERFACE_PRINTING_H
#define SWIFT_IDE_MODULE_INTERFACE_PRINTING_H

#include "swift/Basic/LLVM.h"

namespace swift {
class ASTPrinter;
class Module;
struct PrintOptions;

namespace ide {
void printModuleInterface(Module *M, ASTPrinter &Printer,
                          const PrintOptions &Options);
} // namespace ide

} // namespace swift

#endif // SWIFT_IDE_MODULE_INTERFACE_PRINTING_H

