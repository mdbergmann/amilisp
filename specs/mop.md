# CLOS Metaobject Protocol (MOP)

## Goal

Implement enough of the CLOS Metaobject Protocol (AMOP, Kiczales et al.) to run `closer-mop` without stubs, and by extension the libraries that depend on it: `serapeum`, `lisp-namespace`, `trivia`, `mito`, and the rest of the modern CL ecosystem that expects a MOP-capable host.

MOP is not part of ANSI CL, but every serious CL implementation supports it and large swaths of quicklisp assume it. Without it we stop at the first `class-slots` call.

## Scope

Target: the subset exported by `closer-mop` (see `closer-mop-packages.lisp` lines 22-134). This is the de-facto portable MOP — narrower than full AMOP, but what libraries actually call.

## Current State (audit)

**DONE** — first-class class metaobjects (`standard-class`, `standard-object`, `built-in-class` as 11-slot structs); C3 CPL; GF dispatch with class + EQL caching; standard method combination (primary/:before/:after/:around); `defclass`/`defgeneric`/`defmethod`; `make-instance`/`allocate-instance`/`initialize-instance`/`shared-initialize`; `call-next-method`/`next-method-p`.

**PARTIAL** — slot definitions stored as plists (not metaobjects); method combination hardcoded to standard; EQL specializers represented as `(eql value)` cons cells; finalization is implicit at `defclass` time with no hook points; no `:allocation :class`.

**MISSING** — `slot-value-using-class` protocol; `finalize-inheritance` / `compute-slots` / `compute-effective-slot-definition` / `compute-default-initargs` / `validate-superclass` hooks; `funcallable-standard-class`; reified EQL specializer objects; method metaobject protocol (`add-method`/`remove-method`/`ensure-method`); `define-method-combination`; dependents API.

Full audit notes: see commit history and `lib/clos.lisp` lines 23-90, 338-439, 477-629, 775-1184.

## Design Principles

1. **Don't break what works.** The dispatch cache, C3, and `initialize-instance` path carry most of the test suite. Phases that touch these must keep all 693 host tests green.
2. **Fixed class layout, metaclass extension on top.** The first 12 slots of every class metaobject are a *fixed* struct layout (name, direct-superclasses, …) accessed by hard-coded index, so we don't pay the cost of full "classes are instances of metaclasses that are instances of metaclasses" dispatch on a 68020. A user-defined metaclass is a subclass of `standard-class`; its instances (the classes it creates) reuse those fixed 12 slots and append any metaclass-specific slots at indices 12+. A class created with `:metaclass M` is allocated as a struct of type `M`, so `class-of` returns `M` and metaclass methods (`allocate-instance`, `initialize-instance`) dispatch on it.
3. **User-defined metaclasses (limited).** Supported to the extent the ecosystem (serapeum's `abstract-standard-class` and `topmost-object-class`) needs: defining a subclass of `standard-class`, using it via `:metaclass`, specializing `allocate-instance`/`initialize-instance`/`validate-superclass` on it, metaclass slots with `:initarg`/`:reader`, and `:default-initargs` on the metaclass. Class creation for a `:metaclass`-bearing class routes through the `initialize-instance` GF so metaclass `:around` methods can rewrite `:direct-superclasses`. `validate-superclass` still defaults to T between known metaobject classes. Full meta-recursion (metaclasses of metaclasses with their own non-standard layout) remains out of scope.
4. **Hook points, not full protocol.** The AMOP protocol is huge. We implement the hook points closer-mop and serapeum actually call, not every GF in the book.
5. **C89/C99 compatible**, 32-bit-clean heap structs, bounded memory. No new allocation patterns that would break on 8MB Amiga.

## Phased Plan

Each phase lands as one commit with tests. Phases depend on each other left-to-right.

### Phase 1 — Reify slot definitions (foundational, ~500 LOC Lisp)

Replace slot plists with `standard-direct-slot-definition` and `standard-effective-slot-definition` struct instances.

**Deliverables:**
- `slot-definition`, `standard-slot-definition`, `standard-direct-slot-definition`, `standard-effective-slot-definition` classes (structs behind the scenes).
- Accessors: `slot-definition-name`, `-initargs`, `-initform`, `-initfunction`, `-type`, `-allocation`, `-readers`, `-writers`, `-location`, `-documentation`.
- `direct-slot-definition-class`, `effective-slot-definition-class` (GFs returning the class object; default returns the standard one).
- Migrate internal callers in `lib/clos.lisp` (defclass expansion, `%compute-effective-slots`, `%build-slot-index-table`, `make-instance` initform processing) from plist access to accessor calls.

**Acceptance:**
- `(class-direct-slots c)` returns list of `standard-direct-slot-definition` instances.
- `(class-slots c)` returns list of `standard-effective-slot-definition` instances (new public GF; `class-effective-slots` becomes an alias).
- All existing CLOS tests pass unchanged.
- New tests: slot-definition metaobject identity, accessor round-trips, `direct-slot-definition-class` customizable via method.

### Phase 2 — Class protocol hooks (~300 LOC Lisp)

Expose the class-finalization protocol as GFs with default methods that wrap existing code.

**Deliverables:**
- GFs: `finalize-inheritance`, `compute-slots`, `compute-effective-slot-definition`, `compute-class-precedence-list`, `compute-default-initargs`, `validate-superclass`, `class-prototype`, `class-finalized-p` (public reader).
- `ensure-class` / `ensure-class-using-class` as GFs with default methods delegating to existing `%ensure-class`.
- Defclass expansion routes through `ensure-class` so user methods see `:direct-superclasses`, `:direct-slots`, `:direct-default-initargs` as keyword args.

**Acceptance:**
- `(finalize-inheritance c)` callable, idempotent, marks `class-finalized-p` true.
- `closer-mop:class-slots`, `class-direct-slots`, `class-precedence-list` return expected values.
- A user `defmethod compute-slots :around` observes the standard computation and can post-process.

### Phase 3 — `slot-value-using-class` protocol (~200 LOC Lisp + minor C)

The 3-arg slot access GF layer that libraries hook for lazy/memoized/instrumented slots.

**Deliverables:**
- GFs: `slot-value-using-class`, `(setf slot-value-using-class)`, `slot-boundp-using-class`, `slot-makunbound-using-class` — all dispatching on `(class instance effective-slot-definition)`.
- `slot-value`, `(setf slot-value)`, `slot-boundp`, `slot-makunbound` become thin wrappers that resolve the effective slot and dispatch.
- Fast path: when no user method exists for the given class, skip dispatch and go straight to `%struct-ref` (preserve current performance).

**Acceptance:**
- User `defmethod slot-value-using-class :around ((class my-class) inst slot)` is called on `(slot-value inst 'x)`.
- Existing slot-access tests remain green with no measurable perf regression.
- Structs continue to use the struct-slot fallback path we just added (Phase 3 only applies to CLOS instances).

### Phase 4 — `:allocation :class` (~150 LOC Lisp)

Class-allocated slots: shared across all instances, stored on the class.

**Deliverables:**
- `:allocation` parsed in defclass; stored on slot-definition.
- Effective slot layout gains a per-class-slot cell for class-allocated slots.
- `slot-value` redirects to the class-slot cell when `slot-definition-allocation` returns `:class`.
- `slot-definition-location` returns an integer for instance slots, a `cons` cell for class slots (matches AMOP).

**Acceptance:**
- Two instances of a class with a `:allocation :class` slot share writes.
- Subclassing preserves sharing unless the subclass redefines the slot.

### Phase 5 — Reified EQL specializers (~150 LOC Lisp)

Convert `(eql value)` cons representation to `eql-specializer` metaobjects, interned per value.

**Deliverables:**
- `eql-specializer` class with single slot `object`.
- `intern-eql-specializer` (public), backing weak-ish table (we have no weak refs — use a strong `eq` hash, accept the leak).
- `eql-specializer-object` accessor.
- Dispatch paths updated: `%compute-eql-value-sets`, `%gf-dispatch-eql`, method `specializers` lists.
- `extract-specializer-names` returns `(eql value)` form for backward compat.

**Acceptance:**
- `(defmethod foo ((x (eql 42))) ...)` still works, but method's `specializers` slot now contains an `eql-specializer` object.
- `(eq (intern-eql-specializer 42) (intern-eql-specializer 42))` is T.

### Phase 6 — Funcallable standard class (~250 LOC Lisp + ~50 LOC C)

Formalize generic functions as instances of `funcallable-standard-class`, callable via `set-funcallable-instance-function`.

**Deliverables:**
- `funcallable-standard-class` and `funcallable-standard-object` classes.
- `set-funcallable-instance-function` — updates the dispatch entry for an instance (our GF struct already has a discriminating-function slot; just expose it).
- `standard-instance-access` / `funcallable-standard-instance-access` — raw slot-index access (no before/after hooks, no bound check).
- `compute-discriminating-function` as a GF whose default returns the current cached dispatch.

**Acceptance:**
- `(typep #'foo 'funcallable-standard-object)` → T for any GF.
- A user can `set-funcallable-instance-function` on a GF to replace dispatch.
- `(standard-instance-access inst idx)` equals `(%struct-ref inst idx)`.

### Phase 7 — Method metaobject protocol (~200 LOC Lisp)

Expose method creation/installation as a protocol.

**Deliverables:**
- `add-method`, `remove-method` GFs operating on a generic-function + method.
- `ensure-method` (non-standard but in closer-mop) — convenience to define a method programmatically.
- `extract-lambda-list`, `extract-specializer-names` — split a method-defining form into its parts.
- `make-method-lambda` GF — default method builds the method function from body + specializer info (currently inlined in defmethod expansion; refactor to call this GF).
- `method-function`, `method-specializers`, `method-qualifiers`, `method-generic-function`, `method-lambda-list` — all already exist, ensure they're exported and return correct metaobject types.

**Acceptance:**
- Programmatically constructing a method via `make-instance 'standard-method` + `add-method` yields a dispatchable method.
- `closer-mop:ensure-method` works.

### Phase 8 — `define-method-combination` (~400 LOC Lisp)

User-defined method combinations (short-form and long-form).

**Deliverables:**
- `define-method-combination` macro (short-form: `+`, `and`, `or`, `list`, `progn`, etc.; long-form: custom method group qualifiers).
- `find-method-combination` GF.
- Generic function's `method-combination` slot wired into the dispatch pipeline (currently hardcoded standard).
- Built-in combinations preregistered.

**Acceptance:**
- `(defgeneric foo (x) (:method-combination +))` combines primary-method results with `+`.
- `(define-method-combination my-combo (:documentation ...) ((primary ())) (call-method (first primary)))` works.

### Phase 9 — Dependents API (~100 LOC Lisp) [OPTIONAL]

Observer protocol for recompiling/invalidating things when a class or GF changes.

**Deliverables:**
- `add-dependent`, `remove-dependent`, `map-dependents`, `update-dependent`.
- Hook calls into `ensure-class`, `add-method`, `remove-method`, `ensure-generic-function`.

**Acceptance:**
- A dependent object receives `update-dependent` notifications on relevant changes.

Low priority — few libraries actually use it; skip unless something needs it.

### Phase 10 — closer-mop integration (~1-2 days)

Fork `closer-mop` to `lib/local-projects/closer-mop/` with `#+clamiga` branches that map to our bindings, or ship an ASDF system `closer-mop-clamiga` that provides the package.

**Deliverables:**
- `closer-mop-packages.lisp` patched to recognize `:amilisp` feature.
- `closer-mop.lisp` (implementation file) gets amilisp branches for each protocol function.
- Minimal file: most symbols resolve to our CL package since we now export them at MOP names.
- Add to `lib/quicklisp-compat.lisp` as a preloadable system if appropriate.

**Acceptance:**
- `(ql:quickload :closer-mop)` loads.
- `(ql:quickload :serapeum)` loads.

## Non-Goals

- **User-defined metaclasses (limited support — see Design Principle 3).** A subclass of `standard-class` may be defined and used via `:metaclass`; its instances reuse the fixed 12-slot class layout with metaclass slots appended at index 12+. Specializing `allocate-instance`/`initialize-instance`/`validate-superclass` on the metaclass works, as do metaclass slots and `:default-initargs`. Class creation routes through `initialize-instance` so metaclass `:around` methods can rewrite `:direct-superclasses`. What remains out of scope: metaclasses whose *own* layout diverges from `standard-class`, and arbitrary meta-recursion. See `tests/test_mop_metaclass.c` (host) and the "User-defined metaclasses" section of `tests/amiga/run-tests.lisp` (Amiga) for the supported patterns (abstract classes, topmost-object injection), plus the metaclass cases in `tests/test_gc_stress_regression.sh`.
- **Class redefinition with instance migration.** `update-instance-for-redefined-class` and `update-instance-for-different-class` stay stubbed. (Class redefinition itself is CLHS 4.3.6-conformant: `defclass`/`ensure-class` on an existing name updates the same metaobject in place — `find-class` identity and methods specialized on the class survive — and re-finalizes existing subclasses; with a user metaclass it goes through `reinitialize-instance`. Only *existing instances* are not migrated to the new layout.)
- **Full AMOP compliance.** We implement the closer-mop-exported subset. PCL's internal protocol symbols are not exposed.
- **Method combination long-form with arbitrary dispatch.** We support the common shapes (grouped qualifiers, call-method, :arguments), not the full grammar.

## Testing Strategy

- **Each phase adds a `tests/test_mop_<phase>.c`** in the existing host test framework. Phase lands when new tests pass and the full suite (now 693+) stays green.
- **Amiga verification** per commit via `make -f Makefile.cross test-amiga` — we're on a tight memory budget and MOP dispatch chains are deep (see log4cl investigation).
- **Integration checkpoints:**
  - After Phase 3: `(ql:quickload :alexandria)` — sanity, should still work.
  - After Phase 5: `(ql:quickload :trivia)` — heavy EQL specializer user.
  - After Phase 7: `(ql:quickload :closer-mop)` — the goal.
  - After Phase 10: `(ql:quickload :serapeum)` followed by `(asdf:test-system :sento)`.

## Estimated Effort

| Phase | LOC (Lisp) | LOC (C) | Risk |
|-------|-----------|---------|------|
| 1     | ~500      | 0       | Medium — touches hot path |
| 2     | ~300      | 0       | Low |
| 3     | ~200      | ~20     | Medium — slot-value perf |
| 4     | ~150      | 0       | Low |
| 5     | ~150      | 0       | Low |
| 6     | ~250      | ~50     | Medium — funcallable glue |
| 7     | ~200      | 0       | Low |
| 8     | ~400      | 0       | Medium — parser work |
| 9     | ~100      | 0       | Low (skippable) |
| 10    | ~200      | 0       | Low |

**Total:** ~2500 lines Lisp, ~70 lines C. Rough calendar: 2-3 weeks at a steady pace, assuming no major detours. Phase 1 is the riskiest since it touches the slot representation everything else builds on.

## Risks & Open Questions

1. **Slot access perf.** Phase 3 inserts a GF dispatch between every `slot-value` call and `%struct-ref`. Fast-path detection (no user method on this class) is essential; we should benchmark before and after on fiveam self-tests.
2. **Memory cost of reified slot-definitions.** Each slot in every class becomes a ~10-slot struct. For a class with 20 slots, that's 200 extra heap words. On 8MB Amiga this adds up. Measure after Phase 1.
3. **Bootstrap ordering.** Slot-definitions are themselves classes with slots. Defining them requires careful bootstrapping (we already handle `standard-class` bootstrapping at lines 167-173 of `lib/clos.lisp`; extend that pattern).
4. **`compute-applicable-methods-using-classes`.** closer-mop imports this. Our current dispatch does EQL checks after class-based filtering. Need to make sure exposing this GF doesn't force us to re-run EQL checks client-side. May need two GFs internally: `%applicable-by-class` and `%filter-eql`.
5. **Serapeum's actual MOP usage.** Before Phase 10, do a grep over serapeum to confirm it only uses the subset we're implementing. If it reaches into `sb-mop` or CCL internals via feature-gated code, Phase 10 gets messy.

## Out-of-scope References

- AMOP (Kiczales, Rivières, Bobrow, 1991) — the canonical source.
- closer-mop source — the portable subset we actually need.
- SBCL's `src/pcl/` — reference implementation; avoid copying structure, but use for semantics.
