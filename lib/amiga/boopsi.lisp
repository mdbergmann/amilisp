;;; amiga/boopsi.lisp — toolkit-neutral BOOPSI helpers for AmiLisp
;;;
;;; Loaded via (require "amiga/boopsi").
;;;
;;; Everything a BOOPSI object needs from the Lisp side that is the same
;;; whichever toolkit created the object -- ReAction's class libraries
;;; through intuition's NewObjectA, MUI's through MUI_NewObjectA, or the
;;; built-in intuition classes ("buttongclass", "propgclass", "icclass",
;;; ...) that exist on every AmigaOS since 2.0:
;;;
;;;   WITH-FOREIGN-POOL / POOL-ALLOC / POOL-STRING
;;;                      foreign memory that lives as long as the objects
;;;                      that keep pointers to it (GA_Text, label lists,
;;;                      MUIA_String_Contents ...): a C program gets that
;;;                      from string literals and statics, here a pool
;;;                      collects the copies and frees them when the GUI's
;;;                      dynamic extent ends
;;;   WITH-TAGS          a TagItem array from a Lisp plist -- integers,
;;;                      foreign pointers, T/NIL and strings (pooled)
;;;   OBJECT-CLASS       OCLASS(obj): the IClass of an object
;;;   DO-METHOD          IDoMethodA: CallHookPkt on the class dispatcher --
;;;                      any method, OM_GET/OM_SET, WM_OPEN, MUIM_Notify...
;;;   GET-ATTR / GET-ATTR-POINTER / SET-ATTRS
;;;                      intuition's GetAttr / SetAttrsA
;;;   NEW-LIST / FREE-LIST-NODES   exec list headers for the label lists
;;;                      of chooser / clicktab / listbrowser gadgets
;;;
;;; Object CREATION and DISPOSAL are the toolkit's business and live in
;;; the toolkit module: AMIGA.REACTION (NewObjectA / DisposeObject and the
;;; window.class event loop) and AMIGA.MUI (MUI_NewObjectA /
;;; MUI_DisposeObject and the Application event loop).  Both :USE this
;;; package and re-export its names (AMIGA.MUI all but the exec-list
;;; pair, which mean nothing to MUI), so a program written against
;;; amiga.reaction:with-foreign-pool never needs to know where it lives.
;;;
;;; The module loads on any system (the host included): it depends only
;;; on exec/utility/intuition, whose raw modules load everywhere and whose
;;; library bases stay NIL off the Amiga.  tests/test_amiga_boopsi.sh
;;; checks the portable half on the host; tests/amiga/test-boopsi.lisp
;;; drives real objects of the built-in intuition classes on the Amiga,
;;; with no toolkit in sight.

;; compile-time too: COMPILE-FILE (the host builds the lib/amiga FASLs) must see
;; these packages at read time, not only LOAD.
(eval-when (:compile-toplevel :load-toplevel :execute)
  (require "amiga/ffi")
  (require "amiga/raw/exec")
  (require "amiga/raw/utility")
  (require "amiga/raw/intuition"))

(defpackage "AMIGA.BOOPSI"
  (:use "CL" "FFI" "AMIGA.FFI")
  (:export
   ;; Methods
   "DO-METHOD" "OBJECT-CLASS"
   ;; Foreign memory whose lifetime is the GUI's lifetime
   "WITH-FOREIGN-POOL" "POOL-ALLOC" "POOL-STRING" "POOL-HOOK" "POOL-FINALIZER"
   "NEW-LIST" "FREE-LIST-NODES"
   "WITH-TAGS"
   ;; Attributes
   "GET-ATTR" "GET-ATTR-POINTER" "SET-ATTRS"))

(in-package "AMIGA.BOOPSI")

(defconstant +tag-done+ 0)                   ; utility/tagitem.h TAG_DONE

;;; ================================================================
;;; ULONG coercion — what may stand for a tag value / message longword
;;; ================================================================

(defun %ulong (value)
  "Coerce VALUE to the unsigned 32-bit longword the OS sees: integers
\(negative ones two's-complement wrapped), foreign pointers (their
address), T/NIL (TRUE/FALSE or a NULL pointer)."
  (cond ((null value) 0)
        ((eq value t) 1)
        ((integerp value) (logand value #xFFFFFFFF))
        ((ffi:foreign-pointer-p value) (ffi:foreign-pointer-address value))
        (t (error "AMIGA.BOOPSI: ~S cannot be passed as a ULONG -- expected an integer, a foreign pointer, a string (tag values only), T or NIL"
                  value))))

;;; ================================================================
;;; Foreign pool — allocations that must outlive a call
;;; ================================================================

;;; BOOPSI objects keep the POINTERS they are given: GA_Text, the
;;; LABEL_Text pieces, CHOOSER_Labels lists, REQS_Buffer,
;;; MUIA_String_Contents... all must stay valid until the object is
;;; disposed.  A C program gets that for free from string literals and
;;; statics; here WITH-FOREIGN-POOL collects the foreign copies and frees
;;; them when the GUI's dynamic extent ends.

(defvar *foreign-pool* nil
  "The innermost WITH-FOREIGN-POOL: a cons whose CAR is the list of
entries to release on exit -- foreign pointers (freed with
FFI:FREE-FOREIGN) and finalizer functions (called) -- most recent first;
NIL outside any pool.")

(defmacro with-foreign-pool (() &body body)
  "Run BODY with a fresh foreign pool; every POOL-ALLOC / POOL-STRING /
POOL-HOOK / NEW-LIST made inside -- directly, or by WITH-TAGS and the
toolkit functions that build tag lists for you (NEW-OBJECT, SET-ATTRS,
SET-GADGET-ATTRS, OPEN-REQUESTER ...) when they copy a string value -- is
freed when BODY exits, normally or not, in reverse order of creation.
Wrap the whole life of a GUI in one, and dispose the objects inside BODY:
what the pool holds (strings, hooks, custom classes) must outlive them."
  `(let ((*foreign-pool* (list nil)))
     (unwind-protect
          (progn ,@body)
       (dolist (p (car *foreign-pool*))
         (if (functionp p)
             (funcall p)
             (ffi:free-foreign p))))))

(defun %pool-register (entry)
  (unless *foreign-pool*
    (error "AMIGA.BOOPSI: a foreign allocation with the GUI's lifetime was requested outside WITH-FOREIGN-POOL -- wrap the code that builds and runs the GUI in (WITH-FOREIGN-POOL () ...) (AMIGA.BOOPSI:WITH-FOREIGN-POOL, re-exported by AMIGA.REACTION and AMIGA.MUI)"))
  (push entry (car *foreign-pool*))
  entry)

(defun pool-finalizer (function)
  "Call FUNCTION (of no arguments) when the enclosing WITH-FOREIGN-POOL
exits -- after everything registered later has been released, before
everything registered earlier.  For resources with the GUI's lifetime
that are not plain foreign memory (a custom class, a device).  Returns
FUNCTION."
  (unless (functionp function)
    (error "AMIGA.BOOPSI:POOL-FINALIZER: ~S is not a function" function))
  (%pool-register function))

(defun pool-hook (function &key data)
  "AMIGA.FFI:MAKE-HOOK -- a struct Hook calling FUNCTION with (hook object
message), h_Data = DATA -- that lives until the enclosing
WITH-FOREIGN-POOL exits.  What a MUIA_List_DisplayHook, MUIM_CallHook,
LISTBROWSER_*Hook ... value should be: the object keeps the pointer, so
the hook must outlive it, and the pool frees it after BODY has disposed
the objects."
  (let ((hook (amiga.ffi:make-hook function :data data)))
    (pool-finalizer (lambda () (amiga.ffi:free-hook hook)))
    hook))

(defun pool-alloc (size)
  "Allocate SIZE zeroed bytes of foreign memory that live until the
enclosing WITH-FOREIGN-POOL exits."
  (%pool-register (ffi:alloc-foreign size)))

(defun pool-string (string)
  "A NUL-terminated foreign copy of STRING that lives until the
enclosing WITH-FOREIGN-POOL exits."
  (%pool-register (ffi:foreign-string string)))

(defun new-list ()
  "A fresh, initialised exec struct List (what amiga.lib's NewList()
leaves behind), allocated in the enclosing WITH-FOREIGN-POOL.  Feed it
the nodes of a CHOOSER_Labels / CLICKTAB_Labels / LISTBROWSER_Labels
list with AMIGA.RAW.EXEC:ADD-TAIL."
  (let* ((list (pool-alloc amiga.raw.exec:*list-size*))
         (addr (ffi:foreign-pointer-address list)))
    ;; lh_Head = &lh_Tail, lh_Tail = NULL, lh_TailPred = &lh_Head
    (ffi:poke-u32 list (+ addr 4) 0)
    (ffi:poke-u32 list 0 4)
    (ffi:poke-u32 list addr 8)
    list))

(defun free-list-nodes (list free-node)
  "Remove every node from the exec LIST (RemHead until empty) and call
FREE-NODE on each -- e.g. AMIGA.RAW.GADGETS.CHOOSER:FREE-CHOOSER-NODE.
Returns the number of nodes freed."
  (loop for node = (amiga.raw.exec:rem-head list)
        while node
        count (progn (funcall free-node node) t)))

;;; ================================================================
;;; Tag lists with Lisp values
;;; ================================================================

(defun %build-tags (tags)
  "Allocate a TagItem array from the plist TAGS (tag value ...).  Values go
through %ULONG; strings are copied into the foreign pool, because the
object the list is handed to keeps the pointer.  Returns the array; the
caller frees it (the ARRAY is not retained by NewObjectA / SetAttrsA)."
  (unless (evenp (length tags))
    (error "AMIGA.BOOPSI: odd-length tag list ~S -- expected (tag value ...) pairs" tags))
  (let* ((n (floor (length tags) 2))
         (array (ffi:alloc-foreign (* 8 (1+ n))))
         (ok nil))
    (unwind-protect
         (progn
           (loop for (tag value) on tags by #'cddr
                 for offset from 0 by 8
                 do (unless (integerp tag)
                      (error "AMIGA.BOOPSI: tag ~S is not an integer (value ~S) -- the tag constants come from the amiga/raw/... modules" tag value))
                    (ffi:poke-u32 array (logand tag #xFFFFFFFF) offset)
                    (ffi:poke-u32 array
                                  (if (stringp value)
                                      (ffi:foreign-pointer-address (pool-string value))
                                      (%ulong value))
                                  (+ offset 4)))
           (ffi:poke-u32 array +tag-done+ (* 8 n))
           (ffi:poke-u32 array 0 (+ (* 8 n) 4))
           (setf ok t)
           array)
      (unless ok (ffi:free-foreign array)))))

(defmacro %with-tags ((var tags) &body body)
  `(let ((,var (%build-tags ,tags)))
     (unwind-protect (progn ,@body)
       (ffi:free-foreign ,var))))

(defmacro with-tags ((var &rest tags) &body body)
  "Bind VAR to a TagItem array built from the TAGS plist (tag value
...) for the extent of BODY -- for the library functions that take a tag
list themselves: AllocListBrowserNodeA, AllocChooserNodeA,
AllocClickTabNodeA, GetListBrowserNodeAttrsA, MUI_RequestA...  Values
may be integers, foreign pointers, T/NIL and strings (copied into the
enclosing WITH-FOREIGN-POOL, since the receiver keeps the pointer); the
array itself is freed on exit."
  `(%with-tags (,var (list ,@tags))
     ,@body))

;;; ================================================================
;;; Methods
;;; ================================================================

(defun object-class (object)
  "OCLASS(object): the IClass of a BOOPSI object.  The object pointer
points just past its struct _Object header, whose last longword is
o_Class."
  (ffi:peek-pointer (ffi:pointer+ object -4)))

(defun do-method (object method-id &rest args)
  "IDoMethodA: invoke METHOD-ID on the BOOPSI OBJECT.  ARGS become the
longwords following the MethodID in the message (integers, foreign
pointers, T/NIL).  Returns the dispatcher's d0 as an unsigned integer --
wrap it with FFI:MAKE-FOREIGN-POINTER when the method returns a pointer
\(AMIGA.REACTION:OPEN-WINDOW does).  A class's dispatcher is the Hook at
the start of its IClass, so this is CallHookPkt(OCLASS(obj), obj, msg) --
exactly what amiga.lib's DoMethodA does, for ReAction, MUI and built-in
intuition objects alike."
  (let* ((n (length args))
         (msg (ffi:alloc-foreign (* 4 (1+ n)))))
    (unwind-protect
         (progn
           (ffi:poke-u32 msg (logand method-id #xFFFFFFFF) 0)
           (loop for a in args
                 for offset from 4 by 4
                 do (ffi:poke-u32 msg (%ulong a) offset))
           (amiga.raw.utility:call-hook-pkt (object-class object) object msg))
      (ffi:free-foreign msg))))

;;; ================================================================
;;; Attributes
;;; ================================================================

(defun get-attr (attribute object)
  "GetAttr(ATTRIBUTE, OBJECT): the attribute's current value as an
unsigned integer, or NIL if the object does not know the attribute."
  (let ((storage (ffi:alloc-foreign 4)))
    (unwind-protect
         (if (zerop (amiga.raw.intuition:get-attr attribute object storage))
             nil
             (ffi:peek-u32 storage 0))
      (ffi:free-foreign storage))))

(defun get-attr-pointer (attribute object)
  "GET-ATTR for a pointer-valued attribute (WINDOW_Window,
MUIA_Window_Window, ...): a foreign pointer, or NIL for NULL / unknown."
  (let ((value (get-attr attribute object)))
    (if (or (null value) (zerop value))
        nil
        (ffi:make-foreign-pointer value))))

(defun set-attrs (object &rest tags)
  "SetAttrsA(OBJECT, TAGS) -- same value rules as WITH-TAGS (integers,
foreign pointers, T/NIL, strings copied into the enclosing
WITH-FOREIGN-POOL).  Returns the class's result (non-zero usually means
a visual refresh is due)."
  (%with-tags (array tags)
    (amiga.raw.intuition:set-attrs-a object array)))

(provide "amiga/boopsi")
