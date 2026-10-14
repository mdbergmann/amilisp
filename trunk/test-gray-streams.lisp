;; Regression test: (typep gray-stream 'stream) => T
;;
;; Verifies the fix for the bug where Gray stream instances (CLOS objects)
;; were not recognised as STREAMs by TYPEP, even though STREAMP and
;; OUTPUT-STREAM-P already returned T.
;;
;; Usage (host):
;;   ./build/host/amilisp --heap 24M --load trunk/test-gray-streams.lisp
;;
;; Usage (Amiga, from the Amiga shell):
;;   stack 65536
;;   amilisp --heap 24M --load trunk/test-gray-streams.lisp

(setq *pass-count* 0)
(setq *fail-count* 0)

(defmacro check (name expected actual)
  (let ((e (gensym "E")) (a (gensym "A")) (c (gensym "C")))
    `(handler-case
         (let ((,e ,expected) (,a ,actual))
           (if (equal ,e ,a)
               (progn (incf *pass-count*)
                      (format t "PASS: ~A~%" ,name))
               (progn (incf *fail-count*)
                      (format t "FAIL: ~A  expected ~S  got ~S~%" ,name ,e ,a))))
       (error (,c)
         (incf *fail-count*)
         (format t "FAIL: ~A  signaled: ~A~%" ,name ,c)))))

(format t "~%--- Loading gray-streams ---~%")
(load #-amigaos "lib/gray-streams.lisp"
       #+amigaos "lib/gray-streams.lisp")

(format t "~%--- Gray stream typep regression ---~%")

;; Define a minimal output-only Gray stream subclass.
(defclass test-gray-out-stream (gray:fundamental-output-stream) ())

;; GRAY::STREAM-WRITE-CHAR is the only mandatory output method; provide a
;; no-op so the class is complete enough for type-check tests.
(defmethod gray:stream-write-char ((s test-gray-out-stream) char)
  (declare (ignore s char))
  nil)

(let ((g (make-instance 'test-gray-out-stream)))

  ;; These two were already passing before the fix.
  (check "streamp on gray instance"         t (streamp g))
  (check "output-stream-p on gray instance" t (output-stream-p g))
  (check "input-stream-p on gray output instance" nil (input-stream-p g))

  ;; This is the regression: was returning NIL before the fix.
  (check "typep gray-stream 'stream"        t (typep g 'stream))

  ;; Verify the negative cases are unaffected.
  (check "typep fixnum 'stream"   nil (typep 42 'stream))
  (check "typep string 'stream"   nil (typep "hi" 'stream))
  (check "typep nil 'stream"      nil (typep nil 'stream))

  ;; Gray output stream must not match built-in stream subtypes.
  (check "gray not file-stream"    nil (typep g 'file-stream))
  (check "gray not string-stream"  nil (typep g 'string-stream))
  (check "gray not synonym-stream" nil (typep g 'synonym-stream))

  ;; Native streams must still pass.
  (let ((ss (make-string-output-stream)))
    (check "typep native stream 'stream" t (typep ss 'stream))))

(format t "~%--- Gray fundamental-input-stream typep ---~%")

;; A minimal input-only Gray stream subclass.
(defclass test-gray-in-stream (gray:fundamental-input-stream) ())

(defmethod gray:stream-read-char ((s test-gray-in-stream))
  (declare (ignore s))
  :eof)

(let ((gi (make-instance 'test-gray-in-stream)))
  (check "streamp on gray input instance"           t   (streamp gi))
  (check "typep gray-input-stream 'stream"          t   (typep gi 'stream))
  (check "input-stream-p on gray input instance"    t   (input-stream-p gi))
  (check "output-stream-p on gray input instance"   nil (output-stream-p gi)))

(format t "~%--- Printer functions route to Gray streams (item-2) ---~%")

;; A capturing Gray output stream that stores chars for later inspection.
(defclass test-gray-cap-stream (gray:fundamental-character-output-stream)
  ((buf :initform nil)))

(defmethod gray:stream-write-char ((s test-gray-cap-stream) c)
  (setf (slot-value s 'buf) (cons c (slot-value s 'buf))))

(defun gray-cap-flush (s)
  "Return accumulated string and reset the buffer."
  (let ((str (coerce (nreverse (slot-value s 'buf)) 'string)))
    (setf (slot-value s 'buf) nil)
    str))

(let ((g (make-instance 'test-gray-cap-stream)))

  ;; PRINC routes to gray stream.
  (check "princ to gray-stream"
         "hello"
         (progn (princ "hello" g) (gray-cap-flush g)))

  ;; PRIN1 routes to gray stream (strings get quotes).
  (check "prin1 to gray-stream"
         "\"hi\""
         (progn (prin1 "hi" g) (gray-cap-flush g)))

  ;; WRITE :stream routes to gray stream.
  (check "write to gray-stream"
         "\"hi\""
         (progn (write "hi" :stream g) (gray-cap-flush g)))

  ;; FORMAT with a stream destination routes to gray stream.
  (check "format to gray-stream"
         "42"
         (progn (format g "~A" 42) (gray-cap-flush g)))

  ;; FORMAT nil is unaffected — still returns a string.
  (check "format nil unaffected" "99" (format nil "~A" 99))

  ;; PRINT prepends newline and appends space.
  (check "print to gray-stream"
         (concatenate 'string (string #\Newline) "42 ")
         (progn (print 42 g) (gray-cap-flush g)))

  ;; PPRINT routes to gray stream (produces at least one char of output).
  (check "pprint to gray-stream (non-empty)"
         t
         (progn (pprint '(a b c) g) (> (length (gray-cap-flush g)) 0)))

  ;; The motivating SLDB case: PRINC on a condition must not leak to terminal.
  (handler-case
      (error "slynk-test-condition")
    (error (e)
      (princ e g)
      (let ((s (gray-cap-flush g)))
        (check "princ condition to gray-stream (non-empty)" t (> (length s) 0))))))

(format t "~%--- Gray two-way stream tests (item-2) ---~%")

;; Minimal Gray input stream that pops chars from a queue.
(defclass test-gray-q-stream (gray:fundamental-character-input-stream)
  ((chars :initarg :chars :initform nil)))

(defmethod gray:stream-read-char ((s test-gray-q-stream))
  (let ((q (slot-value s 'chars)))
    (if q
        (progn (setf (slot-value s 'chars) (cdr q)) (car q))
        :eof)))

;; Both-Gray two-way stream tests.
(let* ((gin  (make-instance 'test-gray-q-stream :chars (list #\A #\B #\C)))
       (gcap (make-instance 'test-gray-cap-stream))
       (tw   (make-two-way-stream gin gcap)))
  (check "gray tw: streamp"                  t   (streamp tw))
  (check "gray tw: typep two-way-stream"     t   (typep tw 'two-way-stream))
  (check "gray tw: typep stream"             t   (typep tw 'stream))
  (check "gray tw: input-stream-p"           t   (input-stream-p tw))
  (check "gray tw: output-stream-p"          t   (output-stream-p tw))
  (check "gray tw: input accessor"           t   (eq (two-way-stream-input-stream tw)  gin))
  (check "gray tw: output accessor"          t   (eq (two-way-stream-output-stream tw) gcap))
  (check "gray tw: read-char A"              #\A (read-char tw))
  (check "gray tw: read-char B"              #\B (read-char tw))
  (write-char #\X tw)
  (write-string "hi" tw)
  (check "gray tw: write accumulates"        "Xhi" (gray-cap-flush gcap))
  (format tw "f~D" 9)
  (check "gray tw: format delegates"         "f9"  (gray-cap-flush gcap)))

;; Mixed: native input + Gray output.
(let* ((nin  (make-string-input-stream "hello"))
       (gcap (make-instance 'test-gray-cap-stream))
       (tw   (make-two-way-stream nin gcap)))
  (check "mixed tw native-in gray-out: read-char"   #\h  (read-char tw))
  (write-char #\Z tw)
  (check "mixed tw native-in gray-out: write-char"  "Z"  (gray-cap-flush gcap)))

;; Mixed: Gray input + native output.
(let* ((gin  (make-instance 'test-gray-q-stream :chars (list #\W)))
       (nout (make-string-output-stream))
       (tw   (make-two-way-stream gin nout)))
  (check "mixed tw gray-in native-out: read-char"   #\W  (read-char tw))
  (write-char #\Q tw)
  (check "mixed tw gray-in native-out: write-char"  "Q"  (get-output-stream-string nout)))

(format t "~%Passed: ~A  Failed: ~A~%" *pass-count* *fail-count*)
(if (= *fail-count* 0)
    (format t "ALL TESTS PASSED~%")
    (progn (format t "SOME TESTS FAILED~%")
           (quit 1)))
