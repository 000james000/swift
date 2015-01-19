// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=PLAIN_TOP_LEVEL_1 > %t.toplevel.txt
// RUN: FileCheck %s -check-prefix=PLAIN_TOP_LEVEL < %t.toplevel.txt
// RUN: FileCheck %s -check-prefix=NO_STDLIB_PRIVATE < %t.toplevel.txt

// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=PRIVATE_NOMINAL_MEMBERS_1 > %t.members.txt
// RUN: FileCheck %s -check-prefix=PRIVATE_NOMINAL_MEMBERS_1 < %t.members.txt
// RUN: FileCheck %s -check-prefix=NO_STDLIB_PRIVATE < %t.members.txt

// NO_STDLIB_PRIVATE: Begin completions
// NO_STDLIB_PRIVATE-NOT: Decl[FreeFunction]{{[^:]*}}: _
// NO_STDLIB_PRIVATE: End completions

#^PLAIN_TOP_LEVEL_1^#

// PLAIN_TOP_LEVEL: Begin completions
// PLAIN_TOP_LEVEL-DAG: Decl[Struct]/OtherModule: Array[#Array#]{{$}}
// PLAIN_TOP_LEVEL: End completions

func privateNominalMembers(a: String) {
  a.#^PRIVATE_NOMINAL_MEMBERS_1^#
}

// PRIVATE_NOMINAL_MEMBERS_1: Begin completions
// PRIVATE_NOMINAL_MEMBERS_1-DAG: Decl[InstanceVar]/CurrNominal: startIndex[#String.Index#]{{$}}
// PRIVATE_NOMINAL_MEMBERS_1: End completions

