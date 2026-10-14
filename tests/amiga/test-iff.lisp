;;; test-iff.lisp — Amiga-side tests of lib/amiga/iff.lisp (AMIGA.IFF,
;;; the iffparse.library module grown from the NDK 3.1 sift example).
;;;
;;; Loaded from run-tests.lisp via (load ...) inside #+amigaos, like
;;; test-asyncio.lisp: CHECK comes from run-tests.lisp.  The fixture
;;; file is written byte by byte with plain CL streams — independent of
;;; AMIGA.IFF's own write side — so SIFT's exact output is pinned
;;; against known bytes; the write side is then proven by round-trip
;;; through the same independent SIFT, on a file and on the clipboard.
;;; The host mirror (tests/test_amiga_iff.sh) checks the pure-Lisp half
;;; and that everything loads and bows out where iffparse does not
;;; exist.

(require "amiga/iff")
(format t "; iff: amiga/iff loaded~%")
(finish-output)

(defparameter *iff-fixture* "T:clamiga-iff-fixture.iff")
(defparameter *iff-rt*      "T:clamiga-iff-roundtrip.iff")
(defparameter *iff-bad*     "T:clamiga-iff-bad.dat")

(defun iff-test-w32 (s n)
  (write-byte (ldb (byte 8 24) n) s)
  (write-byte (ldb (byte 8 16) n) s)
  (write-byte (ldb (byte 8 8) n) s)
  (write-byte (ldb (byte 8 0) n) s))

(defun iff-test-wid (s id)
  (loop for c across id do (write-byte (char-code c) s)))

;; The fixture, written as raw bytes:
;;   FORM 64 ILBM              64 = 4 + (8+20) + (8+5+1 pad) + (8+10)
;;     BMHD 20   bytes 0..19
;;     ANNO 5    "hello" + pad
;;     BODY 10   bytes 100..109
;; Written inside CHECK so a failure here (T: unavailable, disk full,
;; etc.) is counted as a FAIL instead of signaling past run-tests.lisp's
;; enclosing handler-case and silently aborting every check below.
(check "iff-fixture-written" t
  (progn
    (with-open-file (s *iff-fixture* :direction :output :if-exists :supersede
                                     :element-type '(unsigned-byte 8))
      (iff-test-wid s "FORM") (iff-test-w32 s 64) (iff-test-wid s "ILBM")
      (iff-test-wid s "BMHD") (iff-test-w32 s 20)
      (dotimes (i 20) (write-byte i s))
      (iff-test-wid s "ANNO") (iff-test-w32 s 5)
      (iff-test-wid s "hello") (write-byte 0 s)
      (iff-test-wid s "BODY") (iff-test-w32 s 10)
      (dotimes (i 10) (write-byte (+ 100 i) s)))
    t))

(check "iff-available" t (amiga.iff:available-p))

;; The pure-Lisp helpers agree with the C's IDtoStr / MAKE_ID and the
;; sift error message table.
(check "iff-id-helpers" (list "FORM" #x494C424D "8SVX" "Not an IFF file.")
  (list (amiga.iff:id-string #x464F524D)
        (amiga.iff:string-id "ILBM")
        (amiga.iff:id-string (amiga.iff:string-id "8SVX"))
        (amiga.iff:error-string -10)))

;; SIFT prints the C program's exact listing for the known bytes, and
;; returns the chunk count.
(check "iff-sift-fixture"
       (list (format nil ". FORM 64 ILBM~%. . BMHD 20 ILBM~%. . ANNO 5 ILBM~%. . BODY 10 ILBM~%File scan complete.~%")
             4)
  (let (count)
    (let ((out (with-output-to-string (s)
                 (setf count (amiga.iff:sift *iff-fixture* :stream s)))))
      (list out count))))

;; MAP-CHUNKS delivers id, type, size and depth for every chunk entered.
;; iff_Depth counts the default outer context too: 1 at the top FORM.
(check "iff-map-chunks"
       '(("FORM" "ILBM" 64 1) ("BMHD" "ILBM" 20 2)
         ("ANNO" "ILBM" 5 2) ("BODY" "ILBM" 10 2))
  (let (rows)
    (amiga.iff:map-chunks
     (lambda (id type size depth)
       (push (list (amiga.iff:id-string id) (amiga.iff:id-string type)
                   size depth)
             rows))
     *iff-fixture*)
    (nreverse rows)))

;; PARSE-STEP under RAWSTEP: a push for every chunk, an :END-OF-CHUNK
;; just before every pop (the outermost FORM included), then :EOF.
(check "iff-parse-steps"
       '(:chunk :chunk :end-of-chunk :chunk :end-of-chunk
         :chunk :end-of-chunk :end-of-chunk :eof)
  (amiga.iff:with-iff (in *iff-fixture*)
    (loop for step = (amiga.iff:parse-step in)
          collect step
          until (eq step :eof))))

;; CURRENT-CHUNK on the just-entered FORM node, and the depth there.
(check "iff-current-chunk-form" '(:chunk "FORM" "ILBM" 64 1)
  (amiga.iff:with-iff (in *iff-fixture*)
    (let ((step (amiga.iff:parse-step in)))
      (multiple-value-bind (id type size) (amiga.iff:current-chunk in)
        (list step (amiga.iff:id-string id) (amiga.iff:id-string type)
              size (amiga.iff:iff-depth in))))))

;; READ-CHUNK-BYTES clips at the chunk boundary: a 64-byte vector gets
;; exactly BMHD's 20 known bytes, a second read finds the chunk spent.
(check "iff-read-chunk-bytes" '(20 t 0)
  (amiga.iff:with-iff (in *iff-fixture*)
    (loop for step = (amiga.iff:parse-step in)
          until (or (eq step :eof)
                    (and (eq step :chunk)
                         (eql (amiga.iff:current-chunk in)
                              (amiga.iff:string-id "BMHD")))))
    (let* ((v (make-array 64 :element-type '(unsigned-byte 8)))
           (n (amiga.iff:read-chunk-bytes in v)))
      (list n
            (loop for i below n always (= (aref v i) i))
            (amiga.iff:read-chunk-bytes in v)))))

;; The write side round-trips through the independent SIFT: PUSH-CHUNK
;; with the IFFSIZE_UNKNOWN default gets back-patched by POP-CHUNK, the
;; odd NAME chunk gets its pad byte (36 = 4 + (8+7+1) + (8+8), 44 bytes
;; on disk), strings and byte vectors both write.
(check "iff-write-roundtrip"
       (list (format nil ". FORM 36 TEST~%. . NAME 7 TEST~%. . DATA 8 TEST~%File scan complete.~%")
             44)
  (progn
    (amiga.iff:with-iff (out *iff-rt* :direction :write)
      (amiga.iff:push-chunk out "TEST" "FORM")
      (amiga.iff:push-chunk out nil "NAME")
      (amiga.iff:write-chunk-bytes out "amilisp")
      (amiga.iff:pop-chunk out)
      (amiga.iff:push-chunk out nil "DATA")
      (amiga.iff:write-chunk-bytes out
                                   (make-array 8 :element-type '(unsigned-byte 8)
                                                 :initial-element 7))
      (amiga.iff:pop-chunk out)
      (amiga.iff:pop-chunk out))
    (list (with-output-to-string (s) (amiga.iff:sift *iff-rt* :stream s))
          (with-open-file (s *iff-rt* :element-type '(unsigned-byte 8))
            (file-length s)))))

;; The C sift's -c: write a FORM FTXT clip, SIFT the clipboard.
;; 32 = 4 + 8 + 19 + 1 pad.
(check "iff-clipboard-roundtrip"
       (list (format nil ". FORM 32 FTXT~%. . CHRS 19 FTXT~%File scan complete.~%")
             2)
  (progn
    (amiga.iff:with-iff (out :clipboard :direction :write)
      (amiga.iff:push-chunk out "FTXT" "FORM")
      (amiga.iff:push-chunk out nil "CHRS")
      (amiga.iff:write-chunk-bytes out "sift clipboard test")
      (amiga.iff:pop-chunk out)
      (amiga.iff:pop-chunk out))
    (let (count)
      (let ((out (with-output-to-string (s)
                   (setf count (amiga.iff:sift :clipboard :stream s)))))
        (list out count)))))

;; A non-IFF file is refused with the C's IFFERR_NOTIFF text.
(check "iff-not-iff-errors" t
  (progn
    (with-open-file (s *iff-bad* :direction :output :if-exists :supersede)
      (write-string "this is not an IFF file at all" s))
    (handler-case
        (progn (amiga.iff:sift *iff-bad* :stream (make-broadcast-stream)) nil)
      (error (e) (and (search "Not an IFF file" (format nil "~A" e)) t)))))

;; A FORM whose size runs past the actual bytes errors out instead of
;; scanning garbage.
(check "iff-truncated-errors" t
  (progn
    (with-open-file (s *iff-bad* :direction :output :if-exists :supersede
                                 :element-type '(unsigned-byte 8))
      (iff-test-wid s "FORM") (iff-test-w32 s 100) (iff-test-wid s "ILBM"))
    (handler-case
        (progn (amiga.iff:sift *iff-bad* :stream (make-broadcast-stream)) nil)
      (error () t))))

;; A missing file signals with the DOS error code in the message.
(check "iff-open-missing-errors" t
  (handler-case
      (progn (amiga.iff:open-iff "T:no-such-dir/nope.iff") nil)
    (error (e) (and (search "DOS error" (format nil "~A" e)) t))))

;; CLOSE-IFF is idempotent; anything else on a closed file errors.
(check "iff-closed" '(t t :closed-error)
  (let ((f (amiga.iff:open-iff *iff-fixture*)))
    (list (amiga.iff:close-iff f)
          (amiga.iff:close-iff f)
          (handler-case (progn (amiga.iff:parse-step f) nil)
            (error () :closed-error)))))

;; WITH-IFF closes on a non-local exit: the follow-up :WRITE open
;; (which replaces the file) succeeds, proving no handle was left.
(check "iff-unwind-closes" :reopened
  (progn
    (ignore-errors
      (amiga.iff:with-iff (f *iff-fixture*)
        (error "boom")))
    (amiga.iff:with-iff (f *iff-fixture* :direction :write)
      (amiga.iff:push-chunk f "TEST" "FORM")
      (amiga.iff:pop-chunk f))
    :reopened))

(check "iff-fixture-cleanup" t
  (progn
    (delete-file *iff-fixture*)
    (delete-file *iff-rt*)
    (delete-file *iff-bad*)
    t))
