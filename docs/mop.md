# `MOP` — CLOS Metaobject Protocol

A subset of the **CLOS Metaobject Protocol** (AMOP), exported from the `MOP`
package. The AmiLisp **closer-mop** shim re-exports these under `CLOSER-MOP`
names, which is how MOP-using Quicklisp libraries (serapeum, trivia, …) reach
them.

- **Package:** `MOP` (uses/used-by `CL` during bootstrap)
- **Inherited by:** `COMMON-LISP-USER`.
- **These follow the AMOP semantics.** This page is a categorized index; the
  authoritative reference for each operator is *The Art of the Metaobject
  Protocol* and the closer-mop documentation. AmiLisp implements a working
  subset — see `lib/clos.lisp` and the limitation note below for what is and
  isn't covered.

## Class metaobjects

| Signature | Kind | Description |
|---|---|---|
| `(classp object)` | function | True if `object` is a class metaobject. |
| `(class-direct-superclasses class)` | function | Direct superclasses as given in the class definition. |
| `(class-direct-subclasses class)` | function | Classes that name this class as a direct superclass. |
| `(class-direct-slots class)` | function | List of direct slot-definition metaobjects. |
| `(class-direct-methods class)` | function | Methods specialized directly on this class. |
| `(class-direct-default-initargs class)` | function | The class's own `:default-initargs`. |
| `(class-default-initargs class)` | function | Effective (inherited) default initargs. |
| `(class-precedence-list class)` | function | The class precedence list (class must be finalized). |
| `(class-slots class)` | function | List of effective slot-definition metaobjects. |
| `(class-effective-slots class)` | function | Effective-slot accessor that `class-slots` delegates to. |
| `(class-slot-index-table class)` | function | Slot-name → index table used for fast slot access. |
| `(class-finalized-p class)` | function | True if the class has been finalized. |
| `(class-prototype class)` | generic function | A (lazily allocated) prototype instance of the class. |
| `(finalize-inheritance class)` | generic function | Finalize the class: compute CPL, effective slots, layout. |
| `(ensure-class name &rest keys)` | function | Define or redefine a class; what `defclass` expands into. Redefinition updates the existing class metaobject in place (same `find-class` object, methods specialized on it keep applying) and re-finalizes its subclasses; with a user metaclass it runs `reinitialize-instance` on the class. |
| `(ensure-class-using-class class name &rest keys)` | generic function | `ensure-class` protocol hook; `class` is the existing class or `nil`. |
| `(validate-superclass class superclass)` | generic function | Whether `superclass` may serve as a superclass of `class`. |
| `forward-referenced-class` | class | Placeholder for a superclass not yet defined. |
| `funcallable-standard-class` | class | Metaclass of funcallable classes (e.g. generic functions). |
| `funcallable-standard-object` | class | Superclass of instances that can be called as functions. |
| `metaobject` | class | Root class of all metaobjects. |

## Slot-definition metaobjects

| Signature | Kind | Description |
|---|---|---|
| `slot-definition` | class | Root class of slot-definition metaobjects. |
| `direct-slot-definition` | class | Slot definition as written in a `defclass` form. |
| `effective-slot-definition` | class | Slot definition after inheritance is merged. |
| `standard-slot-definition` | class | Standard base class for slot definitions. |
| `standard-direct-slot-definition` | class | Standard class of direct slot definitions. |
| `standard-effective-slot-definition` | class | Standard class of effective slot definitions. |
| `(direct-slot-definition-class class &rest initargs)` | generic function | Class of direct slot-definition metaobjects to create. |
| `(effective-slot-definition-class class &rest initargs)` | generic function | Class of effective slot-definition metaobjects to create. |
| `(compute-effective-slot-definition class name direct-slots)` | generic function | Merge the direct slot definitions for `name` into one effective slot. |
| `(compute-slots class)` | generic function | Compute the list of effective slot definitions. |
| `(slot-definition-name sd)` | function | The slot's name (a symbol). |
| `(slot-definition-allocation sd)` | function | Allocation kind, `:instance` or `:class`. |
| `(slot-definition-initargs sd)` | function | List of `:initarg` keywords. |
| `(slot-definition-initform sd)` | function | The `:initform` expression, if any. |
| `(slot-definition-initfunction sd)` | function | Function of no arguments that evaluates the initform. |
| `(slot-definition-type sd)` | function | Declared `:type` of the slot. |
| `(slot-definition-location esd)` | function | Storage location of an effective slot (instance index). |
| `(slot-definition-readers dsd)` | function | Reader generic-function names of a direct slot. |
| `(slot-definition-writers dsd)` | function | Writer generic-function names of a direct slot. |
| `(slot-definition-documentation sd)` | function | The slot's documentation string. |

## Slot access protocol

| Signature | Kind | Description |
|---|---|---|
| `(slot-value-using-class class instance slot)` | generic function | Read a slot through the class metaobject (also `setf`-able). |
| `(slot-boundp-using-class class instance slot)` | generic function | Slot boundness test through the class metaobject. |
| `(slot-makunbound-using-class class instance slot)` | generic function | Make a slot unbound through the class metaobject. |
| `(standard-instance-access instance location)` | function | Direct indexed access to a standard instance's slot storage. |
| `(funcallable-standard-instance-access instance location)` | function | Direct indexed access for funcallable instances. |
| `(set-funcallable-instance-function gf fn)` | function | Install `fn` as the function called when `gf` is invoked. |

## Generic-function & method metaobjects

| Signature | Kind | Description |
|---|---|---|
| `(generic-function-name gf)` | function | Name of the generic function. |
| `(generic-function-lambda-list gf)` | function | The generic function's lambda list. |
| `(generic-function-methods gf)` | function | List of the generic function's method metaobjects. |
| `(generic-function-method-class gf)` | function | Default class of the generic function's methods. |
| `(generic-function-method-combination gf)` | function | The generic function's method-combination metaobject. |
| `(generic-function-declarations gf)` | function | Declarations from the `defgeneric` form. |
| `(generic-function-argument-precedence-order gf)` | function | Argument order used to sort applicable methods. |
| `(ensure-generic-function-using-class gf name &rest args)` | generic function | `ensure-generic-function` protocol hook; `gf` is the existing GF or `nil`. |
| `(compute-discriminating-function gf)` | generic function | Function that dispatches calls to the generic function. |
| `(compute-applicable-methods-using-classes gf classes)` | generic function | Applicable methods from argument classes alone. |
| `(compute-effective-method gf combination methods)` | generic function | Effective-method form combining the sorted applicable methods. |
| `(compute-default-initargs class)` | generic function | Compute a class's effective default initargs. |
| `(compute-class-precedence-list class)` | generic function | Compute a class's precedence list. |
| `(make-method-lambda generic-function method lambda-expression environment)` | generic function | Transform a method body into the function to compile. |
| `(ensure-method gf-or-name lambda-expression &key qualifiers lambda-list specializers method-class)` | function | Create a method object and add it to the generic function. |
| `(method-function m)` | function | The function implementing the method body. |
| `(method-generic-function m)` | function | The generic function the method belongs to. |
| `(method-lambda-list m)` | function | The method's (unspecialized) lambda list. |
| `(method-specializers m)` | function | The method's specializer metaobjects. |
| `(extract-lambda-list specialized-lambda-list)` | function | Ordinary lambda list from a specialized one. |
| `(extract-specializer-names specialized-lambda-list)` | function | Specializer names from a specialized lambda list. |
| `standard-method-combination` | class | Class of the standard method combination. |
| `(find-method-combination generic-function name options)` | generic function | Method-combination metaobject for `name` with `options`. |

## Specializers

| Signature | Kind | Description |
|---|---|---|
| `specializer` | class | Root class of method specializers. |
| `(specializer-direct-methods specializer)` | generic function | Methods that specialize on this specializer. |
| `(specializer-direct-generic-functions specializer)` | generic function | Generic functions with a method specialized on it. |
| `eql-specializer` | class | Specializer matching one object under `eql`. |
| `(eql-specializer-p object)` | function | True if `object` is an eql-specializer. |
| `(eql-specializer-object spec)` | function | The object an eql-specializer matches. |
| `(intern-eql-specializer object)` | function | The canonical eql-specializer for `object`. |

## Accessor / reader / writer methods

| Signature | Kind | Description |
|---|---|---|
| `standard-accessor-method` | class | Superclass of slot reader/writer methods. |
| `standard-reader-method` | class | Class of automatically generated slot reader methods. |
| `standard-writer-method` | class | Class of automatically generated slot writer methods. |
| `(accessor-method-slot-definition method)` | generic function | The direct slot definition an accessor method accesses. |
| `(reader-method-class class direct-slot &rest initargs)` | generic function | Class of reader methods to generate for a slot. |
| `(writer-method-class class direct-slot &rest initargs)` | generic function | Class of writer methods to generate for a slot. |

## Dependent maintenance & class structure mutation

| Signature | Kind | Description |
|---|---|---|
| `(add-dependent metaobject dependent)` | generic function | Register `dependent` for update notifications. |
| `(remove-dependent metaobject dependent)` | generic function | Remove a registered dependent. |
| `(map-dependents metaobject function)` | generic function | Call `function` on each registered dependent. |
| `(update-dependent metaobject dependent &rest initargs)` | generic function | Notification hook called when `metaobject` changes. |
| `(add-direct-method specializer method)` | generic function | Record that `method` specializes on `specializer`. |
| `(remove-direct-method specializer method)` | generic function | Remove that record. |
| `(add-direct-subclass class subclass)` | generic function | Record `subclass` as a direct subclass of `class`. |
| `(remove-direct-subclass class subclass)` | generic function | Remove that record. |

## User-defined metaclasses

A subclass of `standard-class` may be defined and used as a class's metaclass
via the `defclass` `(:metaclass …)` option. The class it creates is an
*instance* of that metaclass, so methods specialized on the metaclass —
`allocate-instance`, `initialize-instance` (including `:around`),
`validate-superclass` — dispatch on it, and metaclass slots (with
`:initarg`/`:reader`) and `:default-initargs` work. A metaclass
`initialize-instance :around` may rewrite `:direct-superclasses` while the
class is being built. This covers the two common ecosystem patterns
(serapeum's `abstract-standard-class` and `topmost-object-class`); see
`tests/test_mop_metaclass.c` for runnable examples.

The class metaobject keeps a fixed internal layout for speed on the 68020:
metaclasses add their own slots after the standard ones, but a metaclass whose
*own* layout diverges from `standard-class` (full meta-recursion) is not
supported.

> **Limitation:** the MOP implementation is a working subset, not the complete
> AMOP — see [Known Limitations](../README.md#known-limitations-and-future-work)
> ("full CLOS MOP").

## Source of truth

`tests/test_clos.c` and the CLOS/MOP blocks in `tests/amiga/run-tests.lisp`;
implementation in `lib/clos.lisp`. The closer-mop re-export mapping lives in
the AmiLisp `closer-mop` fork (`#+amilisp` branch of `closer-mop-packages.lisp`
plus `closer-amilisp.lisp`), installed into quicklisp's local-projects.
