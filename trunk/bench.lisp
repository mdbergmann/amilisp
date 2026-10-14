;;; bench.lisp — general-purpose benchmark across Lisp constructs.
;;;
;;; Each section runs the same body twice — once with the JIT disabled
;;; (pure bytecode) and once with the JIT enabled — and prints the two
;;; timings side-by-side with the speedup ratio.  The function under
;;; test is RE-DEFINED under each JIT state so the bytecode object
;;; matches that state (the JIT toggle only affects functions compiled
;;; while it is active).
;;;
;;; Usage:
;;;   (load "trunk/bench.lisp")
;;;
;;; Tweak *scale* to rescale every bench uniformly: bump on host,
;;; drop on a slow Amiga to keep runtimes reasonable.

(defparameter *scale* 1
  "Multiplier applied to every loop count.")

(defun scaled (n) (* n *scale*))

(defun %time-call (thunk)
  "Run THUNK once, return (values result elapsed-ms bytes-consed gc-cycles)."
  (let ((t0 (amilisp::%get-internal-time))
        (b0 (amilisp::%get-bytes-consed))
        (g0 (amilisp::%get-gc-count)))
    (let ((r (funcall thunk)))
      (values r
              (- (amilisp::%get-internal-time) t0)
              (- (amilisp::%get-bytes-consed) b0)
              (- (amilisp::%get-gc-count) g0)))))

(defun %run-compare (label defining-form thunk)
  ;; --- bytecode ---
  (amilisp::%jit-set-active nil)
  (eval defining-form)
  (funcall thunk)                       ; warmup
  (multiple-value-bind (r-bc t-bc b-bc g-bc) (%time-call thunk)
    ;; --- jit ---
    (amilisp::%jit-set-active t)
    (eval defining-form)
    (funcall thunk)                     ; warmup
    (multiple-value-bind (r-jit t-jit b-jit g-jit) (%time-call thunk)
      (format t "~%--- ~A ---~%" label)
      (format t "              time(ms)   bytes-consed   gc~%")
      (format t "  bytecode:   ~8D   ~12D   ~3D~%" t-bc b-bc g-bc)
      (format t "  jit:        ~8D   ~12D   ~3D~%" t-jit b-jit g-jit)
      (cond ((or (zerop t-bc) (zerop t-jit))
             (format t "  (loop too short to time — increase *scale*)~%"))
            ((< t-bc t-jit)
             (format t "  jit slower by ~,2Fx~%" (/ (float t-jit) (float t-bc))))
            (t
             (format t "  speedup:    ~,2Fx~%" (/ (float t-bc) (float t-jit)))))
      (unless (equal r-bc r-jit)
        (format t "  MISMATCH: bc=~A jit=~A~%" r-bc r-jit)))))

(defmacro bench (label defining-form call-form)
  "Run CALL-FORM under both JIT states.  DEFINING-FORM is the (defun ...)
that creates the function called by CALL-FORM; it is re-evaluated under
each JIT setting so the function body matches that state."
  `(%run-compare ,label ',defining-form (lambda () ,call-form)))

;; --- 1) fixnum loop -------------------------------------------------
;; Tight DOTIMES staying inside the fixnum fast path.  Sum 0..N-1.

(bench "fixnum loop (sum 0..N-1)"
       (defun bench-fixnum-loop (n)
         (let ((s 0))
           (dotimes (i n) (setq s (+ s i)))
           s))
       (bench-fixnum-loop (scaled 100000)))

;; --- 2) fixnum arithmetic chain ------------------------------------
;; Several binary ops per iteration — exercises ADD/SUB/MUL fast paths
;; and the JIT's operand cache.

(bench "fixnum arith chain (binops/iter)"
       (defun bench-arith-chain (n)
         (let ((s 0))
           (dotimes (i n)
             (setq s (+ s (- (* i 3) (* i 2)))))
           s))
       (bench-arith-chain (scaled 30000)))

;; --- 3) bignum arithmetic ------------------------------------------
;; Factorial overflows fixnum after 12!, exercises the bignum allocator
;; and multiply path.

(bench "bignum factorial (100!) repeated"
       (defun bench-bignum-fact (n reps)
         (let ((r 1))
           (dotimes (k reps)
             (setq r 1)
             (dotimes (i n) (setq r (* r (+ i 1)))))
           r))
       (bench-bignum-fact 100 (scaled 200)))

;; --- 4) float arithmetic -------------------------------------------
;; Boxed doubles — every op conses.  Watch the bytes-consed column.

(bench "float sum (boxed doubles)"
       (defun bench-float-sum (n)
         (let ((s 0.0))
           (dotimes (i n) (setq s (+ s 0.5)))
           s))
       (bench-float-sum (scaled 20000)))

;; --- 5) recursive fib ----------------------------------------------
;; Call/return heavy — fib(25) makes 242785 calls.  Stays fixnum.

(bench "recursive fib(25)"
       (defun bench-fib (n)
         (if (< n 2) n
             (+ (bench-fib (- n 1)) (bench-fib (- n 2)))))
       (bench-fib (+ 24 *scale*)))

;; --- 6) cons / list walk -------------------------------------------
;; Build a list of N fixnums, then sum via dolist.

(bench "cons + dolist sum"
       (defun bench-cons-walk (n)
         (let ((acc nil))
           (dotimes (i n) (push i acc))
           (let ((s 0))
             (dolist (x acc) (setq s (+ s x)))
             s)))
       (bench-cons-walk (scaled 20000)))

;; --- 7) simple-vector via AREF (general dispatch) ------------------

(bench "simple-vector fill + sum (AREF)"
       (defun bench-vec-aref (n)
         (let ((v (make-array n :initial-element 0)))
           (dotimes (i n) (setf (aref v i) i))
           (let ((s 0))
             (dotimes (j n) (setq s (+ s (aref v j))))
             s)))
       (bench-vec-aref (scaled 20000)))

;; --- 8) simple-vector via SVREF (fast 1D-T path) -------------------

(bench "simple-vector fill + sum (SVREF)"
       (defun bench-vec-svref (n)
         (let ((v (make-array n :initial-element 0)))
           (dotimes (i n) (setf (svref v i) i))
           (let ((s 0))
             (dotimes (j n) (setq s (+ s (svref v j))))
             s)))
       (bench-vec-svref (scaled 20000)))

;; --- 9) specialized string via AREF (packed chars) -----------------

(bench "string AREF char-code sum"
       (defun bench-string-aref (n)
         (let ((s (make-string n :initial-element #\a))
               (c 0))
           (dotimes (i n) (setq c (+ c (char-code (aref s i)))))
           c))
       (bench-string-aref (scaled 20000)))

;; --- 10) 2D AREF (row-major + multi-dim dispatch) ------------------

(bench "2D-array fill + sum (AREF)"
       (defun bench-2d-aref (rows cols)
         (let ((a (make-array (list rows cols) :initial-element 0)))
           (dotimes (i rows)
             (dotimes (j cols) (setf (aref a i j) (+ i j))))
           (let ((s 0))
             (dotimes (i rows)
               (dotimes (j cols) (setq s (+ s (aref a i j)))))
             s)))
       (bench-2d-aref (scaled 150) (scaled 150)))

;; --- 11) string concat (quadratic — keep N small) -----------------

(bench "string concat (quadratic)"
       (defun bench-strings (n)
         (let ((s ""))
           (dotimes (i n) (setq s (concatenate 'string s "x")))
           (length s)))
       (bench-strings (scaled 500)))

;; --- 12) hash table -----------------------------------------------

(bench "hash-table insert + lookup"
       (defun bench-hash (n)
         (let ((h (make-hash-table)))
           (dotimes (i n) (setf (gethash i h) (* i 2)))
           (let ((s 0))
             (dotimes (j n) (setq s (+ s (gethash j h))))
             s)))
       (bench-hash (scaled 5000)))

;; --- 13) extended LOOP (for/sum) ----------------------------------

(bench "extended LOOP (for/sum)"
       (defun bench-loop-sum (n)
         (loop for i from 0 below n sum i))
       (bench-loop-sum (scaled 50000)))

;; --- 14) extended LOOP (for/when/collect) -------------------------

(bench "extended LOOP (for/when/collect)"
       (defun bench-loop-collect (n)
         (length (loop for i from 0 below n when (evenp i) collect i)))
       (bench-loop-collect (scaled 10000)))

;; --- CLOS classes/methods (defined once, called via JIT'd thunks) ---
;;
;; Classes, generic functions and methods are defined once outside the
;; bench macro — the only thing toggled per JIT state is the calling
;; thunk.  CLOS dispatch happens inside the method body / GF, which is
;; not re-JIT-compiled here; the bench measures call-site overhead +
;; dispatch reaching the (already-compiled) methods.

(defclass bench-point ()
  ((x :initarg :x :accessor bench-point-x)
   (y :initarg :y :accessor bench-point-y)))

(defclass bench-point3 (bench-point)
  ((z :initarg :z :accessor bench-point-z)))

(defgeneric bench-norm (p))
(defmethod bench-norm ((p bench-point))
  (+ (* (bench-point-x p) (bench-point-x p))
     (* (bench-point-y p) (bench-point-y p))))
(defmethod bench-norm ((p bench-point3))
  (+ (call-next-method)
     (* (bench-point-z p) (bench-point-z p))))

(defgeneric bench-add (a b))
(defmethod bench-add ((a bench-point) (b bench-point))
  (+ (bench-point-x a) (bench-point-x b)
     (bench-point-y a) (bench-point-y b)))

;; --- 15) slot accessor in a loop ----------------------------------
;; ACCESSOR-call hot path — defmethod-installed slot reader, called
;; twice per iter on the same instance.

(bench "CLOS slot accessor (reader x2/iter)"
       (defun bench-clos-accessor (n)
         (let ((p (make-instance 'bench-point :x 3 :y 4))
               (s 0))
           (dotimes (i n)
             (setq s (+ s (bench-point-x p) (bench-point-y p))))
           s))
       (bench-clos-accessor (scaled 5000)))

;; --- 16) SLOT-VALUE (no accessor) ---------------------------------
;; SLOT-VALUE goes through a different (slower) path than accessors.

(bench "CLOS slot-value (no accessor)"
       (defun bench-clos-slotval (n)
         (let ((p (make-instance 'bench-point :x 3 :y 4))
               (s 0))
           (dotimes (i n)
             (setq s (+ s (slot-value p 'x) (slot-value p 'y))))
           s))
       (bench-clos-slotval (scaled 5000)))

;; --- 17) generic function dispatch (single-arg) -------------------
;; Calls a 1-arg GF in a loop on the same concrete class — measures
;; dispatch caching + call overhead.

(bench "CLOS GF dispatch (1-arg)"
       (defun bench-clos-gf1 (n)
         (let ((p (make-instance 'bench-point :x 3 :y 4))
               (s 0))
           (dotimes (i n) (setq s (+ s (bench-norm p))))
           s))
       (bench-clos-gf1 (scaled 2000)))

;; --- 18) GF dispatch with CALL-NEXT-METHOD ------------------------
;; Subclass method calls (call-next-method) — exercises the method-
;; combination chain.

(bench "CLOS GF call-next-method"
       (defun bench-clos-cnm (n)
         (let ((p (make-instance 'bench-point3 :x 3 :y 4 :z 5))
               (s 0))
           (dotimes (i n) (setq s (+ s (bench-norm p))))
           s))
       (bench-clos-cnm (scaled 2000)))

;; --- 19) two-arg GF dispatch --------------------------------------
;; Multi-method dispatch — both args specialized.

(bench "CLOS GF dispatch (2-arg)"
       (defun bench-clos-gf2 (n)
         (let ((a (make-instance 'bench-point :x 1 :y 2))
               (b (make-instance 'bench-point :x 3 :y 4))
               (s 0))
           (dotimes (i n) (setq s (+ s (bench-add a b))))
           s))
       (bench-clos-gf2 (scaled 2000)))

;; --- 20) make-instance --------------------------------------------
;; Allocator + initform/initarg processing in a tight loop.

(bench "CLOS make-instance"
       (defun bench-clos-make (n)
         (let ((last nil))
           (dotimes (i n)
             (setq last (make-instance 'bench-point :x i :y i)))
           ;; Return slot values so EQUAL works across the two runs
           ;; (CLOS instances are EQ-only).
           (list (bench-point-x last) (bench-point-y last))))
       (bench-clos-make (scaled 1000)))

(format t "~%done.~%")
