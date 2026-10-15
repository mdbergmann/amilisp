;; Smoke test: chipz make-decompressing-stream (gray-stream gunzip/inflate)
;; on cl-amiga.  Verifies the #+cl-amiga branches added to the chipz fork
;; (chipz.asd gray-streams feature + stream.lisp class/GF defvars).
;;
;; Usage (host):
;;   ./build/host/amilisp --heap 64M --load trunk/test-chipz-stream.lisp

(setq *load-verbose* nil)
(require "asdf")
(load "trunk/load-libs-ql.lisp")
(dolist (s '(:trivial-features :alexandria :babel :flexi-streams :chipz))
  (ensure-ql-lib s))

(format t "~%--- Loading :flexi-streams + :chipz ---~%")
(asdf:load-system :flexi-streams)
(asdf:load-system :chipz)

;; Fail loudly if we fell back to stream-fallback.lisp.
(unless (member 'chipz-system:gray-streams *features*)
  (error "chipz gray-streams feature NOT pushed — AmiLisp branch missing"))

(in-package :chipz)

(defun bytes (&rest bs) (make-array (length bs) :element-type '(unsigned-byte 8)
                                    :initial-contents bs))

;; A tiny gzip stream for the payload "hello\n" (RFC 1952), produced by gzip(1):
;;   1f 8b 08 00 00 00 00 00 00 03 cb 48 cd c9 c9 e7 02 00
;;   20 30 3a 36 06 00 00 00
(defparameter *gz-hello*
  (bytes #x1f #x8b #x08 #x00 #x00 #x00 #x00 #x00 #x00 #x03
         #xcb #x48 #xcd #xc9 #xc9 #xe7 #x02 #x00
         #x20 #x30 #x3a #x36 #x06 #x00 #x00 #x00))

(defun octets->string (octets)
  (map 'string #'code-char octets))

(defun run ()
  (let* ((in (flexi-streams:make-in-memory-input-stream *gz-hello*))
         (ds (chipz:make-decompressing-stream :gzip in)))
    (format t "decompressing-stream = ~S~%" ds)
    ;; (1) read-byte / stream-read-byte path
    (let ((b0 (read-byte ds)))
      (format t "first byte via read-byte = ~D (#\\~C)~%" b0 (code-char b0))
      (assert (= b0 (char-code #\h))))
    ;; (2) read-sequence path (drives stream-read-byte under AmiLisp)
    (let* ((buf (make-array 16 :element-type '(unsigned-byte 8)))
           (n (read-sequence buf ds))
           (rest (octets->string (subseq buf 0 n))))
      (format t "rest via read-sequence (~D bytes) = ~S~%" n rest)
      (assert (string= rest (format nil "ello~%")))))
  (format t "~%CHIPZ-STREAM-OK~%"))

(run)
