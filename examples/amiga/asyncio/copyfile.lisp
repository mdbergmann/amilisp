;;; copyfile.lisp — double-buffered asynchronous file copy.
;;;
;;; Common Lisp port of the technique in the NDK 3.1 AsynchIO package
;;; (Examples1/AsynchIO/asyncio.c on the Amiga Developer CD): file I/O
;;; that talks DOS packets (ACTION_READ / ACTION_WRITE via PutMsg /
;;; WaitPort) straight to the filesystem handler, so the next buffer
;;; load is transferred by the filesystem WHILE the program processes
;;; the current one.  The packet machinery lives in AMIGA.ASYNCIO
;;; (lib/amiga/asyncio.lisp); this example uses it for what it was
;;; invented for — a fast file copy — and times the same copy done with
;;; plain synchronous CL streams next to it.
;;;
;;; What it shows: OPEN-ASYNC / WITH-ASYNC-FILE, bulk READ-ASYNC /
;;; WRITE-ASYNC through one foreign chunk buffer, WRITE-ASYNC straight
;;; from a Lisp byte vector, and READ-ASYNC verification of the result.
;;;
;;; Run on AmigaOS 3+ or MorphOS:
;;;   amilisp --load examples/amiga/asyncio/copyfile.lisp

(require "amiga/asyncio")

(defpackage "ASYNCIO-COPYFILE"
  (:use "CL")
  (:local-nicknames ("AIO" "AMIGA.ASYNCIO")))

(in-package "ASYNCIO-COPYFILE")

(defparameter *source* "T:asyncio-copy.src")
(defparameter *dest*   "T:asyncio-copy.dst")
(defparameter *size*   (* 256 1024))    ; 256 KB test payload
(defparameter *chunk*  16384)           ; bytes moved per read/write call

(defun make-source-file ()
  "Write *SIZE* patterned bytes to *SOURCE* — WRITE-ASYNC from a Lisp
byte vector, 999 bytes at a time so the buffers flip mid-vector."
  (let ((v (make-array 999 :element-type '(unsigned-byte 8))))
    (aio:with-async-file (f *source* :write)
      (let ((written 0))
        (loop while (< written *size*) do
          (let ((n (min 999 (- *size* written))))
            (dotimes (i n)
              (setf (aref v i) (mod (+ written i) 251)))
            (aio:write-async f v n)
            (incf written n)))))))

(defun async-copy (from to)
  "Copy FROM to TO with AMIGA.ASYNCIO.  While WRITE-ASYNC hands a full
buffer to the destination's filesystem, the source's read-ahead packet
for the next chunk is already in flight — both transfers overlap the
Lisp loop."
  (ffi:with-foreign-alloc (chunk *chunk*)
    (aio:with-async-file (in from :read)
      (aio:with-async-file (out to :write)
        (loop for n = (aio:read-async in chunk *chunk*)
              while (plusp n)
              sum (aio:write-async out chunk n))))))

(defun sync-copy (from to)
  "The same copy through plain synchronous CL streams, for comparison."
  (let ((buf (make-array *chunk* :element-type '(unsigned-byte 8))))
    (with-open-file (in from :element-type '(unsigned-byte 8))
      (with-open-file (out to :direction :output :element-type '(unsigned-byte 8)
                              :if-exists :supersede)
        (loop for n = (read-sequence buf in)
              while (plusp n)
              do (write-sequence buf out :end n)
              sum n)))))

(defun verify-copy ()
  "Read *DEST* back with READ-ASYNC into a Lisp vector and check every
byte against the pattern.  Returns the number of bad bytes."
  (let ((v (make-array 4096 :element-type '(unsigned-byte 8)))
        (pos 0)
        (bad 0))
    (aio:with-async-file (f *dest* :read)
      (loop for n = (aio:read-async f v)
            while (plusp n) do
              (dotimes (i n)
                (unless (= (aref v i) (mod (+ pos i) 251))
                  (incf bad)))
              (incf pos n)))
    (unless (= pos *size*)
      (format t "size mismatch: copied ~D of ~D bytes~%" pos *size*))
    bad))

(defun kb/s (bytes ms)
  (if (plusp ms) (round (/ (* bytes 1000) 1024 ms)) 0))

(defun timed (fn)
  (let ((start (get-internal-real-time)))
    (funcall fn)
    (round (* (- (get-internal-real-time) start) 1000)
           internal-time-units-per-second)))

(defun run ()
  (format t "asyncio copyfile: writing ~D KB source (async, from Lisp vectors)...~%"
          (floor *size* 1024))
  (let ((ms (timed #'make-source-file)))
    (format t "  written in ~D ms (~D KB/s)~%" ms (kb/s *size* ms)))
  (let ((ms (timed (lambda () (async-copy *source* *dest*)))))
    (format t "async copy:  ~D ms (~D KB/s)~%" ms (kb/s *size* ms)))
  (let ((bad (verify-copy)))
    (format t "verify:      ~A~%" (if (zerop bad) "OK, every byte matches"
                                      (format nil "~D BAD BYTES" bad))))
  (let ((ms (timed (lambda () (sync-copy *source* *dest*)))))
    (format t "sync copy:   ~D ms (~D KB/s) for comparison~%" ms (kb/s *size* ms)))
  (delete-file *source*)
  (delete-file *dest*)
  (format t "done.~%"))

(if (aio:available-p)
    (run)
    (format t "asyncio not available - AmigaOS/MorphOS with dos.library required~%"))
