;;; bench-scan.lisp -- per-character cost of three text scans, the shape an
;;; editor runs on every RET / TAB / paren match (specs/performance.md 4.4).
;;;
;;;   COMPUTE-INDENT       the Clamacs spike's naive scan: CHAR, a COND of
;;;                        CHAR= tests, PUSH/POP, no declarations
;;;   COMPUTE-INDENT-FAST  the spike's declared state machine: SCHAR, CASE
;;;                        on the state and on the character, fixnum types
;;;   SCAN-PARENS          bench-general's string-scan row
;;;
;;; Each runs 50 times over a 1,007-character Lisp text (warm-up call first,
;;; so the JIT has compiled it); the whole set runs twice.  Output lines:
;;;   SCAN <name>: <chars> chars x <reps> reps, <us>/rep, <us>/char, result <n>
;;; The results are fixed (1000, 1000, 87): a different value is a bug.
;;;
;;; Usage:
;;;   host:     ./build/host/amilisp --no-userinit --non-interactive --load trunk/bench-scan.lisp
;;;   Vampire:  python3 verify/realamiga/run-on-vampire.py trunk/bench-scan.lisp
;;; The Amiga clock ticks in 20 ms, so a cell there resolves to ~0.4 us/char;
;;; take several runs (docs/benchmarks.md 2026-09-16, string-scan, has a
;;; native-code placement effect that moves single functions by 2-3x).

(defun compute-indent (text end)
  (let ((stack '()) (in-string nil) (in-comment nil) (i 0))
    (loop while (< i end)
          do (let ((c (char text i)))
               (cond ((char= c #\Newline) (setf in-comment nil))
                     (in-comment)
                     (in-string (cond ((char= c #\\) (incf i))
                                      ((char= c #\") (setf in-string nil))))
                     ((char= c #\;) (setf in-comment t))
                     ((char= c #\") (setf in-string t))
                     ((char= c #\() (push i stack))
                     ((char= c #\)) (pop stack))))
             (incf i))
    (if (null stack) 0 (first stack))))
(defun compute-indent-fast (text end)
  (declare (optimize (speed 3) (safety 1))
           (type simple-string text) (type fixnum end))
  (let ((stack '()) (state 0) (i 0))
    (declare (type fixnum state i))
    (loop while (< i end)
          do (let ((c (schar text i)))
               (case state
                 (0 (case c
                      (#\( (push i stack))
                      (#\) (pop stack))
                      (#\" (setf state 1))
                      (#\; (setf state 2))))
                 (1 (case c
                      (#\\ (setf state 3))
                      (#\" (setf state 0))))
                 (2 (when (char= c #\Newline) (setf state 0)))
                 (t (setf state 1))))
             (incf i))
    (if (null stack) 0 (first stack))))
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
(defvar *text*
  (coerce
   (with-output-to-string (s)
     (dotimes (k 6)
       (format s "(defun foo~D (a b)~%  \"doc ; not a comment\"~%  ;; comment (with parens~%  (let ((x (+ a b)) (y \"str\\\"ing\"))~%    (when (> x 3)~%      (list x y (car (list 1 2 3)))))~%~%" k))
     (format s "(defun bar (z)~%  (let ((q 1))~%    (foo z "))
   'simple-string))
(defvar *end* (length *text*))
(defun bench (name fn reps)
  (funcall fn)                            ; warm (JIT, cache)
  (let ((t0 (get-internal-real-time)) (r nil))
    (dotimes (i reps) (setq r (funcall fn)))
    (let ((dt (/ (* 1000000.0 (- (get-internal-real-time) t0)) internal-time-units-per-second)))
      (format t "SCAN ~A: ~D chars x ~D reps, ~,1F us/rep, ~,2F us/char, result ~A~%"
              name *end* reps (/ dt reps) (/ dt reps *end*) r))))
(format t "amilisp ~A~%" (lisp-implementation-version))
(bench "naive" (lambda () (compute-indent *text* *end*)) 50)
(bench "declared" (lambda () (compute-indent-fast *text* *end*)) 50)
(bench "scan-parens" (lambda () (scan-parens *text* *end*)) 50)
(bench "naive" (lambda () (compute-indent *text* *end*)) 50)
(bench "declared" (lambda () (compute-indent-fast *text* *end*)) 50)
(bench "scan-parens" (lambda () (scan-parens *text* *end*)) 50)
(format t "SCAN-DONE~%")
