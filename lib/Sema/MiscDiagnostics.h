//===--- MiscDiagnostics.h - AST-Level Diagnostics ------------------------===//
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

#ifndef SWIFT_SEMA_MISC_DIAGNOSTICS_H
#define SWIFT_SEMA_MISC_DIAGNOSTICS_H

namespace swift {
  class DeclContext;
  class Expr;
  class Stmt;
  class TypeChecker;

/// \brief Emit diagnostics for a given expression.
void performExprDiagnostics(TypeChecker &TC, const Expr *E,
                            const DeclContext *DC);
  /// \brief Emit diagnostics for a given statement.
void performStmtDiagnostics(TypeChecker &TC, const Stmt *S);

} // namespace swift

#endif // SWIFT_SEMA_MISC_DIAGNOSTICS_H

