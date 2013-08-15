// RUN: %swift -emit-silgen %s | FileCheck %s

protocol [class_protocol] ClassBound {}
protocol NotClassBound {}

class C : ClassBound, NotClassBound {}
struct S : NotClassBound {}
struct Unloadable : NotClassBound { var x : NotClassBound }

// CHECK: sil @_T13generic_casts36opaque_archetype_to_opaque_archetypeUS_13NotClassBound___FT1tQ__Q0_
func opaque_archetype_to_opaque_archetype
<T:NotClassBound, U>(t:T) -> U {
  return t as! U
  // CHECK: bb0([[RET:%.*]] : $*U, {{%.*}}: $*T):
  // CHECK:   [[DOWNCAST:%.*]] = downcast_archetype_addr unconditional {{%.*}} : {{.*}} to $*U
  // CHECK:   copy_addr [take] [[DOWNCAST]] to [initialization] [[RET]]
}

// CHECK: sil @_T13generic_casts36opaque_archetype_is_opaque_archetypeUS_13NotClassBound___FT1tQ__Sb
func opaque_archetype_is_opaque_archetype
<T:NotClassBound, U>(t:T) -> Bool {
  return t is U
  // CHECK: [[DOWNCAST:%.*]] = downcast_archetype_addr conditional [[VAL:%.*]] : {{.*}} to $*U
  // CHECK: [[RES:%.*]] = is_nonnull [[DOWNCAST]] : $*U
  // -- we don't consume the checked value
  // CHECK: destroy_addr [[VAL]] : $*T
  // CHECK: return [[RES]] : $Bool
}

// CHECK: sil @_T13generic_casts35opaque_archetype_to_class_archetypeUS_13NotClassBound_S_10ClassBound__FT1tQ__Q0_
func opaque_archetype_to_class_archetype
<T:NotClassBound, U:ClassBound> (t:T) -> U {
  return t as! U
  // CHECK: [[DOWNCAST_ADDR:%.*]] = downcast_archetype_addr unconditional {{%.*}} : {{.*}} to $*U
  // CHECK: [[DOWNCAST:%.*]] = load [[DOWNCAST_ADDR]] : $*U
  // CHECK: return [[DOWNCAST]] : $U
}

// CHECK: sil @_T13generic_casts35opaque_archetype_is_class_archetypeUS_13NotClassBound_S_10ClassBound__FT1tQ__Sb
func opaque_archetype_is_class_archetype
<T:NotClassBound, U:ClassBound> (t:T) -> Bool {
  return t is U
  // CHECK: [[DOWNCAST:%.*]] = downcast_archetype_addr conditional [[VAL:%.*]] : {{.*}} to $*U
  // CHECK-NOT: load
  // CHECK: [[RES:%.*]] = is_nonnull [[DOWNCAST]] : $*U
  // CHECK-NOT: load
  // -- we don't consume the checked value
  // CHECK: destroy_addr [[VAL]] : $*T
  // CHECK: return [[RES]] : $Bool
}

// CHECK: sil @_T13generic_casts34class_archetype_to_class_archetypeUS_10ClassBound_S0___FT1tQ__Q0_
func class_archetype_to_class_archetype
<T:ClassBound, U:ClassBound>(t:T) -> U {
  return t as! U
  // CHECK: [[DOWNCAST:%.*]] = downcast_archetype_ref unconditional {{%.*}} : {{.*}} to $U
  // CHECK: return [[DOWNCAST]] : $U
}

// CHECK: sil @_T13generic_casts34class_archetype_is_class_archetypeUS_10ClassBound_S0___FT1tQ__Sb
func class_archetype_is_class_archetype
<T:ClassBound, U:ClassBound>(t:T) -> Bool {
  return t is U
  // CHECK: [[DOWNCAST:%.*]] = downcast_archetype_ref conditional [[VAL:%.*]] : {{.*}} to $U
  // CHECK: [[RES:%.*]] = is_nonnull [[DOWNCAST]] : $U
  // -- we don't consume the checked value
  // CHECK: release [[VAL]] : $T
  // CHECK: return [[RES]] : $Bool
}

// CHECK: sil @_T13generic_casts38opaque_archetype_to_addr_only_concreteUS_13NotClassBound__FT1tQ__VS_10Unloadable
func opaque_archetype_to_addr_only_concrete
<T:NotClassBound> (t:T) -> Unloadable {
  return t as! Unloadable
  // CHECK: bb0([[RET:%.*]] : $*Unloadable, {{%.*}}: $*T):
  // CHECK:   [[DOWNCAST:%.*]] = downcast_archetype_addr unconditional {{%.*}} : {{.*}} to $*Unloadable
  // CHECK:   copy_addr [take] [[DOWNCAST]] to [initialization] [[RET]]
}

// CHECK: sil @_T13generic_casts38opaque_archetype_is_addr_only_concreteUS_13NotClassBound__FT1tQ__Sb
func opaque_archetype_is_addr_only_concrete
<T:NotClassBound> (t:T) -> Bool {
  return t is Unloadable
  // CHECK: [[DOWNCAST:%.*]] = downcast_archetype_addr conditional [[VAL:%.*]] : {{.*}} to $*Unloadable
  // CHECK: [[RES:%.*]] = is_nonnull [[DOWNCAST]] : $*Unloadable
  // -- we don't consume the checked value
  // CHECK: destroy_addr [[VAL]] : $*T
  // CHECK: return [[RES]] : $Bool
}

// CHECK: sil @_T13generic_casts37opaque_archetype_to_loadable_concreteUS_13NotClassBound__FT1tQ__VS_1S
func opaque_archetype_to_loadable_concrete
<T:NotClassBound>(t:T) -> S {
  return t as! S
  // CHECK: [[DOWNCAST_ADDR:%.*]] = downcast_archetype_addr unconditional {{%.*}} to $*S
  // CHECK: [[DOWNCAST:%.*]] = load [[DOWNCAST_ADDR]] : $*S
  // CHECK: return [[DOWNCAST]] : $S
}

// CHECK: sil @_T13generic_casts37opaque_archetype_is_loadable_concreteUS_13NotClassBound__FT1tQ__Sb
func opaque_archetype_is_loadable_concrete
<T:NotClassBound>(t:T) -> Bool {
  return t is S
  // CHECK: [[DOWNCAST:%.*]] = downcast_archetype_addr conditional [[VAL:%.*]] : {{.*}} to $*S
  // CHECK-NOT: load
  // CHECK: [[RES:%.*]] = is_nonnull [[DOWNCAST]] : $*S
  // CHECK-NOT: load
  // -- we don't consume the checked value
  // CHECK: destroy_addr [[VAL]] : $*T
  // CHECK: return [[RES]] : $Bool
}

// CHECK: sil @_T13generic_casts24class_archetype_to_classUS_10ClassBound__FT1tQ__CS_1C
func class_archetype_to_class
<T:ClassBound>(t:T) -> C {
  return t as! C
  // CHECK: [[DOWNCAST:%.*]] = downcast_archetype_ref unconditional {{%.*}} to $C
  // CHECK: return [[DOWNCAST]] : $C
}

// CHECK: sil @_T13generic_casts24class_archetype_is_classUS_10ClassBound__FT1tQ__Sb
func class_archetype_is_class
<T:ClassBound>(t:T) -> Bool {
  return t is C
  // CHECK: [[DOWNCAST:%.*]] = downcast_archetype_ref conditional [[VAL:%.*]] : {{.*}} to $C
  // CHECK: [[RES:%.*]] = is_nonnull [[DOWNCAST]] : $C
  // -- we don't consume the checked value
  // CHECK: release [[VAL]] : $T
  // CHECK: return [[RES]] : $Bool
}

// CHECK: sil @_T13generic_casts38opaque_existential_to_opaque_archetypeUS_13NotClassBound__FT1pPS0___Q_
func opaque_existential_to_opaque_archetype
<T:NotClassBound>(p:NotClassBound) -> T {
  return p as! T
  // CHECK: bb0([[RET:%.*]] : $*T, {{%.*}}: $*NotClassBound):
  // CHECK:   [[DOWNCAST:%.*]] = project_downcast_existential_addr unconditional [[CONTAINER:%.*]] : {{.*}} to $*T
  // CHECK:   copy_addr [take] [[DOWNCAST]] to [initialization] [[RET]]
  // CHECK:   deinit_existential [[CONTAINER]]
}

// CHECK: sil @_T13generic_casts38opaque_existential_is_opaque_archetypeUS_13NotClassBound__FT1pPS0___Sb
func opaque_existential_is_opaque_archetype
<T:NotClassBound>(p:NotClassBound) -> Bool {
  return p is T
  // CHECK: [[DOWNCAST:%.*]] = project_downcast_existential_addr conditional [[VAL:%.*]] : {{.*}} to $*T
  // CHECK: [[RES:%.*]] = is_nonnull [[DOWNCAST]] : $*T
  // -- we don't consume the checked value
  // CHECK: destroy_addr [[VAL]] : $*NotClassBound
  // CHECK: return [[RES]] : $Bool
}

// CHECK: sil @_T13generic_casts37opaque_existential_to_class_archetypeUS_10ClassBound__FT1pPS_13NotClassBound__Q_
func opaque_existential_to_class_archetype
<T:ClassBound>(p:NotClassBound) -> T {
  return p as! T
  // CHECK: [[DOWNCAST_ADDR:%.*]] = project_downcast_existential_addr unconditional {{%.*}} to $*T
  // CHECK: [[DOWNCAST:%.*]] = load [[DOWNCAST_ADDR]] : $*T
  // CHECK: return [[DOWNCAST]] : $T
}

// CHECK: sil @_T13generic_casts37opaque_existential_is_class_archetypeUS_10ClassBound__FT1pPS_13NotClassBound__Sb
func opaque_existential_is_class_archetype
<T:ClassBound>(p:NotClassBound) -> Bool {
  return p is T
  // CHECK: [[DOWNCAST:%.*]] = project_downcast_existential_addr conditional [[VAL:%.*]] : {{.*}} to $*T
  // CHECK-NOT: load
  // CHECK: [[RES:%.*]] = is_nonnull [[DOWNCAST]] : $*T
  // CHECK-NOT: load
  // -- we don't consume the checked value
  // CHECK: destroy_addr [[VAL]] : $*NotClassBound
  // CHECK: return [[RES]] : $Bool
}

// CHECK: sil @_T13generic_casts36class_existential_to_class_archetypeUS_10ClassBound__FT1pPS0___Q_
func class_existential_to_class_archetype
<T:ClassBound>(p:ClassBound) -> T {
  return p as! T
  // CHECK: [[DOWNCAST:%.*]] = downcast_existential_ref unconditional {{%.*}} to $T
  // CHECK: return [[DOWNCAST]] : $T
}

// CHECK: sil @_T13generic_casts36class_existential_is_class_archetypeUS_10ClassBound__FT1pPS0___Sb
func class_existential_is_class_archetype
<T:ClassBound>(p:ClassBound) -> Bool {
  return p is T
  // CHECK: [[DOWNCAST:%.*]] = downcast_existential_ref conditional [[VAL:%.*]] : {{.*}} to $T
  // CHECK: [[RES:%.*]] = is_nonnull [[DOWNCAST]] : $T
  // -- we don't consume the checked value
  // CHECK: release [[VAL]] : $ClassBound
  // CHECK: return [[RES]] : $Bool
}

// CHECK: sil @_T13generic_casts40opaque_existential_to_addr_only_concreteFT1pPS_13NotClassBound__VS_10Unloadable
func opaque_existential_to_addr_only_concrete(p:NotClassBound) -> Unloadable {
  return p as! Unloadable
  // CHECK: bb0([[RET:%.*]] : $*Unloadable, {{%.*}}: $*NotClassBound):
  // CHECK:   [[DOWNCAST:%.*]] = project_downcast_existential_addr unconditional {{%.*}} to $*Unloadable
  // CHECK:   copy_addr [take] [[DOWNCAST]] to [initialization] [[RET]]
}

// CHECK: sil @_T13generic_casts40opaque_existential_is_addr_only_concreteFT1pPS_13NotClassBound__Sb
func opaque_existential_is_addr_only_concrete(p:NotClassBound) -> Bool {
  return p is Unloadable
  // CHECK: [[DOWNCAST:%.*]] = project_downcast_existential_addr conditional [[VAL:%.*]] : {{.*}} to $*Unloadable
  // CHECK: [[RES:%.*]] = is_nonnull [[DOWNCAST]] : $*Unloadable
  // -- we don't consume the checked value
  // CHECK: destroy_addr [[VAL]] : $*NotClassBound
  // CHECK: return [[RES]] : $Bool
}

// CHECK: sil @_T13generic_casts39opaque_existential_to_loadable_concreteFT1pPS_13NotClassBound__VS_1S
func opaque_existential_to_loadable_concrete(p:NotClassBound) -> S {
  return p as! S
  // CHECK: [[DOWNCAST_ADDR:%.*]] = project_downcast_existential_addr unconditional {{%.*}} to $*S
  // CHECK: [[DOWNCAST:%.*]] = load [[DOWNCAST_ADDR]] : $*S
  // CHECK: return [[DOWNCAST]] : $S
}

// CHECK: sil @_T13generic_casts39opaque_existential_is_loadable_concreteFT1pPS_13NotClassBound__Sb
func opaque_existential_is_loadable_concrete(p:NotClassBound) -> Bool {
  return p is S
  // CHECK: [[DOWNCAST:%.*]] = project_downcast_existential_addr conditional [[VAL:%.*]] : {{.*}} to $*S
  // CHECK-NOT: load
  // CHECK: [[RES:%.*]] = is_nonnull [[DOWNCAST]] : $*S
  // CHECK-NOT: load
  // -- we don't consume the checked value
  // CHECK: destroy_addr [[VAL]] : $*NotClassBound
  // CHECK: return [[RES]] : $Bool
}

// CHECK: sil @_T13generic_casts26class_existential_to_classFT1pPS_10ClassBound__CS_1C
func class_existential_to_class(p:ClassBound) -> C {
  return p as! C
  // CHECK: [[DOWNCAST:%.*]] = downcast_existential_ref unconditional {{%.*}} to $C
  // CHECK: return [[DOWNCAST]] : $C
}

// CHECK: sil @_T13generic_casts26class_existential_is_classFT1pPS_10ClassBound__Sb
func class_existential_is_class(p:ClassBound) -> Bool {
  return p is C
  // CHECK: [[DOWNCAST:%.*]] = downcast_existential_ref conditional [[VAL:%.*]] : {{.*}} to $C
  // CHECK: [[RES:%.*]] = is_nonnull [[DOWNCAST]] : $C
  // -- we don't consume the checked value
  // CHECK: release [[VAL]] : $ClassBound
  // CHECK: return [[RES]] : $Bool
}
