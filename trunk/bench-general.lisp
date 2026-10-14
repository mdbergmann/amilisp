;;; bench-general.lisp — general workload benchmark, portable across
;;; amilisp / ECL / SBCL.
;;;
;;; Where trunk/bench-prims.lisp times one primitive per row (the sento
;;; per-message path), this file times WORKLOADS: the classic Gabriel
;;; shapes (tak, fib, deriv, n-queens), sorting and list processing,
;;; hash tables, strings and string streams, the reader and printer,
;;; FORMAT, fixnum / float / bignum arithmetic, arrays, CLOS, structs,
;;; conditions, LOOP, and allocation churn.  The same file runs unchanged
;;; on all three implementations, so every row carries its own native-
;;; compiler reference point.  Baselines: docs/benchmarks.md 2026-09-09.
;;;
;;; Design:
;;;   - Every row is a function; DEFBENCH runs it *reps* times and reports
;;;     the MINIMUM wall-clock time in milliseconds.
;;;   - Every row returns an integer that depends on all the work done, so
;;;     a native compiler cannot dead-code-eliminate the body and the
;;;     `val=` column can be compared across implementations (it must be
;;;     identical — a difference is a correctness bug somewhere).
;;;   - Input data (random lists, key strings, the big string, the shapes)
;;;     is built once at load time, outside the timed regions.
;;;   - Pseudo-random numbers come from a small LCG that stays inside a
;;;     30-bit fixnum, so the data is identical everywhere.
;;;   - (optimize (speed 1) (safety 1) (debug 1)) everywhere: what third-
;;;     party code is compiled with by default.  Run amilisp with
;;;     CLAMIGA_FORCE_SPEED=3 to see the peephole at full strength.
;;;   - Output is one machine-parseable line per row:
;;;       GEN <name> ms=<min> val=<integer>
;;;
;;; Usage:
;;;   amilisp: ./build/host/amilisp --no-userinit --heap 64M --non-interactive \
;;;                --load trunk/bench-general.lisp
;;;   ECL:     ecl --norc --eval '(progn (load (compile-file "trunk/bench-general.lisp"
;;;                :output-file "/tmp/bench-general.fas")) (quit))'
;;;            (--load of the source would run ECL's bytecode interpreter;
;;;             the reference point is its native compile-file output)
;;;   SBCL:    sbcl --non-interactive --no-userinit --load trunk/bench-general.lisp
;;;   Amiga:   (defparameter cl-user::*bg-scale* 1/20) before loading.

(defvar cl-user::*bg-scale* 1
  "Multiplier applied to every loop count.  Scale down on the Amiga.")
(defvar cl-user::*bg-reps* 5
  "Runs per row; the minimum is reported.")

(defpackage :bench-general (:use :cl))
(in-package :bench-general)

(declaim (optimize (speed 1) (safety 1) (debug 1)))

(defvar *scale* cl-user::*bg-scale*)
(defvar *reps* cl-user::*bg-reps*)
(defvar *results* nil)
(defvar *sink* nil)

(defun scaled (n) (max 1 (round (* n *scale*))))

(defun ms (t0 t1)
  (/ (* 1000d0 (- t1 t0)) internal-time-units-per-second))

(defmacro defbench (name fn)
  `(let ((best nil) (val nil))
     (dotimes (r *reps*)
       (let ((t0 (get-internal-real-time)))
         (setq val (,fn))
         (let ((dt (ms t0 (get-internal-real-time))))
           (when (or (null best) (< dt best)) (setq best dt)))))
     (setq *sink* val)
     (push (cons ,name best) *results*)
     (format t "GEN ~20A ms=~9,1F  val=~A~%" ,name best val)
     (finish-output)))

;;; --- deterministic pseudo-random data (30-bit safe) ------------------

(defvar *seed* 42)
(defun rnd (n)
  (setq *seed* (mod (+ (* *seed* 1237) 7919) 65521))
  (mod *seed* n))

(defun make-random-list (n)
  (let ((l nil))
    (dotimes (i n) (push (rnd 65521) l))
    l))

(defun tree-size (x)
  (if (consp x) (+ 1 (tree-size (car x)) (tree-size (cdr x))) 0))

;;; --- Gabriel shapes -------------------------------------------------

(defun tak (x y z)
  (if (not (< y x))
      z
      (tak (tak (1- x) y z)
           (tak (1- y) z x)
           (tak (1- z) x y))))

(defun run-tak ()
  (let ((r 0))
    (dotimes (i (scaled 60)) (setq r (+ r (tak 18 12 6))))
    r))

(defun fib (n)
  (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))

(defun run-fib ()
  (let ((r 0))
    (dotimes (i (scaled 3)) (setq r (+ r (fib 28))))
    r))

(defun queens-ok (col placed dist)
  (cond ((null placed) t)
        ((or (= (car placed) col)
             (= (abs (- (car placed) col)) dist))
         nil)
        (t (queens-ok col (cdr placed) (1+ dist)))))

(defun queens (n row placed)
  (if (= row n)
      1
      (let ((count 0))
        (dotimes (col n)
          (when (queens-ok col placed 1)
            (incf count (queens n (1+ row) (cons col placed)))))
        count)))

(defun run-queens ()
  (let ((r 0))
    (dotimes (i (scaled 5)) (setq r (+ r (queens 9 0 nil))))
    r))

(defun deriv-aux (a) (list '/ (deriv a) a))

(defun deriv (a)
  (cond ((atom a) (if (eq a 'x) 1 0))
        ((eq (car a) '+) (cons '+ (mapcar #'deriv (cdr a))))
        ((eq (car a) '-) (cons '- (mapcar #'deriv (cdr a))))
        ((eq (car a) '*) (list '* a (cons '+ (mapcar #'deriv-aux (cdr a)))))
        ((eq (car a) '/) (list '- (list '/ (deriv (cadr a)) (caddr a))
                               (list '/ (cadr a)
                                     (list '* (caddr a) (caddr a)
                                           (deriv (caddr a))))))
        (t 'error)))

(defun run-deriv ()
  (let ((acc 0))
    (dotimes (i (scaled 20000))
      (incf acc (tree-size (deriv '(+ (* 3 x x) (* a x x) (* b x) 5)))))
    acc))

;;; --- lists ----------------------------------------------------------

(defvar *list-20k* (make-random-list (scaled 20000)))

(defun run-list-sort ()
  (let ((acc 0))
    (dotimes (i (scaled 20))
      (let ((s (sort (copy-list *list-20k*) #'<)))
        (incf acc (+ (first s) (car (last s)) (length s)))))
    acc))

(defun run-list-ops ()
  (let ((l *list-20k*) (acc 0))
    (dotimes (i (scaled 30))
      (let* ((m (mapcar #'1+ l))
             (e (remove-if #'oddp m))
             (r (reverse e))
             (a (append r e)))
        (incf acc (+ (length a)
                     (reduce #'+ e)
                     (count 8 r)
                     (or (position 100 a) 0)
                     (or (find 4096 a) 0)
                     (length (member 2 a))))))
    acc))

(defun run-hof ()
  (let ((acc 0) (l *list-20k*))
    (dotimes (i (scaled 20))
      (let ((k (1+ i)))
        (incf acc (reduce #'+ (mapcar (lambda (x) (logand (* x k) 4095)) l)))
        (incf acc (count-if (lambda (x) (> x k)) l))
        (incf acc (length (remove-if-not (lambda (x) (evenp (+ x k))) l)))
        (incf acc (or (position-if (lambda (x) (= x k)) l) 0))))
    acc))

(defvar *keys*
  (coerce (let ((ks nil))
            (dotimes (i 64) (push (intern (format nil "K~D" i) :keyword) ks))
            (nreverse ks))
          'vector))
(defvar *alist* (let ((al nil))
                  (dotimes (i 64) (push (cons (svref *keys* i) i) al))
                  (nreverse al)))
(defvar *plist* (let ((pl nil))
                  (dotimes (i 64) (push (svref *keys* i) pl) (push i pl))
                  (nreverse pl)))

(defun run-assoc ()
  (let ((acc 0) (n (scaled 100000)))
    (dotimes (i n)
      (incf acc (cdr (assoc (svref *keys* (logand i 63)) *alist*))))
    acc))

(defun run-getf ()
  (let ((acc 0) (n (scaled 100000)))
    (dotimes (i n)
      (incf acc (getf *plist* (svref *keys* (logand i 63)))))
    acc))

;;; --- arrays ---------------------------------------------------------

(defvar *vec-20k* (coerce *list-20k* 'vector))

(defun run-vector-sort ()
  (let ((v (sort (copy-seq *vec-20k*) #'<)))
    (+ (aref v 0) (aref v (1- (length v))) (length v))))

(defun insertion-sort (v)
  (let ((n (length v)))
    (do ((i 1 (1+ i)))
        ((>= i n) v)
      (let ((x (aref v i)) (j (1- i)))
        (loop while (and (>= j 0) (> (aref v j) x))
              do (setf (aref v (1+ j)) (aref v j))
                 (decf j))
        (setf (aref v (1+ j)) x)))))

(defun run-insertion-sort ()
  (let* ((n (scaled 2500))
         (v (make-array n)))
    (dotimes (i n) (setf (aref v i) (aref *vec-20k* i)))
    (insertion-sort v)
    (+ (aref v 0) (aref v (1- n)) (aref v (floor n 2)))))

(defun matmul (a b c n)
  (dotimes (i n)
    (dotimes (j n)
      (let ((s 0))
        (dotimes (k n)
          (incf s (* (aref a i k) (aref b k j))))
        (setf (aref c i j) s))))
  c)

(defun run-matmul ()
  (let* ((n (scaled 110))
         (a (make-array (list n n)))
         (b (make-array (list n n)))
         (c (make-array (list n n) :initial-element 0)))
    (dotimes (i n)
      (dotimes (j n)
        (setf (aref a i j) (mod (+ i j) 17))
        (setf (aref b i j) (mod (* i j) 13))))
    (matmul a b c n)
    (+ (aref c 0 0) (aref c (1- n) (1- n)) (aref c (floor n 2) 1))))

;;; --- floats ---------------------------------------------------------

(defun mandel (w h maxit)
  (let ((count 0))
    (dotimes (py h)
      (dotimes (px w)
        (let ((cx (- (/ (* 3.5d0 px) w) 2.5d0))
              (cy (- (/ (* 2d0 py) h) 1d0))
              (zx 0d0) (zy 0d0) (it 0))
          (loop while (and (< it maxit)
                           (< (+ (* zx zx) (* zy zy)) 4d0))
                do (let ((nx (+ (- (* zx zx) (* zy zy)) cx)))
                     (setq zy (+ (* 2d0 zx zy) cy))
                     (setq zx nx)
                     (incf it)))
          (incf count it))))
    count))

(defun run-mandel ()
  (let ((n (scaled 120)))
    (mandel n n 60)))

(defun run-float-vector ()
  (let* ((n (scaled 20000))
         (a (make-array n :element-type 'double-float :initial-element 0d0))
         (b (make-array n :element-type 'double-float :initial-element 0d0))
         (acc 0d0))
    (dotimes (i n)
      (setf (aref a i) (* (mod i 100) 0.5d0))
      (setf (aref b i) (* (mod i 37) 0.25d0)))
    (dotimes (r (scaled 50))
      (let ((s 0d0))
        (dotimes (i n) (incf s (* (aref a i) (aref b i))))
        (incf acc (sqrt s))))
    (round acc)))

;;; --- bignums --------------------------------------------------------

(defun factorial (n)
  (let ((r 1))
    (dotimes (i n) (setq r (* r (1+ i))))
    r))

(defun run-bignum-fact ()
  (let ((acc 0))
    (dotimes (i (scaled 200))
      (incf acc (mod (factorial 1000) 1000003)))
    acc))

(defun run-bignum-arith ()
  (let* ((a (+ (expt 3 2000) 12345))
         (b (+ (expt 7 1500) 6789))
         (c (+ (expt 2 3000) 1))
         (acc 0))
    (dotimes (i (scaled 2000))
      (let ((p (* a b)))
        (incf acc (mod (floor p c) 1000003))
        (incf acc (mod (+ p a) 1000003))
        (setq a (+ a 1))))
    acc))

;;; --- hash tables ----------------------------------------------------

(defun run-hash-fixnum ()
  (let ((h (make-hash-table)) (n (scaled 200000)) (s 0))
    (dotimes (i n) (setf (gethash (* i 7) h) i))
    (dotimes (i n) (incf s (gethash (* i 7) h)))
    (dotimes (i n) (unless (gethash (1+ (* i 7)) h) (incf s)))
    (+ s (hash-table-count h))))

(defvar *string-keys*
  (let* ((n (scaled 10000)) (v (make-array n)))
    (dotimes (i n) (setf (svref v i) (format nil "key-~D-~A" i (mod i 7))))
    v))

(defun run-hash-string ()
  (let* ((h (make-hash-table :test 'equal))
         (n (length *string-keys*))
         (s 0))
    (dotimes (i n) (setf (gethash (svref *string-keys* i) h) i))
    (dotimes (r 10)
      (dotimes (i n) (incf s (gethash (svref *string-keys* i) h))))
    (dotimes (i n) (unless (gethash "missing" h) (incf s)))
    (+ s (hash-table-count h))))

;;; --- strings --------------------------------------------------------

(defun run-string-ops ()
  (let ((acc 0))
    (dotimes (i (scaled 25000))
      (let* ((s (concatenate 'string "The quick brown fox "
                             (string-upcase "jumps over")
                             " the lazy dog " (princ-to-string i)))
             (u (string-upcase s))
             (p (or (search "LAZY" u) 0))
             (sub (subseq u p (+ p 4)))
             (c (count #\O u)))
        (incf acc (+ p c (length sub)
                     (if (string= sub "LAZY") 1 0)
                     (or (position #\x s) 0)
                     (if (string< s u) 1 0)))))
    acc))

(defun run-string-stream ()
  (let ((n (scaled 150000)) (acc 0))
    (let ((s (with-output-to-string (out)
               (dotimes (i n)
                 (write-string "line " out)
                 (princ i out)
                 (write-char #\Space out)
                 (prin1 (logand i 7) out)
                 (terpri out)))))
      (with-input-from-string (in s)
        (loop for line = (read-line in nil nil)
              while line
              do (incf acc (length line)))))
    acc))

(defvar *big-string*
  (let ((s (make-string (scaled 1000000))))
    (dotimes (i (length s))
      (setf (char s i) (code-char (+ 97 (mod (* i 7) 26)))))
    s))

(defun run-char-loop ()
  (let ((c 0) (s *big-string*))
    (dotimes (i (length s))
      (let ((ch (char s i)))
        (when (or (char= ch #\a) (char= ch #\e) (char= ch #\i)
                  (char= ch #\o) (char= ch #\u))
          (incf c))))
    c))

;;; string-scan: the editor's paren/string/comment state machine over a
;;; Lisp-like text — SCHAR per character, CASE on the character, PUSH/POP
;;; of a paren stack, a fixnum index loop.  What every RET/TAB/paren-match
;;; in an editor does (specs/performance.md 4.4).
(defvar *lisp-text*
  (with-output-to-string (o)
    (dotimes (k (scaled 400))
      (format o "(defun fn~D (a b)~%  \"doc ; not a comment\"~%  ;; a comment (with parens~%  (let ((x (+ a b)) (y \"str\\\"ing\"))~%    (when (> x ~D)~%      (list x y (car (list 1 2 3))))))~%~%"
              k k))))

(defun scan-parens (text end)
  (declare (type simple-string text) (type fixnum end))
  (let ((stack '()) (state 0) (i 0) (opens 0))
    (declare (type fixnum state i opens))
    (loop while (< i end)
          do (let ((c (schar text i)))
               (case state
                 (0 (case c
                      (#\( (push i stack) (incf opens))
                      (#\) (pop stack))
                      (#\" (setq state 1))
                      (#\; (setq state 2))))
                 (1 (case c
                      (#\\ (setq state 3))
                      (#\" (setq state 0))))
                 (2 (when (char= c #\Newline) (setq state 0)))
                 (t (setq state 1))))
             (incf i))
    (+ opens (length stack))))

(defun run-string-scan ()
  (let ((text (coerce *lisp-text* 'simple-string)) (acc 0))
    (dotimes (r 5)
      (incf acc (scan-parens text (length text))))
    acc))

;;; --- format, reader, printer ----------------------------------------

(defun run-format ()
  (let ((acc 0))
    (dotimes (i (scaled 50000))
      (incf acc (length (format nil "~A: ~D items at ~,2F each = ~S~%"
                                "widget" i (* i 1.5d0) (list i :x)))))
    acc))

(defvar *form*
  (let ((l nil))
    (dotimes (i 200)
      (push (case (mod i 5)
              (0 i)
              (1 (intern (format nil "SYM~D" i) :bench-general))
              (2 (format nil "str~D" i))
              (3 (* i 0.25))
              (t (list i (list :k i) "x" #\c)))
            l))
    (nreverse l)))

(defvar *form-string*
  (let ((*print-pretty* nil)) (prin1-to-string *form*)))

(defun run-reader ()
  (let ((acc 0))
    (dotimes (i (scaled 1000))
      (incf acc (tree-size (read-from-string *form-string*))))
    acc))

;; *PRINT-PRETTY*'s initial value is implementation-dependent (CLHS 22.1.3):
;; SBCL and ECL start with T and would wrap the form over several lines.
(defun run-printer ()
  (let ((acc 0) (*print-pretty* nil))
    (dotimes (i (scaled 2000))
      (incf acc (length (prin1-to-string *form*))))
    acc))

;;; --- CLOS -----------------------------------------------------------

(defclass shape () ((id :initarg :id :accessor shape-id)))
(defclass circle (shape) ((r :initarg :r :accessor circle-r)))
(defclass rect (shape) ((w :initarg :w :accessor rect-w)
                        (h :initarg :h :accessor rect-h)))
(defclass square (rect) ())

(defgeneric area (s))
(defmethod area ((c circle)) (* 3 (circle-r c) (circle-r c)))
(defmethod area ((r rect)) (* (rect-w r) (rect-h r)))
(defmethod area ((s square)) (let ((w (rect-w s))) (* w w)))

(defvar *shapes*
  (let ((v (make-array 300)))
    (dotimes (i 300)
      (setf (svref v i)
            (case (mod i 3)
              (0 (make-instance 'circle :id i :r (logand i 7)))
              (1 (make-instance 'rect :id i :w (logand i 7) :h 2))
              (t (make-instance 'square :id i :w 3 :h 3)))))
    v))

(defun run-clos-dispatch ()
  (let ((acc 0) (v *shapes*))
    (dotimes (r (scaled 1000))
      (dotimes (i 300)
        (let ((s (svref v i)))
          (incf acc (+ (area s) (shape-id s))))))
    acc))

(defun run-make-instance ()
  (let ((n (scaled 40000)) (acc 0))
    (dotimes (i n)
      (let ((c (make-instance 'circle :id i :r (logand i 15))))
        (incf acc (circle-r c))))
    acc))

;;; --- structs --------------------------------------------------------

(defstruct node key left right)

(defun bst-insert (tree key)
  (cond ((null tree) (make-node :key key))
        ((< key (node-key tree))
         (setf (node-left tree) (bst-insert (node-left tree) key))
         tree)
        ((> key (node-key tree))
         (setf (node-right tree) (bst-insert (node-right tree) key))
         tree)
        (t tree)))

(defun bst-find (tree key)
  (cond ((null tree) nil)
        ((< key (node-key tree)) (bst-find (node-left tree) key))
        ((> key (node-key tree)) (bst-find (node-right tree) key))
        (t tree)))

(defun bst-size (tree)
  (if (null tree) 0 (+ 1 (bst-size (node-left tree)) (bst-size (node-right tree)))))

(defun run-struct-bst ()
  (let ((tree nil) (found 0))
    (dolist (k *list-20k*) (setq tree (bst-insert tree k)))
    (dotimes (r (scaled 5))
      (dolist (k *list-20k*) (when (bst-find tree k) (incf found)))
      (dotimes (i 1000) (when (bst-find tree (+ 65521 i)) (incf found))))
    (+ (bst-size tree) found)))

;;; --- control, loops, allocation ------------------------------------

(defun run-fixnum-loop ()
  (let ((s 0) (n (scaled 3000000)))
    (dotimes (i n) (setq s (logand (+ s (* i 3) 1) #xFFFFFF)))
    s))

(defun run-loop-collect ()
  (let ((acc 0) (n (scaled 100000)))
    (dotimes (r (scaled 10))
      (incf acc (length (loop for i below n when (evenp i) collect (logand i 1023))))
      (incf acc (loop for i below n sum (logand i 255)))
      (incf acc (loop for x in *list-20k* maximize x)))
    acc))

(define-condition bench-error (error)
  ((code :initarg :code :reader bench-error-code)))

(defun run-conditions ()
  (let ((acc 0) (n (scaled 80000)))
    (dotimes (i n)
      (incf acc (handler-case (progn (error 'bench-error :code (logand i 7)) 0)
                  (bench-error (e) (bench-error-code e))))
      (incf acc (handler-case (progn (error "plain ~A" i) 0)
                  (error () 1))))
    acc))

(defun run-alloc-churn ()
  (let ((ring (make-array 1000 :initial-element nil))
        (n (scaled 500000))
        (acc 0))
    (dotimes (i n)
      (let ((slot (mod i 1000)))
        (setf (svref ring slot)
              (if (evenp i)
                  (make-list 10 :initial-element i)
                  (make-array 16 :initial-element i)))
        (incf acc (length (svref ring (mod (* i 7) 1000))))))
    acc))

;;; --- run ------------------------------------------------------------

(format t "~%~A ~A, scale ~A, min of ~D runs per row~%"
        (lisp-implementation-type) (lisp-implementation-version) *scale* *reps*)

(defbench "tak" run-tak)
(defbench "fib" run-fib)
(defbench "nqueens" run-queens)
(defbench "deriv" run-deriv)
(defbench "list-sort" run-list-sort)
(defbench "list-ops" run-list-ops)
(defbench "hof" run-hof)
(defbench "assoc" run-assoc)
(defbench "getf" run-getf)
(defbench "vector-sort" run-vector-sort)
(defbench "insertion-sort" run-insertion-sort)
(defbench "matmul" run-matmul)
(defbench "mandel" run-mandel)
(defbench "float-vector" run-float-vector)
(defbench "bignum-fact" run-bignum-fact)
(defbench "bignum-arith" run-bignum-arith)
(defbench "hash-fixnum" run-hash-fixnum)
(defbench "hash-string" run-hash-string)
(defbench "string-ops" run-string-ops)
(defbench "string-stream" run-string-stream)
(defbench "char-loop" run-char-loop)
(defbench "string-scan" run-string-scan)
(defbench "format" run-format)
(defbench "reader" run-reader)
(defbench "printer" run-printer)
(defbench "clos-dispatch" run-clos-dispatch)
(defbench "make-instance" run-make-instance)
(defbench "struct-bst" run-struct-bst)
(defbench "fixnum-loop" run-fixnum-loop)
(defbench "loop-collect" run-loop-collect)
(defbench "conditions" run-conditions)
(defbench "alloc-churn" run-alloc-churn)

(format t "~%GEN-TOTAL ms=~,1F over ~D rows~%"
        (reduce #'+ *results* :key #'cdr) (length *results*))
(format t "sink: ~A~%" (type-of *sink*))
