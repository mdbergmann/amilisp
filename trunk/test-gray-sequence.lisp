;; Regression test: bulk sequence I/O on Gray streams
;;
;; Verifies that CL:READ-SEQUENCE / CL:WRITE-SEQUENCE dispatch to the GRAY
;; package's STREAM-READ-SEQUENCE / STREAM-WRITE-SEQUENCE generic functions
;; for Gray streams, that the default per-element methods are correct, and
;; that a stream class may override STREAM-READ-SEQUENCE to provide a genuine
;; bulk transfer (the optimisation that lets libraries like chipz fill a
;; buffer in one shot instead of paying per-byte STREAM-READ-BYTE overhead).
;;
;; Native (non-Gray) streams must keep the boot.lisp behaviour.
;;
;; Usage (host):
;;   ./build/host/amilisp --heap 24M --load trunk/test-gray-sequence.lisp
;;
;; Usage (Amiga, from the Amiga shell):
;;   stack 65536
;;   amilisp --heap 24M --load trunk/test-gray-sequence.lisp

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
(load "lib/gray-streams.lisp")

(format t "~%--- GRAY exports the bulk-I/O GFs ---~%")

;; trivial-gray-streams' unknown-implementation fallback imports these from
;; the GRAY ("...") package, so they must be present AND external.
(check "gray exports stream-read-sequence"
       :external
       (nth-value 1 (find-symbol "STREAM-READ-SEQUENCE" "GRAY")))
(check "gray exports stream-write-sequence"
       :external
       (nth-value 1 (find-symbol "STREAM-WRITE-SEQUENCE" "GRAY")))

(format t "~%--- Default STREAM-READ-SEQUENCE (binary) ---~%")

;; A binary Gray input stream that pops bytes from a queue.  Provides only
;; STREAM-READ-BYTE, so READ-SEQUENCE uses the default per-element GF.
(defclass test-byte-in (gray:fundamental-binary-input-stream)
  ((bytes :initarg :bytes :initform nil)))

(defmethod gray:stream-read-byte ((s test-byte-in))
  (let ((q (slot-value s 'bytes)))
    (if q
        (progn (setf (slot-value s 'bytes) (cdr q)) (car q))
        :eof)))

;; Full read into a byte buffer.
(let* ((s (make-instance 'test-byte-in :bytes (list 10 20 30 40)))
       (buf (make-array 4 :element-type '(unsigned-byte 8))))
  (check "read-sequence binary full count" 4 (read-sequence buf s))
  (check "read-sequence binary contents" (list 10 20 30 40) (coerce buf 'list)))

;; Short read: EOF before the buffer fills — return the fill index.
(let* ((s (make-instance 'test-byte-in :bytes (list 1 2)))
       (buf (make-array 5 :element-type '(unsigned-byte 8) :initial-element 0)))
  (check "read-sequence binary short count" 2 (read-sequence buf s))
  (check "read-sequence binary short contents" (list 1 2 0 0 0) (coerce buf 'list)))

;; :start / :end honoured.
(let* ((s (make-instance 'test-byte-in :bytes (list 7 8 9)))
       (buf (make-array 5 :element-type '(unsigned-byte 8) :initial-element 0)))
  (check "read-sequence start/end count" 4 (read-sequence buf s :start 1 :end 4))
  (check "read-sequence start/end contents" (list 0 7 8 9 0) (coerce buf 'list)))

(format t "~%--- STREAM-READ-SEQUENCE into a list sequence ---~%")

;; read-sequence into a list: (setf (elt list i) v) must be used, not aref.
(let* ((s (make-instance 'test-byte-in :bytes (list 10 20 30)))
       (seq (list 0 0 0)))
  (check "read-sequence list count" 3 (read-sequence seq s))
  (check "read-sequence list contents" (list 10 20 30) seq))

;; Short read into a list — EOF stops early.
(let* ((s (make-instance 'test-byte-in :bytes (list 1 2)))
       (seq (list 0 0 0)))
  (check "read-sequence list short count" 2 (read-sequence seq s))
  (check "read-sequence list short contents" (list 1 2 0) seq))

(format t "~%--- Default STREAM-READ-SEQUENCE (character) ---~%")

;; A character Gray input stream that pops chars from a queue.
(defclass test-char-in (gray:fundamental-character-input-stream)
  ((chars :initarg :chars :initform nil)))

(defmethod gray:stream-read-char ((s test-char-in))
  (let ((q (slot-value s 'chars)))
    (if q
        (progn (setf (slot-value s 'chars) (cdr q)) (car q))
        :eof)))

(let* ((s (make-instance 'test-char-in :chars (list #\f #\o #\o)))
       (buf (make-string 3)))
  (check "read-sequence char count" 3 (read-sequence buf s))
  (check "read-sequence char contents" "foo" buf))

(format t "~%--- Overridden STREAM-READ-SEQUENCE (bulk path) ---~%")

;; A stream that OVERRIDES stream-read-sequence — confirms the bulk method is
;; actually dispatched: it fills the buffer in one shot and records the call.
(defclass test-bulk-in (gray:fundamental-binary-input-stream)
  ((fill-byte :initarg :fill-byte :initform 0)
   (calls :initform 0 :accessor bulk-calls)))

(defmethod gray:stream-read-sequence ((s test-bulk-in) seq start end &key)
  (incf (bulk-calls s))
  (do ((i start (1+ i)))
      ((>= i end) end)
    (setf (aref seq i) (slot-value s 'fill-byte))))

(let* ((s (make-instance 'test-bulk-in :fill-byte 99))
       (buf (make-array 6 :element-type '(unsigned-byte 8))))
  (check "read-sequence bulk count" 6 (read-sequence buf s))
  (check "read-sequence bulk one GF call" 1 (bulk-calls s))
  (check "read-sequence bulk contents" (list 99 99 99 99 99 99) (coerce buf 'list)))

(format t "~%--- Default STREAM-WRITE-SEQUENCE ---~%")

;; A capturing character output stream.
(defclass test-char-cap (gray:fundamental-character-output-stream)
  ((buf :initform nil)))
(defmethod gray:stream-write-char ((s test-char-cap) c)
  (setf (slot-value s 'buf) (cons c (slot-value s 'buf))))
(defun char-cap-flush (s)
  (let ((str (coerce (nreverse (slot-value s 'buf)) 'string)))
    (setf (slot-value s 'buf) nil)
    str))

(let ((g (make-instance 'test-char-cap)))
  (check "write-sequence char return" "abc" (write-sequence "abc" g))
  (check "write-sequence char contents" "abc" (char-cap-flush g))
  (write-sequence "xYYz" g :start 1 :end 3)
  (check "write-sequence start/end contents" "YY" (char-cap-flush g)))

;; A capturing binary output stream.
(defclass test-byte-cap (gray:fundamental-binary-output-stream)
  ((buf :initform nil :accessor byte-cap-buf)))
(defmethod gray:stream-write-byte ((s test-byte-cap) b)
  (push b (byte-cap-buf s)))

(let* ((g (make-instance 'test-byte-cap))
       (data (make-array 3 :element-type '(unsigned-byte 8)
                           :initial-contents '(5 6 7))))
  (check "write-sequence byte return" data (write-sequence data g))
  (check "write-sequence byte contents" (list 5 6 7) (nreverse (byte-cap-buf g))))

(format t "~%--- Native streams unaffected ---~%")

(let ((ss (make-string-output-stream)))
  (write-sequence "native" ss)
  (check "write-sequence native unaffected" "native" (get-output-stream-string ss)))

(let* ((sis (make-string-input-stream "native"))
       (buf (make-string 6)))
  (check "read-sequence native count" 6 (read-sequence buf sis))
  (check "read-sequence native contents" "native" buf))

(format t "~%Passed: ~A  Failed: ~A~%" *pass-count* *fail-count*)
(if (= *fail-count* 0)
    (format t "ALL TESTS PASSED~%")
    (progn (format t "SOME TESTS FAILED~%")
           (quit 1)))
