;;; gray-streams.lisp — Gray Streams implementation for AmiLisp
;;;
;;; Provides the fundamental stream classes and generic functions required
;;; by trivial-gray-streams and flexi-streams.
;;;
;;; Package is named GRAY (same as ECL/CLISP) with "..." as nickname
;;; so trivial-gray-streams' fallback for unknown implementations finds it.
;;;
;;; Load BEFORE trivial-gray-streams (i.e., before quickloading libraries
;;; that depend on it).

(in-package "COMMON-LISP-USER")

(defpackage "GRAY"
  (:use "COMMON-LISP")
  (:nicknames "...")
  (:export
   ;; Classes
   #:fundamental-stream
   #:fundamental-input-stream
   #:fundamental-output-stream
   #:fundamental-character-stream
   #:fundamental-binary-stream
   #:fundamental-character-input-stream
   #:fundamental-character-output-stream
   #:fundamental-binary-input-stream
   #:fundamental-binary-output-stream
   ;; Generic functions
   #:stream-read-char
   #:stream-unread-char
   #:stream-read-char-no-hang
   #:stream-peek-char
   #:stream-listen
   #:stream-read-line
   #:stream-clear-input
   #:stream-write-char
   #:stream-line-column
   #:stream-start-line-p
   #:stream-write-string
   #:stream-terpri
   #:stream-fresh-line
   #:stream-finish-output
   #:stream-force-output
   #:stream-clear-output
   #:stream-advance-to-column
   #:stream-read-byte
   #:stream-write-byte
   ;; Bulk sequence I/O (trivial-gray-streams extension)
   #:stream-read-sequence
   #:stream-write-sequence))

(in-package "GRAY")

;;; ====================================================================
;;; Fundamental stream class hierarchy
;;; ====================================================================

;; Inherit from the built-in STREAM class (not just STANDARD-OBJECT) so that
;; CLOS methods specialized on STREAM dispatch to Gray stream instances — e.g.
;; rfc2388:parse-mime ((input stream) ...), which hunchentoot's multipart
;; form-data parser calls on a flexi (Gray) stream.  Without STREAM in the CPL
;; that dispatch fails with "No applicable method" even though TYPEP reports
;; the instance as a STREAM.
(defclass fundamental-stream (stream standard-object)
  ((open-p :initform t :accessor stream-open-p))
  (:documentation "Base class for Gray streams."))

(defclass fundamental-input-stream (fundamental-stream)
  ()
  (:documentation "Base class for Gray input streams."))

(defclass fundamental-output-stream (fundamental-stream)
  ()
  (:documentation "Base class for Gray output streams."))

(defclass fundamental-character-stream (fundamental-stream)
  ()
  (:documentation "Base class for Gray character streams."))

(defclass fundamental-binary-stream (fundamental-stream)
  ()
  (:documentation "Base class for Gray binary streams."))

(defclass fundamental-character-input-stream
    (fundamental-input-stream fundamental-character-stream)
  ()
  (:documentation "Base class for Gray character input streams."))

(defclass fundamental-character-output-stream
    (fundamental-output-stream fundamental-character-stream)
  ()
  (:documentation "Base class for Gray character output streams."))

(defclass fundamental-binary-input-stream
    (fundamental-input-stream fundamental-binary-stream)
  ()
  (:documentation "Base class for Gray binary input streams."))

(defclass fundamental-binary-output-stream
    (fundamental-output-stream fundamental-binary-stream)
  ()
  (:documentation "Base class for Gray binary output streams."))

;;; ====================================================================
;;; Generic functions — character input
;;; ====================================================================

(defgeneric stream-read-char (stream)
  (:documentation "Read one character from STREAM. Return character or :EOF."))

(defgeneric stream-unread-char (stream character)
  (:documentation "Un-read CHARACTER on STREAM."))

(defgeneric stream-read-char-no-hang (stream)
  (:documentation "Read one character from STREAM if available. Return character, NIL, or :EOF."))

(defmethod stream-read-char-no-hang ((stream fundamental-character-input-stream))
  (stream-read-char stream))

(defgeneric stream-peek-char (stream)
  (:documentation "Peek at the next character from STREAM without consuming it."))

(defmethod stream-peek-char ((stream fundamental-character-input-stream))
  (let ((ch (stream-read-char stream)))
    (unless (eq ch :eof)
      (stream-unread-char stream ch))
    ch))

(defgeneric stream-listen (stream)
  (:documentation "Return true if a character is available on STREAM."))

(defmethod stream-listen ((stream fundamental-character-input-stream))
  (let ((ch (stream-read-char-no-hang stream)))
    (when (and ch (not (eq ch :eof)))
      (stream-unread-char stream ch)
      t)))

(defgeneric stream-read-line (stream)
  (:documentation "Read a line from STREAM. Return (values string missing-newline-p)."))

(defmethod stream-read-line ((stream fundamental-character-input-stream))
  ;; Collect chars into a reversed list, then build the string once at the end.
  ;; This is O(n) instead of O(n^2) from repeated concatenation.
  (let ((chars nil)
        (len 0))
    (loop
      (let ((ch (stream-read-char stream)))
        (when (eq ch :eof)
          (if (zerop len)
              (return (values "" t))
              (let ((s (make-string len)))
                (dolist (c chars)
                  (decf len)
                  (setf (char s len) c))
                (return (values s t)))))
        (when (char= ch #\Newline)
          (if (zerop len)
              (return (values "" nil))
              (let ((s (make-string len)))
                (dolist (c chars)
                  (decf len)
                  (setf (char s len) c))
                (return (values s nil)))))
        (push ch chars)
        (incf len)))))

(defgeneric stream-clear-input (stream)
  (:documentation "Clear buffered input on STREAM."))

(defmethod stream-clear-input ((stream fundamental-input-stream))
  nil)

;;; ====================================================================
;;; Generic functions — character output
;;; ====================================================================

(defgeneric stream-write-char (stream character)
  (:documentation "Write CHARACTER to STREAM."))

(defgeneric stream-line-column (stream)
  (:documentation "Return the column number of STREAM, or NIL if unknown."))

(defmethod stream-line-column ((stream fundamental-character-output-stream))
  nil)

(defgeneric stream-start-line-p (stream)
  (:documentation "Return true if STREAM is at the start of a line."))

(defmethod stream-start-line-p ((stream fundamental-character-output-stream))
  (let ((col (stream-line-column stream)))
    (and col (zerop col))))

(defgeneric stream-write-string (stream string &optional start end)
  (:documentation "Write STRING (or substring) to STREAM."))

(defmethod stream-write-string ((stream fundamental-character-output-stream)
                                string &optional (start 0) end)
  (let ((e (or end (length string))))
    (do ((i start (1+ i)))
        ((>= i e) string)
      (stream-write-char stream (char string i)))))

(defgeneric stream-terpri (stream)
  (:documentation "Write a newline to STREAM."))

(defmethod stream-terpri ((stream fundamental-character-output-stream))
  (stream-write-char stream #\Newline)
  nil)

(defgeneric stream-fresh-line (stream)
  (:documentation "Write a newline if STREAM is not at the start of a line."))

(defmethod stream-fresh-line ((stream fundamental-character-output-stream))
  (unless (stream-start-line-p stream)
    (stream-terpri stream)
    t))

(defgeneric stream-finish-output (stream)
  (:documentation "Ensure all output to STREAM has been sent."))

(defmethod stream-finish-output ((stream fundamental-output-stream))
  nil)

(defgeneric stream-force-output (stream)
  (:documentation "Initiate sending any buffered output on STREAM."))

(defmethod stream-force-output ((stream fundamental-output-stream))
  nil)

(defgeneric stream-clear-output (stream)
  (:documentation "Clear any buffered output on STREAM."))

(defmethod stream-clear-output ((stream fundamental-output-stream))
  nil)

(defgeneric stream-advance-to-column (stream column)
  (:documentation "Advance STREAM to COLUMN, writing spaces if necessary."))

(defmethod stream-advance-to-column ((stream fundamental-character-output-stream) column)
  (let ((cur (stream-line-column stream)))
    (when cur
      (dotimes (i (- column cur))
        (stream-write-char stream #\Space)))
    (not (null cur))))

;;; ====================================================================
;;; Generic functions — binary I/O
;;; ====================================================================

(defgeneric stream-read-byte (stream)
  (:documentation "Read one byte from STREAM. Return byte or :EOF."))

(defgeneric stream-write-byte (stream byte)
  (:documentation "Write BYTE to STREAM."))

;;; ====================================================================
;;; Generic functions — bulk sequence I/O
;;; ====================================================================
;;; These mirror the trivial-gray-streams extension GFs and use the same
;;; lambda list — (stream sequence start end &key &allow-other-keys) — so
;;; trivial-gray-streams' unknown-implementation fallback (which imports
;;; STREAM-READ-SEQUENCE / STREAM-WRITE-SEQUENCE from the GRAY/"..." package)
;;; finds them.  CL:READ-SEQUENCE and CL:WRITE-SEQUENCE dispatch here for
;;; Gray streams (see the COMMON-LISP wrappers below).
;;;
;;; The default methods loop element-by-element, but call STREAM-READ-BYTE /
;;; STREAM-READ-CHAR (etc.) DIRECTLY — bypassing CL:READ-BYTE/READ-CHAR's
;;; per-element stream-resolution and %gray-stream-p type checks.  A library
;;; can specialise these GFs on its own stream class to provide a genuine
;;; bulk transfer (e.g. a decompressor that fills the whole buffer at once)
;;; instead of paying per-element overhead.

(defgeneric stream-read-sequence (stream sequence start end &key &allow-other-keys)
  (:documentation
   "Fill SEQUENCE from START to END with elements read from STREAM.
Return the index of the first element not modified -- END on a full read, or
the position where end-of-file was reached."))

(defmethod stream-read-sequence ((stream fundamental-input-stream)
                                 sequence start end &key)
  ;; Element kind follows the sequence's element type, matching CL:READ-SEQUENCE.
  (let ((e (or end (length sequence)))
        (i start)
        (use-char (and (arrayp sequence)
                       (subtypep (array-element-type sequence) 'character))))
    (block nil
      (loop
        (when (>= i e) (return))
        (let ((elem (if use-char
                        (stream-read-char stream)
                        (stream-read-byte stream))))
          (when (eq elem :eof) (return))
          (setf (elt sequence i) elem)
          (setf i (1+ i)))))
    i))

(defgeneric stream-write-sequence (stream sequence start end &key &allow-other-keys)
  (:documentation
   "Write the elements of SEQUENCE from START to END to STREAM.  Return SEQUENCE."))

(defmethod stream-write-sequence ((stream fundamental-output-stream)
                                  sequence start end &key)
  (let ((e (or end (length sequence))))
    (do ((i start (1+ i)))
        ((>= i e) sequence)
      (let ((elt (elt sequence i)))
        (if (characterp elt)
            (stream-write-char stream elt)
            (stream-write-byte stream elt))))))

;;; ====================================================================
;;; Integration with CL I/O functions
;;; ====================================================================
;;; Make CL:READ-CHAR, CL:WRITE-CHAR, etc. dispatch to gray stream
;;; generic functions when given a CLOS stream instance.

(in-package "COMMON-LISP")

;; Install the CL-I/O integration exactly ONCE per image.  Re-loading this
;; file (quicklisp-compat.lisp LOADs it after boot already did, and any
;; library that pulls trivial-gray-streams can trigger another LOAD) must
;; NOT re-run the LET below.  Each ORIG-X binding captures (SYMBOL-FUNCTION
;; 'X) at load time; after the first load X is already the gray-overridden
;; generic function, so a second load would bind e.g. ORIG-CLOSE to the
;; CLOSE GF itself.  The (STREAM T) method then does (FUNCALL ORIG-CLOSE
;; STREAM), which re-dispatches CLOSE on the same class → unbounded
;; self-recursion (observed: "%GF-DISPATCH-CACHED" VM frame stack overflow
;; closing an ordinary file stream while quickloading).  The marker is a
;; symbol plist entry so it persists across LOADs and is package-safe.
(unless (get 'gray::%cl-io-integration-installed 'done)
;; Save original C builtins
(let ((orig-read-char (symbol-function 'read-char))
      (orig-write-char (symbol-function 'write-char))
      (orig-read-byte (symbol-function 'read-byte))
      (orig-write-byte (symbol-function 'write-byte))
      (orig-read-sequence (symbol-function 'read-sequence))
      (orig-write-sequence (symbol-function 'write-sequence))
      (orig-peek-char (symbol-function 'peek-char))
      (orig-unread-char (symbol-function 'unread-char))
      (orig-read-line (symbol-function 'read-line))
      (orig-write-string (symbol-function 'write-string))
      (orig-write-line (symbol-function 'write-line))
      (orig-terpri (symbol-function 'terpri))
      (orig-fresh-line (symbol-function 'fresh-line))
      (orig-finish-output (symbol-function 'finish-output))
      (orig-force-output (symbol-function 'force-output))
      (orig-clear-output (symbol-function 'clear-output))
      (orig-streamp (symbol-function 'streamp))
      (orig-input-stream-p (symbol-function 'input-stream-p))
      (orig-output-stream-p (symbol-function 'output-stream-p))
      (orig-close (symbol-function 'close))
      (orig-open-stream-p (symbol-function 'open-stream-p))
      (orig-stream-element-type (if (fboundp 'stream-element-type)
                                    (symbol-function 'stream-element-type)
                                    nil))
      (orig-princ   (symbol-function 'princ))
      (orig-prin1   (symbol-function 'prin1))
      (orig-print   (symbol-function 'print))
      (orig-write   (symbol-function 'write))
      (orig-format  (symbol-function 'format))
      (orig-pprint  (symbol-function 'pprint))
      (orig-listen  (symbol-function 'listen))
      (orig-file-position (symbol-function 'file-position)))

  (defun %gray-stream-p (x)
    "Return T if X is a Gray stream (CLOS instance of fundamental-stream)."
    (and (typep x 'standard-object)
         (typep x 'gray:fundamental-stream)))

  (defun %resolve-input-stream (stream)
    (let ((s (cond ((null stream) *standard-input*)
                   ((eq stream t) *terminal-io*)
                   (t stream))))
      (if (typep s 'two-way-stream) (two-way-stream-input-stream s) s)))

  (defun %resolve-output-stream (stream)
    (let ((s (cond ((null stream) *standard-output*)
                   ((eq stream t) *terminal-io*)
                   (t stream))))
      (if (typep s 'two-way-stream) (two-way-stream-output-stream s) s)))

  ;; STREAMP — also true for gray streams
  (defun streamp (x)
    (or (funcall orig-streamp x)
        (%gray-stream-p x)))

  ;; INPUT-STREAM-P — GF so libraries can add methods
  (defgeneric input-stream-p (stream))
  (defmethod input-stream-p ((stream gray:fundamental-input-stream))
    t)
  (defmethod input-stream-p ((stream t))
    (if (%gray-stream-p stream) nil (funcall orig-input-stream-p stream)))

  ;; OUTPUT-STREAM-P — GF so libraries can add methods
  (defgeneric output-stream-p (stream))
  (defmethod output-stream-p ((stream gray:fundamental-output-stream))
    t)
  (defmethod output-stream-p ((stream t))
    (if (%gray-stream-p stream) nil (funcall orig-output-stream-p stream)))

  ;; CLOSE — define as GF so libraries can add methods (e.g. flexi-streams)
  ;; while built-in streams keep working via the default (T) method.
  (defgeneric close (stream &key abort))
  (defmethod close ((stream gray:fundamental-stream) &key abort)
    (declare (ignore abort))
    ;; The OPEN-P slot lives in the GRAY package (see DEFCLASS
    ;; FUNDAMENTAL-STREAM); these methods are defined in COMMON-LISP, so an
    ;; unqualified 'OPEN-P would read as CL:OPEN-P and SLOT-VALUE would fail
    ;; with "no slot named COMMON-LISP::OPEN-P".  Qualify it.
    (setf (slot-value stream 'gray::open-p) nil)
    t)
  (defmethod close ((stream t) &key abort)
    (declare (ignore abort))
    (funcall orig-close stream))

  ;; OPEN-STREAM-P — GF so libraries can add methods
  (defgeneric open-stream-p (stream))
  (defmethod open-stream-p ((stream gray:fundamental-stream))
    (slot-value stream 'gray::open-p))
  (defmethod open-stream-p ((stream t))
    (funcall orig-open-stream-p stream))

  ;; STREAM-ELEMENT-TYPE — GF so libraries can add methods
  (defgeneric stream-element-type (stream))
  (defmethod stream-element-type ((stream t))
    (if orig-stream-element-type
        (funcall orig-stream-element-type stream)
        'character))

  ;; READ-CHAR
  (defun read-char (&optional stream (eof-error-p t) eof-value recursive-p)
    (declare (ignore recursive-p))
    (let ((s (%resolve-input-stream stream)))
      (if (%gray-stream-p s)
          (let ((result (gray:stream-read-char s)))
            (if (eq result :eof)
                (if eof-error-p
                    (error 'end-of-file :stream s)
                    eof-value)
                result))
          (funcall orig-read-char s eof-error-p eof-value))))

  ;; WRITE-CHAR
  (defun write-char (character &optional stream)
    (let ((s (%resolve-output-stream stream)))
      (if (%gray-stream-p s)
          (progn (gray:stream-write-char s character) character)
          (funcall orig-write-char character s))))

  ;; READ-BYTE
  (defun read-byte (stream &optional (eof-error-p t) eof-value)
    (let ((s (%resolve-input-stream stream)))
      (if (%gray-stream-p s)
          (let ((result (gray:stream-read-byte s)))
            (if (eq result :eof)
                (if eof-error-p
                    (error 'end-of-file :stream s)
                    eof-value)
                result))
          (funcall orig-read-byte s eof-error-p eof-value))))

  ;; WRITE-BYTE
  (defun write-byte (byte stream)
    (let ((s (%resolve-output-stream stream)))
      (if (%gray-stream-p s)
          (progn (gray:stream-write-byte s byte) byte)
          (funcall orig-write-byte byte s))))

  ;; READ-SEQUENCE — dispatch to the bulk GF for Gray streams; native
  ;; streams keep the boot.lisp implementation.  END is resolved to a
  ;; concrete integer so specialised STREAM-READ-SEQUENCE methods (and
  ;; trivial-gray-streams) always receive a non-NIL END, matching the tgs
  ;; (stream sequence start end) convention.
  (defun read-sequence (sequence stream &key (start 0) end)
    (let ((s (%resolve-input-stream stream)))
      (if (%gray-stream-p s)
          (gray:stream-read-sequence s sequence start (or end (length sequence)))
          (funcall orig-read-sequence sequence stream :start start :end end))))

  ;; WRITE-SEQUENCE — dispatch to the bulk GF for Gray streams.
  (defun write-sequence (sequence stream &key (start 0) end)
    (let ((s (%resolve-output-stream stream)))
      (if (%gray-stream-p s)
          (gray:stream-write-sequence s sequence start (or end (length sequence)))
          (funcall orig-write-sequence sequence stream :start start :end end))))

  ;; PEEK-CHAR
  (defun peek-char (&optional peek-type stream (eof-error-p t) eof-value recursive-p)
    (declare (ignore recursive-p))
    (let ((s (%resolve-input-stream stream)))
      (if (%gray-stream-p s)
          (cond
            ((null peek-type)
             (let ((ch (gray:stream-peek-char s)))
               (if (eq ch :eof)
                   (if eof-error-p
                       (error 'end-of-file :stream s)
                       eof-value)
                   ch)))
            ((eq peek-type t)
             ;; Skip whitespace
             (loop
               (let ((ch (gray:stream-read-char s)))
                 (cond ((eq ch :eof)
                        (return (if eof-error-p
                                    (error 'end-of-file :stream s)
                                    eof-value)))
                       ((not (member ch '(#\Space #\Tab #\Newline #\Return #\Page)))
                        (gray:stream-unread-char s ch)
                        (return ch))))))
            ((characterp peek-type)
             ;; Skip until peek-type
             (loop
               (let ((ch (gray:stream-read-char s)))
                 (cond ((eq ch :eof)
                        (return (if eof-error-p
                                    (error 'end-of-file :stream s)
                                    eof-value)))
                       ((char= ch peek-type)
                        (gray:stream-unread-char s ch)
                        (return ch)))))))
          (funcall orig-peek-char peek-type s eof-error-p eof-value))))

  ;; UNREAD-CHAR
  (defun unread-char (character &optional stream)
    (let ((s (%resolve-input-stream stream)))
      (if (%gray-stream-p s)
          (progn (gray:stream-unread-char s character) nil)
          (funcall orig-unread-char character s))))

  ;; READ-LINE
  (defun read-line (&optional stream (eof-error-p t) eof-value recursive-p)
    (declare (ignore recursive-p))
    (let ((s (%resolve-input-stream stream)))
      (if (%gray-stream-p s)
          (multiple-value-bind (line missing-newline-p)
              (gray:stream-read-line s)
            (if (and missing-newline-p (zerop (length line)))
                (if eof-error-p
                    (error 'end-of-file :stream s)
                    (values eof-value t))
                (values line missing-newline-p)))
          (funcall orig-read-line s eof-error-p eof-value))))

  ;; WRITE-STRING
  (defun write-string (string &optional stream &key (start 0) end)
    (let ((s (%resolve-output-stream stream)))
      (if (%gray-stream-p s)
          (progn (gray:stream-write-string s string start end) string)
          (funcall orig-write-string string s :start start :end end))))

  ;; WRITE-LINE
  (defun write-line (string &optional stream &key (start 0) end)
    (let ((s (%resolve-output-stream stream)))
      (if (%gray-stream-p s)
          (progn
            (gray:stream-write-string s string start end)
            (gray:stream-terpri s)
            string)
          (funcall orig-write-line string s :start start :end end))))

  ;; TERPRI
  (defun terpri (&optional stream)
    (let ((s (%resolve-output-stream stream)))
      (if (%gray-stream-p s)
          (progn (gray:stream-terpri s) nil)
          (funcall orig-terpri s))))

  ;; FRESH-LINE
  (defun fresh-line (&optional stream)
    (let ((s (%resolve-output-stream stream)))
      (if (%gray-stream-p s)
          (gray:stream-fresh-line s)
          (funcall orig-fresh-line s))))

  ;; FINISH-OUTPUT
  (defun finish-output (&optional stream)
    (let ((s (%resolve-output-stream stream)))
      (if (%gray-stream-p s)
          (gray:stream-finish-output s)
          (funcall orig-finish-output s))))

  ;; FORCE-OUTPUT
  (defun force-output (&optional stream)
    (let ((s (%resolve-output-stream stream)))
      (if (%gray-stream-p s)
          (gray:stream-force-output s)
          (funcall orig-force-output s))))

  ;; CLEAR-OUTPUT
  (defun clear-output (&optional stream)
    (let ((s (%resolve-output-stream stream)))
      (if (%gray-stream-p s)
          (gray:stream-clear-output s)
          (funcall orig-clear-output s))))

  ;; Helper: return plist with KEY (and its value) removed
  (defun %plist-drop-key (plist key)
    (let ((result nil))
      (do ((rest plist (cddr rest)))
          ((null rest) (nreverse result))
        (unless (eq (car rest) key)
          (push (car rest) result)
          (push (cadr rest) result)))))

  ;; The gray fallbacks below capture through WITH-OUTPUT-TO-STRING (not a bare
  ;; MAKE-STRING-OUTPUT-STREAM) so the temp stream is CLOSEd on exit and its
  ;; platform outbuf slot is freed eagerly.  A bare, never-closed temp stream
  ;; leaks its outbuf until the next GC; per-output-call leaks saturate the
  ;; outbuf table under logging-heavy loads (one slot per princ/format call).

  ;; PRINC
  (defun princ (object &optional stream)
    (let ((s (%resolve-output-stream stream)))
      (if (%gray-stream-p s)
          (progn
            (write-string (with-output-to-string (tmp)
                            (funcall orig-princ object tmp))
                          s)
            object)
          (funcall orig-princ object s))))

  ;; PRIN1
  (defun prin1 (object &optional stream)
    (let ((s (%resolve-output-stream stream)))
      (if (%gray-stream-p s)
          (progn
            (write-string (with-output-to-string (tmp)
                            (funcall orig-prin1 object tmp))
                          s)
            object)
          (funcall orig-prin1 object s))))

  ;; PRINT
  (defun print (object &optional stream)
    (let ((s (%resolve-output-stream stream)))
      (if (%gray-stream-p s)
          (progn
            (write-string (with-output-to-string (tmp)
                            (funcall orig-print object tmp))
                          s)
            object)
          (funcall orig-print object s))))

  ;; WRITE — intercept :stream keyword, forward all other keys to orig-write
  (defun write (object &rest args)
    (let* ((raw-stream (getf args :stream *standard-output*))
           (s (%resolve-output-stream raw-stream)))
      (if (%gray-stream-p s)
          (let ((other-args (%plist-drop-key args :stream)))
            (write-string (with-output-to-string (tmp)
                            (apply orig-write object :stream tmp other-args))
                          s)
            object)
          (apply orig-write object args))))

  ;; FORMAT — NIL destination always fast-paths to orig-format (returns string).
  ;; T means *standard-output* (per HyperSpec), not *terminal-io*.
  (defun format (destination control-string &rest format-args)
    (cond
      ((null destination)
       (apply orig-format nil control-string format-args))
      (t
       (let* ((dest0 (if (eq destination t) *standard-output* destination))
              (s (if (typep dest0 'two-way-stream)
                     (two-way-stream-output-stream dest0)
                     dest0)))
         (if (%gray-stream-p s)
             (progn
               (write-string (with-output-to-string (tmp)
                               (apply orig-format tmp control-string format-args))
                             s)
               nil)
             (apply orig-format destination control-string format-args))))))

  ;; PPRINT
  (defun pprint (object &optional stream)
    (let ((s (%resolve-output-stream stream)))
      (if (%gray-stream-p s)
          (progn
            (write-string (with-output-to-string (tmp)
                            (funcall orig-pprint object tmp))
                          s)
            nil)
          (funcall orig-pprint object s))))

  ;; LISTEN — for Gray streams dispatch to stream-listen; otherwise delegate
  ;; to the original C builtin (which knows about socket / string streams).
  ;; The earlier "no C builtin" assumption was wrong and made LISTEN return
  ;; NIL for every non-Gray stream, breaking e.g. usocket:wait-for-input over
  ;; socket streams (hunchentoot's accept loop).
  (defun listen (&optional stream)
    (let ((s (%resolve-input-stream stream)))
      (if (%gray-stream-p s)
          (gray:stream-listen s)
          (funcall orig-listen s))))
  (export 'listen)

  ;; FILE-POSITION — a Gray instance is not a builtin CL_Stream, so the C
  ;; builtin used to signal "not a stream" on it, crashing e.g.
  ;; flexi-streams' MAYBE-REWIND probe (drakma reading a chunked text body
  ;; calls it on the underlying chunga stream).  For Gray streams consult
  ;; the trivial-gray-streams STREAM-FILE-POSITION protocol when that
  ;; system is loaded (its default method on T answers NIL); without it, or
  ;; when the setter fails, answer NIL — CLHS' "position cannot be
  ;; determined / repositioning not performed", which is exactly what
  ;; MAYBE-REWIND expects from a non-positionable stream.
  (defun file-position (stream &optional (position nil position-p))
    (if (%gray-stream-p stream)
        (let* ((pkg (find-package "TRIVIAL-GRAY-STREAMS"))
               (sym (and pkg (find-symbol "STREAM-FILE-POSITION" pkg))))
          (cond ((not (and sym (fboundp sym))) nil)
                (position-p
                 (let ((setter (ignore-errors (fdefinition (list 'setf sym)))))
                   (and setter (funcall setter position stream))))
                (t (funcall sym stream))))
        (if position-p
            (funcall orig-file-position stream position)
            (funcall orig-file-position stream))))
  (export 'file-position)

) ;; end let
(setf (get 'gray::%cl-io-integration-installed 'done) t)
) ;; end (unless ... already installed)

;; CLEAR-INPUT — no C builtin, define directly
(defun clear-input (&optional stream)
  (let ((s (%resolve-input-stream stream)))
    (when (%gray-stream-p s)
      (gray:stream-clear-input s)))
  nil)
(export 'clear-input)

;; Register the module so a second (require "gray-streams") is a no-op
;; instead of reloading and recompiling this file every time.
(provide "gray-streams")

(in-package "COMMON-LISP-USER")
