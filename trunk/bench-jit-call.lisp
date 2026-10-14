;;; bench-jit-call.lisp -- the per-call cost of the m68k JIT's call paths.
;;;
;;; The phase-0 spike of the Clamacs-in-Lisp port (clamacs/specs/
;;; clamacs-lisp.md) found that the JIT makes call-heavy generic code
;;; SLOWER than the bytecode VM (per key 1.04 vs 0.71 ms on a Vampire V4)
;;; while a declared arithmetic loop gains 2x.  This file isolates the
;;; call shapes such code is made of and times each one under both JIT
;;; states, so the dispatch path is the only difference:
;;;
;;;   - a call to a C builtin (LOGTEST, GETHASH, ...: the bulk of any
;;;     generic function body -- there are no opcodes for these),
;;;   - a call to a Lisp function that is native / bytecode / &optional /
;;;     &rest / &key (native on both backends since 2026-10-03), with the callee PINNED
;;;     to one state so only the caller's path changes,
;;;   - FUNCALL through a variable, a local BLOCK/RETURN-FROM, a fixnum
;;;     CASE, an FFI peek, and a decode-key-like mix of all of them.
;;;
;;; Each row prints bytecode vs JIT milliseconds and the per-iteration
;;; cost in microseconds; "BENCH-JIT-CALL" lines are grep-friendly.  The
;;; loop driver is re-defined under each JIT state (as in bench.lisp), so
;;; its body is native in the JIT column and interpreted in the other.
;;;
;;; Usage (Amiga; on a host build every row reports the same two numbers
;;; because the JIT is compiled out):
;;;   amilisp --no-userinit --heap 8M --non-interactive --load trunk/bench-jit-call.lisp
;;;   (defparameter cl-user::*bjc-n* 50000) first to rescale on a slow box.

(defvar *bjc-n* 200000
  "Iterations per row.  200k is ~1-2 s per row on a real 68040.")

(defvar *bjc-h* (make-hash-table))
(dotimes (i 16) (setf (gethash i *bjc-h*) (* i 3)))

(defvar *bjc-ptr* (ffi:alloc-foreign 16))
(ffi:poke-u16 *bjc-ptr* #x4C 0)

;;; --- callees pinned to one state, whatever the caller's ---

(defvar *bjc-jit-was* (clamiga::%jit-active-p))

(clamiga::%jit-set-active nil)
(defun bjc-bc-add (a b) (+ a b))                ; always bytecode
(defun bjc-bc-key (code mods) (logior code (ash mods 16)))

(clamiga::%jit-set-active t)
(defun bjc-nat-add (a b) (+ a b))               ; native when the JIT is built in
(defun bjc-nat-key (code mods) (logior code (ash mods 16)))
(defun bjc-opt-add (a &optional (b 1)) (+ a b)) ; native: the &optional prologue
(defun bjc-rest-add (a &rest r) (+ a (car r)))   ; native: the &rest prologue
(defun bjc-key-add (a &key (b 1)) (+ a b))       ; native: the &key prologue

(clamiga::%jit-set-active *bjc-jit-was*)

(defvar *bjc-fn* #'bjc-nat-add)

;;; --- driver ---

(defun bjc-time (thunk)
  (let ((t0 (get-internal-real-time))
        (g0 (clamiga::%get-gc-count)))
    (let ((r (funcall thunk)))
      (values r
              (round (* 1000 (- (get-internal-real-time) t0))
                     internal-time-units-per-second)
              (- (clamiga::%get-gc-count) g0)))))

(defun bjc-compare (label defining-form thunk)
  (clamiga::%jit-set-active nil)
  (eval defining-form)
  (funcall thunk)
  (multiple-value-bind (r-bc t-bc g-bc) (bjc-time thunk)
    (clamiga::%jit-set-active t)
    (eval defining-form)
    (funcall thunk)
    (multiple-value-bind (r-jit t-jit g-jit) (bjc-time thunk)
      (clamiga::%jit-set-active *bjc-jit-was*)
      (format t "BENCH-JIT-CALL ~28A bc=~6D ms  jit=~6D ms  per-iter bc=~6,2F us jit=~6,2F us  gc=~D/~D~A~%"
              label t-bc t-jit
              (/ (* t-bc 1000.0) *bjc-n*) (/ (* t-jit 1000.0) *bjc-n*)
              g-bc g-jit
              (if (equal r-bc r-jit) "" (format nil "  MISMATCH bc=~A jit=~A" r-bc r-jit))))))

(defmacro bjc-bench (label defining-form call-form)
  `(bjc-compare ,label ',defining-form (lambda () ,call-form)))

(format t "~&=== bench-jit-call: ~D iterations per row, ~A ===~%"
        *bjc-n* (lisp-implementation-version))

;;; --- rows ---

(bjc-bench "loop only"
  (defun bjc-r0 (n) (let ((s 0)) (dotimes (i n) (setq s (+ s 1))) s))
  (bjc-r0 *bjc-n*))

(bjc-bench "builtin 2-arg (logtest)"
  (defun bjc-r1 (n) (let ((s 0)) (dotimes (i n) (when (logtest i 4) (setq s (+ s 1)))) s))
  (bjc-r1 *bjc-n*))

(bjc-bench "builtin 1-arg (hash-table-p)"
  (defun bjc-r2 (n h) (let ((s 0)) (dotimes (i n) (when (hash-table-p h) (setq s (+ s 1)))) s))
  (bjc-r2 *bjc-n* *bjc-h*))

(bjc-bench "builtin gethash"
  (defun bjc-r3 (n h) (let ((s 0)) (dotimes (i n) (setq s (+ s (gethash (logand i 15) h 0)))) s))
  (bjc-r3 *bjc-n* *bjc-h*))

(bjc-bench "call bytecode leaf"
  (defun bjc-r4 (n) (let ((s 0)) (dotimes (i n) (setq s (bjc-bc-add s 1))) s))
  (bjc-r4 *bjc-n*))

(bjc-bench "call native leaf"
  (defun bjc-r5 (n) (let ((s 0)) (dotimes (i n) (setq s (bjc-nat-add s 1))) s))
  (bjc-r5 *bjc-n*))

(bjc-bench "call &optional leaf"
  (defun bjc-r6 (n) (let ((s 0)) (dotimes (i n) (setq s (bjc-opt-add s))) s))
  (bjc-r6 *bjc-n*))

;; One extra argument: a cons per call, in both columns.  On the 68040 a
;; cons costs more the longer the process has consed, interpreted or native
;; (2026-10-03: 5 -> 70 us per consing call within one run, not reset by GC
;; or compaction), so this row's JIT column, timed second, reads high.
(bjc-bench "call &rest leaf"
  (defun bjc-r14 (n) (let ((s 0)) (dotimes (i n) (setq s (bjc-rest-add s 1))) s))
  (bjc-r14 *bjc-n*))

(bjc-bench "call &key leaf"
  (defun bjc-r15 (n) (let ((s 0)) (dotimes (i n) (setq s (bjc-key-add s :b 1))) s))
  (bjc-r15 *bjc-n*))

(bjc-bench "call same-state leaf"
  (progn (defun bjc-same-add (a b) (+ a b))
         (defun bjc-r7 (n) (let ((s 0)) (dotimes (i n) (setq s (bjc-same-add s 1))) s)))
  (bjc-r7 *bjc-n*))

;; Tail calls between two functions, in chains of 10 (a chain as deep as
;; the iteration count would need one frame per call where they are
;; calls).  Per iteration: one tail call plus a tenth of the outer call.
(bjc-bench "tail call ping-pong (x10)"
  (progn (defun bjc-tp-a (k s) (if (= k 0) s (bjc-tp-b (- k 1) (+ s 1))))
         (defun bjc-tp-b (k s) (if (= k 0) s (bjc-tp-a (- k 1) (+ s 1))))
         (defun bjc-r16 (n) (let ((s 0)) (dotimes (i (floor n 10)) (setq s (+ s (bjc-tp-a 10 0)))) s)))
  (bjc-r16 *bjc-n*))

(bjc-bench "funcall native leaf"
  (defun bjc-r8 (n f) (let ((s 0)) (dotimes (i n) (setq s (funcall f s 1))) s))
  (bjc-r8 *bjc-n* *bjc-fn*))

(bjc-bench "local block/return-from"
  (defun bjc-r9 (n) (let ((s 0)) (dotimes (i n) (setq s (+ s (block b (when (evenp i) (return-from b 2)) 1)))) s))
  (bjc-r9 *bjc-n*))

(bjc-bench "fixnum case (8 keys)"
  (defun bjc-r10 (n) (let ((s 0)) (dotimes (i n) (setq s (+ s (case (logand i 7) (0 1) (1 2) (2 3) (3 4) (4 5) (5 6) (6 7) (t 8))))) s))
  (bjc-r10 *bjc-n*))

(bjc-bench "ffi peek-u16"
  (defun bjc-r11 (n p) (let ((s 0)) (dotimes (i n) (setq s (+ s (ffi:peek-u16 p 0)))) s))
  (bjc-r11 *bjc-n* *bjc-ptr*))

;;; decode-key from the spike, minus MapRawKey: three LOGTESTs, a
;;; qualifier decode of three more, a CASE on the raw code, a range
;;; test, a peek and a 2-arg helper call -- the per-key mix.
(bjc-bench "decode-key mix (native helper)"
  (defun bjc-r12 (n p)
    (let ((s 0))
      (dotimes (i n)
        (let ((code (logand i 127)) (qual (logand (ash i -7) 255)))
          (unless (or (logtest code 128) (logtest qual 192))
            (let ((mods (logior (if (logtest qual 8) 1 0)
                                (if (logtest qual 48) 2 0)
                                (if (logtest qual 3) 4 0))))
              (setq s (+ s (case code
                             (#x4C (bjc-nat-key 1 mods))
                             (#x4D (bjc-nat-key 2 mods))
                             (#x4E (bjc-nat-key 3 mods))
                             (#x4F (bjc-nat-key 4 mods))
                             (t (if (<= #x50 code #x59)
                                    (bjc-nat-key (+ 10 (- code #x50)) mods)
                                    (bjc-nat-key (ffi:peek-u16 p 0) mods))))))))))
      s))
  (bjc-r12 *bjc-n* *bjc-ptr*))

(bjc-bench "decode-key mix (bytecode helper)"
  (defun bjc-r13 (n p)
    (let ((s 0))
      (dotimes (i n)
        (let ((code (logand i 127)) (qual (logand (ash i -7) 255)))
          (unless (or (logtest code 128) (logtest qual 192))
            (let ((mods (logior (if (logtest qual 8) 1 0)
                                (if (logtest qual 48) 2 0)
                                (if (logtest qual 3) 4 0))))
              (setq s (+ s (case code
                             (#x4C (bjc-bc-key 1 mods))
                             (#x4D (bjc-bc-key 2 mods))
                             (#x4E (bjc-bc-key 3 mods))
                             (#x4F (bjc-bc-key 4 mods))
                             (t (if (<= #x50 code #x59)
                                    (bjc-bc-key (+ 10 (- code #x50)) mods)
                                    (bjc-bc-key (ffi:peek-u16 p 0) mods))))))))))
      s))
  (bjc-r13 *bjc-n* *bjc-ptr*))

(format t "=== bench-jit-call end ===~%")
(finish-output)
