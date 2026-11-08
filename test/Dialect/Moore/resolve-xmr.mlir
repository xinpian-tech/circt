// RUN: circt-opt --moore-resolve-xmr --split-input-file --verify-diagnostics %s | FileCheck %s

// A reference into a sibling subtree: exported from the target's module up to
// the common ancestor, passed down from there.
// CHECK-LABEL: moore.module private @Core(out state : !moore.ref<l1>)
// CHECK:   %state = moore.variable : <l1>
// CHECK:   moore.output %state : !moore.ref<l1>
moore.module private @Core() {
  %state = moore.variable : <l1>
  moore.output
}
// CHECK-LABEL: moore.module private @Ctrl(in %clk : !moore.l1, in %u_core.state : !moore.ref<l1>)
// CHECK:   moore.read %u_core.state
// CHECK-NOT: moore.xmr
moore.module private @Ctrl(in %clk : !moore.l1) {
  %0 = moore.xmr up 1 path ["u_core"] target "state" : <l1>
  %1 = moore.read %0 : ref<l1>
  moore.output
}
// CHECK-LABEL: moore.module @Top(in %clk : !moore.l1)
// CHECK:   %u_core.state = moore.instance "u_core" @Core() -> (state: !moore.ref<l1>)
// CHECK:   moore.instance "u_ctrl" @Ctrl(clk: %clk: !moore.l1, u_core.state: %u_core.state: !moore.ref<l1>) -> ()
moore.module @Top(in %clk : !moore.l1) {
  moore.instance "u_core" @Core() -> ()
  moore.instance "u_ctrl" @Ctrl(clk: %clk: !moore.l1) -> ()
  moore.output
}

// -----

// A module instantiated in two instances refers to the target of each; two
// references to the same target share the ports.
// CHECK-LABEL: moore.module private @Leaf(out x : !moore.ref<l1>)
moore.module private @Leaf() {
  %x = moore.variable : <l1>
  moore.output
}
// CHECK-LABEL: moore.module private @Chk(in %l.x : !moore.ref<l1>)
// CHECK:   moore.read %l.x
// CHECK:   moore.read %l.x
moore.module private @Chk() {
  %0 = moore.xmr up 1 path ["l"] target "x" : <l1>
  %1 = moore.read %0 : ref<l1>
  %2 = moore.xmr up 1 path ["l"] target "x" : <l1>
  %3 = moore.read %2 : ref<l1>
  moore.output
}
// CHECK-LABEL: moore.module private @Mid()
// CHECK:   %l.x = moore.instance "l" @Leaf() -> (x: !moore.ref<l1>)
// CHECK:   moore.instance "c" @Chk(l.x: %l.x: !moore.ref<l1>) -> ()
moore.module private @Mid() {
  moore.instance "l" @Leaf() -> ()
  moore.instance "c" @Chk() -> ()
  moore.output
}
// CHECK-LABEL: moore.module @Top2()
// CHECK:   moore.instance "m1" @Mid() -> ()
// CHECK:   moore.instance "m2" @Mid() -> ()
moore.module @Top2() {
  moore.instance "m1" @Mid() -> ()
  moore.instance "m2" @Mid() -> ()
  moore.output
}

// -----

// Upward references (named after the common ancestor), a downward reference
// used in a procedure, and a net as target.
// CHECK-LABEL: moore.module private @Sub(in %Root.r : !moore.ref<i32>, in %Root.w : !moore.ref<l1>, out v : !moore.ref<i32>)
// CHECK:   moore.read %Root.r
// CHECK:   moore.read %Root.w
moore.module private @Sub() {
  %v = moore.variable : <i32>
  %0 = moore.xmr up 1 path [] target "r" : <i32>
  %1 = moore.read %0 : ref<i32>
  %2 = moore.xmr up 1 path [] target "w" : <l1>
  %3 = moore.read %2 : ref<l1>
  moore.output
}
// CHECK-LABEL: moore.module @Root()
// CHECK:   %r = moore.variable : <i32>
// CHECK:   %w = moore.net wire : <l1>
// CHECK:   %s.v = moore.instance "s" @Sub(Root.r: %r: !moore.ref<i32>, Root.w: %w: !moore.ref<l1>) -> (v: !moore.ref<i32>)
// CHECK:   moore.procedure initial {
// CHECK:     moore.read %s.v
moore.module @Root() {
  %r = moore.variable : <i32>
  %w = moore.net wire : <l1>
  moore.instance "s" @Sub() -> ()
  moore.procedure initial {
    %0 = moore.xmr up 0 path ["s"] target "v" : <i32>
    %1 = moore.read %0 : ref<i32>
    moore.return
  }
  moore.output
}

// -----

moore.module private @A() {
  // expected-error @below {{hierarchical reference goes above module `A`, which is not instantiated}}
  %0 = moore.xmr up 1 path [] target "x" : <l1>
  moore.output
}

// -----

moore.module private @B() {
  moore.output
}
moore.module @NoInst() {
  moore.instance "b" @B() -> ()
  // expected-error @below {{hierarchical reference path: no instance `c` in module `NoInst`}}
  %0 = moore.xmr up 0 path ["c"] target "x" : <l1>
  moore.output
}

// -----

moore.module private @C() {
  %y = moore.variable : <l1>
  moore.output
}
moore.module @NoTarget() {
  moore.instance "c" @C() -> ()
  // expected-error @below {{hierarchical reference target `x` not found in module `C`}}
  %0 = moore.xmr up 0 path ["c"] target "x" : <l1>
  moore.output
}

// -----

moore.module private @D() {
  %x = moore.variable : <i8>
  moore.output
}
moore.module @WrongType() {
  moore.instance "d" @D() -> ()
  // expected-error @below {{hierarchical reference has type '!moore.ref<l1>', but its target `x` in `D` has type '!moore.ref<i8>'}}
  %0 = moore.xmr up 0 path ["d"] target "x" : <l1>
  moore.output
}

// -----

moore.module private @E() {
  %0 = moore.variable name "x" : <l1>
  %1 = moore.variable name "x" : <l1>
  moore.output
}
moore.module @Ambiguous() {
  moore.instance "e" @E() -> ()
  // expected-error @below {{hierarchical reference target `x` is not unique in module `E`}}
  %0 = moore.xmr up 0 path ["e"] target "x" : <l1>
  moore.output
}
