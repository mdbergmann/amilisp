;;;; CLOS Implementation for AmiLisp
;;;; Practical subset: defclass, defgeneric, defmethod, make-instance,
;;;; slot-value, class-of, with-slots, standard method combination.
;;;; Loaded on demand via (require "clos").
;;;;
;;;; PACKAGE DISCIPLINE: read in COMMON-LISP when source-loaded at boot but
;;;; in COMMON-LISP-USER under `make fasl` — every %-helper referenced across
;;;; files must be listed in clos_internal_names[] (src/core/package.c).
;;;; See the matching note at the top of lib/boot.lisp.

;;; ---- Boot-phase profiling ----
;;; Info messages so future regressions in CLOS load time show up
;;; immediately.  Same shape as the C-level "; [boot] ..." markers in
;;; cl_repl_init_no_userinit; opt-in via --boot-log (quiet otherwise,
;;; so piped tests match output exactly).
;;; Goes through the C builtin %BOOT-TRACE-CLOS rather than (format t)
;;; so unit-test setups that skip cl_stream_init still work.
(defparameter *%clos-load-start* (get-internal-real-time))
(defparameter *%clos-prev*       *%clos-load-start*)
(defun %clos-trace (phase)
  (let ((now (get-internal-real-time)))
    (%boot-trace-clos (- now *%clos-load-start*)
                      (- now *%clos-prev*)
                      phase)
    (setq *%clos-prev* now)))

;;; ====================================================================
;;; Step 2: Bootstrap Core Classes
;;; ====================================================================

;;; STANDARD-CLASS is a struct type with 12 slots.
;;; Slot layout:
;;;   0:  name                     - symbol
;;;   1:  direct-superclasses      - list of class objects
;;;   2:  direct-slots             - list of canonical slot specs
;;;   3:  cpl                      - class precedence list
;;;   4:  effective-slots          - merged slots from CPL
;;;   5:  slot-index-table         - hash table: slot-name -> effective-slot-def
;;;   6:  direct-subclasses        - list of class objects
;;;   7:  direct-methods           - list
;;;   8:  prototype                - lazily created by CLASS-PROTOTYPE
;;;   9:  finalized-p              - t or nil
;;;   10: default-initargs         - effective (CPL-merged) default initargs
;;;   11: direct-default-initargs  - as supplied to DEFCLASS

(%register-struct-type 'standard-class 12 nil
  '((name nil) (direct-superclasses nil) (direct-slots nil)
    (cpl nil) (effective-slots nil) (slot-index-table nil)
    (direct-subclasses nil) (direct-methods nil)
    (prototype nil) (finalized-p nil) (default-initargs nil)
    (direct-default-initargs nil)))

;; built-in-class: same layout as standard-class, used as metaclass for built-in types
(%register-struct-type 'built-in-class 12 'standard-class
  '((name nil) (direct-superclasses nil) (direct-slots nil)
    (cpl nil) (effective-slots nil) (slot-index-table nil)
    (direct-subclasses nil) (direct-methods nil)
    (prototype nil) (finalized-p nil) (default-initargs nil)
    (direct-default-initargs nil)))

;;; --- Class metaobject accessors ---

(defun class-name (class)
  (%struct-ref class 0))

(defun class-direct-superclasses (class)
  (%struct-ref class 1))

(defun class-direct-slots (class)
  (%struct-ref class 2))

(defun class-precedence-list (class)
  (%struct-ref class 3))

(defun class-effective-slots (class)
  (%struct-ref class 4))

(defun class-slot-index-table (class)
  (%struct-ref class 5))

(defun class-direct-subclasses (class)
  (%struct-ref class 6))

(defun class-direct-methods (class)
  (%struct-ref class 7))

;;; Internal setters (not exported, used during bootstrap)

(defun %set-class-direct-slots (class val)
  (%struct-set class 2 val))

(defun %set-class-cpl (class val)
  (%struct-set class 3 val))

(defun %set-class-effective-slots (class val)
  (%struct-set class 4 val))

(defun %set-class-slot-index-table (class val)
  (%struct-set class 5 val))

(defun %set-class-direct-subclasses (class val)
  (%struct-set class 6 val))

(defun %set-class-direct-methods (class val)
  (%struct-set class 7 val))

(defun %set-class-finalized-p (class val)
  (%struct-set class 9 val))

(defun class-default-initargs (class)
  (%struct-ref class 10))

(defun %set-class-default-initargs (class val)
  (%struct-set class 10 val))

(defun class-direct-default-initargs (class)
  (%struct-ref class 11))

(defun %set-class-direct-default-initargs (class val)
  (%struct-set class 11 val))

;;; MOP public reader for the finalized flag.
(defun class-finalized-p (class)
  (%struct-ref class 9))

;;; --- Class registry ---

;; Synchronized: DEFCLASS/ENSURE-CLASS run on whatever thread loads code,
;; so concurrent loads mutate this table at once; a plain table's bucket
;; chains corrupt under concurrent insert/rehash (see *EQL-SPECIALIZER-
;; TABLE*).  Note the C fast paths (class-of/typep via ht_eq_lookup in
;; builtins_struct.c) still read it lock-free; they are exposed only
;; while a defclass is actually in flight on another thread.
(defvar *class-table* (amilisp::%make-sync-hash-table 'eq))

;; Register with C for typep to check CLOS class-precedence-lists
(%set-clos-class-table *class-table*)

(defun find-class (name &optional (errorp t) environment)
  "Find the class named NAME.  Signal an error if not found and
ERRORP is true.  Common libraries (serapeum, closer-mop callers) pass
an already-reified class metaobject in place of a class name and
expect it to round-trip -- when NAME is itself a class, return it
directly instead of attempting a symbol lookup."
  (declare (ignore environment))
  (when (and (structurep name)
             (member (%struct-type-name name)
                     '(standard-class built-in-class
                       funcallable-standard-class
                       forward-referenced-class)))
    (return-from find-class name))
  (multiple-value-bind (class found-p)
      (gethash name *class-table*)
    (if found-p
        class
        (if errorp
            (error "No class named ~S" name)
            nil))))

(defun %set-find-class (name class)
  "Register CLASS under NAME in the class table."
  (setf (gethash name *class-table*) class)
  class)

(defsetf find-class %set-find-class)

;;; --- Metaclass detection ---
;;; A user-defined metaclass is a subclass of STANDARD-CLASS.  Its
;;; instances are class metaobjects: they reuse the fixed 12-slot class
;;; layout (NAME, DIRECT-SUPERCLASSES, ... at indices 0-11) and append
;;; any metaclass-specific slots at indices 12+.  See specs/mop.md
;;; Design Principle 2.  STANDARD-CLASS itself and BUILT-IN-CLASS are not
;;; treated as user metaclasses (they are created during bootstrap, never
;;; via %ENSURE-CLASS).
(defun %metaclass-p (class)
  "True if CLASS is a metaclass -- i.e. a proper subclass of STANDARD-CLASS."
  (let ((std (find-class 'standard-class nil)))
    (and std
         (not (eq class std))
         (member std (class-precedence-list class) :test #'eq)
         t)))

;;; The fixed 12-slot layout shared by every class metaobject (and by the
;;; struct type of any user metaclass, which appends its own slots after).
(defconstant +standard-class-slot-layout+
  '((name nil) (direct-superclasses nil) (direct-slots nil)
    (cpl nil) (effective-slots nil) (slot-index-table nil)
    (direct-subclasses nil) (direct-methods nil)
    (prototype nil) (finalized-p nil) (default-initargs nil)
    (direct-default-initargs nil)))

;;; --- CPL computation for bootstrap classes ---

;;; C3 linearization merge.  Defined here (before the bootstrap classes)
;;; so %compute-builtin-cpl can produce a *correct* precedence order for
;;; built-in classes with multiple superclasses (the diamonds NULL ->
;;; SYMBOL+LIST and VECTOR -> ARRAY+SEQUENCE).  A naive append+dedup
;;; merge places the shared ancestor T (and SEQUENCE) too early, e.g.
;;; (NULL SYMBOL T LIST SEQUENCE), which then makes a method specialized
;;; on T look *more* specific than one on LIST during dispatch ordering.
;;; %compute-class-precedence-list (defined later) reuses this same merge.
(defun %c3-merge (lists)
  "Merge step of C3 linearization. LISTS is a list of lists."
  (let ((result nil))
    (loop
      ;; Remove empty lists
      (setq lists (remove nil lists))
      (when (null lists)
        (return (nreverse result)))
      ;; Find a candidate: head of some list that doesn't appear in
      ;; the tail of any other list
      (let ((candidate nil))
        (dolist (l lists)
          (let ((head (car l))
                (dominated nil))
            ;; Check if head appears in tail of any list
            (dolist (l2 lists)
              (when (member head (cdr l2) :test #'eq)
                (setq dominated t)
                (return)))
            (unless dominated
              (setq candidate head)
              (return))))
        (unless candidate
          (error "Inconsistent class precedence list -- C3 linearization failed"))
        ;; Remove candidate from all lists
        (push candidate result)
        (setq lists
              (mapcar (lambda (l)
                        (if (eq (car l) candidate)
                            (cdr l)
                            l))
                      lists))))))

(defun %compute-builtin-cpl (class)
  "Compute class precedence list for a built-in class via C3 linearization.
   Supers must already have their CPLs set (bootstrap creates classes in
   dependency order, supers before subs)."
  (let ((supers (class-direct-superclasses class)))
    (if (null supers)
        (list class)
        (%c3-merge
         (cons (list class)
               (append
                (mapcar #'class-precedence-list supers)
                (list supers)))))))

;;; --- Bootstrap helper ---
;;; Classes are created in dependency order (supers before subs),
;;; so CPL and subclass links are computed inline.

(defun %make-bootstrap-class (name direct-superclasses)
  "Create a class metaobject during bootstrap. Supers must already exist."
  (let ((class (%make-struct 'standard-class
                 name                    ; 0:  name
                 direct-superclasses     ; 1:  direct-superclasses
                 nil                     ; 2:  direct-slots
                 nil                     ; 3:  cpl (set below)
                 nil                     ; 4:  effective-slots
                 nil                     ; 5:  slot-index-table
                 nil                     ; 6:  direct-subclasses
                 nil                     ; 7:  direct-methods
                 nil                     ; 8:  prototype
                 t                       ; 9:  finalized-p
                 nil                     ; 10: default-initargs
                 nil)))                  ; 11: direct-default-initargs
    ;; Register in class table
    (setf (find-class name) class)
    ;; Compute CPL (supers already have theirs)
    (%set-class-cpl class (%compute-builtin-cpl class))
    ;; Register as subclass of each direct super
    (dolist (super direct-superclasses)
      (%set-class-direct-subclasses super
        (cons class (class-direct-subclasses super))))
    class))

;;; --- Bootstrap built-in classes ---
;;; Order matters: superclasses must be created before subclasses.

;; T is the root of the class hierarchy
(%make-bootstrap-class 't '())

;; Standard base classes
(%make-bootstrap-class 'standard-object
  (list (find-class 't)))
;; CLASS must precede STANDARD-CLASS so STANDARD-CLASS's CPL includes CLASS
(%make-bootstrap-class 'class
  (list (find-class 'standard-object)))
(%make-bootstrap-class 'standard-class
  (list (find-class 'class)))
(%make-bootstrap-class 'built-in-class
  (list (find-class 'class)))
(%make-bootstrap-class 'structure-class
  (list (find-class 'class)))
(%make-bootstrap-class 'structure-object
  (list (find-class 't)))
;; MOP classes that are STANDARD-OBJECT subclasses
(%make-bootstrap-class 'method
  (list (find-class 'standard-object)))
(%make-bootstrap-class 'standard-method
  (list (find-class 'method)))
(%make-bootstrap-class 'method-combination
  (list (find-class 'standard-object)))
(%make-bootstrap-class 'restart
  (list (find-class 'standard-object)))

;; Numeric tower
(%make-bootstrap-class 'number
  (list (find-class 't)))
(%make-bootstrap-class 'real
  (list (find-class 'number)))
(%make-bootstrap-class 'rational
  (list (find-class 'real)))
(%make-bootstrap-class 'integer
  (list (find-class 'rational)))
(%make-bootstrap-class 'fixnum
  (list (find-class 'integer)))
(%make-bootstrap-class 'bignum
  (list (find-class 'integer)))
(%make-bootstrap-class 'ratio
  (list (find-class 'rational)))
(%make-bootstrap-class 'float
  (list (find-class 'real)))
(%make-bootstrap-class 'single-float
  (list (find-class 'float)))
(%make-bootstrap-class 'double-float
  (list (find-class 'float)))

;; Other built-in classes — CL type hierarchy
(%make-bootstrap-class 'sequence
  (list (find-class 't)))
(%make-bootstrap-class 'list
  (list (find-class 'sequence)))
(%make-bootstrap-class 'symbol
  (list (find-class 't)))
(%make-bootstrap-class 'null
  (list (find-class 'symbol) (find-class 'list)))
(%make-bootstrap-class 'cons
  (list (find-class 'list)))
(%make-bootstrap-class 'character
  (list (find-class 't)))
(%make-bootstrap-class 'array
  (list (find-class 't)))
(%make-bootstrap-class 'vector
  (list (find-class 'array) (find-class 'sequence)))
(%make-bootstrap-class 'string
  (list (find-class 'vector)))
(%make-bootstrap-class 'bit-vector
  (list (find-class 'vector)))
(%make-bootstrap-class 'function
  (list (find-class 't)))
(%make-bootstrap-class 'compiled-function
  (list (find-class 'function)))
(%make-bootstrap-class 'generic-function
  (list (find-class 'function)))
(%make-bootstrap-class 'standard-generic-function
  (list (find-class 'generic-function)))
(%make-bootstrap-class 'hash-table
  (list (find-class 't)))
(%make-bootstrap-class 'package
  (list (find-class 't)))
(%make-bootstrap-class 'stream
  (list (find-class 't)))
(%make-bootstrap-class 'file-stream
  (list (find-class 'stream)))
(%make-bootstrap-class 'broadcast-stream
  (list (find-class 'stream)))
(%make-bootstrap-class 'concatenated-stream
  (list (find-class 'stream)))
(%make-bootstrap-class 'echo-stream
  (list (find-class 'stream)))
(%make-bootstrap-class 'string-stream
  (list (find-class 'stream)))
(%make-bootstrap-class 'synonym-stream
  (list (find-class 'stream)))
(%make-bootstrap-class 'two-way-stream
  (list (find-class 'stream)))
(%make-bootstrap-class 'pathname
  (list (find-class 't)))
(%make-bootstrap-class 'logical-pathname
  (list (find-class 'pathname)))
(%make-bootstrap-class 'random-state
  (list (find-class 't)))
(%make-bootstrap-class 'readtable
  (list (find-class 't)))
(%make-bootstrap-class 'complex
  (list (find-class 'number)))
(%make-bootstrap-class 'condition
  (list (find-class 't)))

;; Condition type hierarchy (for method dispatch on condition types)
(%make-bootstrap-class 'serious-condition
  (list (find-class 'condition)))
(%make-bootstrap-class 'warning
  (list (find-class 'condition)))
(%make-bootstrap-class 'simple-condition
  (list (find-class 'condition)))
(%make-bootstrap-class 'error
  (list (find-class 'serious-condition)))
(%make-bootstrap-class 'type-error
  (list (find-class 'error)))
(%make-bootstrap-class 'arithmetic-error
  (list (find-class 'error)))
(%make-bootstrap-class 'division-by-zero
  (list (find-class 'arithmetic-error)))
(%make-bootstrap-class 'floating-point-inexact
  (list (find-class 'arithmetic-error)))
(%make-bootstrap-class 'floating-point-invalid-operation
  (list (find-class 'arithmetic-error)))
(%make-bootstrap-class 'floating-point-overflow
  (list (find-class 'arithmetic-error)))
(%make-bootstrap-class 'floating-point-underflow
  (list (find-class 'arithmetic-error)))
(%make-bootstrap-class 'control-error
  (list (find-class 'error)))
(%make-bootstrap-class 'program-error
  (list (find-class 'error)))
(%make-bootstrap-class 'simple-error
  (list (find-class 'error) (find-class 'simple-condition)))
(%make-bootstrap-class 'simple-warning
  (list (find-class 'warning) (find-class 'simple-condition)))
(%make-bootstrap-class 'style-warning
  (list (find-class 'warning)))
(%make-bootstrap-class 'storage-condition
  (list (find-class 'serious-condition)))
(%make-bootstrap-class 'cell-error
  (list (find-class 'error)))
(%make-bootstrap-class 'unbound-slot
  (list (find-class 'cell-error)))
;; CLHS: UNDEFINED-FUNCTION and UNBOUND-VARIABLE are CELL-ERRORs too
(%make-bootstrap-class 'undefined-function
  (list (find-class 'cell-error)))
(%make-bootstrap-class 'unbound-variable
  (list (find-class 'cell-error)))
(%make-bootstrap-class 'stream-error
  (list (find-class 'error)))
(%make-bootstrap-class 'end-of-file
  (list (find-class 'stream-error)))
(%make-bootstrap-class 'file-error
  (list (find-class 'error)))
(%make-bootstrap-class 'package-error
  (list (find-class 'error)))
(%make-bootstrap-class 'parse-error
  (list (find-class 'error)))
(%make-bootstrap-class 'reader-error
  (list (find-class 'parse-error) (find-class 'stream-error)))
(%make-bootstrap-class 'print-not-readable
  (list (find-class 'error)))
(%make-bootstrap-class 'simple-type-error
  (list (find-class 'type-error) (find-class 'simple-condition)))

;;; ====================================================================
;;; Slot Definition Metaobjects (MOP)
;;; ====================================================================
;;;
;;; Reified slot definitions: struct instances rather than cons+plist.
;;; STANDARD-DIRECT-SLOT-DEFINITION — one per slot declared in defclass.
;;; STANDARD-EFFECTIVE-SLOT-DEFINITION — one per merged slot in a class.

;;; standard-direct-slot-definition layout (9 slots):
;;;   0: name           1: initargs      2: initform
;;;   3: initfunction   4: type          5: allocation
;;;   6: readers        7: writers       8: documentation
(%register-struct-type 'standard-direct-slot-definition 9 nil
  '((name nil) (initargs nil) (initform nil) (initfunction nil)
    (type nil) (allocation :instance)
    (readers nil) (writers nil) (documentation nil)))

;;; standard-effective-slot-definition layout (5 slots) — compact form
;;; sharing indices 0/1/3 with direct-slot-definition so that the
;;; runtime hot path (shared-initialize) needs no type dispatch:
;;;   0: name           1: initargs      2: location
;;;   3: initfunction   4: allocation
;;;
;;; Introspection accessors for type/initform/documentation return NIL
;;; on effective slot defs (the source form lives on the primary direct
;;; slot reachable via class-direct-slots).
(%register-struct-type 'standard-effective-slot-definition 5 nil
  '((name nil) (initargs nil) (location nil)
    (initfunction nil) (allocation :instance)))

;;; Class hierarchy for slot-definition metaobjects.
;;; AMOP places DIRECT-SLOT-DEFINITION and EFFECTIVE-SLOT-DEFINITION as
;;; abstract siblings under SLOT-DEFINITION; the "standard-" classes
;;; mix in STANDARD-SLOT-DEFINITION to inherit the default accessors.
;;; We reproduce that shape so closer-mop's TYPEP on the abstract names
;;; succeeds on our reified slot-definition objects.
(%make-bootstrap-class 'slot-definition
  (list (find-class 'standard-object)))
(%make-bootstrap-class 'direct-slot-definition
  (list (find-class 'slot-definition)))
(%make-bootstrap-class 'effective-slot-definition
  (list (find-class 'slot-definition)))
(%make-bootstrap-class 'standard-slot-definition
  (list (find-class 'slot-definition)))
(%make-bootstrap-class 'standard-direct-slot-definition
  (list (find-class 'standard-slot-definition)
        (find-class 'direct-slot-definition)))
(%make-bootstrap-class 'standard-effective-slot-definition
  (list (find-class 'standard-slot-definition)
        (find-class 'effective-slot-definition)))

;;; --- Accessors (AMOP / closer-mop names) ---
;;; Slots 0, 1, 3 share indices between direct and effective so name,
;;; initargs, and initfunction reads avoid any per-call dispatch.

(defun slot-definition-name         (sd) (%struct-ref sd 0))
(defun slot-definition-initargs     (sd) (%struct-ref sd 1))
(defun slot-definition-initfunction (sd) (%struct-ref sd 3))

(defun %effective-slot-def-p (sd)
  (eq (%struct-type-name sd) 'standard-effective-slot-definition))

(defun slot-definition-initform (sd)
  (if (%effective-slot-def-p sd) nil (%struct-ref sd 2)))

(defun slot-definition-type (sd)
  (if (%effective-slot-def-p sd) nil (%struct-ref sd 4)))

(defun slot-definition-allocation (sd)
  (if (%effective-slot-def-p sd) (%struct-ref sd 4) (%struct-ref sd 5)))

(defun slot-definition-documentation (sd)
  (if (%effective-slot-def-p sd) nil (%struct-ref sd 8)))

;; Direct-only: readers, writers
(defun slot-definition-readers (dsd) (%struct-ref dsd 6))
(defun slot-definition-writers (dsd) (%struct-ref dsd 7))

;; Effective-only: location
(defun slot-definition-location (esd) (%struct-ref esd 2))

;;; Internal setters (used by the effective-slot builder and class
;;; finalization; not part of the MOP API).
(defun %set-slot-definition-initargs (sd v) (%struct-set sd 1 v))
(defun %set-slot-definition-initfunction (sd v) (%struct-set sd 3 v))

(defun %set-slot-definition-initform (sd v)
  (unless (%effective-slot-def-p sd) (%struct-set sd 2 v)))

(defun %set-slot-definition-type (sd v)
  (unless (%effective-slot-def-p sd) (%struct-set sd 4 v)))

(defun %set-slot-definition-location (esd v) (%struct-set esd 2 v))

(defun %set-slot-definition-documentation (sd v)
  (unless (%effective-slot-def-p sd) (%struct-set sd 8 v)))

;;; --- Constructors ---
;;; Positional args (no &key) so defclass expansion stays small and the
;;; call site is a plain %make-struct with no keyword-argument dispatch.

(defun %make-direct-slot-def (name initargs initform initfunction
                              type allocation readers writers documentation)
  (%make-struct 'standard-direct-slot-definition
    name initargs initform initfunction type allocation
    readers writers documentation))

(defun %make-effective-slot-def (name initargs initfunction allocation location)
  (%make-struct 'standard-effective-slot-definition
    name initargs location initfunction allocation))

;;; --- DEFSTRUCT class registration with reified slot metaobjects ---
;;;
;;; DEFSTRUCT (in boot.lisp) registers a CLOS class for each structure so
;;; CLOS dispatch on structure types works.  The bare %MAKE-BOOTSTRAP-CLASS
;;; leaves the class with no slot metaobjects, so MOP introspection
;;; (CLASS-SLOTS / COMPUTE-SLOTS — used by closer-mop callers such as
;;; trivia's structure-pattern decomposition and serapeum) sees an empty
;;; slot list.  This helper populates the effective- and direct-slot
;;; definitions so introspection reflects the real layout.
;;;
;;; ALL-SLOT-NAMES is the full ordered slot list (inherited slots first),
;;; matching the structure's physical slot indices, so each effective slot's
;;; LOCATION is its %STRUCT-REF index.  We deliberately leave the
;;; slot-index-table NIL: SLOT-VALUE / (SETF SLOT-VALUE) detect structures
;;; via STRUCTUREP and use %FIND-STRUCT-SLOT-INDEX, so routing them through
;;; the index-table path is unnecessary and would duplicate that logic.
(defun %register-struct-class (name supers all-slot-names)
  (let* ((class (%make-bootstrap-class name supers))
         (idx 0)
         (eslots
           (mapcar (lambda (sname)
                     (prog1
                         (%make-effective-slot-def
                          sname
                          (list (intern (symbol-name sname) :keyword))
                          nil :instance idx)
                       (setq idx (1+ idx))))
                   all-slot-names)))
    (%set-class-effective-slots class eslots)
    ;; Direct-slot-definitions mirror the effective ones (DEFSTRUCT has no
    ;; per-class slot inheritance metadata to distinguish own vs inherited,
    ;; and COMPUTE-SLOTS dedups by name across the CPL anyway).
    (%set-class-direct-slots class
      (mapcar (lambda (esd)
                (%make-direct-slot-def
                 (slot-definition-name esd)
                 (slot-definition-initargs esd)
                 nil nil nil :instance nil nil nil))
              eslots))
    class))

;;; --- Slot-definition-class protocol ---
;;;
;;; Plain functions for now; upgraded to GFs at the bottom of clos.lisp
;;; once defgeneric/defmethod are available. User-defined metaclasses
;;; are out of scope, so the customization point is an :around method
;;; on the standard-class specializer.
(defun direct-slot-definition-class (class &rest initargs)
  (declare (ignore class initargs))
  (find-class 'standard-direct-slot-definition))

(defun effective-slot-definition-class (class &rest initargs)
  (declare (ignore class initargs))
  (find-class 'standard-effective-slot-definition))

;;; ====================================================================
;;; EQL specializer metaobjects (MOP)
;;; ====================================================================
;;;
;;; AMOP §5.4 reifies (eql value) specializers as metaobjects interned
;;; per value (by EQL on the object). Callers that previously built a
;;; cons (eql value) now call INTERN-EQL-SPECIALIZER so that
;;;   (eq (intern-eql-specializer 42) (intern-eql-specializer 42)) => T
;;; and user code can reach them via the MOP accessor.
;;;
;;; One slot: OBJECT. The intern table never shrinks — we have no weak
;;; hash tables and the leak is bounded by the set of literal values
;;; used in DEFMETHOD forms.

(%register-struct-type 'eql-specializer 1 nil
  '((object nil)))

(%make-bootstrap-class 'eql-specializer
  (list (find-class 'standard-object)))

;; Synchronized: interning happens on every DEFMETHOD with an EQL
;; specializer, and defmethods run on whatever thread loads the code —
;; two threads loading concurrently mutate this table at once.  A plain
;; table's bucket chains corrupt under concurrent insert/rehash (observed
;; as silently missing entries and as a hard hang walking a cyclic
;; chain), same hazard %MAKE-DISPATCH-CACHE documents.
(defvar *eql-specializer-table* (amilisp::%make-sync-hash-table 'eql))

(defvar *eql-specializer-intern-lock* (mp:make-lock "eql-specializer-intern")
  "Serializes the get-or-create in INTERN-EQL-SPECIALIZER.  The sync
   table makes each gethash/put individually safe, but without this lock
   two threads interning the SAME value could both miss and create two
   non-EQ specializers -- breaking the EQ identity the MOP guarantees
   (and with it method replacement, which compares specializers).")

(defun eql-specializer-p (object)
  "Return T if OBJECT is an EQL-SPECIALIZER metaobject."
  (and (structurep object)
       (eq (%struct-type-name object) 'eql-specializer)))

(defun eql-specializer-object (spec)
  "AMOP: return the object associated with an EQL specializer metaobject."
  (%struct-ref spec 0))

(defun intern-eql-specializer (object)
  "AMOP: return the canonical EQL-SPECIALIZER for OBJECT. Repeated calls
   with EQL-identical OBJECTs return the same metaobject so that method
   equality and dispatch caching can use EQ."
  ;; Lock-free hit path; misses take the lock and re-check so exactly
  ;; one specializer is ever created per value.
  (multiple-value-bind (existing found-p)
      (gethash object *eql-specializer-table*)
    (if found-p
        existing
        (mp:with-lock-held (*eql-specializer-intern-lock*)
          (multiple-value-bind (raced raced-p)
              (gethash object *eql-specializer-table*)
            (if raced-p
                raced
                (let ((spec (%make-struct 'eql-specializer object)))
                  (setf (gethash object *eql-specializer-table*) spec)
                  spec)))))))

;;; --- Register condition types as CLOS classes (for method dispatch) ---

(defun %register-condition-class (name direct-super-names)
  "Register a condition type as a CLOS class for method dispatch."
  (unless (find-class name nil)
    (let* ((supers (mapcar #'find-class direct-super-names))
           (class (%make-struct 'standard-class
                    name supers nil nil nil nil nil nil nil t nil nil)))
      (%set-class-cpl class (%compute-builtin-cpl class))
      (setf (find-class name) class)
      (dolist (super supers)
        (%set-class-direct-subclasses super
          (cons class (class-direct-subclasses super))))
      class)))

;;; --- class-of ---

(defun class-of (object)
  "Return the class of OBJECT."
  (let ((type-name (%class-of object)))
    (or (gethash type-name *class-table*)
        (find-class 't))))

(%clos-trace "bootstrap classes")
;;; ====================================================================
;;; Phase 1: Slot Access Infrastructure
;;; ====================================================================

;;; Sentinel value for unbound slots — uninterned so it can't collide
(defvar *slot-unbound-marker* (make-symbol "SLOT-UNBOUND"))

;;; Publish the marker to the VM's reader fast path (OP_CALL), which has to
;;; tell an unbound slot from a real value without a caller-supplied MISS.
;;; Until this runs the fast path reports a miss for every call — inert,
;;; not wrong.
(%set-slot-unbound-marker *slot-unbound-marker*)

;;; When NIL, SLOT-VALUE / SLOT-BOUNDP / SLOT-MAKUNBOUND / (SETF SLOT-VALUE)
;;; bypass generic dispatch and go straight to %STRUCT-REF / %STRUCT-SET —
;;; preserving the tight loop we rely on for fiveam, fset, etc.
;;;
;;; Flipped to T the first time a user installs a non-default method on any
;;; of the four SLOT-*-USING-CLASS GFs. Once set it stays set — tracking
;;; method removals would cost more than it saves, and the slow path only
;;; adds one GF call per slot access (still cache-friendly).
(defvar *slot-access-protocol-extended-p* nil)

;;; Structure slot lookup: when CLASS-OF returns a class without a
;;; slot-index-table (i.e. a DEFSTRUCT instance), resolve slot names
;;; against the struct's slot list so SLOT-VALUE / WITH-SLOTS work on
;;; structures as they do in SBCL/CCL.
(defun %find-struct-slot-index (instance slot-name)
  "Return the slot index in INSTANCE for SLOT-NAME, or NIL if not found.
   INSTANCE must be a structure."
  ;; Hot path: every SLOT-VALUE / (SETF SLOT-VALUE) / SLOT-BOUNDP on a
  ;; struct instance resolves through here.  %STRUCT-SLOT-INDEX matches
  ;; the name against the registry specs in C without consing (the old
  ;; %STRUCT-SLOT-NAMES shape allocated a fresh name list per access).
  (%struct-slot-index (%struct-type-name instance) slot-name))

;;; Fast path (spec 3.1, second half): %STRUCT-SLOT-VALUE fuses
;;; type-name -> registry entry (O(1) hash) -> slot index -> slot read
;;; into one non-erroring builtin call, passing *SLOT-UNBOUND-MARKER* as
;;; the miss sentinel.  A miss means "not the simple case" — not a
;;; struct/instance, unregistered type, :CLASS-allocated slot (those are
;;; not in the registry), no such slot, or a genuinely unbound slot
;;; (whose storage holds the marker) — and falls back to the full
;;; protocol below, which owns SLOT-UNBOUND, condition objects, :CLASS
;;; storage, and error reporting.  The registry is consulted on EVERY
;;; access, so the resolved index always reflects the instance's own
;;; current layout — correct across class redefinition and for subclass
;;; instances whose inherited slots sit at different indices.
;;; SLOT-VALUE-USING-CLASS extensions flip
;;; *SLOT-ACCESS-PROTOCOL-EXTENDED-P*, which bypasses the fast path
;;; entirely so user methods still intercept every access.

(defun slot-value (instance slot-name)
  "Return the value of SLOT-NAME in INSTANCE."
  (if *slot-access-protocol-extended-p*
      (%slot-value-slow instance slot-name)
      (let ((v (%struct-slot-value instance slot-name *slot-unbound-marker*)))
        (if (eq v *slot-unbound-marker*)
            (%slot-value-slow instance slot-name)
            v))))

(defun %slot-value-slow (instance slot-name)
  "Full SLOT-VALUE protocol: SLOT-VALUE-USING-CLASS routing, :CLASS
   slots, unbound slots, condition objects, and error reporting."
  (let* ((class (class-of instance))
         (index-table (class-slot-index-table class)))
    (cond
      (index-table
       (let ((esd (gethash slot-name index-table)))
         (unless esd
           (error "~S has no slot named ~S" instance slot-name))
         (if *slot-access-protocol-extended-p*
             (slot-value-using-class class instance esd)
             (let* ((location (slot-definition-location esd))
                    (val (if (consp location)
                             (cdr location)
                             (%struct-ref instance location))))
               (if (eq val *slot-unbound-marker*)
                   (slot-unbound class instance slot-name)
                   val)))))
      ((structurep instance)
       (let ((idx (%find-struct-slot-index instance slot-name)))
         (unless idx
           (error "~S has no slot named ~S" instance slot-name))
         (%struct-ref instance idx)))
      ;; Conditions are a distinct object type in amilisp (not CLOS instances),
      ;; but CLHS 9.1 permits SLOT-VALUE on them — delegate to the condition
      ;; slot machinery so WITH-SLOTS / SLOT-VALUE work on condition objects.
      ((conditionp instance)
       (condition-slot-value instance slot-name))
      (t
       (error "~S has no slot-index-table (not a CLOS instance)" instance)))))

;;; COMPARE-AND-SWAP on (SLOT-VALUE INSTANCE SLOT-NAME) — the (SLOT-VALUE ...)
;;; place of MP:COMPARE-AND-SWAP / MP:ATOMIC-INCF expands to this.  Resolves
;;; the slot exactly as %SLOT-VALUE-SLOW does — the registry index for an
;;; instance slot, the shared (name . value) cons for a :CLASS slot, the
;;; positional index for a defstruct instance — and swaps that one cell
;;; with the MP cell primitive.  Returns the value the slot held when the
;;; CAS was decided (EQ to OLD exactly when it swapped); an unbound slot
;;; goes through SLOT-UNBOUND like a read would.  SLOT-VALUE-USING-CLASS
;;; extensions are not consulted: there is no portable CAS protocol for
;;; them, and the only atomic thing available is the storage cell itself.
(defun mp::%cas-slot-value (instance slot-name old new)
  (let* ((class (class-of instance))
         (index-table (class-slot-index-table class)))
    (cond
      (index-table
       (let ((esd (gethash slot-name index-table)))
         (unless esd
           (error "~S has no slot named ~S" instance slot-name))
         (let* ((location (slot-definition-location esd))
                (prev (if (consp location)
                          (mp::%cas-cdr location old new)
                          (mp::%cas-struct-slot instance location old new))))
           (if (eq prev *slot-unbound-marker*)
               (slot-unbound class instance slot-name)
               prev))))
      ((structurep instance)
       (let ((idx (%find-struct-slot-index instance slot-name)))
         (unless idx
           (error "~S has no slot named ~S" instance slot-name))
         (mp::%cas-struct-slot instance idx old new)))
      (t
       (error "MP:COMPARE-AND-SWAP on SLOT-VALUE: ~S is not a standard-object ~
               or structure-object" instance)))))

(defun slot-unbound (class instance slot-name)
  "Called when an unbound slot is accessed. Default signals an error.
Specialize via defmethod to provide lazy initialization."
  (declare (ignore class))
  ;; CLHS 7.7.10 / unbound-slot: the error is of type UNBOUND-SLOT, a
  ;; CELL-ERROR subtype whose CELL-ERROR-NAME is the slot name and whose
  ;; UNBOUND-SLOT-INSTANCE is the object.  Handlers written against the
  ;; standard catch (unbound-slot) and inspect both.
  (error 'unbound-slot
         :name slot-name
         :instance instance
         :format-control "The slot ~S is unbound in ~S"
         :format-arguments (list slot-name instance)))

(defun %set-slot-value (instance slot-name new-value)
  "Set the value of SLOT-NAME in INSTANCE to NEW-VALUE."
  ;; Fast path mirrors SLOT-VALUE's: one fused builtin call for the
  ;; simple instance-slot case, full protocol otherwise.  The extended-
  ;; protocol check comes FIRST so (SETF SLOT-VALUE-USING-CLASS) methods
  ;; see the write before any storage is touched.
  (if (or *slot-access-protocol-extended-p*
          (not (%struct-slot-store instance slot-name new-value)))
      (%set-slot-value-slow instance slot-name new-value)
      new-value))

(defun %set-slot-value-slow (instance slot-name new-value)
  (let* ((class (class-of instance))
         (index-table (class-slot-index-table class)))
    (cond
      (index-table
       (let ((esd (gethash slot-name index-table)))
         (unless esd
           (error "~S has no slot named ~S" instance slot-name))
         (if *slot-access-protocol-extended-p*
             (setf (slot-value-using-class class instance esd) new-value)
             (let ((location (slot-definition-location esd)))
               (if (consp location)
                   (rplacd location new-value)
                   (%struct-set instance location new-value))
               new-value))))
      ((structurep instance)
       (let ((idx (%find-struct-slot-index instance slot-name)))
         (unless idx
           (error "~S has no slot named ~S" instance slot-name))
         (%struct-set instance idx new-value)
         new-value))
      ((conditionp instance)
       (%set-condition-slot-value instance slot-name new-value)
       new-value)
      (t
       (error "~S has no slot-index-table (not a CLOS instance)" instance)))))

(defsetf slot-value %set-slot-value)

;;; Compile-time inlining of the SLOT-VALUE fast path.  SLOT-VALUE and
;;; %SET-SLOT-VALUE are ordinary DEFUNs, so every access paid a full
;;; Lisp call frame before reaching the fused %STRUCT-SLOT-VALUE /
;;; %STRUCT-SLOT-STORE builtin — the frame, not the slot lookup, is the
;;; bulk of the cost.  These compiler macros splice the DEFUN bodies'
;;; exact fast-path test into every compiled call site, the same
;;; treatment DEFCLASS accessors already get from the body templates
;;; below; WITH-SLOTS expands to SLOT-VALUE forms and inherits this.
;;;
;;; Semantics are identical to the DEFUNs: the extended-protocol check
;;; and the unbound-marker miss fall back to the same slow-path
;;; functions.  (DECLAIM (NOTINLINE SLOT-VALUE)) inhibits the expansion
;;; (compile_call checks notinline before consulting compiler macros),
;;; and FUNCALL/APPLY of #'SLOT-VALUE still goes through the DEFUN.
;;;
;;; The instance form is always LET-bound (it may be a symbol-macro or
;;; have effects; the expansion references it twice).  A constant
;;; slot-name — the common (slot-value obj 'x) shape — is spliced in
;;; directly; re-evaluating a constant is unobservable, so evaluation
;;; order is preserved either way.

(defun %slot-name-constant-p (form)
  (or (keywordp form)
      (and (consp form) (eq (car form) 'quote))))

(define-compiler-macro slot-value (instance slot-name)
  (let* ((obj (gensym "OBJ"))
         (const-p (%slot-name-constant-p slot-name))
         (name (if const-p slot-name (gensym "NAME")))
         (v (gensym "V")))
    `(let ((,obj ,instance)
           ,@(unless const-p `((,name ,slot-name))))
       (if *slot-access-protocol-extended-p*
           (%slot-value-slow ,obj ,name)
           (let ((,v (%struct-slot-value ,obj ,name *slot-unbound-marker*)))
             (if (eq ,v *slot-unbound-marker*)
                 (%slot-value-slow ,obj ,name)
                 ,v))))))

(define-compiler-macro %set-slot-value (instance slot-name new-value)
  (let* ((obj (gensym "OBJ"))
         (const-p (%slot-name-constant-p slot-name))
         (name (if const-p slot-name (gensym "NAME")))
         (val (gensym "VAL")))
    `(let ((,obj ,instance)
           ,@(unless const-p `((,name ,slot-name)))
           (,val ,new-value))
       (if (or *slot-access-protocol-extended-p*
               (not (%struct-slot-store ,obj ,name ,val)))
           (%set-slot-value-slow ,obj ,name ,val)
           ,val))))

(defun slot-boundp (instance slot-name)
  "Return T if SLOT-NAME is bound in INSTANCE.
   Structures are always considered bound (DEFSTRUCT slots have initial values)."
  ;; Fast path: a non-marker read means the slot exists and is bound.
  ;; A miss can be "unbound" (-> NIL), "no such slot" (-> error), or any
  ;; other non-simple case — the slow path distinguishes them.
  (if (or *slot-access-protocol-extended-p*
          (eq (%struct-slot-value instance slot-name *slot-unbound-marker*)
              *slot-unbound-marker*))
      (%slot-boundp-slow instance slot-name)
      t))

(defun %slot-boundp-slow (instance slot-name)
  (let* ((class (class-of instance))
         (index-table (class-slot-index-table class)))
    (cond
      (index-table
       (let ((esd (gethash slot-name index-table)))
         (unless esd
           (error "~S has no slot named ~S" instance slot-name))
         (if *slot-access-protocol-extended-p*
             (slot-boundp-using-class class instance esd)
             (let ((location (slot-definition-location esd)))
               (not (eq (if (consp location)
                            (cdr location)
                            (%struct-ref instance location))
                        *slot-unbound-marker*))))))
      ((structurep instance)
       (unless (%find-struct-slot-index instance slot-name)
         (error "~S has no slot named ~S" instance slot-name))
       t)
      ;; A condition slot is "bound" when it has a value in the slots alist
      ;; (a supplied initarg or an evaluated :initform); CONDITION-SLOT-BOUNDP
      ;; distinguishes that from a genuinely unbound slot.
      ((conditionp instance)
       (condition-slot-boundp instance slot-name))
      (t
       (error "~S has no slot-index-table (not a CLOS instance)" instance)))))

(defun slot-makunbound (instance slot-name)
  "Make SLOT-NAME unbound in INSTANCE.
   Signals an error for structures per CLHS (undefined behavior)."
  (let* ((class (class-of instance))
         (index-table (class-slot-index-table class)))
    (cond
      (index-table
       (let ((esd (gethash slot-name index-table)))
         (unless esd
           (error "~S has no slot named ~S" instance slot-name))
         (if *slot-access-protocol-extended-p*
             (slot-makunbound-using-class class instance esd)
             (let ((location (slot-definition-location esd)))
               (if (consp location)
                   (rplacd location *slot-unbound-marker*)
                   (%struct-set instance location *slot-unbound-marker*))
               instance))))
      ((structurep instance)
       (error "SLOT-MAKUNBOUND is not supported for structures: ~S" instance))
      ((conditionp instance)
       (%condition-slot-makunbound instance slot-name)
       instance)
      (t
       (error "~S has no slot-index-table (not a CLOS instance)" instance)))))

(defun slot-exists-p (instance slot-name)
  "Return T if INSTANCE has a slot named SLOT-NAME."
  (let* ((class (class-of instance))
         (index-table (class-slot-index-table class)))
    (cond
      (index-table
       (multiple-value-bind (esd found-p)
           (gethash slot-name index-table)
         (declare (ignore esd))
         found-p))
      ((structurep instance)
       (if (%find-struct-slot-index instance slot-name) t nil))
      (t nil))))

;;; ====================================================================
;;; Phase 2: C3 Linearization
;;; ====================================================================

;; %c3-merge is defined earlier (before the bootstrap classes) so the
;; built-in class CPLs can use it too.

(defun %compute-class-precedence-list (class)
  "Compute CPL using C3 linearization. Supers must already have CPLs."
  (let ((supers (class-direct-superclasses class)))
    (if (null supers)
        (list class)
        (%c3-merge
         (cons (list class)
               (append
                (mapcar #'class-precedence-list supers)
                (list supers)))))))

(%clos-trace "slot access")
;;; ====================================================================
;;; Phase 3: defclass
;;; ====================================================================

;;; --- Slot spec helpers ---

(defun %parse-slot-spec (spec)
  "Parse a defclass slot specifier into (name . plist) form."
  (if (symbolp spec)
      (list spec)
      (cons (car spec) (cdr spec))))

(defun %slot-spec-name (spec)
  (car spec))

(defun %slot-spec-option (spec key)
  "Get option value from slot spec plist, or NIL if absent."
  (let ((tail (member key (cdr spec))))
    (if tail (cadr tail) nil)))

(defun %slot-spec-has-option-p (spec key)
  "Return T if slot spec contains KEY."
  (if (member key (cdr spec)) t nil))

(defun %slot-spec-all-options (spec key)
  "Get all values for KEY from slot spec plist (supports multiple occurrences)."
  (let ((result nil)
        (tail (cdr spec)))
    (loop
      (setq tail (member key tail))
      (unless tail (return (nreverse result)))
      (push (cadr tail) result)
      (setq tail (cddr tail)))))

;;; --- Effective slot computation ---
;;;
;;; Direct slots arrive as STANDARD-DIRECT-SLOT-DEFINITION structs already
;;; built by the defclass expansion. Effective slots are fresh
;;; STANDARD-EFFECTIVE-SLOT-DEFINITION structs, merged per AMOP §5.5:
;;; most-specific direct slot supplies initform/type/allocation; initargs
;;; are unioned across all direct slots with that name.

(defun %find-slot-def (name slot-defs)
  "Return the first slot-definition in SLOT-DEFS with the given NAME, or NIL."
  (dolist (sd slot-defs nil)
    (when (eq (slot-definition-name sd) name)
      (return sd))))

(defun %direct-to-effective (dsd)
  "Build a fresh effective-slot-definition from a direct-slot-definition.
   Only runtime-critical fields are stored on the effective slot; type,
   initform source, and documentation are introspected via the primary
   direct slot (reachable from class-direct-slots)."
  (%make-effective-slot-def (slot-definition-name dsd)
    (copy-list (slot-definition-initargs dsd))
    (slot-definition-initfunction dsd)
    (slot-definition-allocation dsd)
    nil))

(defun %fold-parent-into-effective (eff parent-dsd)
  "Fold inherited options from PARENT-DSD into effective slot EFF.
   Most-specific already wins (EFF was seeded first); we union initargs
   and fill in initfunction if the child left it blank."
  ;; Union initargs
  (let ((parent-initargs (slot-definition-initargs parent-dsd)))
    (when parent-initargs
      (let ((combined (slot-definition-initargs eff)))
        (dolist (ia parent-initargs)
          (unless (member ia combined :test #'eq)
            (setq combined (append combined (list ia)))))
        (%set-slot-definition-initargs eff combined))))
  ;; initfunction: inherit only if child had none
  (unless (slot-definition-initfunction eff)
    (let ((pfn (slot-definition-initfunction parent-dsd)))
      (when pfn
        (%set-slot-definition-initfunction eff pfn))))
  eff)

(defun %compute-effective-slots (cpl)
  "Compute effective slots from a class precedence list of classes."
  (let ((effective nil))
    (dolist (class cpl)
      (dolist (dsd (class-direct-slots class))
        (let* ((name (slot-definition-name dsd))
               (existing (%find-slot-def name effective)))
          (if existing
              (%fold-parent-into-effective existing dsd)
              (setq effective
                    (append effective (list (%direct-to-effective dsd))))))))
    effective))

;;; --- Build slot-index-table ---

(defun %build-slot-index-table (effective-slots)
  "Build a hash table mapping slot-name -> effective-slot-definition.
   Callers reach the actual storage via SLOT-DEFINITION-LOCATION on the
   returned slot-def -- an integer struct index for :INSTANCE allocation,
   or a cons cell (NAME . VALUE) for :CLASS allocation."
  (let ((table (make-hash-table :test 'eq)))
    (dolist (slot effective-slots)
      (setf (gethash (slot-definition-name slot) table) slot))
    table))

;;; --- Class creation at runtime ---

(defun %finalize-and-register-struct-type (class name direct-super-names)
  "Run the finalization protocol on CLASS and (re)register its instance
   struct type from the finalized slot layout.  Shared by fresh class
   creation and by the re-finalization of an already-registered class
   (redefinition of the class itself, or of one of its superclasses)."
  ;; Run the finalization protocol. Routes through the GF if available
  ;; (always the case once clos.lisp finishes loading), otherwise falls
  ;; back to the internal body.
  (if (fboundp 'finalize-inheritance)
      (finalize-inheritance class)
      (%finalize-inheritance-body class))
  ;; Register struct type with the finalized slot layout. The struct only
  ;; holds :INSTANCE-allocated slots — :CLASS slots live on the class
  ;; itself via the cons cell returned by SLOT-DEFINITION-LOCATION.  A
  ;; metaclass (subclass of STANDARD-CLASS) reuses the fixed 12-slot class
  ;; layout and appends its own slots after index 11 (see %METACLASS-P).
  (let* ((instance-esds nil))
    (dolist (esd (class-effective-slots class))
      (when (eq (slot-definition-allocation esd) :instance)
        (push esd instance-esds)))
    (setq instance-esds (nreverse instance-esds))
    (let ((struct-slot-specs
            (mapcar (lambda (esd) (list (slot-definition-name esd) nil))
                    instance-esds)))
      (if (%metaclass-p class)
          (%register-struct-type name (+ 12 (length instance-esds))
                                 'standard-class
                                 (append +standard-class-slot-layout+
                                         struct-slot-specs))
          (%register-struct-type name (length instance-esds)
                                 (if direct-super-names (car direct-super-names) nil)
                                 struct-slot-specs))))
  class)

(defun %unfinalize-class (class)
  "Mark CLASS as needing re-finalization: clear FINALIZED-P so the next
   FINALIZE-INHERITANCE recomputes the CPL, effective slots, slot index
   table and default initargs, and drop the cached CLASS-PROTOTYPE, which
   was allocated with the old slot layout.  Direct-subclasses and
   direct-methods are deliberately kept -- the metaobject stays the same
   object, so every method specialized on it and every subclass linked to
   it remains valid."
  (%struct-set class 8 nil)                   ; 8: prototype
  (%set-class-finalized-p class nil)
  class)

(defun %refinalize-subclasses (class &optional visited)
  "CLHS 4.3.6 / AMOP: redefining a class updates its subclasses too.
   Re-finalize every (transitive) subclass of CLASS so their CPLs,
   effective slots and struct layouts reflect the redefined superclass.
   Parents are re-finalized before children (the recursion runs after the
   subclass's own finalization), which COMPUTE-SLOTS relies on.  VISITED
   is an EQ hash table threaded through the recursion so a class reachable
   via multiple diamond-inheritance paths is re-finalized only once."
  (let ((visited (or visited (make-hash-table :test 'eq))))
    (dolist (sub (class-direct-subclasses class))
      (unless (gethash sub visited)
        (setf (gethash sub visited) t)
        (%unfinalize-class sub)
        (%finalize-and-register-struct-type
           sub (class-name sub)
           (mapcar #'class-name (class-direct-superclasses sub)))
        (%refinalize-subclasses sub visited)))))

(defun %finalize-and-register-class (class name supers direct-super-names)
  "Shared tail of class creation: run the finalization protocol, register
   the instance struct type, install the class in the class table, link it
   into each super's direct-subclasses, re-finalize any existing
   subclasses (a redefined class keeps its metaobject, and with it its
   subclass links), and invalidate dispatch caches.  Used by both the
   standard %ENSURE-CLASS path and the metaclass path (the
   INITIALIZE-INSTANCE / REINITIALIZE-INSTANCE methods on class
   metaobjects)."
  (%finalize-and-register-struct-type class name direct-super-names)
  ;; Register class
  (setf (find-class name) class)
  ;; Register as subclass of each direct super (idempotent: a direct
  ;; REINITIALIZE-INSTANCE on a class metaobject re-runs this without
  ;; the unlink step, and must not duplicate the link).
  (dolist (super supers)
    (unless (member class (class-direct-subclasses super) :test #'eq)
      (%set-class-direct-subclasses super
        (cons class (class-direct-subclasses super)))))
  ;; Redefinition: propagate the new layout to existing subclasses.  A
  ;; freshly created class has no subclasses, so this is a no-op for it.
  (%refinalize-subclasses class)
  ;; Invalidate all GF dispatch caches (class hierarchy changed)
  (when (fboundp '%invalidate-all-gf-caches)
    (%invalidate-all-gf-caches))
  class)

(defun %reusable-class-metaobject-p (old-class type-name)
  "True when OLD-CLASS (from FIND-CLASS, may be NIL) is a class metaobject
   whose struct type is exactly TYPE-NAME, so a redefinition can update it
   in place.  CLHS DEFCLASS / 4.3.6: redefining a class modifies the
   EXISTING class object -- FIND-CLASS keeps returning the same object, and
   methods specialized on it (or on its superclasses) keep dispatching on
   instances of classes defined after the redefinition.  A metaobject of a
   different struct type (a metaclass change, a BUILT-IN-CLASS or a
   FORWARD-REFERENCED-CLASS stub) cannot be reused and is replaced."
  (and old-class
       (structurep old-class)
       (eq (%struct-type-name old-class) type-name)))

(defun %unlink-from-old-supers (old-class)
  "Drop OLD-CLASS from each of its former supers' direct-subclasses; the
   redefinition re-links it under its (possibly different) new supers."
  (dolist (old-super (class-direct-superclasses old-class))
    (%set-class-direct-subclasses old-super
      (remove old-class (class-direct-subclasses old-super) :test #'eq))))

(defun %ensure-class (name direct-super-names direct-slot-defs
                      &optional direct-default-initargs metaclass)
  "Create or update a CLOS class. Called by defclass expansion (and by
   the ENSURE-CLASS GF after keyword args have been destructured).
   DIRECT-SLOT-DEFS is a list of STANDARD-DIRECT-SLOT-DEFINITION instances
   (already constructed by the defclass macro with initfunctions closed
   over the lexical environment).

   The class struct is allocated here; the CPL, effective slots, and
   effective default-initargs are computed by FINALIZE-INHERITANCE so
   that MOP :around methods on COMPUTE-SLOTS, COMPUTE-CLASS-PRECEDENCE-LIST,
   and COMPUTE-DEFAULT-INITARGS get a chance to run.

   When METACLASS names a user metaclass (a subclass of STANDARD-CLASS),
   the class metaobject is allocated as an instance of that metaclass and
   built through the INITIALIZE-INSTANCE generic function, so metaclass
   methods (e.g. an :AROUND that rewrites :DIRECT-SUPERCLASSES, as in
   serapeum's TOPMOST-OBJECT-CLASS) and metaclass :DEFAULT-INITARGS apply."
  (if (and metaclass (not (eq metaclass 'standard-class)))
      (%ensure-class-via-metaclass name direct-super-names direct-slot-defs
                                   direct-default-initargs metaclass)
      (let* ((old-class (find-class name nil))
             (supers (if direct-super-names
                         (mapcar #'find-class direct-super-names)
                         (list (find-class 'standard-object))))
             (reuse (%reusable-class-metaobject-p old-class 'standard-class))
             (class (if reuse
                        old-class
                        (%make-struct 'standard-class
                          name supers direct-slot-defs
                          nil nil nil nil nil nil nil nil nil))))
        ;; On redefinition, drop the old class from each former super's
        ;; direct-subclasses (read BEFORE the direct-superclasses slot is
        ;; overwritten below); %FINALIZE-AND-REGISTER-CLASS re-links it
        ;; under the new supers.
        (when old-class
          (%unlink-from-old-supers old-class))
        (when reuse
          ;; Update the existing metaobject in place (CLHS 4.3.6): new
          ;; direct supers / slots, and force re-finalization.  Slot 6
          ;; (direct-subclasses) and slot 7 (direct-methods) survive.
          (%struct-set class 1 supers)                 ; 1: direct-superclasses
          (%set-class-direct-slots class direct-slot-defs)
          (%unfinalize-class class))
        (%set-class-direct-default-initargs class direct-default-initargs)
        (%finalize-and-register-class class name supers direct-super-names))))

;;; --- Metaclass-driven class creation ---
;;; Allocate the class metaobject as an instance of its metaclass (reusing
;;; the fixed 12-slot layout plus the metaclass's own slots) and run it
;;; through INITIALIZE-INSTANCE so metaclass methods and :DEFAULT-INITARGS
;;; participate.  The primary INITIALIZE-INSTANCE method specialized on
;;; STANDARD-CLASS (defined in Phase 7) does the actual class building.

(defun %merge-metaclass-default-initargs (meta-class initargs)
  "Prepend the metaclass's effective :DEFAULT-INITARGS to INITARGS for any
   key not already supplied, evaluating each default initfunction."
  (let ((defaults (and meta-class (class-default-initargs meta-class)))
        (result initargs))
    (when defaults
      (let ((not-found (cons nil nil)))
        (dolist (default defaults)
          (let ((key (first default))
                (initfn (second default)))
            (when (eq (getf result key not-found) not-found)
              (setq result (append result (list key (funcall initfn)))))))))
    result))

(defun %ensure-class-via-metaclass (name direct-super-names direct-slot-defs
                                    direct-default-initargs meta-name)
  (let* ((old-class (find-class name nil))
         ;; Same metaclass as before: update the existing metaobject in
         ;; place through REINITIALIZE-INSTANCE (AMOP ENSURE-CLASS-USING-
         ;; CLASS on an existing class), so FIND-CLASS identity and every
         ;; method specialized on the class survive.  A different
         ;; metaclass (or no old class) allocates a fresh instance and
         ;; goes through INITIALIZE-INSTANCE.
         (reuse (%reusable-class-metaobject-p old-class meta-name))
         (meta-class (find-class meta-name))
         (initargs (%merge-metaclass-default-initargs meta-class
                     (list :name name
                           :direct-superclasses direct-super-names
                           :direct-slots direct-slot-defs
                           :direct-default-initargs direct-default-initargs))))
    ;; On redefinition, drop the old class from each former super's
    ;; direct-subclasses (parity with the standard %ENSURE-CLASS path);
    ;; read before the direct-superclasses slot is overwritten.
    (when old-class
      (%unlink-from-old-supers old-class))
    (if reuse
        (progn
          (apply #'reinitialize-instance old-class initargs)
          old-class)
        (let* ((total-slots (length (%struct-slot-names meta-name)))
               (n-extra (- total-slots 12))
               ;; 12 fixed class slots (filled by INITIALIZE-INSTANCE) +
               ;; the metaclass's own slots, initially unbound.
               (class (apply #'%make-struct meta-name
                             (append (make-list 12 :initial-element nil)
                                     (make-list (max 0 n-extra)
                                                :initial-element *slot-unbound-marker*)))))
          (apply #'initialize-instance class initargs)
          class))))

;;; --- Class finalization body ---
;;; Shared between the FINALIZE-INHERITANCE GF default method and the
;;; bootstrap path used by %ENSURE-CLASS before the GFs are defined.
;;; Calling twice is idempotent — a redefinition keeps the class struct
;;; but clears FINALIZED-P (see %UNFINALIZE-CLASS), so the next call
;;; recomputes everything from the new direct supers / slots.

(defun %find-inherited-class-slot-cell (class slot-name)
  "Walk CLASS's direct-superclasses' effective slots looking for a
   class-allocated slot with SLOT-NAME. Return its location cons cell
   so the subclass can share the same storage -- per AMOP section 5.5,
   subclasses inherit :class storage unless they redefine the slot."
  (dolist (super (class-direct-superclasses class) nil)
    (when (class-finalized-p super)
      (let ((super-esd (%find-slot-def slot-name
                                       (class-effective-slots super))))
        (when (and super-esd
                   (eq (slot-definition-allocation super-esd) :class))
          (let ((super-loc (slot-definition-location super-esd)))
            (when (consp super-loc)
              (return super-loc))))))))

(defun %assign-slot-locations (class effective-slots)
  "Fill in SLOT-DEFINITION-LOCATION for each effective slot. Instance
   slots get the next integer struct index. Class-allocated slots get
   a cons (NAME . VALUE) -- inherited from a superclass when the class
   does not provide its own direct definition for the same name.

   A metaclass (subclass of STANDARD-CLASS) reserves indices 0-11 for the
   fixed class layout, so its own instance slots start at index 12."
  (let ((instance-i (if (%metaclass-p class) 12 0)))
    (dolist (esd effective-slots)
      (let ((name (slot-definition-name esd)))
        (cond
          ((eq (slot-definition-allocation esd) :class)
           (let ((own-direct (%find-slot-def name (class-direct-slots class))))
             (%set-slot-definition-location esd
               (if own-direct
                   (cons name *slot-unbound-marker*)
                   (or (%find-inherited-class-slot-cell class name)
                       (cons name *slot-unbound-marker*))))))
          (t
           (%set-slot-definition-location esd instance-i)
           (setq instance-i (+ instance-i 1))))))))

(defun %finalize-inheritance-body (class)
  (unless (class-finalized-p class)
    (%set-class-cpl class (%compute-class-precedence-list class))
    (let ((effective (%compute-slots-default class)))
      (%set-class-effective-slots class effective)
      (%set-class-slot-index-table class (%build-slot-index-table effective))
      (%assign-slot-locations class effective))
    (%set-class-default-initargs class
      (%compute-default-initargs-default class))
    (%set-class-finalized-p class t))
  class)

;;; --- compute-slots default body ---
;;; AMOP §5.5: collect direct slots across the CPL (most-specific first)
;;; grouped by name, then call COMPUTE-EFFECTIVE-SLOT-DEFINITION for each
;;; name. User methods on COMPUTE-EFFECTIVE-SLOT-DEFINITION see the full
;;; per-name slot group.

(defun %compute-slots-default (class)
  (let* ((cpl (class-precedence-list class))
         (order nil)
         (groups (make-hash-table :test 'eq)))
    (dolist (c cpl)
      (dolist (dsd (class-direct-slots c))
        (let ((name (slot-definition-name dsd)))
          (unless (gethash name groups)
            (push name order))
          (setf (gethash name groups)
                (append (gethash name groups) (list dsd))))))
    (setq order (nreverse order))
    (mapcar (lambda (name)
              (%compute-effective-slot-definition-default
                 class name (gethash name groups)))
            order)))

(defun %compute-effective-slot-definition-default (class name direct-slots)
  "AMOP: build an effective slot def from a list of direct slot defs
   (most-specific first). The most-specific supplies initform/type/
   allocation; initargs are unioned and initfunction is inherited only
   when the most-specific leaves it blank."
  (declare (ignore class name))
  (let ((primary (first direct-slots))
        (parents (rest direct-slots)))
    (let ((eff (%direct-to-effective primary)))
      (dolist (p parents)
        (%fold-parent-into-effective eff p))
      eff)))

(defun %compute-default-initargs-default (class)
  "Collect effective default-initargs from the CPL, most-specific wins."
  (let ((result nil)
        (keys-seen nil))
    (dolist (c (class-precedence-list class))
      (let ((directs (class-direct-default-initargs c)))
        (dolist (entry directs)
          (let ((key (first entry)))
            (unless (member key keys-seen :test #'eq)
              (push key keys-seen)
              (setq result (append result (list entry))))))))
    result))

;;; --- defclass macro ---
;;;
;;; Emits a list of runtime %MAKE-DIRECT-SLOT-DEF calls. The initfunction
;;; slot is a lambda closing over the defclass-site lexical environment,
;;; so initforms can reference surrounding bindings.

(defun %slot-spec->direct-def-form (spec)
  "Turn a raw slot specifier into a form that builds a direct-slot-def."
  (let* ((parsed (%parse-slot-spec spec))
         (slot-name (%slot-spec-name parsed))
         (initargs (%slot-spec-all-options parsed :initarg))
         (has-initform (%slot-spec-has-option-p parsed :initform))
         (initform (%slot-spec-option parsed :initform))
         (type (%slot-spec-option parsed :type))
         (allocation (or (%slot-spec-option parsed :allocation) :instance))
         (readers (%slot-spec-all-options parsed :reader))
         (writers (%slot-spec-all-options parsed :writer))
         (accessors (%slot-spec-all-options parsed :accessor))
         (documentation (%slot-spec-option parsed :documentation)))
    `(%make-direct-slot-def ',slot-name
       ',initargs
       ',initform
       ,(if has-initform `(lambda () ,initform) nil)
       ',type
       ',allocation
       ',(append readers accessors)
       ',(append writers (mapcar (lambda (a) `(setf ,a)) accessors))
       ',documentation)))

;;; Accessor body templates (spec 3.1, second half).  The symbols the
;;; templates reference (*SLOT-ACCESS-PROTOCOL-EXTENDED-P*,
;;; *SLOT-UNBOUND-MARKER*, %STRUCT-SLOT-VALUE, %STRUCT-SLOT-STORE) are
;;; captured as symbol objects when clos.lisp is read, so expansions
;;; compiled in user packages (and their FASLs) reference the same
;;; home-package symbols regardless of *PACKAGE*.

(defun %accessor-reader-body (obj-var slot-name)
  "Body form for a DEFCLASS :reader/:accessor function on SLOT-NAME."
  ;; Fall back to %SLOT-VALUE-SLOW directly, not SLOT-VALUE: the
  ;; SLOT-VALUE compiler macro would re-inline the fast-path test this
  ;; body just failed, bloating every generated accessor for nothing.
  ;; %SLOT-VALUE-SLOW re-checks the extended-protocol latch itself, so
  ;; both fallback reasons route exactly as SLOT-VALUE's DEFUN does.
  `(if *slot-access-protocol-extended-p*
       (%slot-value-slow ,obj-var ',slot-name)
       (let ((v (%struct-slot-value ,obj-var ',slot-name
                                    *slot-unbound-marker*)))
         (if (eq v *slot-unbound-marker*)
             (%slot-value-slow ,obj-var ',slot-name)
             v))))

(defun %accessor-writer-body (obj-var slot-name val-var)
  "Body form for a DEFCLASS :writer/:accessor setter on SLOT-NAME."
  `(if (or *slot-access-protocol-extended-p*
           (not (%struct-slot-store ,obj-var ',slot-name ,val-var)))
       (%set-slot-value-slow ,obj-var ',slot-name ,val-var)
       ,val-var))

(defmacro defclass (name direct-superclasses slot-specifiers &rest class-options)
  "Define a new CLOS class."
  (let ((accessor-defs nil)
        (slot-def-forms nil)
        (default-initarg-forms nil))
    ;; Parse class options
    (dolist (opt class-options)
      (when (and (consp opt) (eq (car opt) :default-initargs))
        (let ((args (cdr opt)))
          (loop while args
                do (let ((key (pop args))
                         (val (pop args)))
                     (push `(list ',key (lambda () ,val)) default-initarg-forms))))))
    ;; Parse each slot specifier
    (dolist (spec slot-specifiers)
      (let* ((parsed (%parse-slot-spec spec))
             (slot-name (%slot-spec-name parsed))
             (accessors (%slot-spec-all-options parsed :accessor))
             (readers (%slot-spec-all-options parsed :reader))
             (writers (%slot-spec-all-options parsed :writer)))
        (push (%slot-spec->direct-def-form spec) slot-def-forms)
        ;; Generate accessor functions.  Each inlines the fused
        ;; %STRUCT-SLOT-VALUE / %STRUCT-SLOT-STORE fast path (see
        ;; SLOT-VALUE above) instead of calling SLOT-VALUE, saving a
        ;; full function call per access on the hot path; any miss —
        ;; :CLASS slot, unbound slot, extended slot-access protocol,
        ;; wrong object type — falls back to SLOT-VALUE's full protocol.
        (dolist (accessor accessors)
          (let ((setter-name (amilisp::%setf-store-symbol accessor)))
            (push `(defun ,accessor (obj)
                     ,(%accessor-reader-body 'obj slot-name))
                  accessor-defs)
            (push `(defun ,setter-name (val obj)
                     ,(%accessor-writer-body 'obj slot-name 'val))
                  accessor-defs)))
        (dolist (reader readers)
          (push `(defun ,reader (obj)
                   ,(%accessor-reader-body 'obj slot-name))
                accessor-defs))
        (dolist (writer writers)
          (push `(defun ,writer (val obj)
                   ,(%accessor-writer-body 'obj slot-name 'val))
                accessor-defs))))
    (setq slot-def-forms (nreverse slot-def-forms))
    (setq accessor-defs (nreverse accessor-defs))
    `(eval-when (:compile-toplevel :load-toplevel :execute)
       (%ensure-class ',name
                      ',direct-superclasses
                      (list ,@slot-def-forms)
                      (list ,@(nreverse default-initarg-forms)))
       ,@accessor-defs
       (find-class ',name))))

(%clos-trace "defclass")
;;; ====================================================================
;;; Phase 4: make-instance + Initialization
;;; ====================================================================

(defun allocate-instance (class)
  "Allocate a fresh instance of CLASS with all instance slots unbound.
   Class-allocated slots don't take struct storage -- they live in the
   cons cell attached to their effective-slot-definition."
  (let ((name (class-name class))
        (n 0))
    (dolist (esd (class-effective-slots class))
      (when (eq (slot-definition-allocation esd) :instance)
        (setq n (+ n 1))))
    (apply #'%make-struct name
           (make-list n :initial-element *slot-unbound-marker*))))

(defun %initarg-to-slot-index (class initarg)
  "Find the slot index for INITARG in CLASS, or NIL."
  (let ((effective (class-effective-slots class))
        (i 0))
    (dolist (slot effective)
      (when (member initarg (slot-definition-initargs slot) :test #'eq)
        (return-from %initarg-to-slot-index i))
      (setq i (+ i 1)))
    nil))

(defun %find-initarg-value (initargs slot-initargs)
  "Scan INITARGS (plist) for the first key that appears in SLOT-INITARGS.
   Return (values value supplied-p)."
  (let ((tail initargs))
    (loop while tail
          do (let ((key (car tail)))
               (when (member key slot-initargs :test #'eq)
                 (return-from %find-initarg-value (values (cadr tail) t)))
               (setq tail (cddr tail)))))
  (values nil nil))

(defun shared-initialize (instance slot-names &rest initargs)
  "Initialize slots of INSTANCE from INITARGS and initforms.
   Routes writes through SLOT-DEFINITION-LOCATION so that :CLASS slots
   update their shared cons cell while :INSTANCE slots write into the
   struct directly."
  (let* ((class (class-of instance))
         (effective (class-effective-slots class)))
    (dolist (slot effective)
      (let* ((slot-name (slot-definition-name slot))
             (slot-initargs (slot-definition-initargs slot))
             (location (slot-definition-location slot)))
        (multiple-value-bind (initarg-val initarg-supplied)
            (%find-initarg-value initargs slot-initargs)
          (cond
            (initarg-supplied
             (if (consp location)
                 (rplacd location initarg-val)
                 (%struct-set instance location initarg-val)))
            ((and (or (eq slot-names t)
                      (member slot-name slot-names :test #'eq))
                  (eq (if (consp location)
                          (cdr location)
                          (%struct-ref instance location))
                      *slot-unbound-marker*)
                  (slot-definition-initfunction slot))
             (let ((val (funcall (slot-definition-initfunction slot))))
               (if (consp location)
                   (rplacd location val)
                   (%struct-set instance location val))))))))
    instance))

(defun initialize-instance (instance &rest initargs)
  "Initialize a newly allocated instance."
  (apply #'shared-initialize instance t initargs))

(defun make-instance (class-or-name &rest initargs)
  "Create a new instance of CLASS-OR-NAME with INITARGS."
  (let* ((class (if (symbolp class-or-name)
                    (find-class class-or-name)
                    class-or-name))
         ;; Apply default-initargs: for each default not already in initargs,
         ;; evaluate the initform function and prepend to initargs
         (defaults (class-default-initargs class))
         (effective-initargs initargs))
    (when defaults
      (dolist (default defaults)
        (let ((key (first default))
              (initfn (second default)))
          (unless (member key initargs :test #'eq)
            (setq effective-initargs
                  (append effective-initargs (list key (funcall initfn)))))))
      (setq initargs effective-initargs))
    (let ((instance (allocate-instance class)))
      ;; Uses initialize-instance which is initially a plain function,
      ;; then upgraded to a GF in Phase 7
      (apply #'initialize-instance instance initargs)
      instance)))

(%clos-trace "make-instance")
;;; ====================================================================
;;; Phase 5: defgeneric + defmethod + Dispatch
;;; ====================================================================

;;; --- Generic function and method struct types ---

;;; standard-generic-function: 9 slots
;;;   0: name, 1: lambda-list, 2: methods, 3: discriminating-function,
;;;   4: method-combination, 5: dispatch-cache, 6: cacheable-p,
;;;   7: eql-value-sets, 8: inline-cache
;;;
;;; INLINE-CACHE is a small (NIL or class+EMF tuple) cell consulted by
;;; arity-specialized discriminators before they fall back to the hash-
;;; table cache.  Layouts by arity:
;;;   1-arg: (class . emf)
;;;   2-arg: (class1 class2 . emf)
;;;   3-arg: ((class1 class2 class3) . emf)
;;; %INSTALL-METHOD-IN-GF / %UNINSTALL-METHOD-FROM-GF /
;;; %INVALIDATE-ALL-GF-CACHES all clear this slot alongside the hash
;;; cache, so stale EMFs cannot linger past a method-set change.
(%register-struct-type 'standard-generic-function 9 nil
  '((name nil) (lambda-list nil) (methods nil)
    (discriminating-function nil) (method-combination nil)
    (dispatch-cache nil) (cacheable-p nil) (eql-value-sets nil)
    (inline-cache nil)))

;;; standard-method: 6 slots
;;;   0: generic-function, 1: specializers, 2: qualifiers, 3: function,
;;;   4: lambda-list, 5: simple-primary-p
;;;
;;; SIMPLE-PRIMARY-P is set by DEFMETHOD when the source body uses
;;; neither CALL-NEXT-METHOD nor NEXT-METHOD-P (and the method has no
;;; qualifiers).  Detection is conservative — it is a symbol-name scan,
;;; so any reference (even in a quoted form) marks the method as
;;; non-simple.  The dispatch fast path relies on this flag to skip the
;;; method-chain wrapper that establishes the three CNM dynamic
;;; bindings on every call.
(%register-struct-type 'standard-method 6 nil
  '((generic-function nil) (specializers nil) (qualifiers nil)
    (function nil) (lambda-list nil) (simple-primary-p nil)))

;;; Funcallable metaclass hierarchy (AMOP §5.5).
;;;
;;; A funcallable instance is a metaobject that can be invoked as a
;;; function; in our implementation, the C-side call path recognises a
;;; STANDARD-GENERIC-FUNCTION struct and dispatches through its
;;; discriminating-function slot (see cl_unwrap_funcallable in vm.c).
;;;
;;; User-defined metaclasses are out of scope (see specs/mop.md), so
;;; FUNCALLABLE-STANDARD-CLASS exists as a placeholder for API
;;; completeness — DEFCLASS ignores `:metaclass`. FUNCALLABLE-STANDARD-OBJECT
;;; is a real superclass of STANDARD-GENERIC-FUNCTION so that
;;;   (typep #'foo 'funcallable-standard-object) => T
;;; and typep on 'function succeeds too (function is in the CPL).
(%make-bootstrap-class 'funcallable-standard-class
  (list (find-class 'standard-class)))
(%make-bootstrap-class 'funcallable-standard-object
  (list (find-class 'function) (find-class 'standard-object)))

;;; Register these as classes so dispatch works on them
(%make-bootstrap-class 'standard-generic-function
  (list (find-class 'funcallable-standard-object)))
(%make-bootstrap-class 'standard-method
  (list (find-class 'standard-object)))
;; standard-class is already registered from the bootstrap section

;;; --- GF/method accessors ---

(defun gf-name (gf) (%struct-ref gf 0))
(defun gf-lambda-list (gf) (%struct-ref gf 1))
(defun gf-methods (gf) (%struct-ref gf 2))
(defun %set-gf-methods (gf val) (%struct-set gf 2 val))
(defun gf-discriminating-function (gf) (%struct-ref gf 3))
(defun %set-gf-discriminating-function (gf val) (%struct-set gf 3 val))
(defun gf-method-combination (gf) (%struct-ref gf 4))
(defun %set-gf-method-combination (gf val) (%struct-set gf 4 val))
(defun gf-dispatch-cache (gf) (%struct-ref gf 5))
(defun %set-gf-dispatch-cache (gf val) (%struct-set gf 5 val))
(defun gf-cacheable-p (gf)
  ;; Lazy: %install-method-in-gf marks the slot :DIRTY so bulk method
  ;; installation during boot doesn't re-derive the dispatch mode on every
  ;; add.  Resolve on first read — accessor must return the user-visible
  ;; mode (integer / :EQL / NIL), not the internal :DIRTY sentinel.
  (let ((mode (%struct-ref gf 6)))
    (if (eq mode :dirty)
        (let ((computed (%compute-gf-cacheable-p gf)))
          (%struct-set gf 6 computed)
          (%struct-set gf 7
                       (if (eq computed :eql)
                           (%compute-eql-value-sets gf)
                           nil))
          computed)
        mode)))
(defun %set-gf-cacheable-p (gf val) (%struct-set gf 6 val))
(defun gf-eql-value-sets (gf) (%struct-ref gf 7))
(defun %set-gf-eql-value-sets (gf val) (%struct-set gf 7 val))
(defun gf-inline-cache (gf) (%struct-ref gf 8))
(defun %set-gf-inline-cache (gf val) (%struct-set gf 8 val))

(defun method-generic-function (m) (%struct-ref m 0))
(defun %set-method-generic-function (m gf) (%struct-set m 0 gf))
(defun method-specializers (m) (%struct-ref m 1))
(defun method-qualifiers (m) (%struct-ref m 2))
(defun method-function (m) (%struct-ref m 3))
(defun method-lambda-list (m) (%struct-ref m 4))
(defun method-simple-primary-p (m) (%struct-ref m 5))
(defun %set-method-simple-primary-p (m val) (%struct-set m 5 val))

;;; --- GF table ---
;; Synchronized: DEFGENERIC/ENSURE-GENERIC-FUNCTION run on the loading
;; thread; concurrent loads mutate this table at once (see
;; *EQL-SPECIALIZER-TABLE* for the plain-table corruption hazard).
(defvar *generic-function-table* (amilisp::%make-sync-hash-table 'equal))

;;; --- call-next-method support ---
;;;
;;; A method with a next-method chain runs with *CNM* bound to
;;; (NEXT . ARGS): NEXT is the per-EMF list of the method functions still
;;; to run after it (most specific first, built once per EMF, never
;;; mutated), ARGS the argument list.  CALL-NEXT-METHOD pops NEXT;
;;; NEXT-METHOD-P asks whether it is a cons.  A chain whose LAST function
;;; never uses CALL-NEXT-METHOD / NEXT-METHOD-P ends in T instead of NIL,
;;; so that function is called bare (no binding) — a non-simple last
;;; function gets NEXT = NIL and signals "No next method".  One cons, one
;;; dynamic binding and only inline CAR/CDR per level: this replaced a
;;; per-level closure chain that bound three specials and allocated a
;;; closure and an &rest list for every method entered (specs/
;;; performance.md 4.2 item 7).  A first version kept the functions in a
;;; simple-vector with an index; the LENGTH and SVREF calls per level
;;; made it slower than the closures it replaced.
(defvar *cnm* nil)
(defvar *current-method-args* nil)

;;; --- Dependent-maintenance protocol (AMOP §5.4) ---
;;; Observer protocol for classes and generic functions.  Dependents
;;; are stored in a side table keyed EQ on the metaobject, not a slot,
;;; so metaobjects with no dependents (the overwhelming common case)
;;; pay nothing.  ADD-DEPENDENT / REMOVE-DEPENDENT / MAP-DEPENDENTS /
;;; UPDATE-DEPENDENT are defined later in this file; %NOTIFY-DEPENDENTS
;;; is the internal broadcaster invoked from ENSURE-CLASS, ADD-METHOD,
;;; REMOVE-METHOD, and ENSURE-GENERIC-FUNCTION.
(defvar *metaobject-dependents* (amilisp::%make-sync-hash-table 'eq)
  "EQ hash from class or generic-function metaobject to a list of
registered dependents (via ADD-DEPENDENT).  Synchronized: read by
%NOTIFY-DEPENDENTS on every method (un)install from any thread while
ADD-DEPENDENT may mutate it on another (see *EQL-SPECIALIZER-TABLE*
for the plain-table corruption hazard).")

(defun %notify-dependents (metaobject &rest initargs)
  "Broadcast UPDATE-DEPENDENT to each dependent of METAOBJECT with
INITARGS describing the change.  Short-circuits when there are no
dependents and (defensively) when the UPDATE-DEPENDENT GF is not yet
bound -- both conditions hold during bootstrap before the protocol GFs
are defined."
  (let ((deps (gethash metaobject *metaobject-dependents*)))
    (when (and deps (fboundp 'update-dependent))
      (dolist (dep deps)
        (apply #'update-dependent metaobject dep initargs)))))

(defun %cnm-invoke (chain args)
  "Run the first function of CHAIN (a chain list, see *CNM*) on ARGS.
A chain terminated by T instead of NIL says its last function never reads
*CNM*: that one is called bare.  The hot callers (the chain entry lambdas
and CALL-NEXT-METHOD) inline this body: a call per level is exactly what
the chain is meant to save."
  (let ((rest (cdr chain)))
    (if (eq rest t)
        (apply (car chain) args)
        (let ((*cnm* (cons rest args)))
          (apply (car chain) args)))))

(defun %cnm-chain (methods)
  "The chain list of METHODS' functions, most specific first, terminated
by T when the last method is simple-primary (see *CNM*) and NIL otherwise."
  (let ((chain (if (method-simple-primary-p (car (last methods))) t nil)))
    (dolist (m (reverse methods))
      (setq chain (cons (method-function m) chain)))
    chain))

(defun call-next-method (&rest args)
  "Call the next most-specific method.
When called with no arguments, passes the original method arguments."
  ;; (car st) is the remaining chain: a cons, or NIL when this is the last
  ;; method (T never appears there — it is only ever a chain's terminator,
  ;; consumed below).  So a bare AND is the whole test: no CONSP call.
  (let* ((st *cnm*)
         (chain (and st (car st))))
    (if chain
        ;; %CNM-INVOKE, inlined: one call fewer per level.
        (let ((args (if args args (cdr st)))
              (rest (cdr chain)))
          (if (eq rest t)
              (apply (car chain) args)
              (let ((*cnm* (cons rest args)))
                (apply (car chain) args))))
        (error "No next method"))))

(defun next-method-p ()
  "Return T if there is a next method."
  (let ((st *cnm*))
    (if (and st (car st)) t nil)))

;;; --- Subclass predicate ---

(defun %subclassp (sub super)
  "Return T if SUB is a subclass of (or eq to) SUPER."
  (member super (class-precedence-list sub) :test #'eq))

;;; --- Applicability and specificity ---

(defun %method-applicable-p (method args)
  "Return T if METHOD is applicable to ARGS."
  (let ((specializers (method-specializers method))
        (applicable t))
    (do ((specs specializers (cdr specs))
         (as args (cdr as)))
        ((or (null specs) (null as)) applicable)
      (let ((spec (car specs))
            (arg (car as)))
        (cond
          ((eql-specializer-p spec)
           (unless (eql arg (eql-specializer-object spec))
             (setq applicable nil)
             (return nil)))
          (t
           (let ((arg-class (class-of arg)))
             (unless (%subclassp arg-class spec)
               (setq applicable nil)
               (return nil)))))))))

(defun %method-more-specific-p (m1 m2 args)
  "Return T if M1 is more specific than M2 for ARGS."
  (let ((s1 (method-specializers m1))
        (s2 (method-specializers m2)))
    (do ((sp1 s1 (cdr sp1))
         (sp2 s2 (cdr sp2))
         (as args (cdr as)))
        ((or (null sp1) (null sp2)) nil)
      (let ((c1 (car sp1))
            (c2 (car sp2)))
        ;; EQ handles the interned-EQL-specializer case as well as class
        ;; identity; only unequal specializers need ordering.
        (unless (eq c1 c2)
          (cond
            ((eql-specializer-p c1) (return t))
            ((eql-specializer-p c2) (return nil))
            (t
             (let* ((arg-class (class-of (car as)))
                    (cpl (class-precedence-list arg-class)))
               (dolist (c cpl)
                 (when (eq c c1) (return-from %method-more-specific-p t))
                 (when (eq c c2) (return-from %method-more-specific-p nil)))))))))))

(defun %compute-applicable-methods (gf args)
  "Return applicable methods sorted most-specific-first."
  (let ((applicable nil))
    (dolist (m (gf-methods gf))
      (when (%method-applicable-p m args)
        (push m applicable)))
    ;; Sort by specificity (insertion sort — small lists)
    (let ((sorted nil))
      (dolist (m applicable)
        (if (null sorted)
            (setq sorted (list m))
            (let ((inserted nil)
                  (result nil)
                  (rest sorted))
              (loop
                (when (null rest)
                  (push m result)
                  (setq inserted t)
                  (return))
                (if (and (not inserted)
                         (%method-more-specific-p m (car rest) args))
                    (progn
                      (push m result)
                      (setq inserted t)
                      (dolist (r rest) (push r result))
                      (return))
                    (progn
                      (push (car rest) result)
                      (setq rest (cdr rest)))))
              (setq sorted (nreverse result)))))
      sorted)))

;;; --- Standard method combination ---

(defvar *gf-cache-heals* 0
  "Count of self-healed dispatch inconsistencies: a stale negative
   dispatch-cache entry, or a standard-combination method set that lacked a
   primary until recomputed from the live method list.  A non-zero value
   means the dispatch cache / applicable-method computation disagreed with
   the authoritative method list at least once -- a GC relocation artifact, a
   concurrent cache write / method-list read from another thread, or a missed
   invalidation -- and was corrected.  Exposed for field diagnosis; normally
   zero.")

(defvar *clos-diagnose-no-primary* nil
  "When true, a genuine \"no applicable primary method\" miss (after the
   self-heal recompute also fails to find one) dumps a diagnostic to
   *ERROR-OUTPUT* via %REPORT-DISPATCH-NO-PRIMARY: arg classes, their
   precedence lists, and the GF's full method roster.  Default NIL -- this is
   an ordinary, common user error (only :before/:after/:around methods
   defined) as well as the symptom of the rare dispatch-metadata corruption
   the heal targets, so the dump is opt-in field-diagnosis output, not
   printed on every occurrence.")

(defvar *clos-diagnose-no-applicable* nil
  "When true, a fresh dispatch miss that computes an EMPTY applicable-method
   set for a GF that HAS methods -- the first-call analogue of the stale
   negative-cache entry %DISPATCH-NEGATIVE-HIT heals, but reached before any
   negative entry exists to heal -- dumps a diagnostic to *ERROR-OUTPUT* (arg
   classes + their precedence lists + the GF's method roster) after the
   self-heal retries also come up empty.  Default NIL: a fresh empty set is
   usually a genuine user error (a GF called with argument types it has no
   method for), so the dump is opt-in field-diagnosis output, printed only
   when this flag is set before reproducing the symptom.")

(defun %build-effective-method (methods)
  "Build a cacheable effective method closure (args-independent)."
  (let ((around nil)
        (before nil)
        (primary nil)
        (after nil))
    ;; Separate by qualifier
    (dolist (m methods)
      (let ((q (method-qualifiers m)))
        (cond
          ((null q) (push m primary))
          ((equal q '(:around)) (push m around))
          ((equal q '(:before)) (push m before))
          ((equal q '(:after)) (push m after)))))
    (setq primary (nreverse primary))
    (setq before (nreverse before))
    ;; after list is reversed (least-specific-first) — that's what we want
    (setq around (nreverse around))
    (unless primary
      (error "No applicable primary method"))
    ;; Fast path: a single primary method whose body does not use
    ;; CALL-NEXT-METHOD/NEXT-METHOD-P, and no :before/:after/:around.
    ;; The EMF is just the method-function itself — no wrapper closure,
    ;; no three-binding *CALL-NEXT-METHOD-FUNCTION* dance per call.
    (when (and (null before) (null after) (null around)
               (null (cdr primary))
               (method-simple-primary-p (car primary)))
      (return-from %build-effective-method (method-function (car primary))))
    ;; Build the effective method
    (let* ((primary-chain (%make-method-chain primary))
           (call-primary
             (if (and (null before) (null after))
                 ;; Optimization: no before/after, use primary chain directly
                 primary-chain
                 (lambda (&rest call-args)
                   (let ((args (if call-args call-args *current-method-args*)))
                     ;; :before/:after (auxiliary) methods are applied RAW, so
                     ;; without this they would observe whatever CALL-NEXT-METHOD
                     ;; specials an ENCLOSING dispatch left bound — e.g. when this
                     ;; GF is dispatched from inside another GF's method.  Per the
                     ;; standard an auxiliary method has no next method, so bind
                     ;; the trio to NIL: (NEXT-METHOD-P) => NIL and CALL-NEXT-METHOD
                     ;; => "no next method".  Without this a non-conformant :after
                     ;; doing (when (next-method-p) (call-next-method)) jumps into
                     ;; the OUTER method's chain (observed: a sento mailbox :after
                     ;; leaking into hunchentoot's request handler on a worker
                     ;; thread, only because that handler had the specials bound).
                     ;; The primary chain rebinds *CNM* itself (%MAKE-METHOD-
                     ;; CHAIN), so its methods are unaffected.
                     (let ((*cnm* nil))
                       ;; Execute :before methods
                       (dolist (m before)
                         (apply (method-function m) args))
                       ;; Execute primary chain (preserve multiple values)
                       (let ((results (multiple-value-list (apply primary-chain args))))
                         ;; Execute :after methods
                         (dolist (m after)
                           (apply (method-function m) args))
                         (values-list results))))))))
      (if around
          (%make-around-chain around call-primary)
          call-primary))))

(defun %call-with-method-combination (methods args)
  "Execute standard method combination on sorted applicable METHODS."
  (let ((*current-method-args* args))
    (apply (%build-effective-method methods) args)))

(defun %methods-have-primary-p (methods)
  "True if METHODS contains at least one primary (unqualified) method."
  (dolist (m methods nil)
    (when (null (method-qualifiers m)) (return t))))

(defun %dispatch-dump-metadata (gf args applicable recomputed)
  "Dump the argument classes, their class-precedence-lists, and the GF's full
   method roster (qualifiers + specializers) to *ERROR-OUTPUT*.  Shared by the
   no-primary and no-applicable field diagnostics -- a truncated CPL points at
   class-metadata corruption, a short/wrong method list at method-list
   corruption, so the same dump distinguishes the two failure modes for either
   symptom.  APPLICABLE/RECOMPUTED may be NIL when the caller has no such set
   to report (the fresh-empty case)."
  (let ((*print-length* 20) (*print-level* 4))
    (when args
      (format *error-output* "; [dispatch]   arg classes: ~S~%"
              (mapcar (lambda (a) (class-name (class-of a))) args))
      (dolist (a args)
        (let ((c (class-of a)))
          (format *error-output* "; [dispatch]   CPL(~S): ~S~%"
                  (class-name c)
                  (ignore-errors
                    (mapcar #'class-name (class-precedence-list c)))))))
    (when (typep gf 'standard-generic-function)
      (let ((all (gf-methods gf)))
        (format *error-output*
                "; [dispatch]   ~D defined method(s) on GF; ~D applicable, ~D on recompute~%"
                (length all) (length applicable) (length recomputed))
        (format *error-output* "; [dispatch]   applicable qualifiers: ~S~%"
                (mapcar #'method-qualifiers applicable))
        (dolist (m all)
          (format *error-output* "; [dispatch]     method q=~S specs=~S~%"
                  (method-qualifiers m)
                  (ignore-errors
                    (mapcar (lambda (s)
                              (cond ((eql-specializer-p s)
                                     (list 'eql (eql-specializer-object s)))
                                    ((typep s 'class) (class-name s))
                                    (t s)))
                            (method-specializers m)))))))))

(defun %report-dispatch-no-primary (gf args applicable recomputed)
  "Emit a diagnostic when standard combination finds applicable methods but
   NO primary among them.  In conformant code this is a genuine user error
   (only :before/:after/:around defined); but it also surfaces the same
   intermittent dispatch-metadata corruption as the negative-cache path --
   a GC relocation of a class's precedence list or the GF method list, or a
   concurrent mutation from another thread (log4cl's watcher / sento workers
   run while the main thread dispatches), which drops the primary from the
   applicable set even though it plainly exists.  Dump enough to tell the two
   apart on the next occurrence: the arg classes + their precedence lists
   (to spot a truncated CPL), and the GF's full defined-method roster with
   qualifiers/specializers (to spot a short/corrupt method list).  A no-op
   unless *CLOS-DIAGNOSE-NO-PRIMARY* is true -- see its docstring."
  (when *clos-diagnose-no-primary*
    (format *error-output*
            "~&; [dispatch] No applicable PRIMARY method for ~S~%"
            (if (typep gf 'standard-generic-function) (gf-name gf) gf))
    (%dispatch-dump-metadata gf args applicable recomputed)))

(defvar *dispatch-heal-retries* 6
  "How many times a dispatch self-heal recomputes the applicable-method set,
   yielding between tries, before giving up.  A transient dispatch-metadata
   inconsistency (a GC relocation window, or a concurrent metadata mutation
   from a peer thread -- log4cl's watcher / sento workers on the single-core
   AmigaOS target) needs at least one yield for the mutator / GC to make
   progress, after which the recompute observes the corrected set; the extra
   tries add margin.  Only ever reached on a dispatch MISS for a GF that
   plainly has the needed method -- never on the hot path -- so the cost is
   confined to the rare heal.")

(defun %recompute-methods-until (gf args predicate)
  "Recompute (%COMPUTE-APPLICABLE-METHODS GF ARGS) up to *DISPATCH-HEAL-RETRIES*
   times, YIELDING between tries, and return the first recomputed set that
   satisfies PREDICATE -- or NIL if none does.  The yield is what makes the
   retry meaningful: the recompute is a pure function of the live dispatch
   metadata, so a plain re-run with no yield just observes the same
   (transiently corrupt) state every time.  Giving a concurrent mutator / GC a
   scheduling slot lets the window close, after which the recompute sees the
   real set.  Shared by both self-heals -- empty applicable set (PREDICATE =
   non-empty) and missing primary (PREDICATE = has-primary)."
  (when (and args (typep gf 'standard-generic-function))
    (dotimes (i *dispatch-heal-retries*)
      (let ((methods (%compute-applicable-methods gf args)))
        (when (funcall predicate methods)
          (return-from %recompute-methods-until methods)))
      (mp:thread-yield)))
  nil)

(defun %gf-roster-has-primary-p (gf)
  "True if GF's method roster contains at least one primary (unqualified)
   method.  Distinguishes a genuine \"only :around/:before/:after defined\"
   user error (no retry -- it can never yield an applicable primary) from a
   suspicious applicable set that lacks a primary the GF plainly defines
   (retry-and-heal)."
  (and (typep gf 'standard-generic-function)
       (dolist (m (gf-methods gf) nil)
         (when (null (method-qualifiers m)) (return t)))))

(defun %dispatch-standard-emf (gf methods)
  "Build a standard-combination EMF from METHODS, but if there is no primary
   method, first re-verify against the live method list (via
   %RECOMPUTE-METHODS-UNTIL on *CURRENT-METHOD-ARGS*, bound by every dispatch
   resolver) before letting %BUILD-EFFECTIVE-METHOD signal \"No applicable
   primary method\".  A recomputed set that DOES contain a primary means the
   set handed in was stale/corrupt (same class of transient dispatch-metadata
   corruption as the negative-cache bug -- a GC relocation of an argument
   class's precedence list, or a concurrent metadata read while a peer thread
   mutates it, dropping the applicable primary while the ((t)(t)) :around
   survives); retry-with-yield and heal.  Gated on the GF's roster actually
   containing a primary: a GF with only :around/:before/:after can never heal,
   so it errors immediately with no wasted retries.  Otherwise the miss is
   genuine (or the corruption is persistent) -- dump a diagnostic and fall
   through to the normal error.  Preserves the fast path bit-for-bit when a
   primary is present in the handed-in set."
  (if (%methods-have-primary-p methods)
      (%build-effective-method methods)
      (let* ((args *current-method-args*)
             (recomputed (and (%gf-roster-has-primary-p gf)
                              (%recompute-methods-until
                               gf args #'%methods-have-primary-p))))
        (if recomputed
            (progn
              (setq *gf-cache-heals* (+ *gf-cache-heals* 1))
              (%build-effective-method recomputed))
            (progn
              (%report-dispatch-no-primary
               gf args methods
               (and args (typep gf 'standard-generic-function)
                    (%compute-applicable-methods gf args)))
              ;; Genuine miss / persistent corruption: signal as before.
              (%build-effective-method methods))))))

(defun %dispatch-build-emf (gf methods)
  "Build an EMF honouring the GF's method combination.
   GF is the generic-function metaobject.  When the combination slot is
   NIL (pre-Phase-8 GFs) or names the standard combination, the
   %BUILD-EFFECTIVE-METHOD path is used (via %DISPATCH-STANDARD-EMF, which
   adds a no-primary re-verify/heal + diagnostic).  Non-standard combinations
   route through %BUILD-SHORT-EFFECTIVE-METHOD or %BUILD-LONG-EFFECTIVE-METHOD."
  (let ((combo (gf-method-combination gf)))
    (cond
      ((null combo) (%dispatch-standard-emf gf methods))
      ((eq (%struct-ref combo 2) :standard) (%dispatch-standard-emf gf methods))
      ((eq (%struct-ref combo 2) :short)
       (%build-short-effective-method combo methods))
      ((eq (%struct-ref combo 2) :long)
       (%build-long-effective-method gf combo methods))
      (t (%dispatch-standard-emf gf methods)))))

(defun %dispatch-heal-empty (gf args)
  "A *fresh* (uncached) dispatch miss on GF for ARGS computed an EMPTY
   applicable-method set.  This is the first-call analogue of the stale
   negative-cache entry %DISPATCH-NEGATIVE-HIT heals: the same transient
   dispatch-metadata corruption -- a GC relocation of a class precedence list
   or the GF method list, or a concurrent metadata mutation from a peer thread
   (log4cl's watcher / sento workers dispatch while the main thread loads a
   system) -- can make one %COMPUTE-APPLICABLE-METHODS pass observe an empty set
   even though a method plainly applies.  On the *first* miss no negative entry
   exists yet, so nothing had a chance to self-heal, and the plain path would
   cache a (now stale) negative AND immediately signal NO-APPLICABLE-METHOD.

   Defend that first miss the same way: if GF actually has methods, recompute
   with yields between tries (%RECOMPUTE-METHODS-UNTIL) so any in-flight window
   can close.  Return an EMF to dispatch through if a retry finds methods (and
   bump *GF-CACHE-HEALS*), or NIL for a genuine miss -- the caller then caches
   the negative + signals NO-APPLICABLE-METHOD exactly as before.  A GF with
   zero methods, or a real type mismatch, recomputes empty every time and
   returns NIL, so this never turns a genuine miss into a spurious call."
  (when (gf-methods gf)
    (let ((methods (%recompute-methods-until gf args (lambda (m) m))))
      (cond
        (methods
         (setq *gf-cache-heals* (+ *gf-cache-heals* 1))
         (return-from %dispatch-heal-empty (%dispatch-build-emf gf methods)))
        (*clos-diagnose-no-applicable*
         (format *error-output*
                 "~&; [dispatch] Fresh miss computed EMPTY applicable set for ~S (GF has ~D method(s))~%"
                 (if (typep gf 'standard-generic-function) (gf-name gf) gf)
                 (length (gf-methods gf)))
         (%dispatch-dump-metadata gf args nil nil)))))
  nil)

(defun %make-method-chain (methods)
  "Build the primary-method chain of METHODS (most specific first): the EMF
entry that calls the first method with its CALL-NEXT-METHOD state (*CNM*)
established."
  (cond
    ((null methods)
     (lambda (&rest call-args)
       (declare (ignore call-args))
       (error "No next method")))
    ;; Leaf optimization: a single method whose body never references
    ;; CALL-NEXT-METHOD / NEXT-METHOD-P needs no chain at all — its
    ;; function IS the EMF.
    ((and (null (cdr methods))
          (method-simple-primary-p (car methods)))
     (method-function (car methods)))
    (t
     (let ((chain (%cnm-chain methods)))
       (lambda (&rest call-args)
         ;; %CNM-INVOKE inlined (see there)
         (let ((args (if call-args call-args *current-method-args*))
               (rest (cdr chain)))
           (if (eq rest t)
               (apply (car chain) args)
               (let ((*cnm* (cons rest args)))
                 (apply (car chain) args)))))))))

(defun %make-around-chain (around-methods inner)
  "Build an :around chain that wraps INNER (the primary EMF).  INNER is the
last element of the chain list; it never reads *CNM* (a chain entry
rebinds it, a bare method function does not use it), so the chain ends
in T and the last :around's CALL-NEXT-METHOD reaches it without a binding."
  (if (null around-methods)
      inner
      (let ((chain (cons inner t)))
        (dolist (m (reverse around-methods))
          (setq chain (cons (method-function m) chain)))
        (lambda (&rest call-args)
          ;; %CNM-INVOKE inlined (see there); the chain has at least two
          ;; elements here, so the first :around is never called bare.
          (let* ((args (if call-args call-args *current-method-args*))
                 (*cnm* (cons (cdr chain) args)))
            (apply (car chain) args))))))

;;; --- GF dispatch cache ---

(defun %compute-gf-cacheable-p (gf)
  "Return dispatch mode for GF:
   N (integer) = number of specialized arg positions (class cache)
   :EQL = has EQL specializers (EQL cache)
   Always returns at least 1 for class-only dispatch."
  (let ((t-class (find-class 't))
        (has-eql nil)
        (max-specialized 1))
    (dolist (m (gf-methods gf))
      (let ((pos 0))
        (dolist (s (method-specializers m))
          (cond
            ((eql-specializer-p s)
             (setq has-eql t))
            ((not (eq s t-class))
             (when (> (1+ pos) max-specialized)
               (setq max-specialized (1+ pos)))))
          (setq pos (1+ pos)))))
    (if has-eql :eql max-specialized)))

(defun %invalidate-all-gf-caches ()
  "Clear dispatch caches of all generic functions."
  (maphash (lambda (name gf)
             (declare (ignore name))
             (%set-gf-dispatch-cache gf nil)
             (%set-gf-inline-cache gf nil))
           *generic-function-table*))

(defun %compute-eql-value-sets (gf)
  "Compute per-position EQL value sets for GF.
   Returns a list: NIL for positions without EQL specializers,
   or a hash table mapping known EQL values to T."
  (let ((max-pos 0))
    ;; Find max specializer position
    (dolist (m (gf-methods gf))
      (let ((len (length (method-specializers m))))
        (when (> len max-pos) (setq max-pos len))))
    ;; Build per-position hash tables
    (let ((result nil))
      (dotimes (pos max-pos)
        (let ((ht nil))
          (dolist (m (gf-methods gf))
            (let ((specs (method-specializers m)))
              (when (> (length specs) pos)
                (let ((s (nth pos specs)))
                  (when (eql-specializer-p s)
                    (unless ht
                      (setq ht (make-hash-table :test 'eql)))
                    (setf (gethash (eql-specializer-object s) ht) t))))))
          (push ht result)))
      (nreverse result))))

(defun %no-applicable-method (gf args)
  "Invoke the standard NO-APPLICABLE-METHOD generic function (CLHS 7.6.6.3)
   for a dispatch miss on GF with ARGS (a list).  Centralizes every
   no-applicable-method signal so user methods on NO-APPLICABLE-METHOD see
   every miss, and so the signaled condition type is uniform.  Falls back to
   a plain error if NO-APPLICABLE-METHOD is not yet defined (early boot,
   before its GF is established)."
  (if (fboundp 'no-applicable-method)
      (apply #'no-applicable-method gf args)
      (error "No applicable method for ~S with args of types ~S"
             (gf-name gf) (mapcar #'type-of args))))

(defun %make-dispatch-cache (test)
  "Create a hash table for a generic-function dispatch cache.

   Dispatch caches are the one class of hash table in the system that is read
   AND written from multiple threads: the main thread loading a system while
   worker / watcher threads (sento, log4cl) dispatch the same generic
   functions.  A plain (lock-free) table can have its bucket chains corrupted
   when two threads splice or rehash it at once, which crashes rarely under
   contention.  A synchronized table serializes those slow-path cache updates
   internally (see CL_HT_FLAG_SYNC in builtins_hashtable.c).

   This guards only the slow (cache-miss) path -- the monomorphic inline-cache
   fast path in the discriminating function never touches this table, so the
   hot dispatch path pays nothing.  TEST is 'EQ or 'EQL."
  (amilisp::%make-sync-hash-table test))

(defun %dispatch-negative-hit (gf args table key)
  "A cache lookup returned a *negative* entry (present, value NIL) meaning
   \"no method applies\" for this class tuple.  The dispatch cache is only a
   memo of a pure function (the applicable-method set for a tuple of argument
   classes), so a cached negative that contradicts the live method list is by
   definition stale -- it can arise from a GC relocation touching the EQ-keyed
   cache, a concurrent cache write from another thread (sento spins up worker
   threads that dispatch GFs while the main thread loads systems), or a missed
   invalidation.  Rather than trust the memo, recompute from the authoritative
   method list: if methods now apply, heal the cache entry and dispatch;
   otherwise the miss is genuine and we signal NO-APPLICABLE-METHOD.  This
   never turns a correct miss into a spurious call -- a genuine no-method
   recomputes empty and still signals -- it only rescues a stale/corrupt
   negative that would otherwise wrongly report \"No applicable method\" for a
   method that plainly exists."
  (let ((methods (%compute-applicable-methods gf args)))
    (if methods
        (let ((emf (%dispatch-build-emf gf methods)))
          (setq *gf-cache-heals* (+ *gf-cache-heals* 1))
          (setf (gethash key table) emf)
          (apply emf args))
        (%no-applicable-method gf args))))

(defun %gf-dispatch-eql (gf args)
  "Dispatch a GF with EQL specializers using mixed EQL/class cache.
   Cache structure at each level: (eql-ht . class-ht) cons.
   EQL-HT keys are EQL values, CLASS-HT keys are class objects.
   Values at intermediate levels are (eql-ht . class-ht) conses.
   Values at final level are EMF closures (or NIL for negative cache)."
  (let* ((eql-sets (gf-eql-value-sets gf))
         (cache (gf-dispatch-cache gf)))
    (unless cache
      ;; Initialize: (eql-ht . class-ht) cons
      (setq cache (cons (%make-dispatch-cache 'eql)
                        (%make-dispatch-cache 'eq)))
      (%set-gf-dispatch-cache gf cache))
    ;; Navigate/create nested levels for each position with EQL/class specializers
    (let ((node cache)
          (a args)
          (sets eql-sets)
          (depth 0)
          (n-levels (length eql-sets)))
      ;; Navigate intermediate levels (0..n-levels-2)
      (loop
        (when (>= depth (1- n-levels))
          (return))
        (let* ((eql-set (car sets))
               (arg (car a))
               (use-eql (and eql-set (gethash arg eql-set)))
               (ht (if use-eql (car node) (cdr node)))
               (key (if use-eql arg (class-of arg)))
               (next (gethash key ht)))
          (unless next
            (setq next (cons (%make-dispatch-cache 'eql)
                             (%make-dispatch-cache 'eq)))
            (setf (gethash key ht) next))
          (setq node next)
          (setq a (cdr a))
          (setq sets (cdr sets))
          (setq depth (1+ depth))))
      ;; Final level: lookup EMF
      (let* ((eql-set (car sets))
             (arg (car a))
             (use-eql (and eql-set (gethash arg eql-set)))
             (ht (if use-eql (car node) (cdr node)))
             (key (if use-eql arg (class-of arg)))
             (*current-method-args* args))
        (multiple-value-bind (emf found)
            (gethash key ht)
          (if found
              (if emf
                  (apply emf args)
                  (%dispatch-negative-hit gf args ht key))
              (let ((methods (%compute-applicable-methods gf args)))
                (if methods
                    (let ((new-emf (%dispatch-build-emf gf methods)))
                      (setf (gethash key ht) new-emf)
                      (apply new-emf args))
                    (let ((healed (%dispatch-heal-empty gf args)))
                      (if healed
                          (progn (setf (gethash key ht) healed)
                                 (apply healed args))
                          (progn
                            (setf (gethash key ht) nil)
                            (%no-applicable-method gf args))))))))))))

(defun %gf-dispatch-cached (gf args n-specialized)
  "Look up or compute effective method using nested class cache.
   N-SPECIALIZED is the number of specialized arg positions."
  (let ((cache (gf-dispatch-cache gf)))
    (unless cache
      (setq cache (%make-dispatch-cache 'eq))
      (%set-gf-dispatch-cache gf cache))
    ;; Navigate/create nested hash tables for positions 0..n-specialized-2
    (let ((table cache)
          (a args)
          (depth 0))
      ;; Navigate intermediate levels
      (loop
        (when (>= depth (1- n-specialized))
          (return))
        (let* ((class (class-of (car a)))
               (next (gethash class table)))
          (unless next
            (setq next (%make-dispatch-cache 'eq))
            (setf (gethash class table) next))
          (setq table next)
          (setq a (cdr a))
          (setq depth (1+ depth))))
      ;; Final level: lookup EMF by class of last specialized arg
      (let* ((class (class-of (car a)))
             (*current-method-args* args))
        (multiple-value-bind (emf found)
            (gethash class table)
          (if found
              (if emf
                  (apply emf args)
                  (%dispatch-negative-hit gf args table class))
              (let ((methods (%compute-applicable-methods gf args)))
                (if methods
                    (let ((new-emf (%dispatch-build-emf gf methods)))
                      (setf (gethash class table) new-emf)
                      (apply new-emf args))
                    (let ((healed (%dispatch-heal-empty gf args)))
                      (if healed
                          (progn (setf (gethash class table) healed)
                                 (apply healed args))
                          (progn
                            (setf (gethash class table) nil)
                            (%no-applicable-method gf args))))))))))))

;;; --- GF dispatch ---

(defun %gf-dispatch (gf args)
  "Dispatch a generic function call."
  ;; gf-cacheable-p resolves the :DIRTY sentinel transparently, so we
  ;; always see a final mode here.
  (let ((mode (gf-cacheable-p gf)))
    (cond
      ((integerp mode)
       (%gf-dispatch-cached gf args mode))
      ((eq mode :eql)
       (%gf-dispatch-eql gf args))
      (t
       (let ((methods (%compute-applicable-methods gf args)))
         (unless methods
           (let* ((*current-method-args* args)
                  (healed (%dispatch-heal-empty gf args)))
             (return-from %gf-dispatch
               (if healed
                   (apply healed args)
                   (%no-applicable-method gf args)))))
         (let ((*current-method-args* args))
           (apply (%dispatch-build-emf gf methods) args)))))))

;;; --- Inline-cached, arity-specialized discriminators ---
;;;
;;; The default discriminating function captures the GF in its closure
;;; and consults an inline cache (a small cons cell in GF slot 8) before
;;; falling back to %GF-DISPATCH.  On a cache hit the dispatcher invokes
;;; the cached EMF directly (no &rest cons, no APPLY).  The arity
;;; templates below specialize to 1/2/3 required arguments — the common
;;; cases for accessors and small GFs; anything else uses a variadic
;;; fallback that matches the pre-IC behaviour.

(defun %gf-lambda-list-required-count (lambda-list)
  "Return the count of required (positional, non-lambda-list-keyword)
   parameters in LAMBDA-LIST, or NIL if any non-required keyword is
   present.  Used to pick a fixed-arity discriminator template."
  (let ((n 0))
    (dolist (p lambda-list n)
      (when (member p '(&optional &rest &key &body &aux &allow-other-keys
                        &whole &environment)
                    :test #'eq)
        (return-from %gf-lambda-list-required-count nil))
      (setq n (1+ n)))))

;;; An EMF is "direct" when it is the raw METHOD-FUNCTION of a single
;;; simple-primary method (no qualifiers, no CALL-NEXT-METHOD).  Direct
;;; EMFs do not consult *CURRENT-METHOD-ARGS*, so the IC fast path can
;;; call them without binding it — which is what makes the fast path
;;; allocation-free.  Non-direct EMFs (chain wrappers, around chains,
;;; custom combinations) stay in the hash cache and reach the slow path
;;; on every call, which establishes the binding before invoking them.
;;; The IC is populated only at fresh-EMF computation time, when we have
;;; the methods in scope; cache-hit paths intentionally leave the IC
;;; alone — for monomorphic call sites it was set on the first miss and
;;; stays valid until method changes nil it.
(defun %emf-direct-p (emf methods)
  (and (null (cdr methods))
       (let ((m (car methods)))
         (and (method-simple-primary-p m)
              (eq emf (method-function m))))))

;;; Slow paths for the IC discriminators.  These mirror %GF-DISPATCH but
;;; populate the inline cache on a class-mode hit so subsequent calls
;;; bypass the hash-table cache entirely.  EQL / fallback modes return
;;; through %GF-DISPATCH without populating the IC — the next call
;;; misses the IC again and re-enters here, which is fine for the rare
;;; EQL-dispatched paths.

(defun %gf-1-no-method-error (gf a)
  (%no-applicable-method gf (list a)))

(defun %gf-dispatch-1-slow (gf a)
  (let ((mode (gf-cacheable-p gf))
        (args (list a)))
    (cond
      ((integerp mode)
       (let ((cache (gf-dispatch-cache gf))
             (class (class-of a))
             (*current-method-args* args))
         (unless cache
           (setq cache (%make-dispatch-cache 'eq))
           (%set-gf-dispatch-cache gf cache))
         (multiple-value-bind (emf found) (gethash class cache)
           (cond
             ((and found emf) (funcall emf a))
             (found (%dispatch-negative-hit gf args cache class))
             (t
              (let ((methods (%compute-applicable-methods gf args)))
                (cond
                  (methods
                   (let ((new-emf (%dispatch-build-emf gf methods)))
                     (setf (gethash class cache) new-emf)
                     (when (%emf-direct-p new-emf methods)
                       (%set-gf-inline-cache gf (cons class new-emf)))
                     (funcall new-emf a)))
                  (t
                   (let ((healed (%dispatch-heal-empty gf args)))
                     (if healed
                         (progn (setf (gethash class cache) healed)
                                (funcall healed a))
                         (progn
                           (setf (gethash class cache) nil)
                           (%gf-1-no-method-error gf a))))))))))))
      (t (%gf-dispatch gf args)))))

(defun %gf-2-no-method-error (gf a b)
  (%no-applicable-method gf (list a b)))

(defun %gf-2-resolve (gf a b args table key c1 c2)
  "Look up EMF for the 2-arg call in TABLE keyed by KEY; compute on
   miss.  *CURRENT-METHOD-ARGS* is bound to ARGS by the caller so any
   chain-wrapper / CALL-METHOD form in the resulting EMF can read it."
  (multiple-value-bind (emf found) (gethash key table)
    (cond
      ((and found emf) (funcall emf a b))
      (found (%dispatch-negative-hit gf args table key))
      (t
       (let ((methods (%compute-applicable-methods gf args)))
         (cond
           (methods
            (let ((new-emf (%dispatch-build-emf gf methods)))
              (setf (gethash key table) new-emf)
              (when (%emf-direct-p new-emf methods)
                (%set-gf-inline-cache gf (cons c1 (cons c2 new-emf))))
              (funcall new-emf a b)))
           (t
            (let ((healed (%dispatch-heal-empty gf args)))
              (if healed
                  (progn (setf (gethash key table) healed)
                         (funcall healed a b))
                  (progn
                    (setf (gethash key table) nil)
                    (%gf-2-no-method-error gf a b)))))))))))

(defun %gf-dispatch-2-slow (gf a b)
  (let ((mode (gf-cacheable-p gf))
        (args (list a b)))
    (cond
      ((integerp mode)
       (let ((cache (gf-dispatch-cache gf))
             (c1 (class-of a))
             (c2 (class-of b))
             (*current-method-args* args))
         (unless cache
           (setq cache (%make-dispatch-cache 'eq))
           (%set-gf-dispatch-cache gf cache))
         (cond
           ((>= mode 2)
            ;; Two specialized positions: navigate nested hash, matching
            ;; %GF-DISPATCH-CACHED's layout so the IC and hash cache
            ;; agree on key shape.
            (let ((inner (gethash c1 cache)))
              (unless inner
                (setq inner (%make-dispatch-cache 'eq))
                (setf (gethash c1 cache) inner))
              (%gf-2-resolve gf a b args inner c2 c1 c2)))
           (t
            ;; Single specialized position: cache keyed on c1 only.
            (%gf-2-resolve gf a b args cache c1 c1 c2)))))
      (t (%gf-dispatch gf args)))))


(defun %build-discriminating-function (gf lambda-list)
  "Return the discriminating function for GF -- an arity-specialized
   closure when the GF takes 1, 2, or 3 required arguments and no
   non-required parameters, otherwise the variadic fallback."
  (let ((nreq (%gf-lambda-list-required-count lambda-list)))
    ;; The hit path is a single %GF-IC-EMF builtin call: it reads the
    ;; inline cache, computes the receivers' classes, and compares — in
    ;; C (spec 3.1).  NIL means "no verified cache hit"; every miss goes
    ;; through the unchanged Lisp slow path, which also populates the
    ;; cache.
    (case nreq
      ((1)
       (named-lambda %gf-dispatch-1 (a)
         (let ((emf (%gf-ic-emf gf a)))
           (if emf
               (funcall emf a)
               (%gf-dispatch-1-slow gf a)))))
      ((2)
       (named-lambda %gf-dispatch-2 (a b)
         (let ((emf (%gf-ic-emf gf a b)))
           (if emf
               (funcall emf a b)
               (%gf-dispatch-2-slow gf a b)))))
      (otherwise
       (named-lambda %gf-dispatch-entry (&rest args)
         (%gf-dispatch gf args))))))

;;; --- Reader-GF fast dispatch ---
;;;
;;; A generic function whose entire method set is DEFCLASS-generated
;;; reader methods for one slot means exactly "read slot N of the
;;; receiver".  Such a GF gets a discriminating function whose hit path
;;; is a single non-allocating builtin (%GF-READER-IC): compare the
;;; receiver's type-desc against the inline cache and read the cached
;;; slot index.  That skips both the effective-method funcall and the
;;; per-access CLASS-OF + slot-index resolution %STRUCT-SLOT-VALUE pays.
;;;
;;; The cache is polymorphic: a list of up to 4 (TYPE-NAME . SLOT-INDEX)
;;; entries, most-recently-missed first.  One entry per receiver class
;;; means a call site alternating between a few classes (including
;;; subclasses, which have their own type-desc) hits for all of them —
;;; with a single entry every alternation was a full slow dispatch plus
;;; %COMPUTE-APPLICABLE-METHODS, measured ~80x the hit cost per access.
;;;
;;; Correctness gates, all of them load-bearing:
;;;   - every method is a generated reader for the SAME slot, unqualified,
;;;     under standard method combination, on a 1-required-argument GF;
;;;   - *SLOT-ACCESS-PROTOCOL-EXTENDED-P* is NIL.  A user
;;;     SLOT-VALUE-USING-CLASS method must be honoured, so instead of
;;;     testing that flag on every call we demote every reader GF at the
;;;     moment it flips (%DEMOTE-ALL-READER-GFS) and refuse to promote
;;;     afterwards.  This is the same gate %ACCESSOR-READER-BODY uses.
;;;   - the IC is filled only after the slow path confirms an applicable
;;;     method exists for this receiver's class (a user NO-APPLICABLE-METHOD
;;;     method may return normally, so returning normally is not proof);
;;;   - the fill happens AFTER %GF-DISPATCH-1-SLOW runs, because that
;;;     function also writes slot 8 (the EMF cache) and would otherwise
;;;     clobber the reader IC on every miss, thrashing us back to slow.
;;;
;;; The reader IC shares GF slot 8 with the EMF IC, so it inherits every
;;; existing invalidation site (add/remove-method, class redefinition).
;;; The shapes stay unambiguous: EMF caches put a class object in the
;;; car ((class . emf) / (class1 class2 . emf)); the reader IC's first
;;; element is a (TYPE-NAME . SLOT-INDEX) cons.

(defvar *reader-method-slots* (amilisp::%make-sync-hash-table 'eq)
  "Method object -> slot name, for DEFCLASS-generated reader methods.
   Mutated (gethash/setf/remhash) from %INSTALL-METHOD-IN-GF /
   %UNINSTALL-METHOD-FROM-GF / %NOTE-READER-METHOD, which run whenever
   DEFMETHOD/ADD-METHOD/REMOVE-METHOD fire on ANY thread -- the same
   multi-thread hazard %MAKE-DISPATCH-CACHE guards against above, so this
   table needs the same CL_HT_FLAG_SYNC treatment rather than a plain
   (lock-free) table.")
(defvar *reader-gfs* (amilisp::%make-sync-hash-table 'eq)
  "GF -> slot name, for GFs currently running the reader discriminator.
   Same concurrent-mutation hazard as *READER-METHOD-SLOTS* above.")

(defun %gf-reader-slot-name (gf)
  "Slot name when every method of GF is a generated reader for that one
   slot and GF is a plain 1-argument standard-combination GF; else NIL."
  (let ((methods (gf-methods gf))
        (slot nil))
    (and methods
         (not *slot-access-protocol-extended-p*)
         (eql 1 (%gf-lambda-list-required-count (gf-lambda-list gf)))
         (let ((combo (gf-method-combination gf)))
           (or (null combo)
               (eq (method-combination-name combo) 'standard)))
         (dolist (m methods t)
           (let ((s (gethash m *reader-method-slots*)))
             (when (or (null s) (method-qualifiers m))
               (return nil))
             (if slot
                 (unless (eq s slot) (return nil))
                 (setq slot s))))
         slot)))

(defun %reader-ic-entries (ic)
  "IC when it is reader-shaped -- a list of (TYPE-NAME . SLOT-INDEX)
   entries -- else NIL.  The EMF caches sharing slot 8 put a class object
   (never a cons) in their car, so one CONSP look disambiguates."
  (if (and (consp ic) (consp (car ic))) ic nil))

(defun %gf-reader-1-miss (gf a slot-name)
  "Reader IC miss (or unbound slot).  Take the ordinary slow path, then
   push (TYPE-NAME . SLOT-INDEX) for A's class onto the reader IC,
   keeping up to 3 other classes' entries (the C probe walks at most 4;
   pushing evicts the oldest).  The old entries are snapshotted BEFORE
   the slow path, whose own EMF-cache write to slot 8 would otherwise
   throw them away on every miss.  Each kept entry's index is
   re-validated against the current tables, so an entry made stale by a
   class redefinition racing this dispatch cannot be resurrected.
   Entry conses are never mutated and each install builds a fresh
   spine, so a probe mid-walk on another thread only ever sees a
   consistent list."
  (let ((old (%reader-ic-entries (gf-inline-cache gf))))
    (multiple-value-prog1 (%gf-dispatch-1-slow gf a)
      (when (and (not *slot-access-protocol-extended-p*)
                 (structurep a)
                 (%compute-applicable-methods gf (list a)))
        (let* ((type-name (%struct-type-name a))
               (idx (%struct-slot-index type-name slot-name)))
          (when idx
            (let ((kept nil) (n 0))
              (dolist (e old)
                (when (and (< n 3) (consp e))
                  (let ((ty (car e)))
                    (when (and (not (eq ty type-name))
                               (eql (cdr e) (%struct-slot-index ty slot-name)))
                      (push e kept)
                      (setq n (+ n 1))))))
              (%set-gf-inline-cache
               gf (cons (cons type-name idx) (nreverse kept))))))))))

(defun %make-reader-discriminator (gf slot-name)
  (let ((miss *slot-unbound-marker*))
    (named-lambda %gf-dispatch-1-reader (a)
      (let ((v (%gf-reader-ic gf a miss)))
        (if (eq v miss)
            (%gf-reader-1-miss gf a slot-name)
            v)))))

(defun %demote-reader-gf (gf)
  (remhash gf *reader-gfs*)
  (%set-gf-inline-cache gf nil)
  (%set-gf-discriminating-function gf
    (%build-discriminating-function gf (gf-lambda-list gf))))

(defun %refresh-reader-discriminator (gf)
  "Install or retract GF's reader fast-path discriminating function."
  (let ((slot (%gf-reader-slot-name gf)))
    (cond (slot
           (setf (gethash gf *reader-gfs*) slot)
           (%set-gf-inline-cache gf nil)
           (%set-gf-discriminating-function gf
             (%make-reader-discriminator gf slot)))
          ((gethash gf *reader-gfs*)
           (%demote-reader-gf gf)))))

(defun %demote-all-reader-gfs ()
  "Called when *SLOT-ACCESS-PROTOCOL-EXTENDED-P* flips: no reader GF may
   bypass SLOT-VALUE once the slot-access protocol has user methods."
  (let ((gfs nil))
    (maphash (lambda (gf slot) (declare (ignore slot)) (push gf gfs))
             *reader-gfs*)
    (dolist (gf gfs) (%demote-reader-gf gf))))

(defun %note-reader-method (method slot-name)
  "Tag METHOD as a DEFCLASS-generated reader for SLOT-NAME and reconsider
   its GF for the reader fast path.  Returns METHOD."
  (setf (gethash method *reader-method-slots*) slot-name)
  (let ((gf (method-generic-function method)))
    (when gf (%refresh-reader-discriminator gf)))
  method)

;;; --- Writer-GF fast dispatch ---
;;;
;;; The mirror image for (setf (x obj) v): a GF whose entire method set
;;; is DEFCLASS-generated writer methods for one slot means exactly
;;; "store the first argument into slot N of the second" ((setf x) takes
;;; VAL OBJ, new-value first, per CLHS 5.1.1.2).  Promotion, demotion,
;;; invalidation, and thread-safety rules are identical to the reader
;;; machinery above — see that block's comment for the reasoning.
;;;
;;; The writer IC shares GF slot 8 and reuses the (TYPE-NAME . FIXNUM)
;;; entry shape, but encodes the slot index as (- -1 index) — always
;;; negative — so the reader and writer C probes can never answer from
;;; each other's caches (a 1-arg call to a promoted writer GF must reach
;;; the slow path and signal its arity error, not read the slot).

(defvar *writer-method-slots* (amilisp::%make-sync-hash-table 'eq)
  "Method object -> slot name, for DEFCLASS-generated writer methods.
   Same concurrent-mutation hazard as *READER-METHOD-SLOTS*.")
(defvar *writer-gfs* (amilisp::%make-sync-hash-table 'eq)
  "GF -> slot name, for GFs currently running the writer discriminator.
   Same concurrent-mutation hazard as *READER-METHOD-SLOTS*.")

(defun %gf-writer-slot-name (gf)
  "Slot name when every method of GF is a generated writer for that one
   slot and GF is a plain 2-argument standard-combination GF; else NIL."
  (let ((methods (gf-methods gf))
        (slot nil))
    (and methods
         (not *slot-access-protocol-extended-p*)
         (eql 2 (%gf-lambda-list-required-count (gf-lambda-list gf)))
         (let ((combo (gf-method-combination gf)))
           (or (null combo)
               (eq (method-combination-name combo) 'standard)))
         (dolist (m methods t)
           (let ((s (gethash m *writer-method-slots*)))
             (when (or (null s) (method-qualifiers m))
               (return nil))
             (if slot
                 (unless (eq s slot) (return nil))
                 (setq slot s))))
         slot)))

(defun %gf-writer-1-miss (gf val obj slot-name)
  "Writer IC miss.  Take the ordinary slow path (which performs the
   store through the full protocol), then push (TYPE-NAME . (- -1 IDX))
   for OBJ's class onto the writer IC.  Snapshot/re-validate/fresh-spine
   discipline is %GF-READER-1-MISS's -- see its docstring.
   %READER-IC-ENTRIES' shape check covers writer entries too (both are
   lists of conses; only the cdr's sign differs)."
  (let ((old (%reader-ic-entries (gf-inline-cache gf))))
    (multiple-value-prog1 (%gf-dispatch-2-slow gf val obj)
      (when (and (not *slot-access-protocol-extended-p*)
                 (structurep obj)
                 (%compute-applicable-methods gf (list val obj)))
        (let* ((type-name (%struct-type-name obj))
               (idx (%struct-slot-index type-name slot-name)))
          (when idx
            (let ((kept nil) (n 0))
              (dolist (e old)
                (when (and (< n 3) (consp e))
                  (let* ((ty (car e))
                         (i (and (not (eq ty type-name))
                                 (%struct-slot-index ty slot-name))))
                    (when (and i (eql (cdr e) (- -1 i)))
                      (push e kept)
                      (setq n (+ n 1))))))
              (%set-gf-inline-cache
               gf (cons (cons type-name (- -1 idx)) (nreverse kept))))))))))

(defun %make-writer-discriminator (gf slot-name)
  (let ((miss *slot-unbound-marker*))
    (named-lambda %gf-dispatch-2-writer (val obj)
      (let ((v (%gf-writer-ic gf val obj miss)))
        (if (eq v miss)
            (%gf-writer-1-miss gf val obj slot-name)
            v)))))

(defun %demote-writer-gf (gf)
  (remhash gf *writer-gfs*)
  (%set-gf-inline-cache gf nil)
  (%set-gf-discriminating-function gf
    (%build-discriminating-function gf (gf-lambda-list gf))))

(defun %refresh-writer-discriminator (gf)
  "Install or retract GF's writer fast-path discriminating function."
  (let ((slot (%gf-writer-slot-name gf)))
    (cond (slot
           (setf (gethash gf *writer-gfs*) slot)
           (%set-gf-inline-cache gf nil)
           (%set-gf-discriminating-function gf
             (%make-writer-discriminator gf slot)))
          ((gethash gf *writer-gfs*)
           (%demote-writer-gf gf)))))

(defun %demote-all-writer-gfs ()
  "Called when *SLOT-ACCESS-PROTOCOL-EXTENDED-P* flips: no writer GF may
   bypass (SETF SLOT-VALUE) once the slot-access protocol has user methods."
  (let ((gfs nil))
    (maphash (lambda (gf slot) (declare (ignore slot)) (push gf gfs))
             *writer-gfs*)
    (dolist (gf gfs) (%demote-writer-gf gf))))

(defun %note-writer-method (method slot-name)
  "Tag METHOD as a DEFCLASS-generated writer for SLOT-NAME and reconsider
   its GF for the writer fast path.  Returns METHOD."
  (setf (gethash method *writer-method-slots*) slot-name)
  (let ((gf (method-generic-function method)))
    (when gf (%refresh-writer-discriminator gf)))
  method)

;;; --- ensure-generic-function ---

(defun ensure-generic-function (name &key lambda-list
                                          (method-combination-name 'standard
                                                                   method-combination-name-p)
                                          method-combination-options
                                          (generic-function-class
                                            'standard-generic-function))
  "Find or create a generic function named NAME.
Installs the GF metaobject itself in the symbol-function cell; the VM
transparently unwraps funcallable instances to their discriminating
function, so (FOO ...) and (FUNCALL #'FOO ...) both dispatch through
the GF's slot 3. SET-FUNCALLABLE-INSTANCE-FUNCTION can retarget that
slot without touching the symbol-function cell.
METHOD-COMBINATION-NAME + METHOD-COMBINATION-OPTIONS select the
combination used to build the effective method; when omitted on an
already-existing GF the installed combination is preserved."
  (multiple-value-bind (existing found-p)
      (gethash name *generic-function-table*)
    (if found-p
        (progn
          ;; Re-install the GF object in the function cell, in case a
          ;; (defun ...) reload overwrote it.
          (cond
            ((symbolp name)
             (setf (symbol-function name) existing))
            ((and (consp name) (eq (car name) 'setf) (consp (cdr name)))
             (let* ((accessor (cadr name))
                    (hidden-sym (amilisp::%setf-store-symbol accessor)))
               (setf (symbol-function hidden-sym) existing))))
          ;; Update method combination if explicitly supplied (or if we
          ;; can now resolve STANDARD — important for GFs created before
          ;; the combination registry was populated at boot).
          (when method-combination-name-p
            (let ((new-combo (%resolve-method-combination
                               method-combination-name method-combination-options)))
              (unless (eq new-combo (gf-method-combination existing))
                (%set-gf-method-combination existing new-combo)
                (%set-gf-dispatch-cache existing nil)
                (%set-gf-inline-cache existing nil)
                ;; A promoted reader/writer GF bypasses the effective-method
                ;; machinery entirely; a non-standard combination must
                ;; retract that (the refresh re-checks the combination).
                (when (gethash existing *reader-gfs*)
                  (%refresh-reader-discriminator existing))
                (when (gethash existing *writer-gfs*)
                  (%refresh-writer-discriminator existing))
                (%notify-dependents existing 'reinitialize-instance
                                    :method-combination new-combo))))
          (when (null (gf-method-combination existing))
            (let ((combo (%resolve-method-combination 'standard nil)))
              (when combo (%set-gf-method-combination existing combo))))
          existing)
        (let* ((combo (%resolve-method-combination
                        method-combination-name method-combination-options))
               ;; CLHS :generic-function-class — a STANDARD-GENERIC-FUNCTION
               ;; subclass.  When supplied the GF struct is tagged with that
               ;; class (so TYPE-OF / dispatch see it) and the class is
               ;; declared funcallable so the VM still dispatches through its
               ;; discriminating-function slot.  Custom-class GFs are created
               ;; via the make-instance protocol (initialize-instance below)
               ;; so user (initialize-instance :after) methods fire.
               (gf-class-name (if (symbolp generic-function-class)
                                  generic-function-class
                                  (class-name generic-function-class)))
               (custom-class-p (not (eq gf-class-name 'standard-generic-function)))
               (gf (progn
                     (when custom-class-p
                       (%register-funcallable-gf-type gf-class-name))
                     (%make-struct gf-class-name
                       name lambda-list nil nil combo nil nil nil nil)))
               (dispatch-fn (%build-discriminating-function gf lambda-list)))
          (%set-gf-discriminating-function gf dispatch-fn)
          (setf (gethash name *generic-function-table*) gf)
          (cond
            ((symbolp name)
             (setf (symbol-function name) gf))
            ;; (setf accessor) — install GF on hidden symbol
            ((and (consp name) (eq (car name) 'setf) (consp (cdr name)))
             (let* ((accessor (cadr name))
                    (hidden-sym (amilisp::%setf-store-symbol accessor)))
               (setf (symbol-function hidden-sym) gf)
               (%register-setf-function accessor hidden-sym))))
          ;; Fire the make-instance initialization protocol for custom GF
          ;; classes.  The 9 GF slots are already populated (non-unbound), so
          ;; the default SHARED-INITIALIZE leaves them intact and only user
          ;; INITIALIZE-INSTANCE :AFTER methods (e.g. snooze route
          ;; registration) run.  STANDARD-GENERIC-FUNCTION skips this to keep
          ;; the boot/common path unchanged.
          (when custom-class-p
            (initialize-instance gf))
          gf))))

(defun %resolve-method-combination (name options)
  "Look up NAME in *METHOD-COMBINATIONS*; return NIL if either the name
   is unknown or the registry has not been initialised yet (this happens
   during bootstrap before method-combination support has been loaded).
   OPTIONS attaches a fresh copy of the combination prototype so each
   GF can carry its own options without sharing mutable state."
  (cond
    ((not (boundp '*method-combinations*)) nil)
    (t
     (let ((proto (gethash (%method-combination-key name) *method-combinations*)))
       (cond
         ((null proto)
          (error "Unknown method combination ~S" name))
         ((null options) proto)
         (t (%clone-method-combination proto options)))))))

;;; --- defgeneric macro ---

(defmacro defgeneric (name lambda-list &rest options)
  "Define a generic function."
  (let ((method-defs nil)
        (combo-name 'standard)
        (combo-options nil)
        (documentation nil)
        (gf-class 'standard-generic-function))
    (dolist (opt options)
      (cond
        ;; CLHS :generic-function-class — name of a STANDARD-GENERIC-FUNCTION
        ;; subclass to instantiate (e.g. snooze's resource-generic-function).
        ((and (consp opt) (eq (car opt) :generic-function-class))
         (setq gf-class (cadr opt)))
        ;; (:documentation "...") -> the FUNCTION doc-type on the name.
        ((and (consp opt) (eq (car opt) :documentation))
         (setq documentation (cadr opt)))
        ;; Inline method definition
        ((and (consp opt) (eq (car opt) :method))
         ;; (:method [qualifiers...] specialized-lambda-list &body body)
         (let ((rest (cdr opt))
               (qualifiers nil))
           ;; CLHS: qualifiers are non-list atoms.
           (loop
             (if (and rest (not (listp (car rest))))
                 (progn (push (car rest) qualifiers)
                        (setq rest (cdr rest)))
                 (return)))
           (setq qualifiers (nreverse qualifiers))
           (let ((spec-ll (car rest))
                 (body (cdr rest)))
             (if qualifiers
                 (push `(defmethod ,name ,@qualifiers ,spec-ll ,@body) method-defs)
                 (push `(defmethod ,name ,spec-ll ,@body) method-defs)))))
        ;; Method combination selection
        ((and (consp opt) (eq (car opt) :method-combination))
         (setq combo-name (cadr opt))
         (setq combo-options (cddr opt)))))
    (setq method-defs (nreverse method-defs))
    (let ((egf-form
            `(ensure-generic-function ',name
               :lambda-list ',lambda-list
               :method-combination-name ',combo-name
               :method-combination-options ',combo-options
               :generic-function-class ',gf-class))
          (doc-forms
            (when (stringp documentation)
              `((%set-documentation ',name 'function ,documentation)))))
      (if (or method-defs doc-forms)
          `(progn ,egf-form ,@doc-forms ,@method-defs)
          egf-form))))

;;; --- defmethod helpers ---

(defun %parse-specialized-lambda-list (spec-ll)
  "Parse a specialized lambda-list into (unspec-ll . specializer-names).
   E.g. ((x point) y) -> ((x y) . (point t))"
  (let ((unspec nil)
        (specs nil)
        (in-required t))
    (dolist (param spec-ll)
      (cond
        ;; Lambda-list keyword — stop specializing.  &aux must be here too:
        ;; an &aux binding like (var (accessor obj)) is NOT a specialized
        ;; parameter, so its init-form must never be treated as a specializer
        ;; name (otherwise FIND-CLASS is handed the init form, e.g. cl-routes'
        ;; `(defmethod unify/impl ((tmpl wildcard-template) x bindings
        ;;    &aux (var (template-data tmpl))) ...)`).
        ((member param '(&rest &optional &key &body &allow-other-keys &aux) :test #'eq)
         (setq in-required nil)
         (push param unspec))
        ;; Specialized parameter: (var class-name) or (var (eql val))
        ((and in-required (consp param))
         (push (car param) unspec)
         (push (cadr param) specs))
        ;; Unspecialized required parameter
        (in-required
         (push param unspec)
         (push 't specs))
        ;; Non-required parameter
        (t (push param unspec))))
    (cons (nreverse unspec) (nreverse specs))))

(defun %resolve-specializers (specializer-names)
  "Resolve specializer names to specializer metaobjects.
   Symbols -> (find-class sym), (eql val) -> interned EQL-SPECIALIZER
   for the evaluated value."
  (mapcar (lambda (s)
            (if (and (consp s) (eq (car s) 'eql))
                (intern-eql-specializer (eval (cadr s)))
                (find-class s)))
          specializer-names))

(defun extract-specializer-names (specialized-lambda-list)
  "AMOP: given a specialized lambda list, return the list of specializer
   names -- class-name symbols or (EQL value) forms, padded with T for
   unspecialized required parameters.  Non-required parameters are
   skipped."
  (let ((names nil))
    (dolist (p specialized-lambda-list)
      (cond
        ((member p '(&optional &rest &key &body &allow-other-keys &aux) :test #'eq)
         (return))
        ((consp p)
         (push (cadr p) names))
        (t (push 't names))))
    (nreverse names)))

(defun extract-lambda-list (specialized-lambda-list)
  "AMOP: given a specialized lambda list, return the corresponding plain
   lambda list -- specialized required parameters are replaced by their
   variable names; non-required parameters are preserved verbatim."
  (let ((unspec nil)
        (in-required t))
    (dolist (p specialized-lambda-list)
      (cond
        ((member p '(&optional &rest &key &body &allow-other-keys &aux) :test #'eq)
         (setq in-required nil)
         (push p unspec))
        ((and in-required (consp p))
         (push (car p) unspec))
        (t (push p unspec))))
    (nreverse unspec)))

;;; --- defmethod macro ---

;;; Conservative detector for "simple primary" methods: methods that
;;; have no qualifiers and whose body source contains no reference to
;;; CALL-NEXT-METHOD or NEXT-METHOD-P (any symbol whose name matches,
;;; package-blind).  When true, dispatch can skip the method-chain
;;; wrapper and call the method-function directly — saving three
;;; dynamic-binding push/pops per call.  Pessimistic for macros that
;;; expand to CNM after install, but those are rare.
(defun %tree-contains-symbol-named-p (tree name)
  (cond
    ((null tree) nil)
    ((symbolp tree) (string= (symbol-name tree) name))
    ((atom tree) nil)
    (t (or (%tree-contains-symbol-named-p (car tree) name)
           (%tree-contains-symbol-named-p (cdr tree) name)))))

(defun %body-simple-primary-p (body)
  (not (or (%tree-contains-symbol-named-p body "CALL-NEXT-METHOD")
           (%tree-contains-symbol-named-p body "NEXT-METHOD-P"))))

(defmacro defmethod (name &rest args)
  "Define a method on generic function NAME."
  ;; CLHS: method qualifiers are non-list atoms (symbols or numbers)
  ;; appearing before the specialized lambda list.  This permits `+`,
  ;; `and`, `or`, etc. as qualifiers alongside keyword qualifiers such
  ;; as :BEFORE / :AFTER / :AROUND.
  (let ((qualifiers nil)
        (rest args))
    (loop
      (if (and rest (not (listp (car rest))))
          (progn
            (push (car rest) qualifiers)
            (setq rest (cdr rest)))
          (return)))
    (setq qualifiers (nreverse qualifiers))
    (let* ((spec-ll (car rest))
           (body (cdr rest))
           (parsed (%parse-specialized-lambda-list spec-ll))
           (unspec-ll (car parsed))
           (spec-names (cdr parsed))
           ;; Block name must be a symbol; (setf foo) -> foo
           (block-name (if (and (consp name) (eq (car name) 'setf))
                           (cadr name)
                           name)))
      ;; Per CL spec, keyword checking for GFs is based on the union of
      ;; all applicable methods' keywords. Add &allow-other-keys so the
      ;; VM doesn't reject keywords accepted by other methods.
      (let ((effective-ll (if (and (member '&key unspec-ll)
                                   (not (member '&allow-other-keys unspec-ll)))
                              (append unspec-ll '(&allow-other-keys))
                              unspec-ll))
            (simple-p (and (null qualifiers) (%body-simple-primary-p body))))
        ;; NAMED-LAMBDA, not LAMBDA: the method function carries BLOCK-NAME so
        ;; a backtrace through a method shows the generic function's name
        ;; instead of "<anonymous>".  BLOCK-NAME is already the symbol form of
        ;; NAME ((setf foo) -> foo), which is what every consumer of a function
        ;; name expects.  Qualifiers and specializers are deliberately not
        ;; encoded: they would need a fresh interned symbol per method.
        `(%add-method-to-gf
           ',name ',qualifiers ',spec-names
           (named-lambda ,block-name ,effective-ll (block ,block-name ,@body))
           ',unspec-ll
           ,simple-p)))))

(defun %slot-access-protocol-gf-p (gf-name)
  "True when GF-NAME names one of the four slot-access protocol GFs."
  (or (eq gf-name 'slot-value-using-class)
      (eq gf-name 'slot-boundp-using-class)
      (eq gf-name 'slot-makunbound-using-class)
      (and (consp gf-name)
           (eq (car gf-name) 'setf)
           (eq (cadr gf-name) 'slot-value-using-class))))

(defvar *gf-methods-lock* (mp:make-lock "gf-methods-lock")
  "Serializes the read-modify-write of GF-METHODS across concurrent
   %INSTALL-METHOD-IN-GF / %UNINSTALL-METHOD-FROM-GF calls (possibly on
   different GFs).  The single %SET-GF-METHODS store is atomic at the
   reader's granularity -- a dispatcher never sees a torn list -- but two
   writers that both read the old list before either stores race to a
   lost update: whichever store lands second silently discards the
   other's method/removal.  A coarse global lock (rather than a lock
   per GF) keeps the fix simple and correct; GF (re)definition is not a
   hot path, so serializing it across all GFs has no measurable cost.")

(defun %install-method-in-gf (gf method)
  "Low-level install: put METHOD into GF's method list, replacing any
   method with matching qualifiers and specializers.  Invalidates the
   dispatch cache and recomputes cacheability.  Returns METHOD.
   Primitive; does not dispatch through the ADD-METHOD GF so it is safe
   during bootstrap and from DEFMETHOD expansion.  Broadcasts
   UPDATE-DEPENDENT so observers see every method change, whether the
   caller went through ADD-METHOD or the primitive path."
  (let ((qualifiers (method-qualifiers method))
        (specializers (method-specializers method))
        (gf-name (gf-name gf)))
    (%set-method-generic-function method gf)
    ;; Update the method list with a SINGLE atomic store, under
    ;; *GF-METHODS-LOCK* so concurrent installers/uninstallers don't race
    ;; each other's read-modify-write (see the lock's docstring).  Building
    ;; the full new list (new method consed onto the old list minus any it
    ;; replaces) and storing it once means a concurrent dispatcher on
    ;; another thread always observes a *complete* method list — either the
    ;; old set or the new set, both of which contain every applicable
    ;; method. The previous two-step form (store the filtered list, THEN
    ;; cons the method on) left a window where GF-METHODS transiently
    ;; EXCLUDED the method being (re)defined; a dispatcher that walked the
    ;; list in that window computed an empty applicable set and wrongly
    ;; signalled "No applicable method" for a method that plainly exists.
    ;; That is the multi-threaded race behind the ASDF field reports
    ;; (log4cl's watcher / worker threads dispatch GFs while the main
    ;; thread (re)defines methods during asdf:load-system).  %struct-set is
    ;; a single word store, so the swap is atomic at the reader's
    ;; granularity.
    (mp:with-lock-held (*gf-methods-lock*)
      ;; Drop the reader tag of any method this one supersedes, so the tag
      ;; table cannot grow without bound across DEFCLASS reloads.
      (dolist (m (gf-methods gf))
        (when (and (equal (method-qualifiers m) qualifiers)
                   (equal (method-specializers m) specializers))
          (remhash m *reader-method-slots*)
          (remhash m *writer-method-slots*)))
      (%set-gf-methods gf
        (cons method
              (remove-if (lambda (m)
                           (and (equal (method-qualifiers m) qualifiers)
                                (equal (method-specializers m) specializers)))
                         (gf-methods gf)))))
    (%set-gf-dispatch-cache gf nil)
    (%set-gf-inline-cache gf nil)
    ;; Defer cacheable-p / eql-value-sets recompute to first dispatch.
    ;; During CLOS bootstrap we add ~50 methods with no calls in between,
    ;; so eager recompute on each install was wasted work — now a single
    ;; lazy recompute happens on the first call that needs the mode.
    (%set-gf-cacheable-p gf :dirty)
    (%set-gf-eql-value-sets gf nil)
    (when (and (%slot-access-protocol-gf-p gf-name)
               (> (length (gf-methods gf)) 1))
      (setq *slot-access-protocol-extended-p* t)
      ;; SLOT-VALUE / (SETF SLOT-VALUE) now have user methods behind them;
      ;; no reader or writer GF may keep bypassing them.
      (%demote-all-reader-gfs)
      (%demote-all-writer-gfs))
    ;; A method added to a reader/writer GF (an :around, or a primary that
    ;; is not a generated accessor method) retracts the fast path.
    (when (gethash gf *reader-gfs*)
      (%refresh-reader-discriminator gf))
    (when (gethash gf *writer-gfs*)
      (%refresh-writer-discriminator gf))
    (%notify-dependents gf 'add-method method)
    method))

(defun %uninstall-method-from-gf (gf method)
  "Low-level remove: drop METHOD (by EQ identity) from GF and refresh
   dispatch state.  Returns GF.  Broadcasts UPDATE-DEPENDENT so
   observers see removals regardless of whether the caller used the
   REMOVE-METHOD GF or this primitive."
  (mp:with-lock-held (*gf-methods-lock*)
    (%set-gf-methods gf
      (remove method (gf-methods gf) :test #'eq)))
  (%set-gf-dispatch-cache gf nil)
  (%set-gf-inline-cache gf nil)
  ;; Lazy: see %install-method-in-gf.  Mark dirty; %gf-dispatch recomputes
  ;; cacheable-p / eql-value-sets on first call that needs them.
  (%set-gf-cacheable-p gf :dirty)
  (%set-gf-eql-value-sets gf nil)
  ;; Clear the back-link so the method object no longer claims membership.
  (%set-method-generic-function method nil)
  (remhash method *reader-method-slots*)
  (remhash method *writer-method-slots*)
  (when (gethash gf *reader-gfs*)
    (%refresh-reader-discriminator gf))
  (when (gethash gf *writer-gfs*)
    (%refresh-writer-discriminator gf))
  (%notify-dependents gf 'remove-method method)
  gf)

(defun %add-method-to-gf (gf-name qualifiers specializer-names fn lambda-list
                          &optional simple-primary-p)
  "Bridge used by DEFMETHOD expansion -- construct the method struct and
   install it via the primitive install helper (bypasses the ADD-METHOD
   GF dispatch that is itself built on this path during bootstrap).
   SIMPLE-PRIMARY-P is set by the DEFMETHOD macro when the method body
   uses neither CALL-NEXT-METHOD nor NEXT-METHOD-P; the dispatch fast
   paths use it to skip the method-chain wrapper."
  (let* ((gf (ensure-generic-function gf-name))
         (specializers (%resolve-specializers specializer-names))
         (method (%make-struct 'standard-method
                   gf specializers qualifiers fn lambda-list
                   simple-primary-p)))
    (%install-method-in-gf gf method)))

(%clos-trace "generic functions")
;;; ====================================================================
;;; Phase 6: with-slots
;;; ====================================================================

(defmacro with-slots (slot-entries instance-form &body body)
  "Evaluate BODY with slot names bound via symbol-macrolet.
   SLOT-ENTRIES is ((var slot) ...) or (slot-name ...).
   Each slot-name expands to (slot-value instance 'slot-name)."
  (let ((instance-var (gensym "INSTANCE"))
        (bindings nil))
    (dolist (entry slot-entries)
      (if (consp entry)
          ;; (variable-name slot-name)
          (push (list (car entry)
                      `(slot-value ,instance-var ',(cadr entry)))
                bindings)
          ;; plain slot-name — variable and slot are the same
          (push (list entry
                      `(slot-value ,instance-var ',entry))
                bindings)))
    `(let ((,instance-var ,instance-form))
       (symbol-macrolet ,(nreverse bindings)
         ,@body))))

(defmacro with-accessors (slot-entries instance-form &body body)
  "Evaluate BODY with accessor-based bindings via symbol-macrolet.
   SLOT-ENTRIES is ((var accessor) ...).
   Each var expands to (accessor instance)."
  (let ((instance-var (gensym "INSTANCE"))
        (bindings nil))
    (dolist (entry slot-entries)
      (let ((var (car entry))
            (accessor (cadr entry)))
        (push (list var `(,accessor ,instance-var))
              bindings)))
    `(let ((,instance-var ,instance-form))
       (symbol-macrolet ,(nreverse bindings)
         ,@body))))

;;; ====================================================================
;;; Phase 7: Convert to Generic Functions
;;; ====================================================================

;;; --- Convert initialize-instance and shared-initialize to GFs ---
;;; Save the original plain functions before defgeneric overwrites them.
;;; defun stores in the value cell; #' reads via OP_FLOAD (function cell
;;; first, then value cell). Capture via #' before defgeneric sets the
;;; function cell.

(let ((%si-fn #'shared-initialize))
  (defgeneric shared-initialize (instance slot-names &rest initargs))
  (defmethod shared-initialize ((instance t) slot-names &rest initargs)
    (apply %si-fn instance slot-names initargs)))

(defgeneric initialize-instance (instance &rest initargs))
(defmethod initialize-instance ((instance t) &rest initargs)
  (apply #'shared-initialize instance t initargs))

;;; --- Class metaobject initialization (metaclass support) ---
;;; When a class is created with a user metaclass (:metaclass option),
;;; %ENSURE-CLASS-VIA-METACLASS allocates the class metaobject as an
;;; instance of that metaclass and calls INITIALIZE-INSTANCE on it.  This
;;; primary method (specialized on STANDARD-CLASS, so it applies to every
;;; class metaobject) populates the fixed 12-slot class layout from the
;;; initargs, then any metaclass-specific slots (e.g. TOPMOST-CLASS) via
;;; the slot/initarg machinery, then finalizes and registers the class.
;;;
;;; Metaclass :AROUND methods (e.g. serapeum's TOPMOST-OBJECT-CLASS, which
;;; injects a superclass into :DIRECT-SUPERCLASSES) wrap this primary
;;; method and observe/modify the initargs via CALL-NEXT-METHOD.
;;;
;;; Normal classes (metaclass = STANDARD-CLASS) never reach here: they are
;;; built directly by %ENSURE-CLASS without going through INITIALIZE-INSTANCE.
(defun %initialize-class-metaobject (class initargs name reinit-p)
  "Body shared by the INITIALIZE-INSTANCE and REINITIALIZE-INSTANCE
   methods on STANDARD-CLASS: fill the fixed 12-slot class layout from
   INITARGS, populate metaclass-specific slots, then finalize and
   register the class.  On reinitialization (REINIT-P, class
   redefinition) the metaobject is the SAME struct as before, so its
   direct-subclasses and direct-methods survive and get re-finalized /
   keep dispatching; and -- as in AMOP's class reinitialization protocol --
   :DIRECT-SUPERCLASSES, :DIRECT-SLOTS and :DIRECT-DEFAULT-INITARGS that
   are NOT supplied keep their current values instead of resetting."
  (let ((not-supplied (list nil)))
    (flet ((initarg (key current)
             (let ((v (getf initargs key not-supplied)))
               (if (eq v not-supplied)
                   (if reinit-p current nil)
                   v))))
      (let* ((direct-superclasses-supplied-p
              (not (eq (getf initargs :direct-superclasses not-supplied) not-supplied)))
             (super-spec (initarg :direct-superclasses
                                  (class-direct-superclasses class)))
             (supers (cond
                       (direct-superclasses-supplied-p
                        (if super-spec
                            (mapcar #'find-class super-spec)
                            (list (find-class 'standard-object))))
                       (reinit-p
                        ;; Not supplied on reinit: SUPER-SPEC came from the
                        ;; CURRENT branch of INITARG, i.e. CLASS-DIRECT-
                        ;; SUPERCLASSES — already-resolved class objects, not
                        ;; name specifiers.  Routing them back through
                        ;; FIND-CLASS only round-trips a class-object argument
                        ;; for a narrow metaclass whitelist and errors for any
                        ;; user-defined metaclass instance (AMOP
                        ;; REINITIALIZE-INSTANCE requires keeping the current
                        ;; superclasses here).
                        super-spec)
                       (t
                        (list (find-class 'standard-object))))))
        (when reinit-p
          (%unfinalize-class class))
        (%struct-set class 0 name)                          ; 0: name
        (%struct-set class 1 supers)                        ; 1: direct-superclasses
        (%struct-set class 2 (initarg :direct-slots
                                      (class-direct-slots class))) ; 2
        (%set-class-direct-default-initargs class
          (initarg :direct-default-initargs
                   (class-direct-default-initargs class)))))) ; 11
  (let ((supers (class-direct-superclasses class)))
    ;; Populate metaclass-specific slots (indices 12+) from the initargs,
    ;; matching each slot's :INITARG against the supplied keys.  The
    ;; metaclass's effective slots carry the locations assigned (at 12+)
    ;; by %ASSIGN-SLOT-LOCATIONS.  A slot that is neither supplied nor
    ;; still unbound keeps its value — on reinitialization that is the
    ;; value from the previous definition (CLHS REINITIALIZE-INSTANCE:
    ;; initforms are not re-evaluated for bound slots).
    (let ((index-table (class-slot-index-table (class-of class))))
      (when index-table
        (maphash
         (lambda (slot-name esd)
           (declare (ignore slot-name))
           (when (eq (slot-definition-allocation esd) :instance)
             (multiple-value-bind (val supplied)
                 (%find-initarg-value initargs (slot-definition-initargs esd))
               (if supplied
                   (%struct-set class (slot-definition-location esd) val)
                   (let ((initfn (slot-definition-initfunction esd)))
                     (when (and initfn
                                (eq (%struct-ref class (slot-definition-location esd))
                                    *slot-unbound-marker*))
                       (%struct-set class (slot-definition-location esd)
                                    (funcall initfn))))))))
         index-table)))
    (%finalize-and-register-class class name supers
                                  (mapcar #'class-name supers))
    class))

(defmethod initialize-instance ((class standard-class) &rest initargs
                                &key name &allow-other-keys)
  (%initialize-class-metaobject class initargs name nil))

;;; --- no-applicable-method (CLHS 7.6.6.3) ---
;;; Called by the dispatch machinery (via %NO-APPLICABLE-METHOD) when a
;;; generic function is invoked and no method is applicable.  Defined as a
;;; generic function so users can specialize it on a generic-function to
;;; customize the behavior; the default method signals an error of type
;;; ERROR (a SIMPLE-ERROR), as the standard requires.
(defgeneric no-applicable-method (generic-function &rest function-arguments))
(defmethod no-applicable-method ((generic-function t) &rest function-arguments)
  (error "No applicable method for ~S with args of types ~S"
         (if (typep generic-function 'standard-generic-function)
             (gf-name generic-function)
             generic-function)
         (mapcar #'type-of function-arguments)))

;;; --- slot-unbound as a generic function ---
;;; Upgrade the plain function to a GF so that classes can specialize
;;; it (e.g. for lazy slot initialization).
(let ((%su-fn #'slot-unbound))
  (defgeneric slot-unbound (class instance slot-name))
  (defmethod slot-unbound ((class t) instance slot-name)
    (funcall %su-fn class instance slot-name)))

;;; --- allocate-instance as a generic function (AMOP §5.3.4) ---
;;; Libraries (serapeum/mop.lisp) define methods on allocate-instance
;;; specialized on metaclasses (e.g. abstract-standard-class).  Without
;;; a default (class t) method, calling (allocate-instance standard-class-obj)
;;; after such a specialization errors with "No applicable method".
(let ((%ai-fn #'allocate-instance))
  (defgeneric allocate-instance (class &rest initargs))
  (defmethod allocate-instance ((class t) &rest initargs)
    (declare (ignore initargs))
    (funcall %ai-fn class)))

(%clos-trace "with-slots + GF conversion")
;;; ====================================================================
;;; Slot-access protocol (MOP)
;;; ====================================================================
;;;
;;; AMOP §5.6 exposes raw slot access as a set of generic functions
;;; dispatching on (class instance effective-slot-definition). Libraries
;;; hook these for lazy slots, change tracking, persistence, memoization,
;;; and so on.
;;;
;;; The default methods encode the existing %STRUCT-REF / %STRUCT-SET
;;; behavior. SLOT-VALUE etc. normally bypass this dispatch (see
;;; *SLOT-ACCESS-PROTOCOL-EXTENDED-P*); the first user-installed method
;;; flips the flag and the slot-access wrappers start routing through
;;; the GFs. Struct slot access (non-CLOS) continues to bypass the
;;; protocol entirely — it has no effective-slot-definition to pass.

(defgeneric slot-value-using-class (class instance slot))
(defmethod slot-value-using-class ((class t) instance slot)
  (let* ((location (slot-definition-location slot))
         (val (if (consp location)
                  (cdr location)
                  (%struct-ref instance location))))
    (if (eq val *slot-unbound-marker*)
        (slot-unbound class instance (slot-definition-name slot))
        val)))

(defgeneric (setf slot-value-using-class) (new-value class instance slot))
(defmethod (setf slot-value-using-class) (new-value (class t) instance slot)
  (declare (ignore class))
  (let ((location (slot-definition-location slot)))
    (if (consp location)
        (rplacd location new-value)
        (%struct-set instance location new-value)))
  new-value)

(defgeneric slot-boundp-using-class (class instance slot))
(defmethod slot-boundp-using-class ((class t) instance slot)
  (declare (ignore class))
  (let ((location (slot-definition-location slot)))
    (not (eq (if (consp location)
                 (cdr location)
                 (%struct-ref instance location))
             *slot-unbound-marker*))))

(defgeneric slot-makunbound-using-class (class instance slot))
(defmethod slot-makunbound-using-class ((class t) instance slot)
  (declare (ignore class))
  (let ((location (slot-definition-location slot)))
    (if (consp location)
        (rplacd location *slot-unbound-marker*)
        (%struct-set instance location *slot-unbound-marker*)))
  instance)

;;; --- Upgrade slot-definition-class protocol to GFs ---
;;; So that libraries can customize the slot-definition class. User
;;; metaclasses are out of scope, so specialization is via :around on
;;; the standard-class specializer.
(defgeneric direct-slot-definition-class (class &rest initargs))
(defmethod direct-slot-definition-class ((class t) &rest initargs)
  (declare (ignore initargs))
  (find-class 'standard-direct-slot-definition))

(defgeneric effective-slot-definition-class (class &rest initargs))
(defmethod effective-slot-definition-class ((class t) &rest initargs)
  (declare (ignore initargs))
  (find-class 'standard-effective-slot-definition))

;;; --- class-slots is the AMOP name for the effective-slot accessor ---
(defun class-slots (class)
  "AMOP: return the list of effective slot definitions of CLASS."
  (class-effective-slots class))

;;; ====================================================================
;;; Class finalization protocol (MOP)
;;; ====================================================================
;;;
;;; AMOP exposes class finalization as a set of GFs so that user code
;;; (typically closer-mop callers — serapeum, trivia, …) can intercept
;;; CPL computation, effective-slot merging, and effective default-initargs.
;;;
;;; All defaults delegate to the internal helpers already used during
;;; %ENSURE-CLASS, so the happy path is unchanged. Specialization is on
;;; the class argument; since user-defined metaclasses are out of scope
;;; (see specs/mop.md), practical customization is via :around methods.

(defgeneric validate-superclass (class superclass))
(defmethod validate-superclass ((class t) (super t))
  "Accept all metaclass pairs -- single-metaclass world."
  t)

(defgeneric compute-class-precedence-list (class))
(defmethod compute-class-precedence-list ((class t))
  (%compute-class-precedence-list class))

(defgeneric compute-effective-slot-definition (class name direct-slots))
(defmethod compute-effective-slot-definition ((class t) name direct-slots)
  (%compute-effective-slot-definition-default class name direct-slots))

(defgeneric compute-slots (class))
(defmethod compute-slots ((class t))
  ;; Re-implement the default here (rather than call %compute-slots-default)
  ;; so that the GF dispatches to COMPUTE-EFFECTIVE-SLOT-DEFINITION and user
  ;; methods on that GF are visible.
  (let* ((cpl (class-precedence-list class))
         (order nil)
         (groups (make-hash-table :test 'eq)))
    (dolist (c cpl)
      (dolist (dsd (class-direct-slots c))
        (let ((name (slot-definition-name dsd)))
          (unless (gethash name groups)
            (push name order))
          (setf (gethash name groups)
                (append (gethash name groups) (list dsd))))))
    (setq order (nreverse order))
    (mapcar (lambda (name)
              (compute-effective-slot-definition class name (gethash name groups)))
            order)))

(defgeneric compute-default-initargs (class))
(defmethod compute-default-initargs ((class t))
  (%compute-default-initargs-default class))

(defgeneric finalize-inheritance (class))
(defmethod finalize-inheritance ((class t))
  "Compute CPL, effective slots, and default initargs via the MOP GFs.
   Idempotent: already-finalized classes return immediately (AMOP section 3.4.2)."
  (if (class-finalized-p class)
      class
      (progn
        (%set-class-cpl class (compute-class-precedence-list class))
        (let ((effective (compute-slots class)))
          (%set-class-effective-slots class effective)
          (%set-class-slot-index-table class (%build-slot-index-table effective))
          (%assign-slot-locations class effective))
        (%set-class-default-initargs class (compute-default-initargs class))
        (%set-class-finalized-p class t)
        class)))

(defgeneric class-prototype (class))
(defmethod class-prototype ((class t))
  "Return a (lazily allocated) prototype instance of CLASS. For classes
   that cannot be instantiated (built-ins without a slot-index-table),
   return NIL rather than signal -- callers treat it as advisory."
  (or (%struct-ref class 8)
      (when (class-slot-index-table class)
        (let ((proto (allocate-instance class)))
          (%struct-set class 8 proto)
          proto))))

;;; --- ensure-class / ensure-class-using-class ---
;;; AMOP-style entry point. DEFCLASS expands into a call to ENSURE-CLASS
;;; so that :around methods on ENSURE-CLASS-USING-CLASS can observe the
;;; keyword args.

(defgeneric ensure-class-using-class (class name &rest keys))
(defmethod ensure-class-using-class ((class t) name &rest keys)
  (let* ((direct-supers (getf keys :direct-superclasses))
         (direct-slots  (getf keys :direct-slots))
         (direct-inits  (getf keys :direct-default-initargs))
         (metaclass     (getf keys :metaclass))
         (new-class (%ensure-class name direct-supers direct-slots direct-inits
                                   metaclass)))
    ;; Redefinition: %ENSURE-CLASS normally updates the existing
    ;; metaobject in place, so the dependents registered on it stay
    ;; attached.  Only when the metaobject could NOT be reused (metaclass
    ;; change, BUILT-IN-CLASS / FORWARD-REFERENCED-CLASS stub) is a fresh
    ;; struct allocated — migrate the dependents to it so a later
    ;; redefinition still notifies.  Then notify them with the initargs
    ;; that drove the redefinition.
    (when class
      (unless (eq class new-class)
        (let ((old-deps (gethash class *metaobject-dependents*)))
          (when old-deps
            (setf (gethash new-class *metaobject-dependents*) old-deps)
            (remhash class *metaobject-dependents*))))
      (apply #'%notify-dependents new-class 'reinitialize-instance keys))
    new-class))

(defun ensure-class (name &rest keys)
  "AMOP: look up the existing class (or NIL) and dispatch to
   ENSURE-CLASS-USING-CLASS."
  (apply #'ensure-class-using-class (find-class name nil) name keys))

;;; --- Update defclass to generate GF-based accessors ---
;;; Redefine the defclass macro to use defmethod instead of defun
;;; for :accessor, :reader, :writer

(defmacro defclass (name direct-superclasses slot-specifiers &rest class-options)
  "Define a new CLOS class (with generic function accessors)."
  (let ((accessor-defs nil)
        (slot-def-forms nil)
        (default-initarg-forms nil)
        (metaclass-name nil)
        (documentation nil))
    ;; Parse class options
    (dolist (opt class-options)
      (when (consp opt)
        (cond
          ((eq (car opt) :default-initargs)
           ;; (:default-initargs :key1 val1 :key2 val2 ...)
           (let ((args (cdr opt)))
             (loop while args
                   do (let ((key (pop args))
                            (val (pop args)))
                        (push `(list ',key (lambda () ,val)) default-initarg-forms)))))
          ((eq (car opt) :metaclass)
           ;; (:metaclass NAME)
           (setq metaclass-name (cadr opt)))
          ((eq (car opt) :documentation)
           ;; (:documentation "...") -> the TYPE doc-type on the class name.
           (setq documentation (cadr opt))))))
    ;; Parse each slot specifier
    (dolist (spec slot-specifiers)
      (let* ((parsed (%parse-slot-spec spec))
             (slot-name (%slot-spec-name parsed))
             (accessors (%slot-spec-all-options parsed :accessor))
             (readers (%slot-spec-all-options parsed :reader))
             (writers (%slot-spec-all-options parsed :writer)))
        (push (%slot-spec->direct-def-form spec) slot-def-forms)
        ;; Generate GF-based accessor methods.  Method bodies inline the
        ;; fused %STRUCT-SLOT-VALUE / %STRUCT-SLOT-STORE fast path (see
        ;; %ACCESSOR-READER-BODY) instead of calling SLOT-VALUE; any
        ;; miss falls back to SLOT-VALUE's full protocol.
        (dolist (accessor accessors)
          (push `(defgeneric ,accessor (obj)) accessor-defs)
          (push `(%note-reader-method
                   (defmethod ,accessor ((obj ,name))
                     ,(%accessor-reader-body 'obj slot-name))
                   ',slot-name)
                accessor-defs)
          ;; Writer: use defgeneric + defmethod for (setf accessor)
          ;; so additional methods can be added without replacing the original
          (push `(defgeneric (setf ,accessor) (val obj)) accessor-defs)
          (push `(%note-writer-method
                   (defmethod (setf ,accessor) (val (obj ,name))
                     ,(%accessor-writer-body 'obj slot-name 'val))
                   ',slot-name)
                accessor-defs))
        (dolist (reader readers)
          (push `(defgeneric ,reader (obj)) accessor-defs)
          (push `(%note-reader-method
                   (defmethod ,reader ((obj ,name))
                     ,(%accessor-reader-body 'obj slot-name))
                   ',slot-name)
                accessor-defs))
        (dolist (writer writers)
          (push `(defgeneric ,writer (val obj)) accessor-defs)
          (push `(%note-writer-method
                   (defmethod ,writer (val (obj ,name))
                     ,(%accessor-writer-body 'obj slot-name 'val))
                   ',slot-name)
                accessor-defs))))
    (setq slot-def-forms (nreverse slot-def-forms))
    (setq accessor-defs (nreverse accessor-defs))
    `(eval-when (:compile-toplevel :load-toplevel :execute)
       (ensure-class ',name
         :direct-superclasses ',direct-superclasses
         :direct-slots (list ,@slot-def-forms)
         :direct-default-initargs (list ,@(nreverse default-initarg-forms))
         ,@(when metaclass-name `(:metaclass ',metaclass-name)))
       ,@(when (stringp documentation)
           `((%set-documentation ',name 'type ,documentation)))
       ,@accessor-defs
       (find-class ',name))))

(%clos-trace "class finalization")
;;; ====================================================================
;;; Phase 8: change-class + reinitialize-instance
;;; ====================================================================

(defgeneric reinitialize-instance (instance &rest initargs))
(defmethod reinitialize-instance ((instance t) &rest initargs)
  "Re-apply initargs to an existing instance."
  (apply #'shared-initialize instance nil initargs)
  instance)

;;; AMOP: ENSURE-CLASS-USING-CLASS on an EXISTING class with a user
;;; metaclass reinitializes the class metaobject in place instead of
;;; allocating a new one (see %ENSURE-CLASS-VIA-METACLASS).  Metaclass
;;; :AROUND methods on REINITIALIZE-INSTANCE (e.g. serapeum's
;;; TOPMOST-OBJECT-CLASS re-injecting its superclass) wrap this primary
;;; method exactly as their INITIALIZE-INSTANCE counterparts do.
(defmethod reinitialize-instance ((class standard-class) &rest initargs
                                  &key name &allow-other-keys)
  (%initialize-class-metaobject class initargs
                                (or name (class-name class))
                                t))

(defgeneric change-class (instance new-class &rest initargs))
(defmethod change-class ((instance t) new-class &rest initargs)
  "Change INSTANCE to be an instance of NEW-CLASS.
   Modifies the instance in-place when the allocation has enough space,
   otherwise allocates a new struct."
  (let* ((new-class-obj (if (symbolp new-class)
                            (find-class new-class)
                            new-class))
         (old-class (class-of instance))
         (old-slots (class-effective-slots old-class))
         (new-slots (class-effective-slots new-class-obj))
         (new-type-name (class-name new-class-obj))
         (new-index-table (class-slot-index-table new-class-obj))
         (new-n-slots 0))
    (dolist (esd new-slots)
      (when (eq (slot-definition-allocation esd) :instance)
        (setq new-n-slots (+ new-n-slots 1))))
    ;; Capture surviving slot values as (new-esd . value) pairs.
    (let ((saved-values nil))
      (dolist (old-esd old-slots)
        (let* ((name (slot-definition-name old-esd))
               (old-loc (slot-definition-location old-esd))
               (old-val (if (consp old-loc)
                            (cdr old-loc)
                            (%struct-ref instance old-loc))))
          (unless (eq old-val *slot-unbound-marker*)
            (let ((new-esd (gethash name new-index-table)))
              (when new-esd
                (push (cons new-esd old-val) saved-values))))))
      (flet ((restore-to (target-instance)
               (dolist (pair saved-values)
                 (let ((loc (slot-definition-location (car pair))))
                   (if (consp loc)
                       (rplacd loc (cdr pair))
                       (%struct-set target-instance loc (cdr pair)))))))
        ;; Try in-place modification
        (if (%struct-change-class instance new-type-name new-n-slots)
            (progn
              (dotimes (i new-n-slots)
                (%struct-set instance i *slot-unbound-marker*))
              (restore-to instance)
              (apply #'shared-initialize instance t initargs)
              instance)
            (let ((new-instance (allocate-instance new-class-obj)))
              (restore-to new-instance)
              (apply #'shared-initialize new-instance t initargs)
              new-instance))))))

;;; ====================================================================
;;; Phase 9: print-object generic function
;;; ====================================================================

;;; print-object is a generic function. The *print-object-hook* in C
;;; calls it for struct objects. It must return a string (or NIL to
;;; fall through to default #S(...) printing).

(defgeneric print-object (object stream))

;;; Default method — return NIL to let the C printer handle it
(defmethod print-object ((object t) stream)
  nil)

;;; CLOS metaobject printing
(defmethod print-object ((object standard-class) stream)
  (concatenate 'string "#<STANDARD-CLASS "
               (symbol-name (class-name object)) ">"))

(defmethod print-object ((object standard-generic-function) stream)
  (concatenate 'string "#<STANDARD-GENERIC-FUNCTION "
               (symbol-name (gf-name object)) ">"))

(defmethod print-object ((object standard-direct-slot-definition) stream)
  (concatenate 'string "#<STANDARD-DIRECT-SLOT-DEFINITION "
               (symbol-name (slot-definition-name object)) ">"))

(defmethod print-object ((object standard-effective-slot-definition) stream)
  (concatenate 'string "#<STANDARD-EFFECTIVE-SLOT-DEFINITION "
               (symbol-name (slot-definition-name object)) ">"))

(defmethod print-object ((object standard-method) stream)
  (let* ((gf (method-function object))
         (specs (method-specializers object))
         (quals (method-qualifiers object))
         (spec-names (mapcar (lambda (s)
                               (cond
                                 ((eql-specializer-p s)
                                  (format nil "(EQL ~S)" (eql-specializer-object s)))
                                 ((and (structurep s)
                                       (eq (%struct-type-name s)
                                           'standard-class))
                                  (symbol-name (class-name s)))
                                 (t "?")))
                             specs)))
    (concatenate 'string "#<STANDARD-METHOD"
                 (if quals
                     (concatenate 'string " " (symbol-name (car quals)))
                     "")
                 " (" (format nil "~{~A~^ ~}" spec-names) ")>")))

;;; Wire up the C hook to call print-object.
;;; The hook must return a string (or NIL to fall through to default #S(...)).
;;; Uses with-output-to-string to capture stream-writing methods (CL protocol).
;;; Also checks the return value for methods that return a string directly.
(setq *print-object-hook*
  (lambda (obj)
    (let* ((result nil)
           (s (with-output-to-string (stream)
                (setq result (print-object obj stream)))))
      (cond
        ((and (stringp s) (> (length s) 0)) s)  ; stream-writing method
        ((stringp result) result)                 ; string-returning method
        (t nil)))))

;;; ====================================================================
;;; Funcallable instance protocol (MOP)
;;; ====================================================================
;;;
;;; AMOP §5.5 treats generic functions as callable metaobjects — the
;;; "funcallable instance" concept.  Our STANDARD-GENERIC-FUNCTION struct
;;; serves that role: its slot 3 (discriminating-function) is what the
;;; VM reaches when it sees a call whose operator is a GF metaobject
;;; (see cl_unwrap_funcallable in src/core/vm.c).

(defun set-funcallable-instance-function (gf fn)
  "AMOP: install FN as the discriminating function of GF.  Future calls
to GF invoke FN in place of the standard dispatch.  Returns GF."
  ;; The VM answers calls to a promoted reader/writer GF straight from the
  ;; inline cache, bypassing slot 3 entirely — so retract the promotion
  ;; before retargeting slot 3, or FN would simply never run.  Clearing the
  ;; cache unconditionally is a cheap belt-and-braces: it is only a cache.
  (when (gethash gf *reader-gfs*)
    (%demote-reader-gf gf))
  (when (gethash gf *writer-gfs*)
    (%demote-writer-gf gf))
  (%set-gf-inline-cache gf nil)
  (%set-gf-discriminating-function gf fn)
  gf)

(defun standard-instance-access (instance location)
  "AMOP: unchecked slot access by integer location.  No bound check,
no GF dispatch -- useful inside SLOT-VALUE-USING-CLASS methods that want
to sidestep the protocol they are implementing."
  (%struct-ref instance location))

(defun %set-standard-instance-access (instance location new-value)
  (%struct-set instance location new-value)
  new-value)

(defsetf standard-instance-access %set-standard-instance-access)

(defun funcallable-standard-instance-access (instance location)
  "AMOP: same as STANDARD-INSTANCE-ACCESS but typed for funcallable
metaobjects (e.g. generic functions)."
  (%struct-ref instance location))

(defsetf funcallable-standard-instance-access %set-standard-instance-access)

(defgeneric compute-discriminating-function (gf))
(defmethod compute-discriminating-function ((gf standard-generic-function))
  "AMOP: return the function that will be called when GF is invoked.
Default method returns the currently cached discriminating function;
user methods may specialise to return a customised dispatcher, which
can then be installed via SET-FUNCALLABLE-INSTANCE-FUNCTION."
  (gf-discriminating-function gf))

;;; ====================================================================
;;; Method metaobject protocol (MOP)
;;; ====================================================================
;;;
;;; AMOP §5.4 exposes the pieces of DEFMETHOD expansion as public hooks:
;;; ADD-METHOD / REMOVE-METHOD operate on already-constructed methods,
;;; EXTRACT-LAMBDA-LIST and EXTRACT-SPECIALIZER-NAMES pull a specialized
;;; lambda list apart, ENSURE-METHOD is the closer-mop convenience that
;;; ties them together, and MAKE-METHOD-LAMBDA is the hook a metaclass
;;; uses to rewrite the body of a method during code generation.
;;;
;;; DEFMETHOD itself does not dispatch through ADD-METHOD — it calls the
;;; same primitive (%INSTALL-METHOD-IN-GF) directly.  That keeps
;;; bootstrap simple (no chicken-and-egg when defining the GF), keeps
;;; DEFMETHOD fast, and matches what SBCL and CCL do.  User-installed
;;; (ADD-METHOD ...) still routes through the protocol so :around
;;; methods on ADD-METHOD take effect.

(defgeneric add-method (generic-function method))
(defmethod add-method ((gf standard-generic-function) (method standard-method))
  "AMOP: install METHOD in GF.  Returns GF.  The UPDATE-DEPENDENT
broadcast is fired by %INSTALL-METHOD-IN-GF (the primitive used by
both this GF and the bootstrap DEFMETHOD path), so observers see every
method change regardless of which path installed it."
  (%install-method-in-gf gf method)
  gf)

(defgeneric remove-method (generic-function method))
(defmethod remove-method ((gf standard-generic-function) (method standard-method))
  "AMOP: uninstall METHOD from GF.  Returns GF.  Uses EQ identity -- a
method that is not installed is silently ignored, matching AMOP.  The
UPDATE-DEPENDENT broadcast is fired by %UNINSTALL-METHOD-FROM-GF."
  (%uninstall-method-from-gf gf method)
  gf)

(defgeneric find-method (generic-function qualifiers specializers &optional errorp))
(defmethod find-method ((gf standard-generic-function) qualifiers specializers
                        &optional (errorp t))
  "AMOP: locate the method on GF with the given QUALIFIERS list and
SPECIALIZERS list.  SPECIALIZERS may contain class objects, class
names, or (EQL value) / EQL-SPECIALIZER metaobjects -- names are
resolved to metaobjects via %RESOLVE-SPECIALIZERS before comparison."
  (let* ((resolved (mapcar (lambda (s)
                             (cond
                               ((symbolp s) (find-class s))
                               ((and (consp s) (eq (car s) 'eql))
                                (intern-eql-specializer (cadr s)))
                               (t s)))
                           specializers))
         (match (find-if (lambda (m)
                           (and (equal (method-qualifiers m) qualifiers)
                                (equal (method-specializers m) resolved)))
                         (gf-methods gf))))
    (cond
      (match match)
      (errorp
       (error "No method on ~S with qualifiers ~S and specializers ~S"
              (gf-name gf) qualifiers specializers))
      (t nil))))

(defgeneric make-method-lambda (generic-function method lambda-expression environment))
(defmethod make-method-lambda ((gf standard-generic-function) (method standard-method)
                                lambda-expression environment)
  "AMOP: rewrite a method LAMBDA-EXPRESSION before it is compiled.
Default method returns LAMBDA-EXPRESSION and NIL -- DEFMETHOD builds
the lambda directly and does not consult this GF unless a user method
overrides it.  Returns two values: the possibly-transformed lambda
expression and a list of extra initargs for MAKE-METHOD-LAMBDA callers."
  (declare (ignore gf method environment))
  (values lambda-expression nil))

(defun %gf-or-name (gf-or-name)
  "Resolve a GF designator to a standard-generic-function metaobject --
accepts a GF struct or a function-name (symbol or (SETF name))."
  (cond
    ((and (structurep gf-or-name)
          (eq (%struct-type-name gf-or-name) 'standard-generic-function))
     gf-or-name)
    (t (ensure-generic-function gf-or-name))))

(defun ensure-method (gf-or-name lambda-expression
                      &key (qualifiers '())
                           (lambda-list nil lambda-list-p)
                           (specializers nil specializers-p)
                           (method-class (find-class 'standard-method)))
  "closer-mop: construct a method from LAMBDA-EXPRESSION and install it
on GF-OR-NAME.  LAMBDA-EXPRESSION is either a lambda form or a
specialized lambda method form -- if :lambda-list and :specializers are
not supplied, they are parsed from LAMBDA-EXPRESSION's first argument
list.  METHOD-CLASS is accepted for API completeness but is not used
since user-defined method classes are out of scope."
  (declare (ignore method-class))
  (let* ((gf (%gf-or-name gf-or-name))
         (ll (cond
               (lambda-list-p lambda-list)
               ((and (consp lambda-expression)
                     (eq (car lambda-expression) 'lambda))
                (cadr lambda-expression))
               (t (error "ENSURE-METHOD: :LAMBDA-LIST not supplied and ~S ~
                          is not a lambda expression" lambda-expression))))
         (specs (cond
                  (specializers-p specializers)
                  (t (make-list (length (extract-specializer-names ll))
                                :initial-element 't))))
         (resolved-specs (%resolve-specializers specs))
         (fn (cond
               ((functionp lambda-expression) lambda-expression)
               ((and (consp lambda-expression)
                     (eq (car lambda-expression) 'lambda))
                (eval lambda-expression))
               (t (error "ENSURE-METHOD: cannot coerce ~S to a method function"
                         lambda-expression))))
         ;; ENSURE-METHOD is the AMOP entry point — we have no source body
         ;; to walk for CALL-NEXT-METHOD detection, so assume non-simple.
         (method (%make-struct 'standard-method
                   gf resolved-specs qualifiers fn ll nil)))
    (add-method gf method)
    method))

(%clos-trace "change-class + print-object")
;;; ====================================================================
;;; Method combination protocol (MOP)
;;; ====================================================================
;;;
;;; CLHS 7.6.6 / AMOP §5.3: a method combination describes how the set
;;; of applicable methods is woven into an effective method.  Each GF
;;; carries one method-combination metaobject; the dispatcher consults
;;; it via %DISPATCH-BUILD-EMF to assemble the EMF.
;;;
;;; Three combination flavours are recognised:
;;;   :STANDARD — classic :around / :before / primary / :after wiring
;;;   :SHORT    — short-form combinations (e.g. +, AND, OR, LIST, PROGN)
;;;               built from a single operator and an optional
;;;               identity-with-one-argument flag
;;;   :LONG     — user-supplied combinations created via
;;;               DEFINE-METHOD-COMBINATION with explicit method groups
;;;               and a body returning a form that uses CALL-METHOD

;;; standard-method-combination struct layout:
;;;   0: name                         - symbol
;;;   1: options                      - plist / list supplied at selection
;;;   2: type                         - :STANDARD / :SHORT / :LONG
;;;   3: operator                     - short-form operator symbol (or NIL)
;;;   4: identity-with-one-argument   - short-form flag (or NIL)
;;;   5: builder                      - long-form body closure (or NIL)
(%register-struct-type 'standard-method-combination 6 nil
  '((name nil) (options nil) (type nil)
    (operator nil) (identity-with-one-argument nil) (builder nil)))

(%make-bootstrap-class 'standard-method-combination
  (list (find-class 'standard-object)))

;;; Keyed by SYMBOL-NAME so a combination registered in one package
;;; resolves from any other package (e.g. CL-USER refers to STANDARD
;;; even though the registry entry was interned in CL).  Synchronized:
;;; DEFINE-METHOD-COMBINATION registers from the loading thread while
;;; ENSURE-GENERIC-FUNCTION resolves from any other (see
;;; *EQL-SPECIALIZER-TABLE* for the plain-table corruption hazard).
(defvar *method-combinations* (amilisp::%make-sync-hash-table 'equal))

(defun %method-combination-key (name)
  (if (symbolp name) (symbol-name name) name))

(defun method-combination-name (combo) (%struct-ref combo 0))
(defun method-combination-options (combo) (%struct-ref combo 1))
(defun method-combination-type (combo) (%struct-ref combo 2))

(defun %clone-method-combination (proto options)
  "Return a shallow clone of PROTO carrying fresh OPTIONS.  Called from
%RESOLVE-METHOD-COMBINATION when a GF selects a combination with
non-default options so every GF has its own metaobject to inspect."
  (%make-struct 'standard-method-combination
    (%struct-ref proto 0)
    options
    (%struct-ref proto 2)
    (%struct-ref proto 3)
    (%struct-ref proto 4)
    (%struct-ref proto 5)))

(defun %register-standard-combination ()
  (setf (gethash (%method-combination-key 'standard) *method-combinations*)
        (%make-struct 'standard-method-combination
          'standard nil :standard nil nil nil)))

(defun %define-short-method-combination (name operator identity-with-one-argument
                                         documentation)
  "Register a short-form method combination named NAME.  OPERATOR is the
combining operator symbol (defaults to NAME).  IDENTITY-WITH-ONE-ARGUMENT
controls the single-primary-method optimisation.  DOCUMENTATION, when a
string, is stored under the METHOD-COMBINATION doc-type."
  (when (stringp documentation)
    (%set-documentation name 'method-combination documentation))
  (setf (gethash (%method-combination-key name) *method-combinations*)
        (%make-struct 'standard-method-combination
          name nil :short operator identity-with-one-argument nil))
  name)

(defun %define-long-method-combination (name builder)
  "Register a long-form method combination named NAME.  BUILDER is a
function of two arguments (generic-function, applicable-methods) that
returns a form to be evaluated in the dynamic scope of the dispatcher;
the form typically uses CALL-METHOD on methods drawn from the method
groups bound by DEFINE-METHOD-COMBINATION."
  (setf (gethash (%method-combination-key name) *method-combinations*)
        (%make-struct 'standard-method-combination
          name nil :long nil nil builder))
  name)

;;; Built-in short-form combinations (CLHS 7.6.6.4).
(%register-standard-combination)
(%define-short-method-combination '+        '+     t   nil)
(%define-short-method-combination 'and      'and   t   nil)
(%define-short-method-combination 'or       'or    t   nil)
(%define-short-method-combination 'list     'list  nil nil)
(%define-short-method-combination 'progn    'progn t   nil)
(%define-short-method-combination 'nconc    'nconc t   nil)
(%define-short-method-combination 'append   'append t  nil)
(%define-short-method-combination 'min      'min   t   nil)
(%define-short-method-combination 'max      'max   t   nil)

;;; Retroactively install the standard combination on GFs created before
;;; the table was populated (bootstrap GFs such as SHARED-INITIALIZE).
(let ((std (gethash (%method-combination-key 'standard) *method-combinations*)))
  (maphash (lambda (name gf)
             (declare (ignore name))
             (when (null (gf-method-combination gf))
               (%set-gf-method-combination gf std)))
           *generic-function-table*))

;;; --- find-method-combination ---
(defgeneric find-method-combination (generic-function name options))
(defmethod find-method-combination (gf name options)
  "AMOP: return the method-combination metaobject named NAME, attached
with OPTIONS.  Default method consults the global combination registry
and ignores GF, so it accepts NIL as well as a generic-function object --
the closer-mop calling convention."
  (declare (ignore gf))
  (%resolve-method-combination name options))

;;; --- Short-form EMF builder ---

(defun %short-combine (operator identity-one primary args)
  "Combine PRIMARY method results with OPERATOR.  When IDENTITY-ONE is
true and there is exactly one primary method the method's value is
returned directly (no wrapping call), matching the
:IDENTITY-WITH-ONE-ARGUMENT short-form contract."
  (if (and identity-one (null (cdr primary)))
      (apply (method-function (car primary)) args)
      (case operator
        ((and)
         (let ((last t))
           (dolist (m primary last)
             (setq last (apply (method-function m) args))
             (unless last (return nil)))))
        ((or)
         (let ((result nil))
           (dolist (m primary result)
             (let ((v (apply (method-function m) args)))
               (when v (return v))))))
        ((progn)
         (let ((last nil))
           (dolist (m primary last)
             (setq last (apply (method-function m) args)))))
        ((list)
         (let ((result nil))
           (dolist (m primary)
             (push (apply (method-function m) args) result))
           (nreverse result)))
        ((append)
         (let ((result nil))
           (dolist (m primary result)
             (setq result (append result (apply (method-function m) args))))))
        ((nconc)
         (let ((result nil))
           (dolist (m primary result)
             (setq result (nconc result (apply (method-function m) args))))))
        ((+)
         (let ((sum 0))
           (dolist (m primary sum)
             (setq sum (+ sum (apply (method-function m) args))))))
        ((*)
         (let ((prod 1))
           (dolist (m primary prod)
             (setq prod (* prod (apply (method-function m) args))))))
        ((max)
         (let ((result nil))
           (dolist (m primary result)
             (let ((v (apply (method-function m) args)))
               (setq result (if result (max result v) v))))))
        ((min)
         (let ((result nil))
           (dolist (m primary result)
             (let ((v (apply (method-function m) args)))
               (setq result (if result (min result v) v))))))
        (t
         (apply operator
                (mapcar (lambda (m) (apply (method-function m) args)) primary))))))

(defun %build-short-effective-method (combination methods)
  "Assemble an EMF for a short-form COMBINATION over applicable METHODS.
METHODS are already sorted most-specific-first.  Partitioning:
  (:AROUND)          -> :around chain
  (COMBINATION-NAME) -> primary methods
  NIL                -> primary methods (accepted for convenience)
Other qualifier sets are rejected per CLHS 7.6.6.4.
The :MOST-SPECIFIC-LAST option (supplied via DEFGENERIC's
:METHOD-COMBINATION form) reverses primary argument order."
  (let ((operator (%struct-ref combination 3))
        (identity-one (%struct-ref combination 4))
        (options (%struct-ref combination 1))
        (combo-name (method-combination-name combination))
        (around nil)
        (primary nil))
    (dolist (m methods)
      (let ((q (method-qualifiers m)))
        (cond
          ((equal q '(:around)) (push m around))
          ((null q) (push m primary))
          ((and (consp q) (null (cdr q)) (eq (car q) combo-name))
           (push m primary))
          (t (error "Method ~S has invalid qualifiers ~S for combination ~S"
                    m q combo-name)))))
    (setq around (nreverse around))
    (setq primary (nreverse primary))
    (when (member :most-specific-last options)
      (setq primary (reverse primary)))
    (unless primary
      (error "No applicable primary method for combination ~S"
             (method-combination-name combination)))
    (let ((call-primary
            (lambda (&rest call-args)
              (let ((args (if call-args call-args *current-method-args*)))
                (%short-combine operator identity-one primary args)))))
      (if around
          (%make-around-chain around call-primary)
          call-primary))))

;;; --- Long-form EMF builder ---

;;; %CALL-METHOD-IMPL is the runtime that the CALL-METHOD macro expands
;;; into.  It invokes METHOD on *CURRENT-METHOD-ARGS*; when NEXT-METHODS
;;; is supplied it installs a call-next-method chain so the method body
;;; can walk further.  Intended for the form emitted by a
;;; DEFINE-METHOD-COMBINATION body.
(defun %call-method-impl (method next-methods)
  (let ((args *current-method-args*))
    (if next-methods
        ;; METHOD followed by NEXT-METHODS as one chain; the chain is
        ;; built per call — this is the long-form combination path.
        (%cnm-invoke (%cnm-chain (cons method next-methods)) args)
        (apply (method-function method) args))))

(defun %make-anon-method (fn)
  "Construct an anonymous method (CLHS MAKE-METHOD) whose method-function
   is FN.  Used by CALL-METHOD to wrap a (MAKE-METHOD FORM) designator into
   a real method object.  simple-primary-p is NIL so that a chain ending in
   the anonymous method always runs it with a *CNM* binding whose next
   index is past the end -- CALL-NEXT-METHOD then signals \"No next method\"
   instead of recursing forever when FORM calls call-next-method and the
   anonymous method is the last in the chain."
  (%make-struct 'standard-method
    nil nil nil fn nil nil))

(defun %make-method-designator-p (x)
  "True when X is a (MAKE-METHOD FORM) designator appearing in a
   CALL-METHOD argument position.  Compared by symbol-name so it matches
   regardless of which package the combination body interned MAKE-METHOD
   in."
  (and (consp x)
       (symbolp (car x))
       (string= (symbol-name (car x)) "MAKE-METHOD")))

(defun %call-method-designator-form (m)
  "Expand a CALL-METHOD method designator M into a form yielding a method
   object.  A real method object is quoted as a literal constant; a
   (MAKE-METHOD FORM) designator becomes a freshly built anonymous method
   whose function evaluates FORM (which may itself contain CALL-METHOD)."
  (if (%make-method-designator-p m)
      (list '%make-anon-method
            (list 'lambda '(&rest %mm-args)
                  '(declare (ignore %mm-args))
                  (cadr m)))
      (list 'quote m)))

(defmacro call-method (method &optional next-methods)
  "CLHS pseudo-operator used inside forms returned by
DEFINE-METHOD-COMBINATION bodies.  Expands into a call to
%CALL-METHOD-IMPL on METHOD with NEXT-METHODS as the call-next-method
chain.  METHOD and the elements of NEXT-METHODS are spliced in as literal
method objects by the combination body's backquote; each is wrapped in
QUOTE so the compiler treats it as a constant.  A (MAKE-METHOD FORM)
designator in either position is instead expanded into a runtime call to
%MAKE-ANON-METHOD so FORM is evaluated when that method is invoked."
  (list '%call-method-impl
        (%call-method-designator-form method)
        (cons 'list (mapcar #'%call-method-designator-form next-methods))))

(defun %build-long-effective-method (gf combination methods)
  "Build an EMF for a long-form COMBINATION.  The combination's builder
closure receives GF and METHODS and returns a FORM; the form is wrapped
into a lambda and evaluated once per unique method set (i.e. once per
cache miss).  Any CALL-METHOD occurrences in the form are expanded by
the global CALL-METHOD macro defined above."
  (let* ((builder (%struct-ref combination 5))
         (form (funcall builder gf methods)))
    (eval `(lambda (&rest %emf-args)
             (declare (ignorable %emf-args))
             ,form))))

;;; --- Method group filtering (long form) ---

(defun %match-qualifier-pattern (qualifiers pattern)
  "Match a method's QUALIFIERS list against a long-form group PATTERN.
PATTERN elements are compared with EQL; a lone * in PATTERN means
`any remaining qualifiers'."
  (cond
    ((and (null qualifiers) (null pattern)) t)
    ((equal pattern '(*)) t)
    ((and pattern (eq (car pattern) '*)) t)
    ((or (null qualifiers) (null pattern)) nil)
    ((eql (car qualifiers) (car pattern))
     (%match-qualifier-pattern (cdr qualifiers) (cdr pattern)))
    (t nil)))

(defun %method-group-order (spec-tail)
  "Read the :ORDER long-form group option from SPEC-TAIL, defaulting to
   :MOST-SPECIFIC-FIRST.  Qualifier patterns are lists and predicates are
   non-keyword symbols, so a top-level :ORDER keyword is unambiguous."
  (let ((tail spec-tail))
    (loop
      (when (null tail) (return :most-specific-first))
      (when (eq (car tail) :order)
        (return (or (cadr tail) :most-specific-first)))
      (setq tail (cdr tail)))))

(defun %filter-methods-by-spec (methods spec-tail)
  "Select methods that satisfy the group specifier SPEC-TAIL (the part
after the group-variable name in a long-form group-spec).  Supported:
  ()                 -- unqualified methods
  (qualifier...)     -- exact qualifier list, optionally ending in *
  (symbol)           -- SYMBOL names a predicate of the qualifier list
The :ORDER option reorders the matched methods: METHODS arrive
most-specific-first, and :MOST-SPECIFIC-LAST reverses them."
  (let* ((pattern (car spec-tail))
         (matched
           (cond
             ((null pattern)
              (remove-if-not (lambda (m) (null (method-qualifiers m))) methods))
             ((and (symbolp pattern)
                   (not (keywordp pattern))
                   (fboundp pattern))
              (remove-if-not (lambda (m) (funcall pattern (method-qualifiers m))) methods))
             ((listp pattern)
              (remove-if-not (lambda (m)
                               (%match-qualifier-pattern (method-qualifiers m) pattern))
                             methods))
             (t methods))))
    (if (eq (%method-group-order spec-tail) :most-specific-last)
        (reverse matched)
        matched)))

;;; --- define-method-combination ---

(defmacro define-method-combination (name &rest args)
  "Register a user method combination.  Two forms:

Short form -- all ARGS are :KEYWORD value pairs (or empty):
  (define-method-combination NAME
    [:documentation STRING]
    [:identity-with-one-argument BOOL]
    [:operator SYMBOL])

Long form -- first ARG is a lambda-list, second is a group-spec list:
  (define-method-combination NAME LAMBDA-LIST ({GROUP-SPEC}*)
    [(:arguments . ARG-LIST)] [(:generic-function VAR)]
    [(:documentation STRING)]
    BODY...)

Each group-spec has the form (VAR QUALIFIER-PATTERN-OR-PREDICATE) and
binds VAR within BODY to the methods whose qualifier list matches.
BODY returns a form, normally written with backquote, using CALL-METHOD
to dispatch the methods it pulls out of those groups."
  (cond
    ;; Short form — options only (or none).
    ((or (null args) (keywordp (car args)))
     (let ((documentation nil)
           (identity-p nil)
           (operator name)
           (rest args))
       (loop while rest
             do (let ((k (pop rest))
                      (v (pop rest)))
                  (case k
                    (:documentation (setq documentation v))
                    (:identity-with-one-argument (setq identity-p v))
                    (:operator (setq operator v)))))
       `(%define-short-method-combination
           ',name ',operator ',identity-p ',documentation)))
    ;; Long form.
    (t
     (let* ((lambda-list (car args))
            (group-specs (cadr args))
            (body (cddr args))
            (gf-var nil)
            (documentation nil))
       (declare (ignore lambda-list))
       ;; Strip leading option forms: (:documentation ...), (:arguments ...),
       ;; (:generic-function VAR).
       (loop while (and body (consp (car body))
                        (or (eq (caar body) :documentation)
                            (eq (caar body) :arguments)
                            (eq (caar body) :generic-function)))
             do (let ((opt (pop body)))
                  (case (car opt)
                    (:generic-function (setq gf-var (cadr opt)))
                    (:documentation (setq documentation (cadr opt))))))
       (let ((gf-sym (or gf-var (gensym "GF")))
             (methods-sym (gensym "METHODS")))
         `(progn
            ,@(when (stringp documentation)
                `((%set-documentation ',name 'method-combination ,documentation)))
            (%define-long-method-combination
              ',name
              (lambda (,gf-sym ,methods-sym)
                (declare (ignorable ,gf-sym))
                (let ,(mapcar (lambda (gs)
                                `(,(car gs)
                                  (%filter-methods-by-spec ,methods-sym ',(cdr gs))))
                              group-specs)
                  ,@body)))))))))

;;; --- Dependent-maintenance protocol GFs (AMOP §5.4) ---
;;; *METAOBJECT-DEPENDENTS* and %NOTIFY-DEPENDENTS are defined near the
;;; top of this file so earlier hook sites can call them during
;;; bootstrap without forward references.  The protocol GFs below are
;;; deferred to the end so that DEFGENERIC / DEFMETHOD themselves are
;;; fully operational before we create them.

(defgeneric add-dependent (metaobject dependent))
(defmethod add-dependent ((metaobject t) dependent)
  "AMOP section 5.4: register DEPENDENT so it receives UPDATE-DEPENDENT
notifications when METAOBJECT is modified by ENSURE-CLASS, ADD-METHOD,
REMOVE-METHOD, or ENSURE-GENERIC-FUNCTION.  Re-adding a dependent
already present (EQ compare) is a no-op.  Returns NIL."
  (let ((deps (gethash metaobject *metaobject-dependents*)))
    (unless (member dependent deps :test #'eq)
      (setf (gethash metaobject *metaobject-dependents*)
            (cons dependent deps))))
  nil)

(defgeneric remove-dependent (metaobject dependent))
(defmethod remove-dependent ((metaobject t) dependent)
  "AMOP section 5.4: unregister DEPENDENT from METAOBJECT.  Silently ignores
dependents that were not previously registered.  Returns NIL."
  (let ((deps (gethash metaobject *metaobject-dependents*)))
    (setf (gethash metaobject *metaobject-dependents*)
          (remove dependent deps :test #'eq)))
  nil)

(defgeneric map-dependents (metaobject function))
(defmethod map-dependents ((metaobject t) function)
  "AMOP section 5.4: apply FUNCTION to each dependent of METAOBJECT in
implementation-defined order.  Returns NIL.  FUNCTION is called with
one argument (the dependent)."
  (dolist (dep (gethash metaobject *metaobject-dependents*))
    (funcall function dep))
  nil)

(defgeneric update-dependent (metaobject dependent &rest initargs))
(defmethod update-dependent ((metaobject t) (dependent t) &rest initargs)
  "AMOP section 5.4: notify DEPENDENT that METAOBJECT changed.  INITARGS
describes the change -- the broadcaster passes ('ADD-METHOD METHOD),
('REMOVE-METHOD METHOD), or ('REINITIALIZE-INSTANCE ...keys...).  The
default method is a no-op; users specialise on their own dependent
class to react."
  (declare (ignore metaobject dependent initargs))
  nil)

(%clos-trace "method combination")
;;; ====================================================================
;;; Portable MOP shims (closer-mop compatibility layer)
;;; ====================================================================
;;;
;;; Symbols and classes that closer-mop imports from an implementation's
;;; MOP package.  We keep them in CL/MOP so the AmiLisp closer-mop fork's
;;; #+clamiga package — which (:use #:common-lisp #:mop) and re-exports —
;;; yields the right objects.  Most of these are placeholders — user-defined
;;; metaclasses are out of scope — but the names must exist for the fork's
;;; DEFPACKAGE to succeed and for downstream libraries (serapeum, trivia,
;;; lisp-namespace) to reference a non-bound-to-error symbol.

;;; --- Abstract metaobject / specializer class stubs ---
;;; AMOP places all reified MOP entities under METAOBJECT, with
;;; SPECIALIZER an abstract parent of CLASS and EQL-SPECIALIZER.  We
;;; register the names so they resolve to class objects — we do not
;;; weave them into the existing CPLs because user-defined metaclasses
;;; are out of scope; code that typep's against these names gets a
;;; defined answer of NIL rather than an undefined-class error.
(%make-bootstrap-class 'metaobject
  (list (find-class 'standard-object)))
(%make-bootstrap-class 'specializer
  (list (find-class 'metaobject)))

;;; --- Forward-referenced classes (stub) ---
;;; We don't support forward references (defclass rejects unknown
;;; superclass names at expansion time), but the symbol must name a
;;; class because closer-mop imports it and libraries typep against it.
(%make-bootstrap-class 'forward-referenced-class
  (list (find-class 'class)))

;;; --- Accessor method classes (stubs) ---
;;; AMOP §5.4.1: reader and writer methods are distinct method subclasses
;;; with an ACCESSOR-METHOD-SLOT-DEFINITION back-link to the slot def
;;; they were generated from.  We generate plain STANDARD-METHOD
;;; instances for reader/writer bodies, so the back-link is NIL.
(%make-bootstrap-class 'standard-accessor-method
  (list (find-class 'standard-method)))
(%make-bootstrap-class 'standard-reader-method
  (list (find-class 'standard-accessor-method)))
(%make-bootstrap-class 'standard-writer-method
  (list (find-class 'standard-accessor-method)))

;;; --- CLASSP ---
;;; closer-mop:classp — true for any class metaobject.
(defun classp (object)
  "Return T when OBJECT is a class metaobject (an instance of STANDARD-CLASS,
BUILT-IN-CLASS, or FUNCALLABLE-STANDARD-CLASS).  closer-mop:CLASSP."
  (and (structurep object)
       (let ((type (%struct-type-name object)))
         (or (eq type 'standard-class)
             (eq type 'built-in-class)
             (eq type 'funcallable-standard-class)
             (eq type 'forward-referenced-class)))))

;;; --- Generic-function accessor aliases (MOP names) ---
;;; Our internal GF accessors are GF-NAME / GF-LAMBDA-LIST / GF-METHODS /
;;; GF-METHOD-COMBINATION.  closer-mop imports them under their AMOP
;;; names (GENERIC-FUNCTION-*) so alias them here.

(defun generic-function-name (gf)
  "AMOP: name of GF."
  (gf-name gf))

(defun generic-function-lambda-list (gf)
  "AMOP: lambda-list of GF."
  (gf-lambda-list gf))

(defun generic-function-methods (gf)
  "AMOP: list of METHOD metaobjects installed on GF."
  (gf-methods gf))

(defun generic-function-method-combination (gf)
  "AMOP: method-combination metaobject for GF."
  (gf-method-combination gf))

(defun generic-function-method-class (gf)
  "AMOP: the class of methods added to GF by DEFMETHOD.  We do not
track a per-GF method class -- every method is a STANDARD-METHOD."
  (declare (ignore gf))
  (find-class 'standard-method))

(defun generic-function-argument-precedence-order (gf)
  "AMOP: the argument names in the order they are consulted for
dispatch.  We dispatch left-to-right, so return the required args of
GF's lambda-list."
  (let ((ll (gf-lambda-list gf))
        (required nil))
    (dolist (arg ll)
      (when (member arg lambda-list-keywords) (return))
      (push arg required))
    (nreverse required)))

(defun generic-function-declarations (gf)
  "AMOP: the list of OPTIMIZE / declare-identifier declarations for GF.
We do not track declarations -- return NIL."
  (declare (ignore gf))
  nil)

;;; --- Compute-applicable-methods (CLHS / AMOP 8.6.4) ---
;;; The standard generic function returning the applicable methods of GF for
;;; ARGS, most-specific-first.  Used by portable CLOS clients (e.g. snooze's
;;; route dispatch).  We expose the bootstrap implementation under the public
;;; name; it already honours class and EQL specializers.
(defun compute-applicable-methods (gf args)
  (%compute-applicable-methods gf args))

;;; --- Compute-applicable-methods-using-classes ---
;;; AMOP 8.6.6 / closer-mop: determine applicable methods from a list
;;; of argument classes alone.  Return (VALUES METHODS VALIDP).  VALIDP
;;; is NIL when any applicable method has an EQL specializer on an arg
;;; whose class is consistent with the EQL value — the caller must then
;;; fall back to COMPUTE-APPLICABLE-METHODS with real arguments.
(defgeneric compute-applicable-methods-using-classes (gf classes))
(defmethod compute-applicable-methods-using-classes
    ((gf standard-generic-function) classes)
  "AMOP: applicable methods for GF given argument CLASSES.  Returns
(VALUES APPLICABLE VALIDP); VALIDP is NIL if the result depends on
particular EQL values that the caller would need to re-check."
  (let ((applicable nil)
        (validp t))
    (dolist (m (gf-methods gf))
      (let ((ok t)
            (specs (method-specializers m))
            (cs classes))
        (loop
          (when (or (null specs) (null cs)) (return))
          (let ((spec (car specs))
                (c (car cs)))
            (cond
              ((eql-specializer-p spec)
               (setq validp nil)
               (setq ok nil)
               (return))
              (t
               (unless (%subclassp c spec)
                 (setq ok nil)
                 (return))))
            (setq specs (cdr specs))
            (setq cs (cdr cs))))
        (when ok (push m applicable))))
    (values (nreverse applicable) validp)))

;;; --- Compute-effective-method ---
;;; AMOP: given a GF, its method combination, and a list of applicable
;;; methods, return an effective-method form.  Our internal dispatch
;;; path builds the EMF directly via %DISPATCH-BUILD-EMF; this GF is
;;; exposed so user code can introspect what standard combination would
;;; produce.  We return a simple form that CALL-METHOD-wraps the primary
;;; chain — enough for closer-mop users that want to see the structure.
(defgeneric compute-effective-method (gf combination methods))
(defmethod compute-effective-method ((gf standard-generic-function)
                                     combination
                                     methods)
  "AMOP: produce an effective-method form for the given METHODS under
COMBINATION.  Returns a form that chains primary methods via
CALL-METHOD; callers typically pass the result to a compiler."
  (declare (ignore gf combination))
  (cond
    ((null methods) '(error "No applicable method"))
    ((null (cdr methods)) `(call-method ,(car methods) nil))
    (t `(call-method ,(car methods) ,(cdr methods)))))

;;; --- Ensure-generic-function-using-class ---
;;; AMOP: low-level entry point used by ENSURE-GENERIC-FUNCTION.  We
;;; don't carry a separate GF-class dispatch, so this delegates to the
;;; single ENSURE-GENERIC-FUNCTION implementation.  The GF argument is
;;; the existing generic-function metaobject or NIL.
(defgeneric ensure-generic-function-using-class (gf name &rest args))
(defmethod ensure-generic-function-using-class (gf name &rest args)
  "AMOP: delegate to ENSURE-GENERIC-FUNCTION.  GF is the existing
metaobject (or NIL) -- it is reused or reinitialized by the underlying
implementation."
  (declare (ignore gf))
  (apply #'ensure-generic-function name args))

;;; --- Specializer / class introspection stubs ---
;;; We don't maintain a cross-reference from specializers to methods or
;;; GFs (memory cost too high on 8MB Amiga).  These GFs exist so
;;; closer-mop's DEFPACKAGE imports succeed and downstream code that
;;; calls them gets a defined but empty answer.

(defgeneric specializer-direct-methods (specializer))
(defmethod specializer-direct-methods (specializer)
  "AMOP: methods that directly specialize on SPECIALIZER.  We don't
track a back-link -- return NIL."
  (declare (ignore specializer))
  nil)

(defgeneric specializer-direct-generic-functions (specializer))
(defmethod specializer-direct-generic-functions (specializer)
  "AMOP: generic functions that have a method directly specializing on
SPECIALIZER.  We don't track a back-link -- return NIL."
  (declare (ignore specializer))
  nil)

(defgeneric add-direct-method (specializer method))
(defmethod add-direct-method (specializer method)
  "AMOP: record METHOD as a direct method of SPECIALIZER.  We do not
maintain the back-link -- this is a no-op."
  (declare (ignore specializer method))
  nil)

(defgeneric remove-direct-method (specializer method))
(defmethod remove-direct-method (specializer method)
  "AMOP: remove METHOD from SPECIALIZER's direct-method list.  No-op."
  (declare (ignore specializer method))
  nil)

(defgeneric add-direct-subclass (class subclass))
(defmethod add-direct-subclass (class subclass)
  "AMOP: register SUBCLASS as a direct subclass of CLASS.  We already
maintain this list in class slot 6 -- add SUBCLASS unless it is already
present."
  (let ((subs (class-direct-subclasses class)))
    (unless (member subclass subs :test #'eq)
      (%set-class-direct-subclasses class (cons subclass subs))))
  nil)

(defgeneric remove-direct-subclass (class subclass))
(defmethod remove-direct-subclass (class subclass)
  "AMOP: drop SUBCLASS from CLASS's direct-subclass list."
  (%set-class-direct-subclasses class
    (remove subclass (class-direct-subclasses class) :test #'eq))
  nil)

(defgeneric accessor-method-slot-definition (method))
(defmethod accessor-method-slot-definition (method)
  "AMOP: the direct slot definition that generated METHOD.  We do not
back-link slot accessors to their slot-definition source -- return NIL."
  (declare (ignore method))
  nil)

(defgeneric reader-method-class (class direct-slot &rest initargs))
(defmethod reader-method-class (class direct-slot &rest initargs)
  "AMOP: class of reader methods generated for DIRECT-SLOT on CLASS.
We generate STANDARD-METHOD instances for accessors -- this is the
protocol hook a user metaclass would override to substitute a subclass."
  (declare (ignore class direct-slot initargs))
  (find-class 'standard-reader-method))

(defgeneric writer-method-class (class direct-slot &rest initargs))
(defmethod writer-method-class (class direct-slot &rest initargs)
  "AMOP: class of writer methods generated for DIRECT-SLOT on CLASS."
  (declare (ignore class direct-slot initargs))
  (find-class 'standard-writer-method))

;;; --- DOCUMENTATION as a generic function ---
;;;
;;; CLHS specifies DOCUMENTATION / (SETF DOCUMENTATION) as generic
;;; functions dispatched on the object and doc-type.  boot.lisp had to
;;; define it as a plain DEFUN because CLOS isn't loaded yet; now that
;;; CLOS is available, replace the plain function with a GF that
;;; delegates to the existing *DOCUMENTATION-TABLE* storage.  Without
;;; this, libraries that add methods (e.g. lisp-namespace) silently
;;; shadow the function definition and plain calls like
;;; (DOCUMENTATION 'FOO 'FUNCTION) signal NO-APPLICABLE-METHOD.
(fmakunbound 'documentation)
(defgeneric documentation (x doc-type))
(defmethod documentation (x doc-type)
  (values (gethash (cons x doc-type) *documentation-table*)))   ; one value (CLHS)

(defgeneric (setf documentation) (new-value x doc-type))
(defmethod (setf documentation) (new-value x doc-type)
  (setf (gethash (cons x doc-type) *documentation-table*) new-value))

(%clos-trace "MOP shims")

;;; ====================================================================
;;; MAKE-LOAD-FORM (CLHS 3.2.4.4 / Section 7.6)
;;; ====================================================================
;;;
;;; The file compiler invokes MAKE-LOAD-FORM (through the FASL writer's
;;; pre-pass, see src/core/fasl.c) for every literal object reachable
;;; from compiled code whose class has a user-defined MAKE-LOAD-FORM
;;; method.  Such an object is reconstructed at load time by evaluating
;;; the two forms the method returns (a "creation form" and an "init
;;; form") instead of being dumped slot-for-slot.
;;;
;;; Plain structures with NO applicable method keep using the built-in
;;; FASL_TAG_STRUCT fast path — only classes that opt in via a method
;;; route through load forms.  Accordingly MAKE-LOAD-FORM is a generic
;;; function with NO default method: the standard mandates that the
;;; default behavior on an object with no method is to signal an error
;;; (here NO-APPLICABLE-METHOD), and the absence of a default is what
;;; keeps every ordinary struct on the fast path.

(defgeneric make-load-form (object &optional environment))

(defun %object-load-form-type-name (object)
  "Type-name symbol handed to %ALLOCATE-FOR-LOAD.  For both CLOS
   instances and plain structures this is the struct type descriptor;
   for a CLOS instance it equals the class name."
  (%struct-type-name object))

(defun %object-load-form-slot-names (object)
  "All slot names of OBJECT.  Mirrors SLOT-VALUE's own dispatch: CLOS
   effective slots when OBJECT's class carries a slot-index-table, else
   the structure's slot names."
  (let ((class (find-class (%struct-type-name object) nil)))
    (if (and class (class-slot-index-table class))
        (mapcar #'slot-definition-name (class-slots class))
        (%struct-slot-names (%struct-type-name object)))))

(defun amilisp::%allocate-for-load (type-name)
  "Allocate a bare instance of TYPE-NAME for MAKE-LOAD-FORM-SAVING-SLOTS
   reconstruction: a CLOS instance with unbound slots, or a plain
   structure with NIL slots.  The init form supplies the real values."
  (let ((class (find-class type-name nil)))
    (if (and class (class-slot-index-table class))
        (allocate-instance class)
        (apply #'%make-struct type-name
               (make-list (%struct-slot-count type-name)
                          :initial-element nil)))))

(defun make-load-form-saving-slots (object &key slot-names environment)
  "CLHS 7.6: return (VALUES creation-form init-form) that reconstruct
   OBJECT by allocating an instance of its type and restoring SLOT-NAMES
   (default: all slots).  Unbound slots are left unbound.  The init form
   references OBJECT itself, so the FASL writer shares it with the
   creation result -- the resulting object is EQ to the one the creation
   form produced (the required circular self-reference)."
  (declare (ignore environment))
  (let ((names (or slot-names (%object-load-form-slot-names object)))
        (inits nil))
    (dolist (sn names)
      (when (slot-boundp object sn)
        (push `(setf (slot-value ',object ',sn) ',(slot-value object sn))
              inits)))
    (values
     `(amilisp::%allocate-for-load ',(%object-load-form-type-name object))
     `(progn ,@(nreverse inits) ',object))))

(defun amilisp::%make-load-form-active-p ()
  "T iff MAKE-LOAD-FORM has at least one user method.  The FASL writer's
   pre-pass calls this once per COMPILE-FILE run and skips the constant
   graph walk entirely when it returns NIL -- so every file that does not
   define a MAKE-LOAD-FORM method pays zero pre-pass cost and keeps the
   exact prior serialization behavior."
  (and (fboundp 'make-load-form)
       (let ((gf (fdefinition 'make-load-form)))
         (and (typep gf 'standard-generic-function)
              (gf-methods gf)
              t))))

(defun amilisp::%fasl-load-form (object)
  "Called by the FASL writer's pre-pass.  If OBJECT has an applicable
   MAKE-LOAD-FORM method, call it and return (creation-form . init-form)
   (init-form may be NIL); otherwise return NIL so the writer keeps
   OBJECT on the built-in structure-dump path."
  (when (and (fboundp 'make-load-form)
             (compute-applicable-methods #'make-load-form (list object)))
    (multiple-value-bind (creation init) (make-load-form object)
      (cons creation init))))

;;; --- Provide module ---
(provide "clos")
