;;; amiga/ffi.lisp — AmigaOS-specific FFI utilities
;;;
;;; Loaded via (require "amiga/ffi").
;;; Builds on the generic FFI package and the AMIGA package builtins.

;; compile-time too: COMPILE-FILE (the host builds the lib/amiga FASLs) must see
;; these packages at read time, not only LOAD.
(eval-when (:compile-toplevel :load-toplevel :execute)
  (require "ffi"))

(defpackage "AMIGA.FFI"
  (:use "CL" "FFI")
  (:export "WITH-LIBRARY" "WITH-TAG-LIST" "MAKE-TAG-LIST" "DEFCFUN"
           "*DEFCFUN-DOCSTRINGS*" "DEFINE-BINDING-TABLE"
           "LIBRARY-VERSION" "OPEN-LIBRARY-OR-DIE" "DEFINE-LIBRARY-VARIABLE"
           ;; Lisp functions the OS calls back: struct Hook entries and
           ;; BOOPSI class dispatchers
           "MAKE-HOOK" "FREE-HOOK" "HOOK-ENTRY" "HOOK-DATA"
           "MAKE-DISPATCHER" "FREE-DISPATCHER" "DISPATCHER-STATS" "CALLBACK-ULONG"))

(in-package "AMIGA.FFI")

;;; ================================================================
;;; with-library — open/close AmigaOS library with cleanup
;;; ================================================================

(defmacro with-library ((var name &optional (version 0)) &body body)
  "Open an AmigaOS shared library, bind base to VAR, close on exit.
Signals an error if the library cannot be opened."
  `(let ((,var (amiga:open-library ,name ,version)))
     (when (or (null ,var) (ffi:null-pointer-p ,var))
       (error "Cannot open library ~A version ~D" ,name ,version))
     (unwind-protect
       (progn ,@body)
       (amiga:close-library ,var))))

(defun open-library-or-die (name &optional (version 0))
  "OpenLibrary NAME (at least VERSION) and return its base; signal a
descriptive error if the OS refuses.  Used by the binding modules at
load time, so a missing library fails at REQUIRE with a clear message
instead of at the first call."
  (let ((base (amiga:open-library name version)))
    (when (or (null base) (ffi:null-pointer-p base))
      (error "Cannot open ~A~@[ (version ~D or newer)~] -- is it installed on this system?"
             name (and (plusp version) version)))
    base))

(defun library-version (base)
  "The lib_Version field (struct Library, offset 20) of an open library
base -- the OS revision that the running system actually provides.
Bindings generated from the OS 3.2 NDK guard functions newer than V39
with this at load time."
  (ffi:peek-u16 base 20))

;;; ================================================================
;;; define-library-variable — OS state a module captures at load time
;;; ================================================================
;;;
;;; A library base cannot travel in a heap image: the restore zeroes every
;;; foreign pointer (the address belonged to the saving process), and even
;;; where the address would still be right the restoring process never
;;; opened the library, so a disk-based one (asl, muimaster) could be
;;; expunged while in use.  A module therefore declares its base -- and
;;; whatever it derives from it, the library's version -- with
;;; DEFINE-LIBRARY-VARIABLE: a DEFVAR whose init form runs again after a
;;; restore, before ~/.amilisprc (the runtime's EXT::*SYSTEM-RESTORE-HOOKS*),
;;; exactly as a fresh load runs it.

(defvar *library-variables* '()
  "(SYMBOL . INIT-FUNCTION) for every DEFINE-LIBRARY-VARIABLE, in the
order the variables were defined: re-derived in that order after a
restore, so a version defined after its base sees the new base.")

(defun %note-library-variable (name init)
  "Register NAME's INIT function; a reload replaces its entry in place."
  (let ((entry (assoc name *library-variables*)))
    (if entry
        (setf (cdr entry) init)
        (setf *library-variables*
              (append *library-variables* (list (cons name init))))))
  name)

(defmacro define-library-variable (name init-form &optional (doc nil docp))
  "DEFVAR NAME with INIT-FORM, a value the running system decides -- a
library base (OPEN-LIBRARY-OR-DIE, AMIGA:OPEN-LIBRARY), the version of
one, NIL for a device or resource base the program sets itself -- and
evaluate INIT-FORM again after a heap-image restore, before ~/.amilisprc
runs: the image's copy belongs to the saving process (a restore zeroes
every foreign pointer).  Variables are re-derived in the order they were
defined.  An INIT-FORM that signals then leaves NAME NIL and says so on
*ERROR-OUTPUT*; a library call through the NIL base reports that the
library is not open."
  `(progn
     (defvar ,name ,init-form ,@(when docp (list doc)))
     (%note-library-variable ',name (lambda () ,init-form))))

(defun %rederive-library-variables ()
  "EXT::*SYSTEM-RESTORE-HOOKS* entry: run every library variable's init
form again, in this process."
  (dolist (entry *library-variables*)
    (setf (symbol-value (car entry))
          (handler-case (funcall (cdr entry))
            (error (e)
              (format *error-output*
                      "~&; AMIGA.FFI: ~S could not be re-derived after the image restore -- ~A~%;   it is NIL now: calls through it say the library is not open~%"
                      (car entry) e)
              nil)))))

(unless (member '%rederive-library-variables ext::*system-restore-hooks*)
  (push '%rederive-library-variables ext::*system-restore-hooks*))

;;; ================================================================
;;; Tag list support
;;; ================================================================

;;; AmigaOS tag lists are arrays of (uint32 tag, uint32 data) pairs,
;;; terminated by TAG_DONE (0).

(defconstant +tag-done+ 0)
(defconstant +tag-ignore+ 1)
(defconstant +tag-more+ 2)
(defconstant +tag-skip+ 3)

(defun make-tag-list (pairs)
  "Build a foreign TagItem array from a flat list of (tag value ...) pairs.
Returns a foreign pointer to the allocated array.  Caller must free it."
  (let* ((n (floor (length pairs) 2))
         (size (* (1+ n) 8))  ; n pairs + TAG_DONE, each 8 bytes
         (ptr (ffi:alloc-foreign size)))
    (do ((rest pairs (cddr rest))
         (i 0 (1+ i)))
        ((null rest))
      (let ((tag (car rest))
            (val (cadr rest))
            (offset (* i 8)))
        ;; tag at offset, data at offset+4
        (ffi:poke-u32 ptr tag offset)
        (ffi:poke-u32 ptr
                      (if (ffi:foreign-pointer-p val)
                          (ffi:foreign-pointer-address val)
                          val)
                      (+ offset 4))))
    ;; Terminate with TAG_DONE
    (ffi:poke-u32 ptr +tag-done+ (* n 8))
    (ffi:poke-u32 ptr 0 (+ (* n 8) 4))
    ptr))

(defmacro with-tag-list ((var &rest pairs) &body body)
  "Build a TagItem array from pairs, bind to VAR, free on exit.
Each pair is (tag-constant value).  String values are automatically
copied to foreign memory and freed."
  (let ((tag-pairs (gensym "PAIRS"))
        (strings (gensym "STRINGS")))
    `(let ((,strings nil)
           (,tag-pairs (list ,@(loop for (tag val) on pairs by #'cddr
                                     collect tag
                                     collect (if (stringp val)
                                                 ;; Defer to runtime
                                                 val
                                                 val)))))
       ;; Convert string values to foreign strings
       (do ((rest ,tag-pairs (cddr rest)))
           ((null rest))
         (when (stringp (cadr rest))
           (let ((fstr (ffi:foreign-string (cadr rest))))
             (push fstr ,strings)
             (setf (cadr rest) fstr))))
       (let ((,var (make-tag-list ,tag-pairs)))
         (unwind-protect
           (progn ,@body)
           (ffi:free-foreign ,var)
           (dolist (s ,strings)
             (ffi:free-foreign s)))))))

;;; ================================================================
;;; defcfun — define a named binding for an Amiga library function
;;; ================================================================

;;; (amiga.ffi:defcfun move-to *gfx-base* -240
;;;   (:a1 rastport :d0 x :d1 y) :result :void)
;;;
;;; Installs an FFI STUB in MOVE-TO's function cell: a 20-byte descriptor
;;; (library-base variable, LVO, packed register spec, result kind,
;;; arity) that the runtime calls directly — no wrapper closure, no
;;; bytecode body, no compiler macro.  It is a function for every CL
;;; purpose (FUNCTIONP, #', FUNCALL, APPLY, TRACE, DESCRIBE,
;;; EXT:FUNCTION-ARGLIST); a direct call site `(move-to rp x y)` is
;;; recognized by the compiler and emitted as a bare OP_AMIGA_CALL (the
;;; JIT calls the trampoline natively), an indirect call goes through the
;;; stub's own dispatch (cl_ffi_stub_call).  Inspect one with
;;; (ffi::%ffi-stub-info #'move-to) or DESCRIBE.
;;;
;;; Why a descriptor: the generated raw OS binding modules define
;;; thousands of these; as wrapper functions each cost ~550 bytes of
;;; heap plus three off-heap allocations, which put a full intuition
;;; binding at ~0.4 MB — out of reach on an 8 MB 68020.  See
;;; specs/raw-bindings-footprint.md.
;;;
;;; Register spec: 7 nibbles, one register index per value arg, low to
;;; high (D0..D7 = 0..7, A0..A4 = 8..12); the result kind sits in bits
;;; 28-31 of the stub's u32 regspec (CL_AMIGA_RES_* in builtins.h).
;;;
;;; :RESULT selects how the d0 value comes back:
;;;   :unsigned (default)  ULONG/UWORD/UBYTE/BPTR/Tag... as an integer
;;;   :void                discard d0, return NIL (most rendering calls)
;;;   :pointer             struct X * / APTR / STRPTR -> foreign pointer,
;;;                        NIL for NULL
;;;   :signed              LONG -> signed integer (-1 stays -1)
;;;   :bool                BOOL -> T/NIL (tests d0.w, the 16-bit ABI width)
;;;   :i16 :u16 :i8 :u8    WORD/UWORD/BYTE/UBYTE results — only the low
;;;                        bits of d0 are defined for these C types
;;; All nine are boxed in C by the trampoline.  :VOID T is the legacy
;;; spelling of :RESULT :VOID.
;;;
;;; Argument values, for any register: an integer (its two's-complement
;;; longword -- a negative LONG wraps, a bignum contributes its low 32
;;; bits), a foreign pointer (its address), NIL (0: a NULL pointer or
;;; FALSE) or T (1: TRUE, for a BOOL or LONG flag such as RethinkLayout's
;;; refresh) -- the coercion tag values get in AMIGA.BOOPSI.  Anything
;;; else -- a string, a float, a symbol -- is a TYPE-ERROR naming the
;;; argument, its register and the value; a string an OS function keeps
;;; must be copied out first (FFI:FOREIGN-STRING, AMIGA.BOOPSI:POOL-STRING).
;;;
;;; Functions with more than seven register arguments (a dozen in the
;;; whole OS: BltBitMap, ClipBlit, ModifyProp, CreateUpfrontLayer...)
;;; fall back to a DEFUN over AMIGA:CALL-LIBRARY's plist path, which has
;;; no register cap.  :A5 can never carry an argument — it is the
;;; dispatcher's scratch register (ffi_dispatch_m68k.s).
;;;
;;; The stub is installed at compile time too (EVAL-WHEN), so a module
;;; compiled with COMPILE-FILE gets its own intra-file calls inlined, and
;;; AMIGA:%FFI-CALL remains available as the hand-written spelling of the
;;; same call (kinds 0-3 only — a Lisp fixnum cannot hold bits 28-31).

(defun %defcfun-reg-index (kw)
  "Return the register index (0..12) for an :Dn or :An keyword, or signal."
  (case kw
    (:d0 0)  (:d1 1)  (:d2 2)  (:d3 3)
    (:d4 4)  (:d5 5)  (:d6 6)  (:d7 7)
    (:a0 8)  (:a1 9)  (:a2 10) (:a3 11)
    (:a4 12)
    (:a5 (error "DEFCFUN: register :A5 is reserved by the call dispatcher and cannot carry an argument (use d0-d7/a0-a4)"))
    (otherwise (error "DEFCFUN: unknown register keyword: ~S" kw))))

(defconstant +defcfun-void-bit+ #x10000000)  ; bit 28 -- kind 1 = void

;; CL_AMIGA_RES_* (src/core/builtins.h)
(defconstant +result-kind-unsigned+ 0)
(defconstant +result-kind-void+     1)
(defconstant +result-kind-pointer+  2)
(defconstant +result-kind-signed+   3)
(defconstant +result-kind-bool+     4)
(defconstant +result-kind-u16+      5)
(defconstant +result-kind-i16+      6)
(defconstant +result-kind-u8+       7)
(defconstant +result-kind-i8+       8)

(defun %defcfun-result-kind (result)
  "Map a :RESULT keyword to its CL_AMIGA_RES_* code."
  (ecase result
    (:unsigned +result-kind-unsigned+)
    (:void     +result-kind-void+)
    (:pointer  +result-kind-pointer+)
    (:signed   +result-kind-signed+)
    (:bool     +result-kind-bool+)
    (:u16      +result-kind-u16+)
    (:i16      +result-kind-i16+)
    (:u8       +result-kind-u8+)
    (:i8       +result-kind-i8+)))

(defvar *defcfun-docstrings* t
  "When true (the default) DEFCFUN records its :DOC string as the
function's documentation.  Bound to NIL while the Amiga FASLs are built
(scripts/compile-lib-fasls.sh --no-docstrings, i.e. `make fasl-amiga` and
the binary release): a docstring costs ~130 bytes of heap per binding,
~16 KB for the intuition module alone, that an 8 MB machine would rather
spend elsewhere.  The generated .lisp sources always keep the C
prototypes, so they remain readable on any system.")

(defmacro defcfun (name library-base offset (&rest reg-spec)
                   &key void (result :unsigned) doc)
  "Define a Lisp function NAME that calls the AmigaOS library function at
LVO OFFSET of the library whose base is held in the special variable
LIBRARY-BASE.  REG-SPEC is a plist of (:register param-name ...) pairs.
:RESULT (see the table above) chooses how d0 is returned; :VOID T is the
legacy spelling of :RESULT :VOID.  DOC becomes the function's docstring
(unless *DEFCFUN-DOCSTRINGS* is NIL).

NAME's function cell receives an FFI stub -- a compact binding
descriptor the runtime calls directly.  Direct call sites `(move-to rp
x y)` compile to a bare OP_AMIGA_CALL in the caller; #'NAME, FUNCALL,
APPLY, TRACE and DESCRIBE all work on the stub as on any function."
  (let* ((pairs (loop for (reg param) on reg-spec by #'cddr
                      collect (list reg param)))
         (params (mapcar #'second pairs))
         (n-params (length params))
         (result (if void :void result))
         (kind (%defcfun-result-kind result))
         (doc-forms (when (and doc *defcfun-docstrings*)
                      `((setf (documentation ',name 'function) ,doc)))))
    (if (> n-params 7)
        ;; Plist path: a real DEFUN over CALL-LIBRARY, no register cap.
        (let ((plist (loop for (reg param) in pairs
                           collect reg collect param)))
          (dolist (pair pairs) (%defcfun-reg-index (first pair)))  ; validate
          `(progn
             (defun ,name ,params
               ,@(when (and doc *defcfun-docstrings*) (list doc))
               (amiga:call-library ,library-base ,offset (list ,@plist) ,kind))
             ',name))
        (let ((regspec 0) (shift 0))
          (dolist (pair pairs)
            (setf regspec (logior regspec
                                  (ash (%defcfun-reg-index (first pair)) shift)))
            (incf shift 4))
          `(progn
             ;; amiga::%defcfun builds the stub and installs it in NAME's
             ;; function cell (one small form per binding — the generated
             ;; modules have thousands, and every symbol a top-level form
             ;; names costs FASL bytes).  Compile time as well, so that
             ;; later call sites in the same COMPILE-FILE see the stub and
             ;; inline the library call (what the old compile-time compiler
             ;; macro gave them).
             (eval-when (:compile-toplevel :load-toplevel :execute)
               (amiga::%defcfun ',name ',library-base ,offset ,regspec ,kind ,n-params))
             ,@doc-forms
             ',name)))))

;;; ================================================================
;;; define-binding-table — a whole module's bindings, demand-interned
;;; ================================================================

;;; The generated raw OS modules (lib/amiga/raw/) define ~2000 names
;;; each and a program touches a few dozen.  DEFINE-BINDING-TABLE packs
;;; every binding of a package into ONE byte vector (at macroexpansion
;;; time, so a compiled module's FASL carries that vector and nothing
;;; else) and attaches it to the package; a name is materialised — its
;;; symbol, value or FFI stub, export — the first time anything looks it
;;; up (the reader, FIND-SYMBOL, INTERN, a FASL reference).  Untouched
;;; names cost their table bytes only.  Materialised symbols are ordinary
;;; symbols of an ordinary package; DO-SYMBOLS / APROPOS / UNINTERN and
;;; the other enumerating or mutating operations build every entry first,
;;; so no laziness is observable except as memory.  See
;;; specs/raw-bindings-footprint.md (Phase 2) and src/core/bindtab.c.
;;;
;;;   (amiga.ffi:define-binding-table "AMIGA.RAW.INTUITION"
;;;       (:base *intuition-base* :version *intuition-version*)
;;;     (:const "+WA-LEFT+" #x80000064)          ; DEFCONSTANT
;;;     (:const "+MUIC-WINDOW+" "Window.mui")     ; DEFCONSTANT of a string (ASCII, <= 255)
;;;     (:var "*MSG-SIZE*" 4)                     ; DEFVAR (a struct size)
;;;     (:fn "OPEN-WINDOW" -204 (:a0) :pointer)   ; DEFCFUN: lvo (regs) result
;;;     (:fn "SHOW-WINDOW" -834 (:a0 :a1) :bool :not-morphos 46)   ; guards
;;;     (:struct "WINDOW" 136                     ; DEFCSTRUCT: *WINDOW-SIZE*
;;;       ("LEFT-EDGE" :i16 4)                    ;   + WINDOW-LEFT-EDGE ...
;;;       ("RPORT" :fptr 50)
;;;       ("USER-DATA" (:struct 4) 120))
;;;     (:field "SOME-ACCESSOR" (:array :u16 4) 12)  ; one accessor
;;;     (:name "BLT-BITMAP"))                     ; exported, defined elsewhere
;;;
;;; Names are strings, taken literally (like DEFPACKAGE's :EXPORT).  A
;;; :const / :var value is an integer of any size or an ASCII string of at
;;; most 255 characters (a C header's string #define, mui.h's MUIC_*
;;; class names); the string is materialised as a fresh simple string.
;;; :fn rows are what DEFCFUN takes: the LVO, the argument registers in
;;; order, the :RESULT kind, then optionally :NOT-MORPHOS / :MORPHOS and
;;; a minimum library version (checked against the :VERSION variable at
;;; lookup time; a name whose guard fails still exists, unbound — the
;;; (when guard (defcfun ..)) behaviour).  A :fn name may repeat with
;;; exclusive guards (platform variants at different LVOs).  Struct
;;; accessors get their %SET- writer and DEFSETF like DEFCSTRUCT.  The
;;; package must already exist (DEFPACKAGE); its :EXPORT list needs only
;;; the names defined by ordinary forms (the base and version variables),
;;; everything in the table is exported by the table.

(defmacro define-binding-table (package (&key base version) &body rows)
  "Attach the binding table built from ROWS to PACKAGE (a package
designator) so its names are materialised on first reference.  BASE names
the special variable holding the open library base (needed by :fn rows),
VERSION the variable holding its lib_Version (for version guards).  See
the comment above for the row syntax."
  (let ((blob (clamiga::%make-binding-table rows)))
    `(eval-when (:compile-toplevel :load-toplevel :execute)
       (clamiga::%register-binding-table ,package ,blob ',base ',version))))

;;; ================================================================
;;; Hooks and dispatchers — Lisp functions the OS calls
;;; ================================================================

;;; A struct Hook (utility/hooks.h) is how AmigaOS hands a callback to the
;;; system: MUI's MUIA_List_DisplayHook and MUIM_CallHook, ReAction's
;;; LISTBROWSER_*Hook, ASL's ASLFR_FilterFunc, commodities, the layout
;;; hooks of MUI groups.  The OS jumps to h_Entry with the hook in a0, the
;;; "object" in a2 and the "message" in a1 -- a register convention no C
;;; compiler follows, which is what FFI:MAKE-CALLBACK's REGS argument
;;; describes.  A BOOPSI class dispatcher (MakeClass /
;;; MUI_CreateCustomClass) is entered the same way with the class in a0.
;;;
;;; The Lisp function runs on the OS's stack, between the C frames of the
;;; caller (a semaphore held, a class dispatch in progress): it cannot
;;; unwind across them, so the runtime treats every callback as a
;;; boundary -- an unhandled error, a THROW or RETURN-FROM to a target
;;; outside the callback, or an ABORT restart is caught there, the
;;; callback returns 0, and the condition is re-signaled on the Lisp side
;;; once the library call that invoked the hook has returned (a
;;; HANDLER-CASE around that call catches it; the debugger shows it there).
;;; A HANDLER-CASE inside the callback works as usual.  See the "Foreign-
;;; callback boundary" note in src/core/thread.h and
;;; EXT:*CALLBACK-ERROR-POLICY*.
;;;
;;; On the host these are ordinary C-ABI callbacks (libffi): a hook's
;;; entry is a C function of (hook, object, message), which is how
;;; tests/test_amiga_boopsi.lisp drives the whole path without an Amiga.

(defun callback-ulong (value who)
  "Coerce VALUE, what a hook or dispatcher function returned, to the ULONG
the OS expects in d0: an integer (wrapped to 32 bits), a foreign pointer
\(its address), T (1) or NIL (0).  WHO names the caller for the error."
  (cond ((null value) 0)
        ((eq value t) 1)
        ((integerp value) (logand value #xFFFFFFFF))
        ((ffi:foreign-pointer-p value) (ffi:foreign-pointer-address value))
        (t (error "~A: the Lisp function returned ~S -- a hook or dispatcher returns a ULONG: an integer, a foreign pointer, T or NIL"
                  who value))))

(defun %check-callback-function (function who)
  (unless (functionp function)
    (error "~A: ~S is not a function -- expected a function of three arguments (hook object message), see the docstring"
           who function)))

;; struct Hook: MinNode (8) | h_Entry (+8) | h_SubEntry (+12) | h_Data (+16) = 20 bytes
(defconstant +hook-size+ 20)

(defvar *hooks* (make-hash-table)
  "hook address -> the callback (FFI:MAKE-CALLBACK) installed as its
h_Entry.  The struct's own h_Entry longword is what the OS calls; this
table is what HOOK-ENTRY and FREE-HOOK read, exact on the host too, where
a callback's address does not fit the 32-bit field.")

(defun make-hook (function &key data)
  "A struct Hook whose entry calls FUNCTION, a function of three
arguments -- the hook itself, the object and the message, all foreign
pointers (the registers a0, a2, a1 of the hook convention) -- and returns
what the OS gets back in d0: an integer, a foreign pointer, T or NIL
\(CALLBACK-ULONG).  DATA (an integer, a foreign pointer, T/NIL) goes into
h_Data, readable back with HOOK-DATA.  Returns the hook as a foreign
pointer, to pass wherever the OS takes a struct Hook * (a MUIA_*Hook tag
value, CallHookPkt, MUIM_CallHook).  The hook must outlive every object
that was given it; FREE-HOOK releases it -- or use AMIGA.BOOPSI:POOL-HOOK
and let the foreign pool do that.  What an error inside FUNCTION does is
described at the top of this section."
  (%check-callback-function function 'make-hook)
  (let* ((callback (ffi:make-callback
                    :uint32 '(:pointer :pointer :pointer)
                    (lambda (hook object message)
                      (callback-ulong (funcall function hook object message) 'make-hook))
                    '(:a0 :a2 :a1)))
         (hook (ffi:alloc-foreign +hook-size+)))
    (ffi:poke-u32 hook (logand (ffi:foreign-pointer-address callback) #xFFFFFFFF) 8) ; h_Entry
    (ffi:poke-u32 hook 0 12)                                        ; h_SubEntry: unused
    (ffi:poke-u32 hook (callback-ulong data 'make-hook) 16)         ; h_Data
    (setf (gethash (ffi:foreign-pointer-address hook) *hooks*) callback)
    hook))

(defun hook-entry (hook)
  "The h_Entry of HOOK as a foreign pointer -- the callback FFI:MAKE-CALLBACK
built (on the host, a C function of (hook, object, message))."
  (or (gethash (ffi:foreign-pointer-address hook) *hooks*)
      (error "AMIGA.FFI:HOOK-ENTRY: ~S is not a live MAKE-HOOK hook" hook)))

(defun hook-data (hook)
  "The h_Data longword of HOOK, as an unsigned integer.  SETF-able."
  (ffi:peek-u32 hook 16))

(defun (setf hook-data) (value hook)
  (ffi:poke-u32 hook (callback-ulong value 'hook-data) 16)
  value)

(defun free-hook (hook)
  "Release a MAKE-HOOK hook: the callback and the struct.  Only after every
object that holds the hook is disposed; NIL is ignored."
  (when hook
    (ffi:free-callback (hook-entry hook))
    (remhash (ffi:foreign-pointer-address hook) *hooks*)
    (ffi:free-foreign hook))
  nil)

(defun make-dispatcher (function &key (methods nil methods-p))
  "A BOOPSI class dispatcher -- what intuition's MakeClass and MUI's
MUI_CreateCustomClass take -- that calls FUNCTION with the class, the
object and the message (foreign pointers; the registers a0, a2, a1).  The
message's MethodID is its first longword; FUNCTION returns the method's
result (CALLBACK-ULONG), typically what DoSuperMethodA returned for the
methods it does not handle itself.

With METHODS, a list of method ids (unsigned 32-bit integers), FUNCTION is
called for those methods only: every other message goes to the superclass
in native code, without entering Lisp.  An object is sent many methods the
class has no interest in -- OM_SET, OM_GET, MUIM_Draw for every repaint --
and each one that reaches FUNCTION costs a callback into Lisp, so a class
on an interactive path should list what it handles.  DISPATCHER-STATS
counts both kinds.

Returns the entry as a foreign pointer; FREE-DISPATCHER releases it once
the class is removed."
  (%check-callback-function function 'make-dispatcher)
  (let ((entry (lambda (class object message)
                 (callback-ulong (funcall function class object message)
                                 'make-dispatcher))))
    (cond (methods-p
           (unless (and (listp methods)
                        (every (lambda (id) (typep id '(unsigned-byte 32))) methods))
             (error "AMIGA.FFI:MAKE-DISPATCHER: METHODS must be a list of unsigned 32-bit method ids, got ~S"
                    methods))
           (ffi::%make-method-dispatcher entry methods))
          (t
           (ffi:make-callback :uint32 '(:pointer :pointer :pointer) entry
                              '(:a0 :a2 :a1))))))

(defun dispatcher-stats (dispatcher)
  "Three values for a MAKE-DISPATCHER entry made with METHODS: how many
messages reached it, how many of those went to the superclass without
entering Lisp, and how many method ids it lists.  NIL for a dispatcher
made without METHODS."
  (values-list (ffi::%method-dispatcher-stats dispatcher)))

(defun free-dispatcher (dispatcher)
  "Release a MAKE-DISPATCHER entry (after the class is gone); NIL is ignored."
  (when dispatcher (ffi:free-callback dispatcher))
  nil)

;;; ================================================================
;;; Provide module
;;; ================================================================

(provide "amiga/ffi")
