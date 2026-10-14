;;; bench-prims.lisp — per-primitive cost table (ns per operation).
;;;
;;; The regression harness for specs/performance.md Tier 4 ("2x vs ECL").
;;; One row per operation the sento per-message path executes (calls,
;;; generic dispatch, slot access, dynamic binding, unwind-protect /
;;; handler-case, locks, allocation), each timed in a tight loop with the
;;; empty-loop cost subtracted.  The file is portable — the same rows run
;;; unchanged on amilisp, ECL and SBCL — so every row carries its own
;;; native-compiler reference point.  Baselines: docs/benchmarks.md
;;; 2026-09-08.
;;;
;;; Design:
;;;   - Rows are NET of the empty-loop baseline (printed at the end); the
;;;     absolute cost of a row is `net + baseline`.  Small negative values
;;;     are loop-overhead noise, i.e. "free".
;;;   - Every row assigns its result to ACC and the last value is stored in
;;;     *SINK*, so a native compiler cannot dead-code-eliminate the body.
;;;   - Callees are declared NOTINLINE so a call really is a call.
;;;   - (optimize (speed 1) (safety 1) (debug 1)) everywhere: the settings
;;;     third-party code (sento) is compiled with by default.  Re-run
;;;     amilisp with CLAMIGA_FORCE_SPEED=3 to see the peephole's effect
;;;     (2026-09-08: none — see the spec).
;;;   - Output is one machine-parseable line per row:
;;;       PRIM <name> ns=<net>
;;;
;;; Usage:
;;;   amilisp: ./build/host/amilisp --no-userinit --heap 64M --non-interactive \
;;;                --load trunk/bench-prims.lisp
;;;   ECL:     ecl --norc --eval '(progn (load (compile-file "trunk/bench-prims.lisp"
;;;                :output-file "/tmp/bench-prims.fas")) (quit))'
;;;            (--load of the source would run ECL's bytecode interpreter;
;;;             the reference point is its native compile-file output)
;;;   SBCL:    sbcl --non-interactive --load trunk/bench-prims.lisp
;;;   Amiga:   (defparameter cl-user::*bp-n* 20000) before loading.

(defvar cl-user::*bp-n* 1000000
  "Iterations per row.  Scale down on the Amiga (20000 is plenty).")

(defpackage :bench-prims (:use :cl))
(in-package :bench-prims)

(declaim (optimize (speed 1) (safety 1) (debug 1)))

(defvar *n* cl-user::*bp-n*)
(defvar *results* nil)
(defvar *empty-ns* 0d0)
(defvar *d* 0)
(defvar *sink* nil)

(defun ns-per-iter (t0 t1 n)
  (/ (* 1d9 (- t1 t0)) internal-time-units-per-second n))

(defmacro defbench (name (&key (n '*n*) (setup nil)) &body body)
  (let ((fname (intern (format nil "BENCH-~A" (string-upcase name)))))
    `(progn
       (defun ,fname ()
         (let* (,@setup)
           (let ((n ,n) (acc nil) (t0 0) (t1 0))
             (setq t0 (get-internal-real-time))
             (dotimes (i n)
               (setq acc (progn ,@body)))
             (setq t1 (get-internal-real-time))
             (setq *sink* acc)
             (- (ns-per-iter t0 t1 n) *empty-ns*))))
       (push (cons ,name (,fname)) *results*)
       (format t "PRIM ~32A ns=~,1F~%" ,name (cdar *results*))
       (finish-output))))

(declaim (notinline f1 f3 fk fr fv))
(defun f1 (x) x)
(defun f3 (x y z) (if x y z))
(defun fk (x &key a b c) (if a x (if b c x)))
(defun fr (x &rest r) (if r x r))
(defun fv (x) (values x x))

(defclass thing ()
  ((a :initarg :a :initform 0 :accessor thing-a)
   (b :initarg :b :initform 0 :accessor thing-b)))
(defclass other (thing) ())
(defstruct st (a 0) (b 0))

(defgeneric g1 (x))
(defmethod g1 ((x thing)) x)
(defgeneric g2 (x y))
(defmethod g2 ((x thing) y) y)
(defmethod g2 ((x other) y) x)
(defgeneric g3 (x))
(defmethod g3 ((x thing)) x)
(defmethod g3 :around ((x thing)) (call-next-method))
(defgeneric g4 (x))
(defmethod g4 ((x t)) x)
(defmethod g4 ((x thing)) (call-next-method))

(format t "~%~A ~A, ~D iterations per row~%"
        (lisp-implementation-type) (lisp-implementation-version) *n*)

(defbench "empty-loop" () i)
(setq *empty-ns* (cdar *results*))

;; --- calls ---
(defbench "fixnum-add" () (+ i 1))
(defbench "call-1arg" () (f1 i))
(defbench "call-3arg" () (f3 i i i))
(defbench "call-&key-2of3" () (fk i :a i :b i))
(defbench "call-&rest-2" () (fr i i i))
(defbench "call-mvbind" () (multiple-value-bind (a b) (fv i) (if a b a)))
(defbench "flet-call" () (flet ((h (x) (if x i x))) (h i)))
(defbench "funcall-closure" (:setup ((k 1) (clo (lambda (x) (+ x k))))) (funcall clo i))
(defbench "closure-cell-incf" (:setup ((k 0) (inc (lambda () (incf k))))) (funcall inc))
(defbench "apply-3list" (:setup ((lst (list 1 2 3)))) (apply #'f3 lst))
;; --- generic dispatch ---
(defbench "gf-1arg-1method" (:setup ((o (make-instance 'thing)))) (g1 o))
(defbench "gf-2arg-2methods" (:setup ((o (make-instance 'thing)))) (g2 o i))
(defbench "gf-around+primary" (:setup ((o (make-instance 'thing)))) (g3 o))
(defbench "gf-call-next-method" (:setup ((o (make-instance 'thing)))) (g4 o))
;; --- slot access ---
(defbench "accessor-read" (:setup ((o (make-instance 'thing :a 1)))) (thing-a o))
(defbench "accessor-write" (:setup ((o (make-instance 'thing)))) (setf (thing-a o) i))
(defbench "slot-value-read" (:setup ((o (make-instance 'thing :a 1)))) (slot-value o 'a))
(defbench "slot-value-write" (:setup ((o (make-instance 'thing)))) (setf (slot-value o 'a) i))
(defbench "with-slots-incf" (:setup ((o (make-instance 'thing)))) (with-slots (a) o (incf a)))
(defbench "struct-read" (:setup ((s (make-st :a 1)))) (st-a s))
(defbench "struct-write" (:setup ((s (make-st)))) (setf (st-a s) i))
(defbench "struct-push-pop" (:setup ((s (make-st)))) (progn (push i (st-a s)) (pop (st-a s))))
;; --- allocation ---
(defbench "make-instance-2init" (:n (floor *n* 4)) (make-instance 'thing :a i :b i))
(defbench "make-struct-2init" () (make-st :a i :b i))
(defbench "cons" () (cons i i))
(defbench "list-4" () (list i i i i))
(defbench "closure-alloc" () (let ((j i)) (lambda () j)))
;; --- specials, non-local exits ---
(defbench "special-read" () *d*)
(defbench "special-bind" () (let ((*d* i)) *d*))
(defbench "handler-case" () (handler-case (f1 i) (error () nil)))
(defbench "handler-bind" () (handler-bind ((error #'identity)) (f1 i)))
(defbench "unwind-protect" () (unwind-protect (f1 i) (setq *sink* nil)))
(defbench "catch-throw" () (catch 'tag (throw 'tag i)))
;; --- misc builtins ---
(defbench "typep-class" (:setup ((o (make-instance 'other)))) (typep o 'thing))
(defbench "case-keyword" () (case (if (evenp i) :a :b) (:stop 1) (:a 2) (t 3)))
(defbench "gethash-eq" (:setup ((h (let ((h (make-hash-table :test 'eq)))
                                     (setf (gethash 'k h) 1) h))))
  (gethash 'k h))
(defbench "svref" (:setup ((v (make-array 16 :initial-element 0)))) (svref v (logand i 15)))
;; --- locks ---
(defbench "lock-acquire-release" (:setup ((l (mp:make-lock))))
  (#+ecl mp:with-lock #+clamiga mp:with-lock-held #+sbcl sb-thread:with-mutex (l) i))
(defbench "lock+condvar-notify" (:setup ((l (mp:make-lock)) (cv (mp:make-condition-variable))))
  (#+ecl mp:with-lock #+clamiga mp:with-lock-held #+sbcl sb-thread:with-mutex (l)
    #+ecl (mp:condition-variable-signal cv)
    #+clamiga (mp:condition-notify cv)
    #+sbcl (sb-thread:condition-notify cv)
    i))

(format t "~%empty-loop baseline: ~,1F ns (rows above are net of it)~%" *empty-ns*)
(format t "sink: ~A~%" (type-of *sink*))
