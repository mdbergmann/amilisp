;;; bench-opt.lisp — optimization-tracking micro-benchmark suite.
;;;
;;; One benchmark (or a before/after pair) per PENDING item in
;;; specs/performance.md, so each optimization's gain can be captured as
;;; a like-for-like delta against the baselines in docs/benchmarks.md:
;;;
;;;   1.3  declaim-speed (const folding, dead branches, check elision)
;;;        -> opt.const-fold, opt.dead-branch, safety0.* vs safety1.*
;;;   1.8  bytecode peephole post-pass
;;;        -> vm.* (store/load, dead code), opt.dead-branch
;;;   2.4  set operations as C builtins
;;;        -> set.*
;;;   2.5  free-list size class segregation
;;;        -> alloc.*
;;;   3.1  CLOS slot access optimization
;;;        -> clos.slot-value, clos.accessor, struct.*
;;;   3.2  keyword argument pre-computation
;;;        -> kw.call-8keys, clos.make-instance
;;;   3.3  cl_mv_count write reduction
;;;        -> vm.* (every simple opcode writes cl_mv_count today)
;;;
;;; Design:
;;;   - All functions are compiled with the JIT OFF (pure bytecode), so
;;;     VM/compiler changes are measured directly and results are
;;;     comparable across platforms.  trunk/bench.lisp is the suite for
;;;     JIT-vs-bytecode comparisons.
;;;   - Every benchmark has a closed-form expected result computed
;;;     independently of the code under test; a mismatch prints
;;;     BENCH-FAIL — an optimization that breaks semantics fails loudly.
;;;   - Output is one machine-parseable line per benchmark:
;;;       BENCH <name> ms=<best-of-N> bytes=<consed> gc=<cycles> check=<digest> <ok|FAIL>
;;;   - Inner loop sizes are FIXED (expected values stay valid);
;;;     *bo-scale* multiplies only the repetition counts.
;;;
;;; Usage:
;;;   host:   ./build/host/amilisp --heap 64M --load trunk/bench-opt.lisp
;;;   Amiga:  set the knobs before loading, e.g.:
;;;             (defparameter cl-user::*bo-scale* 1/20)
;;;             (defparameter cl-user::*bo-set-size* 150)
;;;             (load "trunk/bench-opt.lisp")

(defvar *bo-scale* 1
  "Multiplier applied to every repetition count (not to inner sizes).")
(defvar *bo-set-size* 600
  "Element count of the large set-operation lists (must be even).")
(defvar *bo-runs* 3
  "Timed runs per benchmark; the best (minimum) time is reported.")

(defvar *bo-total-ms* 0)
(defvar *bo-fails* 0)

(defun %bo-scaled (n) (max 1 (round (* n *bo-scale*))))

;; Sum of the integers in [0, n): n(n-1)/2.
(defun %bo-sum-below (n) (/ (* n (1- n)) 2))
;; Sum of the integers in [a, b).
(defun %bo-sum-range (a b) (- (%bo-sum-below b) (%bo-sum-below a)))

(defun %bo-iota (n &optional (start 0))
  "Ascending list (start start+1 ... start+n-1)."
  (let ((acc nil))
    (dotimes (i n)
      (push (+ start (- n 1 i)) acc))
    acc))

(defun %bo-digest (lst &optional key)
  "Order-independent digest of a list: (count . sum-of-keyed-elements).
Set-operation result order is unspecified by the HyperSpec, so
benchmarks compare digests, not lists."
  (let ((c 0) (s 0))
    (dolist (x lst)
      (setq c (+ c 1))
      (setq s (+ s (if key (funcall key x) x))))
    (cons c s)))

(defun %bo-time-call (thunk)
  "Run THUNK once; return (values result elapsed-ms bytes-consed gc-cycles)."
  (let ((t0 (clamiga::%get-internal-time))
        (b0 (clamiga::%get-bytes-consed))
        (g0 (clamiga::%get-gc-count)))
    (let ((r (funcall thunk)))
      (values r
              (- (clamiga::%get-internal-time) t0)
              (- (clamiga::%get-bytes-consed) b0)
              (- (clamiga::%get-gc-count) g0)))))

(defun %bo-fmt-check (v)
  (if (consp v)
      (format nil "~D:~D" (car v) (cdr v))
      (format nil "~A" v)))

(defun %bo-run (name expected thunk)
  "Warm up once, then time *BO-RUNS* runs of THUNK; report the best.
Verify the (deterministic) result against EXPECTED."
  (funcall thunk)                       ; warmup
  (let ((best-ms nil) (best-b 0) (best-g 0) (result nil) (steady t))
    (dotimes (k *bo-runs*)
      (multiple-value-bind (r ms b g) (%bo-time-call thunk)
        (when (and (> k 0) (not (equal r result)))
          (setq steady nil))
        (setq result r)
        (when (or (null best-ms) (< ms best-ms))
          (setq best-ms ms best-b b best-g g))))
    (setq *bo-total-ms* (+ *bo-total-ms* best-ms))
    (let ((ok (and steady (equal result expected))))
      (unless ok (setq *bo-fails* (+ *bo-fails* 1)))
      (format t "BENCH ~A ms=~D bytes=~D gc=~D check=~A ~A~%"
              name best-ms best-b best-g (%bo-fmt-check result)
              (if ok "ok" "FAIL"))
      (unless ok
        (format t "BENCH-FAIL ~A expected=~A got=~A~A~%"
                name (%bo-fmt-check expected) (%bo-fmt-check result)
                (if steady "" " (nondeterministic across runs)"))))))

;; ---------------------------------------------------------------------
;; Compile everything below as pure bytecode; restore the JIT setting at
;; the end of the file.

(defvar *bo-prior-jit* (clamiga::%jit-active-p))
(clamiga::%jit-set-active nil)

(format t "~%BENCH-META platform=~A scale=~A set-size=~D runs=~D engine=bytecode~%"
        #+amigaos "amiga" #-amigaos "host"
        *bo-scale* *bo-set-size* *bo-runs*)

;; =====================================================================
;; vm.* — VM dispatch-loop hot paths (spec 3.3 mv_count writes, 1.8
;; peephole).  Every arithmetic/load/store opcode currently writes
;; cl_mv_count = 1; these tight loops make that write a measurable
;; fraction of the work.

;; Fixnum accumulate: OP_LOAD/OP_ADD/OP_STORE per iteration.  Inner sum
;; stays comfortably inside fixnum range and resets per rep.
(defun %bo-fixnum-loop (n reps)
  (let ((s 0))
    (dotimes (r reps)
      (setq s 0)
      (dotimes (i n)
        (setq s (+ s i))))
    s))

;; Local shuffle: pure load/store traffic (peephole store-then-reload
;; target).  N is a multiple of 3, so the rotation returns to identity.
(defun %bo-local-shuffle (n reps)
  (let ((a 1) (b 2) (c 3) (tmp 0))
    (dotimes (r reps)
      (setq a 1 b 2 c 3)
      (dotimes (i n)
        (setq tmp a)
        (setq a b)
        (setq b c)
        (setq c tmp)))
    (+ a (* 10 b) (* 100 c))))

;; Call/return overhead at default safety (arg-count check per call).
(defun %bo-inc (x) (+ x 1))
(defun %bo-call-return (n reps)
  (let ((s 0))
    (dotimes (r reps)
      (setq s 0)
      (dotimes (i n)
        (setq s (+ s (%bo-inc i)))))
    s))

(let ((n 20000) (reps (%bo-scaled 100)))
  (%bo-run "vm.fixnum-loop" (%bo-sum-below n)
           (lambda () (%bo-fixnum-loop n reps))))

(let ((n 21000) (reps (%bo-scaled 60)))
  (%bo-run "vm.local-shuffle" 321
           (lambda () (%bo-local-shuffle n reps))))

(let ((n 10000) (reps (%bo-scaled 100)))
  (%bo-run "vm.call-return" (+ (%bo-sum-below n) n)
           (lambda () (%bo-call-return n reps))))

;; =====================================================================
;; mt.* — the same VM hot paths run by 8 threads at once.  Single-threaded
;; numbers cannot see a per-call write to shared memory (a process-wide
;; counter, a global inline cache, ...): every thread then bounces one
;; cache line on every call and the 8-thread wall time explodes while the
;; 1-thread figure is unchanged.  That exact bug cost the sento matrix
;; 10–27% in the 0.8 cycle (docs/sento-bench-results-0.8.md) — the
;; Ctrl-C break-poll counter was a static.  Healthy: mt.call-x8 is within
;; ~1.5x of vm.call-return's ms (same total work, spread over 8 cores);
;; a shared write shows as 3–4x.

(defun %bo-mt-run (nthreads fn n)
  "Run (FN N) on NTHREADS threads; return the sum of their results."
  (let* ((results (make-array nthreads :initial-element 0))
         (threads (let ((acc nil))
                    (dotimes (k nthreads)
                      (let ((k k))
                        (push (mp:make-thread
                               (lambda () (setf (svref results k) (funcall fn n)))
                               :name "bo-mt")
                              acc)))
                    acc)))
    (dolist (th threads) (mp:join-thread th))
    (let ((s 0))
      (dotimes (k nthreads) (setq s (+ s (svref results k))))
      s)))

(defvar *bo-mt-special* 0)
(defun %bo-mt-dynbind (n reps)
  (let ((s 0))
    (dotimes (r reps)
      (setq s 0)
      (dotimes (i n)
        (let ((*bo-mt-special* i))
          (setq s (+ s *bo-mt-special*)))))
    s))

(let ((n 10000) (reps (%bo-scaled 100)))
  (%bo-run "mt.call-x8" (* 8 (+ (%bo-sum-below n) n))
           (lambda () (%bo-mt-run 8 (lambda (n) (%bo-call-return n reps)) n))))

(let ((n 10000) (reps (%bo-scaled 100)))
  (%bo-run "mt.dynbind-x8" (* 8 (%bo-sum-below n))
           (lambda () (%bo-mt-run 8 (lambda (n) (%bo-mt-dynbind n reps)) n))))

;; =====================================================================
;; opt.* — emit-time optimization targets (spec 1.3).

;; Constant subexpressions: today each (* 3 4), (- 100 58), (ash 1 5),
;; (logand 12 10), (logior 1 2) emits CONST/CONST/op; after constant
;; folding each becomes a single CONST.  Adds 97 per iteration.
(defun %bo-const-fold (n reps)
  (let ((s 0))
    (dotimes (r reps)
      (setq s 0)
      (dotimes (i n)
        (setq s (+ s (* 3 4) (- 100 58) (ash 1 5) (logand 12 10) (logior 1 2)))))
    s))

;; Constant-test branches (the shape macro expansions produce): today
;; each emits test + conditional jump + both arms; after dead-branch
;; elimination only the live arm remains.  Adds 6 per iteration.
(defun %bo-dead-branch (n reps)
  (let ((s 0))
    (dotimes (r reps)
      (setq s 0)
      (dotimes (i n)
        (setq s (+ s
                   (if t 1 99)
                   (if nil 98 2)
                   (if t 3 97)))))
    s))

(let ((n 10000) (reps (%bo-scaled 30)))
  (%bo-run "opt.const-fold" (* 97 n)
           (lambda () (%bo-const-fold n reps))))

(let ((n 10000) (reps (%bo-scaled 60)))
  (%bo-run "opt.dead-branch" (* 6 n)
           (lambda () (%bo-dead-branch n reps))))

;; =====================================================================
;; safety0.* vs safety1.* — safety-gated check elision (spec 1.3).
;; Identical function bodies compiled under (safety 0) and (safety 1)
;; via PROCLAIM (deterministic today and after local-declare scoping
;; lands).  Today only OP_ASSERT_TYPE from `the` is safety-gated, so the
;; pairs should time ~equal; after 1.3 (bounds/arg-count check elision)
;; the safety0 variant should pull ahead.

(proclaim '(optimize (safety 0)))

(defun %bo-svref-sum-s0 (v n reps)
  (let ((s 0))
    (dotimes (r reps)
      (setq s 0)
      (dotimes (j n)
        (setq s (+ s (svref v j)))))
    s))

(defun %bo-add3-s0 (a b c) (+ a b c))
(defun %bo-call3-s0 (n reps)
  (let ((s 0))
    (dotimes (r reps)
      (setq s 0)
      (dotimes (i n)
        (setq s (+ s (%bo-add3-s0 i 1 2)))))
    s))

(proclaim '(optimize (safety 1)))

(defun %bo-svref-sum-s1 (v n reps)
  (let ((s 0))
    (dotimes (r reps)
      (setq s 0)
      (dotimes (j n)
        (setq s (+ s (svref v j)))))
    s))

(defun %bo-add3-s1 (a b c) (+ a b c))
(defun %bo-call3-s1 (n reps)
  (let ((s 0))
    (dotimes (r reps)
      (setq s 0)
      (dotimes (i n)
        (setq s (+ s (%bo-add3-s1 i 1 2)))))
    s))

(let* ((n 1000) (reps (%bo-scaled 1000))
       (v (make-array n)))
  (dotimes (j n) (setf (svref v j) j))
  (%bo-run "safety1.svref-loop" (%bo-sum-below n)
           (lambda () (%bo-svref-sum-s1 v n reps)))
  (%bo-run "safety0.svref-loop" (%bo-sum-below n)
           (lambda () (%bo-svref-sum-s0 v n reps))))

(let ((n 10000) (reps (%bo-scaled 60)))
  (%bo-run "safety1.call-args" (+ (%bo-sum-below n) (* 3 n))
           (lambda () (%bo-call3-s1 n reps)))
  (%bo-run "safety0.call-args" (+ (%bo-sum-below n) (* 3 n))
           (lambda () (%bo-call3-s0 n reps))))

;; =====================================================================
;; set.* — set operations (spec 2.4: today pure Lisp O(n*m) in
;; boot.lisp; planned C builtins with hash-set path for large inputs).
;; Inputs are duplicate-free integer ranges with 50% overlap, so the
;; result digests have closed forms regardless of result order.

;; Small lists: measures per-call/interpreter overhead.
(let* ((l1 (%bo-iota 16))               ; 0..15
       (l2 (%bo-iota 16 8))             ; 8..23
       (reps (%bo-scaled 3000)))
  (%bo-run "set.intersection-small"
           (cons 8 (%bo-sum-range 8 16))
           (lambda ()
             (let ((d nil))
               (dotimes (r reps)
                 (setq d (%bo-digest (intersection l1 l2))))
               d)))
  (%bo-run "set.union-small"
           (cons 24 (%bo-sum-below 24))
           (lambda ()
             (let ((d nil))
               (dotimes (r reps)
                 (setq d (%bo-digest (union l1 l2))))
               d))))

;; Large lists: measures the O(n*m) -> O(n+m) algorithmic win.
(let* ((m *bo-set-size*)                ; even
       (h (/ m 2))
       (l1 (%bo-iota m))                ; 0..m-1
       (l2 (%bo-iota m h))              ; h..m+h-1
       (lsub (%bo-iota h))              ; 0..h-1 (subset of l1)
       (reps (%bo-scaled 5))
       (sub-reps (%bo-scaled 30)))
  (%bo-run "set.intersection-large"
           (cons h (%bo-sum-range h m))
           (lambda ()
             (let ((d nil))
               (dotimes (r reps)
                 (setq d (%bo-digest (intersection l1 l2))))
               d)))
  (%bo-run "set.union-large"
           (cons (+ m h) (%bo-sum-below (+ m h)))
           (lambda ()
             (let ((d nil))
               (dotimes (r reps)
                 (setq d (%bo-digest (union l1 l2))))
               d)))
  (%bo-run "set.difference-large"
           (cons h (%bo-sum-below h))
           (lambda ()
             (let ((d nil))
               (dotimes (r reps)
                 (setq d (%bo-digest (set-difference l1 l2))))
               d)))
  (%bo-run "set.subsetp-large" t
           (lambda ()
             (let ((d nil))
               (dotimes (r sub-reps)
                 (setq d (and (subsetp lsub l1) t)))
               d))))

;; :test #'equal — fresh strings so EQ fails and the equality function
;; is actually exercised.  All strings are length 5 ("s0042").
(let* ((m 200)
       (h (/ m 2))
       (l1 (let ((acc nil))
             (dotimes (i m) (push (format nil "s~4,'0D" i) acc))
             acc))
       (l2 (let ((acc nil))
             (dotimes (i m) (push (format nil "s~4,'0D" (+ h i)) acc))
             acc))
       (reps (%bo-scaled 20)))
  (%bo-run "set.intersection-equal"
           (cons h (* 5 h))
           (lambda ()
             (let ((d nil))
               (dotimes (r reps)
                 (setq d (%bo-digest (intersection l1 l2 :test #'equal)
                                     #'length)))
               d))))

;; :key #'car on alists.
(let* ((m 300)
       (h (/ m 2))
       (l1 (let ((acc nil))
             (dotimes (i m) (push (cons i 'a) acc))
             acc))
       (l2 (let ((acc nil))
             (dotimes (i m) (push (cons (+ h i) 'b) acc))
             acc))
       (reps (%bo-scaled 10)))
  (%bo-run "set.intersection-key"
           (cons h (%bo-sum-range h m))
           (lambda ()
             (let ((d nil))
               (dotimes (r reps)
                 (setq d (%bo-digest (intersection l1 l2 :key #'car)
                                     #'car)))
               d))))

;; =====================================================================
;; kw.* — keyword-argument matching (spec 3.2: O(n_keys * n_supplied)
;; scan per call today; planned per-function keyword map).

(defun %bo-kw8 (a &key (k1 0) (k2 0) (k3 0) (k4 0)
                       (k5 0) (k6 0) (k7 0) (k8 0))
  (+ a k1 k2 k3 k4 k5 k6 k7 k8))

(let ((n 10000) (reps (%bo-scaled 40)))
  (%bo-run "kw.call-8keys" (+ (%bo-sum-below n) (* 10 n))
           (lambda ()
             (let ((s 0))
               (dotimes (r reps)
                 (setq s 0)
                 (dotimes (i n)
                   (setq s (+ s (%bo-kw8 i :k2 1 :k5 2 :k7 3 :k8 4)))))
               s))))

;; =====================================================================
;; clos.* — CLOS slot access (spec 3.1) and make-instance initarg
;; processing (specs 3.1 + 3.2).

(defclass %bo-point ()
  ((x :initarg :x :accessor %bo-point-x)
   (y :initarg :y :accessor %bo-point-y)))

(let ((n 1000) (reps (%bo-scaled 20)))
  (%bo-run "clos.make-instance" (+ (* 2 (%bo-sum-below n)) n)
           (lambda ()
             (let ((s 0))
               (dotimes (r reps)
                 (setq s 0)
                 (dotimes (i n)
                   (let ((p (make-instance '%bo-point :x i :y (+ i 1))))
                     (setq s (+ s (%bo-point-x p) (%bo-point-y p))))))
               s))))

(let ((n 10000) (reps (%bo-scaled 10))
      (p (make-instance '%bo-point :x 3 :y 4)))
  (%bo-run "clos.slot-value" (* 7 n)
           (lambda ()
             (let ((s 0))
               (dotimes (r reps)
                 (setq s 0)
                 (dotimes (i n)
                   (setq s (+ s (slot-value p 'x) (slot-value p 'y)))))
               s)))
  ;; Reader-GF fast dispatch: %BO-POINT-X/Y are DEFCLASS-generated readers, so
  ;; their GFs are promoted and OP_CALL answers the call straight from the
  ;; inline cache.  The metric to watch is clos.accessor / struct.accessor
  ;; (the constant-index floor below) — 1.20x as of 2026-07-09.
  (%bo-run "clos.accessor" (* 7 n)
           (lambda ()
             (let ((s 0))
               (dotimes (r reps)
                 (setq s 0)
                 (dotimes (i n)
                   (setq s (+ s (%bo-point-x p) (%bo-point-y p)))))
               s))))

;; Polymorphic accessor: the same reader GF called on alternating receiver
;; classes.  The IC holds up to 4 (TYPE-NAME . SLOT-INDEX) entries, so both
;; classes hit; with the old single-entry cache every alternation was a full
;; slow dispatch + %COMPUTE-APPLICABLE-METHODS (~80x per access, 2026-07-10).
;; Watch clos.accessor-poly staying close to clos.accessor.
(defclass %bo-point3 (%bo-point) ((z :initform 0)))

(let ((n 10000) (reps (%bo-scaled 10))
      (p (make-instance '%bo-point :x 3 :y 4))
      (q (make-instance '%bo-point3 :x 30 :y 40)))
  (%bo-run "clos.accessor-poly" (* 33 n)
           (lambda ()
             (let ((s 0))
               (dotimes (r reps)
                 (setq s 0)
                 (dotimes (i n)
                   (setq s (+ s (%bo-point-x p) (%bo-point-x q)))))
               s))))

;; Writer-GF fast dispatch (2026-07-10): (SETF %BO-POINT-X)/(SETF %BO-POINT-Y)
;; are DEFCLASS-generated writer methods, so their GFs are promoted and
;; OP_CALL's 2-arg probe stores straight through the inline cache (entries
;; carry the slot index negative-encoded to stay distinct from reader
;; entries).  Watch clos.accessor-write staying close to clos.accessor.
(let ((n 10000) (reps (%bo-scaled 10))
      (p (make-instance '%bo-point :x 0 :y 0)))
  (%bo-run "clos.accessor-write" (* 2 (%bo-sum-below n))
           (lambda ()
             (let ((s 0))
               (dotimes (r reps)
                 (setq s 0)
                 (dotimes (i n)
                   (setq s (+ s (setf (%bo-point-x p) i)
                              (setf (%bo-point-y p) i)))))
               s))))

;; Deep slot of a wide class (2026-07-10): SLOT-VALUE is compile-time
;; inlined (no Lisp call frame) and resolves through the (type, slot)
;; pair index, so a late slot costs the same as slot 0.  Before the pair
;; index the specs-list walk added ~0.75ns per slot position.  Watch
;; clos.slot-value-deep staying equal to clos.slot-value.
(defclass %bo-wide ()
  ((s0 :initform 0) (s1 :initform 1) (s2 :initform 2) (s3 :initform 3)
   (s4 :initform 4) (s5 :initform 5) (s6 :initform 6) (s7 :initform 7)
   (s8 :initform 8) (s9 :initform 9) (s10 :initform 10) (s11 :initform 11)))

(let ((n 10000) (reps (%bo-scaled 10))
      (p (make-instance '%bo-wide)))
  (%bo-run "clos.slot-value-deep" (* 21 n)
           (lambda ()
             (let ((s 0))
               (dotimes (r reps)
                 (setq s 0)
                 (dotimes (i n)
                   (setq s (+ s (slot-value p 's10) (slot-value p 's11)))))
               s))))

;; =====================================================================
;; struct.* — DEFSTRUCT slot access (spec 3.1).  The 2026-07-05 sento
;; runtime profile showed SLOT-VALUE on struct instances resolving the
;; slot on EVERY access: a linear walk of the struct registry alist
;; (find_struct_entry) plus a freshly consed slot-name list per access
;; (cl_struct_slot_names).  The clos.* benches above miss this path
;; entirely — CLOS instances take the slot-index-table hash path.
;;
;; The registry is PREPENDED on registration, so a struct defined right
;; here would sit at the list head and pay almost no walk.  Real
;; workloads access types defined EARLY (base conditions, library
;; structs) that sit behind hundreds of later registrations — so bury
;; %bo-node behind 256 filler registrations before timing it.

(defstruct %bo-node (x 0) (y 0))

(let ((filler-specs '((f1 nil) (f2 nil))))
  (dotimes (i 256)
    (clamiga::%register-struct-type
     (make-symbol (format nil "%BO-FILLER-~D" i)) 2 nil filler-specs)))

(let ((n 10000) (reps (%bo-scaled 10))
      (s (make-%bo-node :x 3 :y 4)))
  ;; Registry walk + name-list cons + linear name match per access.
  (%bo-run "struct.slot-value" (* 7 n)
           (lambda ()
             (let ((acc 0))
               (dotimes (r reps)
                 (setq acc 0)
                 (dotimes (i n)
                   (setq acc (+ acc (slot-value s 'x) (slot-value s 'y)))))
               acc)))
  ;; Reference point: defstruct accessors compile to a constant-index
  ;; %STRUCT-REF — the target cost for the slot-value path above.
  (%bo-run "struct.accessor" (* 7 n)
           (lambda ()
             (let ((acc 0))
               (dotimes (r reps)
                 (setq acc 0)
                 (dotimes (i n)
                   (setq acc (+ acc (%bo-node-x s) (%bo-node-y s)))))
               acc)))
  ;; TYPEP on a struct type: cl_is_struct_type / cl_struct_type_matches
  ;; do the same registry walk (211 leaf samples in the compile profile).
  (%bo-run "struct.typep" n
           (lambda ()
             (let ((acc 0))
               (dotimes (r reps)
                 (setq acc 0)
                 (dotimes (i n)
                   (setq acc (+ acc (if (typep s '%bo-node) 1 0)))))
               acc))))

;; =====================================================================
;; type.* — TYPEP symbol-spec resolution through the condition-hierarchy
;; and deftype-table hash indexes (CL_AlistIndex, 2026-07-14).  With 100
;; registered conditions + 100 deftypes, the pre-index linear walks made
;; every symbol TYPEP pay O(conditions) in cl_is_condition_type — even
;; when the answer was "not a condition type" — plus O(deftypes) in
;; cl_get_type_expander (eta-hab: one log4cl line printing an actor graph
;; ran one TYPEP per node and held the appender lock for ~60s).
(dotimes (i 100)
  (eval `(define-condition ,(intern (format nil "%BO-COND~D" i)) (error) ()))
  (eval `(deftype ,(intern (format nil "%BO-TYPE~D" i)) () 'integer)))
(let ((n 3000) (reps (%bo-scaled 10))
      (c (make-condition '%bo-cond0)))
  (%bo-run "type.typep-cond-deftype" (* 3 n)
           (lambda ()
             (let ((acc 0))
               (dotimes (r reps)
                 (setq acc 0)
                 (dotimes (i n)
                   (setq acc (+ acc
                                (if (typep c 'error) 1 0)         ; parent chain hit
                                (if (typep 5 '%bo-type50) 1 0)    ; deftype expansion
                                (if (typep c '%bo-type50) 0 1))))) ; indexed miss
               acc))))

;; =====================================================================
;; alloc.* — allocator behavior (spec 2.5 free-list size classes).
;; Mixed object sizes retained in a 512-slot ring: continuous frees of
;; assorted sizes keep the free list populated and exercised.

(defun %bo-alloc-mixed (n reps)
  (let ((ring (make-array 512 :initial-element nil))
        (count 0))
    (dotimes (r reps)
      (dotimes (i n)
        (setf (svref ring (logand count 511))
              (list (list i i i i)
                    (make-array 8 :initial-element i)
                    (make-string 16 :initial-element #\x)))
        (setq count (+ count 1))))
    count))

;; Pure cons churn: short-lived lists, the 8-byte size class.
(defun %bo-alloc-cons (len reps)
  (let ((last 0))
    (dotimes (r reps)
      (let ((acc nil))
        (dotimes (i len) (push i acc))
        (setq last (length acc))))
    last))

(let ((n 1000) (reps (%bo-scaled 300)))
  (%bo-run "alloc.mixed-churn" (* n reps)
           (lambda () (%bo-alloc-mixed n reps))))

(let ((len 100) (reps (%bo-scaled 10000)))
  (%bo-run "alloc.cons-churn" len
           (lambda () (%bo-alloc-cons len reps))))

;; =====================================================================
;; compile.* — COMPILE-FILE round-trip (make-load-form pre-pass, profiled
;; 2026-07-05; also captures reader/compiler/FASL-writer throughput).
;; compile.file-plain runs with NO make-load-form method defined, so the
;; writer's pre-pass is gated off.  compile.file-mlf defines one first —
;; the gate then stays open, as it does in any real session once
;; log4cl/serapeum/cl-ppcre/... is loaded — so the delta between the two
;; isolates the pre-pass cost over the file's constant graph (~2N unique
;; conses).  These must run LAST: defining the method opens the gate for
;; the rest of the image's lifetime.

(defvar *bo-src-path*  #+amigaos "T:bo-mlf-src.lisp" #-amigaos "/tmp/bo-mlf-src.lisp")
(defvar *bo-fasl-path* #+amigaos "T:bo-mlf-src.fasl" #-amigaos "/tmp/bo-mlf-src.fasl")

(defun %bo-write-compile-src (n with-box)
  "Write a source file whose one form carries a literal constant graph
of ~2N conses (N-element spine, one pair per element)."
  (with-open-file (s *bo-src-path* :direction :output :if-exists :supersede)
    (format s "(defparameter cl-user::*bo-mlf-data*~%  '(")
    (dotimes (i n)
      (format s "(~D . ~D) " i (* 2 i)))
    (format s "))~%")
    (when with-box
      (format s "(defparameter cl-user::*bo-mlf-box* #.(cl-user::make-%bo-mlf-box :v 42))~%"))))

(defun %bo-compile-bench (n reps with-box)
  (%bo-write-compile-src n with-box)
  (dotimes (r reps)
    (compile-file *bo-src-path* :output-file *bo-fasl-path*))
  (load *bo-fasl-path*)
  (let ((d (%bo-digest (symbol-value 'cl-user::*bo-mlf-data*) #'car)))
    (if with-box
        (cons (car d) (+ (cdr d) (%bo-mlf-box-v (symbol-value 'cl-user::*bo-mlf-box*))))
        d)))

(let ((n 10000) (reps (%bo-scaled 2)))
  (%bo-run "compile.file-plain" (cons n (%bo-sum-below n))
           (lambda () (%bo-compile-bench n reps nil))))

(defstruct %bo-mlf-box v)
(defmethod make-load-form ((b %bo-mlf-box) &optional env)
  (declare (ignore env))
  (values (list 'make-%bo-mlf-box :v (%bo-mlf-box-v b)) nil))

(let ((n 10000) (reps (%bo-scaled 2)))
  (%bo-run "compile.file-mlf" (cons n (+ (%bo-sum-below n) 42))
           (lambda () (%bo-compile-bench n reps t))))

;; ---------------------------------------------------------------------

(clamiga::%jit-set-active *bo-prior-jit*)

(format t "BENCH-TOTAL ms=~D fails=~D~%" *bo-total-ms* *bo-fails*)
(format t "done.~%")
