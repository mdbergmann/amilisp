; Peephole differential corpus (spec 1.8).
;
; Run twice by tests/test_peephole_diff.sh — once with AMILISP_FORCE_SPEED=0
; (peephole off) and once with AMILISP_FORCE_SPEED=3 (peephole on for every
; compile) — and the printed output must be byte-identical.  Every form
; below exercises a rewrite pattern or a guard the pass must respect:
; store-then-reload chains, discarded pure values, (not ...) branch fusion,
; jump threading through nested conditionals, dead code, NLX landing-pad
; relocation (tagbody/go, catch/throw, unwind-protect, block/return-from),
; multiple-values state, closures, and type errors from discarded values.
;
; Results print via a single reporting function so the output is stable and
; machine-diffable.

(defvar *r* nil)
(defun report (name value)
  (push (list name value) *r*)
  (format t "~A = ~S~%" name value))

;; setq chains: STORE;POP;LOAD sites
(defun c-setq-chain (x)
  (setq x (+ x 1))
  (setq x (* x 2))
  (setq x (- x 3))
  x)
(report "setq-chain" (c-setq-chain 10))

;; discarded pure values in progn bodies
(defun c-discard (x)
  1 :k nil t x
  (progn 2 x)
  (+ x 1))
(report "discard" (c-discard 41))

;; (not ...) under if/when/unless: NOT;Jcc fusion both senses
(defun c-not-if (x) (if (not (< x 10)) :big :small))
(report "not-if-a" (c-not-if 5))
(report "not-if-b" (c-not-if 50))
(defun c-not-count (xs)
  (let ((n 0))
    (dolist (x xs)
      (unless (not (numberp x)) (setq n (+ n 1)))
      (when (not (symbolp x)) (setq n (+ n 10))))
    n))
(report "not-count" (c-not-count '(1 a 2.5 b "s")))

;; nested conditionals: jump-to-jump threading + dead branches
(defun c-nest (x)
  (if (< x 0)
      (if (< x -10) :low-low :low)
      (if (> x 10) (if (> x 100) :high-high :high) :mid)))
(report "nest" (list (c-nest -50) (c-nest -5) (c-nest 5)
                     (c-nest 50) (c-nest 500)))

;; loops: backward jumps must stay correct across deletions
(defun c-loop-sum (n)
  (let ((s 0))
    (dotimes (i n) (setq s (+ s i)))
    s))
(report "loop-sum" (c-loop-sum 1000))
(defun c-shuffle (n)
  (let ((a 1) (b 2) (c 3) (tmp 0))
    (dotimes (r n)
      (setq tmp a) (setq a b) (setq b c) (setq c tmp))
    (list a b c)))
(report "shuffle" (c-shuffle 999))

;; tagbody/go: NLX landing pads + local jumps
(defun c-tagbody (n)
  (let ((s 0) (i 0))
    (tagbody
     top (when (>= i n) (go done))
         (setq s (+ s i))
         (setq i (+ i 1))
         (go top)
     done)
    s))
(report "tagbody" (c-tagbody 100))

;; catch/throw + unwind-protect: CATCH/UWPROT landing pads
(defun c-catch (x)
  (catch 'tag
    (when x (throw 'tag :thrown))
    :fell))
(report "catch" (list (c-catch t) (c-catch nil)))
(defvar *cleanups* 0)
(defun c-uwprot (x)
  (catch 'out
    (unwind-protect
        (progn (when x (throw 'out :out)) :body)
      (setq *cleanups* (+ *cleanups* 1)))))
(report "uwprot" (list (c-uwprot t) (c-uwprot nil) *cleanups*))

;; block/return-from
(defun c-block (x)
  (block b
    (dotimes (i 10)
      (when (= i x) (return-from b (* i 100))))
    :not-found))
(report "block" (list (c-block 3) (c-block 99)))

;; multiple values: discarded constants near values forms must not disturb
;; the mv state an observer sees
(defun c-mv-pass ()
  (progn 42 (values 1 2 3)))
(report "mv-pass" (multiple-value-list (c-mv-pass)))
(defun c-mv-single ()
  (values-list '(7 8 9))
  :single)
(report "mv-single" (multiple-value-list (c-mv-single)))
(defun c-mv-bind (x)
  (multiple-value-bind (q r) (floor x 7)
    (list q r)))
(report "mv-bind" (c-mv-bind 100))

;; closures: OP_CLOSURE capture descriptors survive re-encode
(defun c-closures (n)
  (let ((acc nil))
    (dotimes (i n)
      (push (let ((j i)) (lambda () (* j j))) acc))
    (let ((s 0))
      (dolist (f acc) (setq s (+ s (funcall f))))
      s)))
(report "closures" (c-closures 10))

;; ANSI: a discarded (car 5) still signals at any speed
(defun c-discard-car (x)
  (progn (car x) :ok))
(report "car-signal"
        (handler-case (c-discard-car 5)
          (type-error () :signaled)))
(report "car-ok" (c-discard-car '(1 2)))

;; recursion through optimized code
(defun c-fib (n)
  (if (< n 2) n (+ (c-fib (- n 1)) (c-fib (- n 2)))))
(report "fib" (c-fib 15))

;; --- spec 4.3: superinstructions and the two rewrites that came with them ---

;; STORE_POP / LOAD_LOAD / LOAD_CALL_GLOBAL: several bindings, then calls
;; taking them in every order
(defun c-bindings (x y)
  (let ((a (car x)) (b (cdr x)) (c (car y)))
    (list (list a b c) (list c b a) (list b) (list a c))))
(report "bindings" (c-bindings '(1 . 2) '(3)))

;; LOAD_STRUCT_REF: a local's slot; the type error survives fusion
(defstruct c-pt x y)
(defun c-slot (p) (list (c-pt-x p) (c-pt-y p)))
(report "slot" (c-slot (make-c-pt :x 1 :y 2)))
(report "slot-err" (handler-case (c-slot 5) (type-error () :signaled)))

;; LOAD_MV_RESET / LOAD_RET: a returned local is exactly one value
(defun c-ret-local (x) (let ((v (floor x 2))) v))
(report "ret-local" (multiple-value-list (c-ret-local 9)))
(defun c-ret-param (x) x)
(report "ret-param" (multiple-value-list (c-ret-param (values 1 2))))

;; EQ_JNIL / LOAD_JNIL / GLOAD_JNIL: the three test shapes
(defvar *c-flag* nil)
(defun c-tests (a b)
  (list (if (eq a b) :eq :ne)
        (if a :a :not-a)
        (if *c-flag* :flag :no-flag)
        (let ((*c-flag* t)) (if *c-flag* :bound :unbound))
        (when (eq a 'k) :k)))
(report "tests" (list (c-tests 'k 'k) (c-tests nil 'k) (c-tests 1 2)))
(defvar *c-unbound*)
(defun c-unbound-test () (if *c-unbound* 1 2))
(report "unbound-test" (handler-case (c-unbound-test) (unbound-variable () :unbound)))

;; the function-end shape: RETURN-FROM landings, a block result, a
;; builtin tail call that falls through to the RET
(defun c-early (x)
  (when (car x) (return-from c-early :early))
  (if (cdr x) (return-from c-early (list :cdr (cdr x))))
  (setq x (cons :end x)))
(report "early" (list (c-early '(t)) (c-early '(nil . 5)) (c-early '(nil))))
(defun c-tail-builtin (x) (car x))
(report "tail-builtin" (c-tail-builtin '(7)))
(defun c-setq-value (x) (setq x (+ x 1)))
(report "setq-value" (c-setq-value 41))

;; round two: a local-to-local SETQ, a local then a constant, an EQ
;; against a special, a special as the last argument, a discarded value
;; then a local
(defun c-r2 (a b)
  (let ((c a) (d 0))
    (setq d c)
    (list (if (eq b *c-flag*) (list c :k) (list c :n))
          (cadr (list d *c-flag*)) (progn (list 1) d))))
(report "r2" (list (c-r2 1 nil) (c-r2 2 :x) (let ((*c-flag* :x)) (c-r2 2 :x))))
(defun c-r2u (a) (if (eq a *c-unbound*) 1 2))
(report "r2-unbound" (handler-case (c-r2u 1) (unbound-variable () :unbound)))

;; a HANDLER-CASE with several clauses: the landing table is one JMP per
;; clause, entered by index from C — every entry must survive the pass
(defun c-hc3 (k)
  (handler-case (case k (0 (warn "w")) (1 (error "e")) (t (signal 'condition)))
    (warning () :warning)
    (error () :error)
    (condition () :condition)))
(report "hc3" (list (c-hc3 0) (c-hc3 1) (c-hc3 2)))
(defun c-hc-last (x)
  (handler-case (if x (warn "w") (error "e"))
    (warning () :w)
    (error () :e)))
(report "hc-last" (list (c-hc-last t) (c-hc-last nil)))

;; string-scan fast path (spec 4.4): AREF / CHAREQ from the compiler,
;; CMP_BR fused from every comparison under both branch polarities,
;; PUSH_LOCAL / POP_LOCAL, the CASE key shape, INCF/DECF on locals
(defun c-scan (text)
  (let ((stack nil) (state 0) (i 0) (opens 0) (n (length text)))
    (loop while (< i n)
          do (let ((c (schar text i)))
               (case state
                 (0 (case c
                      (#\( (push i stack) (incf opens))
                      (#\) (pop stack))
                      (#\" (setq state 1))
                      (#\; (setq state 2))))
                 (1 (case c (#\\ (setq state 3)) (#\" (setq state 0))))
                 (2 (when (char= c #\Newline) (setq state 0)))
                 (t (setq state 1))))
             (incf i))
    (list opens state stack)))
(report "scan" (c-scan "(a (b \"x)\" ; )(
 (c \"\\\"\" d"))
(defun c-cmp (a b)
  (list (if (< a b) 1 0) (if (> a b) 1 0) (if (<= a b) 1 0) (if (>= a b) 1 0)
        (if (= a b) 1 0) (unless (< a b) 1) (unless (>= a b) 1) (unless (= a b) 1)
        (if (not (> a b)) 1 0)))
(report "cmp" (list (c-cmp 1 2) (c-cmp 2 1) (c-cmp 3 3) (c-cmp 1.5 1) (c-cmp (expt 2 40) 1)
                    (c-cmp 1/2 1/2) (handler-case (c-cmp 'a 1) (type-error () :type-error))))
(defun c-chareq (a b) (list (char= a b) (if (char= a b) :y :n) (unless (char= a b) :n)))
(report "chareq" (list (c-chareq #\a #\a) (c-chareq #\a #\b)
                       (handler-case (c-chareq #\a 1) (type-error () :type-error))))
(defun c-aref (v s i) (list (aref v i) (svref v i) (char s i) (schar s i) (aref s i)))
(report "aref" (list (c-aref (vector 1 2 3) "xyz" 1)
                     (handler-case (c-aref (vector 1) "x" 3) (error () :error))
                     (handler-case (c-aref '(1) "x" 0) (error () :error))))
(defun c-pushpop (n)
  (let ((s nil) (out nil))
    (dotimes (i n) (push (* i i) s) (when (oddp i) (push (pop s) out)))
    (list s out (pop s) (pop s) s)))
(report "pushpop" (list (c-pushpop 5) (c-pushpop 0) (handler-case (let ((x 5)) (pop x)) (type-error () :type-error))))
(defun c-incf (x) (list (incf x) (incf x 10) (decf x) (decf x 2.5) x))
(report "incf" (list (c-incf 1) (c-incf most-positive-fixnum)))
(defun c-case (x) (case x (1 :one) ((2 3) :two-three) (#\a :a) (:k) (t :other)))
(report "case" (mapcar #'c-case '(1 2 3 #\a :k 9)))
(defun c-case-in-loop (l)
  (let ((r nil))
    (dolist (x l) (case x ((a b) (push :ab r)) (c (push :c r)) (t (push x r))))
    r))
(report "case-loop" (c-case-in-loop '(a c b d)))

;; final checksum over everything reported, so a silent mid-corpus
;; divergence still flips the last line
(format t "CHECKSUM = ~S~%" (sxhash (format nil "~S" *r*)))
(format t "CORPUS-DONE~%")
