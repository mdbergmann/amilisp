;;; test-asyncio.lisp — Amiga-side tests of lib/amiga/asyncio.lisp
;;; (AMIGA.ASYNCIO, the DOS-packet double-buffered async file I/O port
;;; of the NDK 3.1 AsynchIO package).
;;;
;;; Loaded from run-tests.lisp via (load ...) inside #+amigaos, like
;;; test-reaction.lisp: CHECK comes from run-tests.lisp.  Everything
;;; here needs a real filesystem handler to send packets to, so unlike
;;; the reaction suite there is no portable half — the host mirror
;;; (tests/test_amiga_asyncio.sh) checks that the module and the example
;;; load and bow out cleanly where DOS packets do not exist.
;;;
;;; The file content is a computable pattern, (mod (* 7 i) 251), so
;;; every seek target has a known byte and the whole file can be
;;; verified without keeping a copy.

(require "amiga/asyncio")
(format t "; asyncio: amiga/asyncio loaded~%")
(finish-output)

(defun asyncio-pat (i) (mod (* 7 i) 251))

(defparameter *asyncio-path* "T:amilisp-asyncio-test.dat")
(defparameter *asyncio-text* "T:amilisp-asyncio-text.dat")
(defparameter *asyncio-size* 50000)

(check "asyncio-available" t (amiga.asyncio:available-p))

;; Write the pattern through the vector API in 999-byte slices — an odd
;; size, so the slices straddle the (4096 -> two 2048-byte) buffers and
;; the ACTION_WRITE flip path runs ~24 times.
(check "asyncio-write-pattern" *asyncio-size*
  (let ((v (make-array 999 :element-type '(unsigned-byte 8)))
        (written 0))
    (amiga.asyncio:with-async-file (f *asyncio-path* :write :buffer-size 4096)
      (loop while (< written *asyncio-size*) do
        (let ((n (min 999 (- *asyncio-size* written))))
          (dotimes (i n)
            (setf (aref v i) (asyncio-pat (+ written i))))
          (amiga.asyncio:write-async f v n)
          (incf written n))))
    written))

;; The synchronous view of the async result: CLOSE-ASYNC flushed
;; everything, a plain CL stream sees exactly the pattern.
(check "asyncio-sync-readback-matches" (list *asyncio-size* 0)
  (with-open-file (s *asyncio-path* :element-type '(unsigned-byte 8))
    (let ((v (make-array *asyncio-size* :element-type '(unsigned-byte 8)))
          (bad 0))
      (let ((n (read-sequence v s)))
        (dotimes (i n)
          (unless (= (aref v i) (asyncio-pat i)) (incf bad)))
        (list n bad)))))

;; Async read of the async write, through the vector API; the final
;; READ-ASYNC comes back short (50000 = 12 x 4096 + 848), then 0, then
;; READ-BYTE-ASYNC says EOF.
(check "asyncio-async-readback" (list *asyncio-size* 0 848 0 nil)
  (amiga.asyncio:with-async-file (f *asyncio-path* :read :buffer-size 4096)
    (let ((v (make-array 4096 :element-type '(unsigned-byte 8)))
          (pos 0) (bad 0) (last-n nil))
      (loop for n = (amiga.asyncio:read-async f v)
            while (plusp n) do
              (dotimes (i n)
                (unless (= (aref v i) (asyncio-pat (+ pos i))) (incf bad)))
              (incf pos n)
              (setf last-n n))
      (list pos bad last-n
            (amiga.asyncio:read-async f v)
            (amiga.asyncio:read-byte-async f)))))

;; SEEK-ASYNC returns the previous logical position (like DOS Seek) and
;; lands on the byte the pattern predicts — across :START, :CURRENT and
;; :END, forward and backward, ending with a position probe at EOF.
(check "asyncio-seek"
       (list (asyncio-pat 0)                  ; first byte
             1 (asyncio-pat 30000)            ; :start 30000 from pos 1
             30001 (asyncio-pat 31000)        ; :current +999 from 30001
             31001 (asyncio-pat 49999)        ; :end -1
             nil                              ; then EOF
             50000                            ; probe: (seek 0 :current)
             50000 (asyncio-pat 100))         ; back-seek inside the file
  (amiga.asyncio:with-async-file (f *asyncio-path* :read :buffer-size 4096)
    (list (amiga.asyncio:read-byte-async f)
          (amiga.asyncio:seek-async f 30000 :start)
          (amiga.asyncio:read-byte-async f)
          (amiga.asyncio:seek-async f 999 :current)
          (amiga.asyncio:read-byte-async f)
          (amiga.asyncio:seek-async f -1 :end)
          (amiga.asyncio:read-byte-async f)
          (amiga.asyncio:read-byte-async f)
          (amiga.asyncio:seek-async f 0 :current)
          (amiga.asyncio:seek-async f 100 :start)
          (amiga.asyncio:read-byte-async f))))

;; :APPEND extends the pattern file; the appended tail reads back.
(check "asyncio-append" (list (+ *asyncio-size* 100) 42)
  (progn
    (amiga.asyncio:with-async-file (f *asyncio-path* :append)
      (dotimes (i 100)
        (amiga.asyncio:write-byte-async f 42)))
    (amiga.asyncio:with-async-file (f *asyncio-path* :read)
      (let ((end (amiga.asyncio:seek-async f 0 :end)))
        (declare (ignore end))
        (let ((size (amiga.asyncio:seek-async f 0 :current)))
          (amiga.asyncio:seek-async f -1 :end)
          (list size (amiga.asyncio:read-byte-async f)))))))

;; Text convenience: lines out, lines back, NIL at EOF — including a
;; final line without a newline.
(check "asyncio-text-lines" '("hello asyncio" "" "last line" nil)
  (progn
    (amiga.asyncio:with-async-file (f *asyncio-text* :write)
      (amiga.asyncio:write-line-async f "hello asyncio")
      (amiga.asyncio:write-line-async f "")
      (amiga.asyncio:write-string-async f "last line"))
    (amiga.asyncio:with-async-file (f *asyncio-text* :read)
      (list (amiga.asyncio:read-line-async f)
            (amiga.asyncio:read-line-async f)
            (amiga.asyncio:read-line-async f)
            (amiga.asyncio:read-line-async f)))))

;; Wrong-direction calls are refused with a clear error.
(check "asyncio-direction-errors" '(:read-refused :write-refused)
  (let ((v (make-array 4 :element-type '(unsigned-byte 8))))
    (list (amiga.asyncio:with-async-file (f *asyncio-path* :append)
            (handler-case (progn (amiga.asyncio:read-async f v) nil)
              (error () :read-refused)))
          (amiga.asyncio:with-async-file (f *asyncio-path* :read)
            (handler-case (progn (amiga.asyncio:write-async f v) nil)
              (error () :write-refused))))))

;; A negative NUM-BYTES is refused up front instead of skipping the
;; transfer-loop guard and corrupting the cursor (BYTES-LEFT growing,
;; OFFSET moving backward past the buffer start).
(check "asyncio-negative-num-bytes-errors" '(:read-refused :write-refused)
  (let ((v (make-array 4 :element-type '(unsigned-byte 8))))
    (list (amiga.asyncio:with-async-file (f *asyncio-path* :read)
            (handler-case (progn (amiga.asyncio:read-async f v -1) nil)
              (error () :read-refused)))
          (amiga.asyncio:with-async-file (f *asyncio-text* :write)
            (handler-case (progn (amiga.asyncio:write-async f v -1) nil)
              (error () :write-refused))))))

;; A missing file signals (with the DOS error code in the message)
;; instead of returning something NULL-ish.
(check "asyncio-open-missing-errors" t
  (handler-case (progn (amiga.asyncio:open-async "T:no-such-dir/nope" :read) nil)
    (error (e) (and (search "DOS error" (format nil "~A" e)) t))))

;; NIL: has no handler process — writes are swallowed (the C original's
;; explicit special case), reads are instant EOF.
(check "asyncio-nil-device" '(1000 0 nil)
  (let ((v (make-array 1000 :element-type '(unsigned-byte 8))))
    (list (amiga.asyncio:with-async-file (f "NIL:" :write :buffer-size 512)
            (amiga.asyncio:write-async f v))
          (amiga.asyncio:with-async-file (f "NIL:" :read)
            (amiga.asyncio:read-async f v))
          (amiga.asyncio:with-async-file (f "NIL:" :read)
            (amiga.asyncio:read-byte-async f)))))

;; CLOSE-ASYNC is idempotent; anything else on a closed file errors.
(check "asyncio-closed" '(t t :closed-error)
  (let ((f (amiga.asyncio:open-async *asyncio-path* :read)))
    (list (amiga.asyncio:close-async f)
          (amiga.asyncio:close-async f)
          (handler-case (progn (amiga.asyncio:read-byte-async f) nil)
            (error () :closed-error)))))

;; WITH-ASYNC-FILE closes on a non-local exit: the follow-up open in
;; :WRITE mode (which deletes and recreates) succeeds, proving no handle
;; was left on the file.
(check "asyncio-unwind-closes" :reopened
  (progn
    (ignore-errors
      (amiga.asyncio:with-async-file (f *asyncio-text* :read)
        (error "boom")))
    (amiga.asyncio:with-async-file (f *asyncio-text* :write)
      (amiga.asyncio:write-line-async f "fresh"))
    :reopened))

(delete-file *asyncio-path*)
(delete-file *asyncio-text*)
