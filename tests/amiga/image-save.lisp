; Heap-image save leg (driven by verify/realamiga/call-on-ustartup):
; build one instance of every state class the image format must carry,
; then save and quit.  tests/amiga/image-verify.lisp checks the restored
; session; Makefile.cross's verify-amiga requires its IMAGE-VERIFY-PASSED
; marker in the results log.

(defun img-fib (n) (if (< n 2) n (+ (img-fib (- n 1)) (img-fib (- n 2)))))
(defmacro img-twice (x) `(* 2 ,x))

(defclass img-animal () ((name :initarg :name :accessor img-name)))
(defmethod img-speak ((a img-animal)) (format nil "~a speaks" (img-name a)))
(defvar *img-pet* (make-instance 'img-animal :name "Rex"))

(defstruct img-point x y)
(defvar *img-pt* (make-img-point :x 3 :y 4))

(defvar *img-ht* (make-hash-table :test 'eq))
(defvar *img-key* (cons 'k 'v))
(setf (gethash *img-key* *img-ht*) 'img-hit)

(defvar *img-close* (let ((n 31)) (lambda (x) (+ x n))))
(set-macro-character #\! (lambda (s c) (declare (ignore s c)) 4242))

(defvar *img-sos* (make-string-output-stream))
(write-string "pre-save;" *img-sos*)

(defvar *img-lock* (mp:make-lock "img-lock"))
(defvar *img-cv* (mp:make-condition-variable "img-cv"))
(defvar *img-dead-thr* (mp:make-thread (lambda () 99)))
(mp:join-thread *img-dead-thr*)

(push (lambda () (format t "IMAGE-RESTORE-HOOK-RAN~%")) ext:*restore-hooks*)

; A demand-interned binding table (bindtab.c) + the delivery mode
; :SHAKE-BINDINGS (specs/raw-bindings-footprint.md, Phase 3): one name is
; touched before the save, the table is shed with the dump.  The restored
; session must keep the touched name — value, identity, stub — and miss
; every untouched one cleanly.  Defined inline rather than REQUIREd from
; lib/amiga/raw so this leg stays independent of the raw FASLs.
(require "amiga/ffi")
(defvar *img-bt-base* nil)
(defpackage "IMG-BT" (:use "CL"))
(amiga.ffi:define-binding-table "IMG-BT" (:base *img-bt-base*)
  (:const "+KEPT+" 11) (:const "+GONE+" 22)
  (:fn "IMG-CALL" -30 (:a0) :bool)
  (:struct "IMG-NODE" 4 ("X" :i16 0)))
(defvar *img-bt-kept* 'img-bt:+kept+)
(defvar *img-bt-fn* (fboundp 'img-bt:img-call))

; Process-derived state the restore must re-derive rather than carry over:
; the verify leg runs from a DIFFERENT directory (AmiLisp:tests, see
; call-on-ustartup) and must see its own cwd in *default-pathname-defaults*,
; and a fresh *random-state* — its first draw must not be the draw this
; (untouched, image-bound) state would have produced.
(defvar *img-saved-dpd* (namestring *default-pathname-defaults*))
(defvar *img-rnd-next* (random 1000000 (make-random-state nil)))

; Library bases (AMIGA.FFI:DEFINE-LIBRARY-VARIABLE).  The restore zeroes
; every foreign pointer of the image, so a base a module opened at load time
; came back as address 0 and the first library call jumped to -LVO -- the
; Clamacs heap image died that way in TextEditor.mcc's version probe.  Both
; kinds of module: the curated amiga/exec (AMIGA:CALL-LIBRARY through its
; *EXEC-BASE*) and a generated one (a DEFCFUN stub through
; AMIGA.RAW.EXEC:*EXEC-BASE*), whose AVAIL-MEM this function touches so the
; shed keeps it.  IMG-AVAIL-RAW is image code: after the restore it runs as
; bytecode, IMAGE-VERIFY's twin is compiled (and JIT'd) there.
(require "amiga/exec")
(require "amiga/raw/exec")
(defun img-avail-raw () (amiga.raw.exec:avail-mem 0))
(format t "IMAGE-LIBCALL-BEFORE-SAVE ~a~%" (plusp (img-avail-raw)))

(format t "IMAGE-STATE-BUILT fib10=~a dpd=~a~%" (img-fib 10) *img-saved-dpd*)
(ext:save-image "build/amiga/amilisp-test.img" :quit t :shake-bindings t)
