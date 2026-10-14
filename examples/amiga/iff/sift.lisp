;;; sift.lisp — print the chunk structure of IFF files.
;;;
;;; Common Lisp port of the NDK 3.1 sift example (Examples2/IFF/other/
;;; sift.c on the Amiga Developer CD): parse any IFF file — or the
;;; clipboard, the C's -c switch — with ParseIFF in IFFPARSE_RAWSTEP
;;; mode and print an IFFCheck-like listing of every chunk, indented by
;;; nesting depth.  The parsing lives in AMIGA.IFF (lib/amiga/iff.lisp);
;;; (amiga.iff:sift "file") is the whole program.
;;;
;;; What it shows: the write side (WITH-IFF :WRITE, PUSH-CHUNK /
;;; WRITE-CHUNK-BYTES / POP-CHUNK with back-patched sizes) building a
;;; nested CAT-of-FORMs file, SIFT listing it, the read side
;;; (PARSE-STEP / CURRENT-CHUNK / READ-CHUNK-BYTES) recovering chunk
;;; data, and SIFT on the clipboard.
;;;
;;; Run on AmigaOS 3+ or MorphOS:
;;;   amilisp --load examples/amiga/iff/sift.lisp
;;; then sift your own files with (amiga.iff:sift "work:image.ilbm").

(require "amiga/iff")

(defpackage "IFF-SIFT"
  (:use "CL")
  (:local-nicknames ("IFF" "AMIGA.IFF")))

(in-package "IFF-SIFT")

(defparameter *demo-file* "T:sift-demo.iff")

(defun build-demo-file ()
  "Write a nested IFF: a CAT of two FTXT FORMs, the second with an
odd-sized NAME chunk (iffparse writes the pad byte).  Every size is
back-patched by POP-CHUNK from the IFFSIZE_UNKNOWN default."
  (iff:with-iff (out *demo-file* :direction :write)
    (iff:push-chunk out "FTXT" "CAT ")
    (iff:push-chunk out "FTXT" "FORM")
    (iff:push-chunk out nil "CHRS")
    (iff:write-chunk-bytes out "sift demonstration text, part one.")
    (iff:pop-chunk out)
    (iff:pop-chunk out)
    (iff:push-chunk out "FTXT" "FORM")
    (iff:push-chunk out nil "NAME")
    (iff:write-chunk-bytes out "amilisp")
    (iff:pop-chunk out)
    (iff:push-chunk out nil "CHRS")
    (iff:write-chunk-bytes out "part two.")
    (iff:pop-chunk out)
    (iff:pop-chunk out)
    (iff:pop-chunk out)))

(defun read-chrs-chunks ()
  "Walk the demo file by hand — PARSE-STEP / CURRENT-CHUNK — and read
every CHRS chunk's data back into a Lisp string."
  (let ((chrs-id (iff:string-id "CHRS"))
        (texts '()))
    (iff:with-iff (in *demo-file* :direction :read)
      (loop for step = (iff:parse-step in)
            until (eq step :eof)
            do (when (eq step :chunk)
                 (multiple-value-bind (id type size) (iff:current-chunk in)
                   (declare (ignore type))
                   (when (= id chrs-id)
                     (let ((data (make-array size
                                             :element-type '(unsigned-byte 8))))
                       (iff:read-chunk-bytes in data)
                       (push (map 'string #'code-char data) texts)))))))
    (nreverse texts)))

(defun run ()
  (format t "sift: building ~A (CAT of two FTXT FORMs)...~%" *demo-file*)
  (build-demo-file)
  (format t "~%(amiga.iff:sift ~S):~%" *demo-file*)
  (iff:sift *demo-file*)
  (format t "~%CHRS chunks read back:~%")
  (dolist (text (read-chrs-chunks))
    (format t "  ~S~%" text))
  (format t "~%(amiga.iff:sift :clipboard):    ; the C sift's -c~%")
  (handler-case (iff:sift :clipboard)
    (error (e) (format t "  no IFF clip: ~A~%" e)))
  (delete-file *demo-file*)
  (format t "~%done.  Try (amiga.iff:sift \"path/to/your.iff\") yourself.~%"))

(if (iff:available-p)
    (run)
    (format t "iff not available - AmigaOS/MorphOS with iffparse.library required~%"))
