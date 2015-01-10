// RUN: %swift -emit-silgen %s | FileCheck %s

func foo() -> String? { return "" }
func bar() -> String? { return "" }

func a(x: String) {}
func b(x: String) {}
func c(x: String) {}

// CHECK-LABEL: sil hidden @_TF16if_while_binding10if_no_else
func if_no_else() {
  // CHECK:   [[OPT_BUF:%.*]] = alloc_stack $Optional<String>
  // CHECK:   [[FOO:%.*]] = function_ref @_TF16if_while_binding3fooFT_GSqSS_
  // CHECK:   [[OPT_RES:%.*]] = apply [[FOO]]()
  // CHECK:   store [[OPT_RES]] to [[OPT_BUF]]#1
  // CHECK:   [[HAS_VALUE:%.*]] = select_enum_addr [[OPT_BUF]]#1
  // CHECK:   cond_br [[HAS_VALUE]], [[YES:bb[0-9]+]], [[NO:bb[0-9]+]]
  if let x = foo() {
  // CHECK: [[YES]]:
  // CHECK:   [[VAL_BUF:%.*]] = unchecked_take_enum_data_addr [[OPT_BUF]]
  // CHECK:   [[VAL:%.*]] = load [[VAL_BUF]]
  // CHECK:   [[A:%.*]] = function_ref @_TF16if_while_binding
  // CHECK:   retain_value [[VAL]]
  // CHECK:   apply [[A]]([[VAL]])
  // CHECK:   release_value [[VAL]]
  // CHECK:   br [[CONT:bb[0-9]+]]
    a(x)
  }
  // CHECK: [[NO]]:
  // CHECK:   destroy_addr [[OPT_BUF]]
  // CHECK:   br [[CONT]]
}

// CHECK-LABEL: sil hidden @_TF16if_while_binding13if_else_chainFT_T_ : $@thin () -> () {
func if_else_chain() {
  // CHECK:   [[OPT_BUF:%.*]] = alloc_stack $Optional<String>
  // CHECK:   [[FOO:%.*]] = function_ref @_TF16if_while_binding3foo
  // CHECK:   [[OPT_RES:%.*]] = apply [[FOO]]()
  // CHECK:   store [[OPT_RES]] to [[OPT_BUF]]#1
  // CHECK:   [[HAS_VALUE:%.*]] = select_enum_addr [[OPT_BUF]]#1
  // CHECK:   cond_br [[HAS_VALUE]], [[YES:bb[0-9]+]], [[NO:bb[0-9]+]]
  if let x = foo() {
  // CHECK: [[YES]]:
  // CHECK:   [[VAL_BUF:%.*]] = unchecked_take_enum_data_addr [[OPT_BUF]]
  // CHECK:   [[VAL:%.*]] = load [[VAL_BUF]]
  // CHECK:   [[A:%.*]] = function_ref @_TF16if_while_binding
  // CHECK:   retain_value [[VAL]]
  // CHECK:   apply [[A]]([[VAL]])
  // CHECK:   release_value [[VAL]]
  // CHECK:   br [[CONT_A:bb[0-9]+]]
    a(x)
  // CHECK: [[NO]]:
  // CHECK:   destroy_addr [[OPT_BUF]]
  // CHECK:   [[OPT_BUF_2:%.*]] = alloc_stack $Optional<String>
  // CHECK:   cond_br {{%.*}}, [[YES:bb[0-9]+]], [[NO:bb[0-9]+]]
  } else if var y = bar() {
  // CHECK: [[YES]]:
  // CHECK:   alloc_box $String
  // CHECK:   br [[CONT_B:bb[0-9]+]]
    b(y)
  } else {
  // CHECK: [[NO]]:
  // CHECK:   destroy_addr [[OPT_BUF_2]]
  // CHECK:   br [[CONT_B]]
    c("")
  }

  // CHECK: [[CONT_B]]:
  // CHECK:   dealloc_stack [[OPT_BUF_2]]#0

  // CHECK: [[CONT_A]]:
  // CHECK:   dealloc_stack [[OPT_BUF]]#0
}

// CHECK-LABEL: sil hidden @_TF16if_while_binding10while_loopFT_T_ : $@thin () -> () {
func while_loop() {
  // CHECK:   [[OPT_BUF:%.*]] = alloc_stack $Optional<String>
  // CHECK:   br [[LOOP_ENTRY:bb[0-9]+]]
  // CHECK: [[LOOP_ENTRY]]:
  // CHECK:   [[HAS_VALUE:%.*]] = select_enum_addr [[OPT_BUF]]#1
  // CHECK:   cond_br [[HAS_VALUE]], [[LOOP_BODY:bb[0-9]+]], [[LOOP_EXIT:bb[0-9]+]]
  while let x = foo() {
  // CHECK: [[LOOP_BODY]]:
  // CHECK:   cond_br {{%.*}}, [[YES:bb[0-9]+]], [[NO:bb[0-9]+]]
    if let y = bar() {
  // CHECK: [[YES]]:
  // CHECK-NOT: destroy_addr [[OPT_BUF]]
  // CHECK:     br [[LOOP_END:bb[0-9]+]]
      a(y)
      break
    }
  // CHECK: [[NO]]:
  // CHECK:   br [[LOOP_ENTRY]]
  }
  // CHECK: [[LOOP_EXIT]]:
  // CHECK:   destroy_addr [[OPT_BUF]]
  // CHECK:   dealloc_stack [[OPT_BUF]]
}

// Don't leak alloc_stacks for address-only conditional bindings in 'while'.
// <rdar://problem/16202294>
// CHECK-LABEL: sil hidden @_TF16if_while_binding18while_loop_generic
// CHECK:         br [[COND:bb[0-9]+]]
// CHECK:       [[COND]]:
// CHECK:         cond_br {{.*}}, [[LOOP:bb.*]], bb{{.*}}
// CHECK:       [[LOOP]]:
// CHECK:         [[X:%.*]] = alloc_stack $T
// CHECK:         destroy_addr [[X]]
// CHECK:         dealloc_stack [[X]]
// CHECK:         br [[COND]]
func while_loop_generic<T>(source: () -> T?) {
  while let x = source() {
  }
}

// <rdar://problem/19382942> Improve 'if let' to avoid optional pyramid of doom
// CHECK-LABEL: sil hidden @_TF16if_while_binding16while_loop_multiFT_T_
func while_loop_multi() {
  // CHECK: [[OPT_BUF1:%.*]] = alloc_stack $Optional<String>
  // CHECK: [[OPT_BUF2:%.*]] = alloc_stack $Optional<String>
  // CHECK:   br [[LOOP_ENTRY:bb[0-9]+]]
  // CHECK: [[LOOP_ENTRY]]:
  // CHECK:   [[HAS_VALUE1:%.*]] = select_enum_addr [[OPT_BUF1]]#1
  // CHECK:   cond_br [[HAS_VALUE1]], [[CHECKBUF2:bb[0-9]+]], [[LOOP_EXIT1:bb[0-9]+]]
  // CHECK: [[CHECKBUF2]]:
  // CHECK:   [[HAS_VALUE2:%.*]] = select_enum_addr [[OPT_BUF2]]#1
  // CHECK:   cond_br [[HAS_VALUE2]], [[LOOP_BODY:bb[0-9]+]], [[LOOP_EXIT2:bb[0-9]+]]
  // CHECK: [[LOOP_BODY]]:
  while let a = foo(), b = bar() {
    // CHECK:   [[VAL_BUF1:%.*]] = unchecked_take_enum_data_addr [[OPT_BUF1]]#1
    // CHECK:   debug_value {{.*}} : $String  // let a
    // CHECK:   [[VAL_BUF2:%.*]] = unchecked_take_enum_data_addr [[OPT_BUF2]]#1
    // CHECK:   debug_value {{.*}} : $String  // let b
    // CHECK:   debug_value {{.*}} : $String  // let c
    // CHECK:   br [[LOOP_ENTRY]]
    let c = a
  }
  // CHECK: [[LOOP_EXIT2]]:
  // CHECK:   destroy_addr [[OPT_BUF2]]#1
  // CHECK:   br [[LOOP_EXIT1]]
  // CHECK: [[LOOP_EXIT1]]:
  // CHECK:   destroy_addr [[OPT_BUF1]]#1
  // CHECK:   dealloc_stack [[OPT_BUF2]]#0
  // CHECK:   dealloc_stack [[OPT_BUF1]]#0
}



