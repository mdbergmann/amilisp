;;; dev-commands.lisp -- the editor-facing command language for AmiLisp
;;;
;;; Loaded via (require "dev-commands").
;;;
;;; A command is one string in, one reply string plus a return code out:
;;;
;;;   (ext.dev:handle-command "LOAD Work:src/foo.lisp")
;;;   => 10, "Work:src/foo.lisp:3: ERROR: Too many arguments to FOO ...
;;;           1 error(s), 0 warning(s)"
;;;
;;; That signature is the whole point of keeping this file separate from
;;; lib/amiga/arexx.lisp: the ARexx port is only a transport for these
;;; strings, so everything that decides what a command MEANS -- parsing,
;;; error capture, diagnostic rendering -- is plain portable Lisp that runs
;;; and is tested on the host, where there is no ARexx at all.
;;;
;;; See tests/test_dev_commands.sh for the executable specification.

(defpackage "EXT.DEV"
  (:use "CL")
  (:export
   ;; Entry point
   "HANDLE-COMMAND"
   ;; Return codes (the ARexx severity ladder -- see +RC-WARN+)
   "+RC-OK+" "+RC-WARN+" "+RC-ERROR+" "+RC-FATAL+"
   ;; Session state
   "*COMMAND-PACKAGE*" "*MAX-RESULT-LENGTH*" "*LAST-RESULT*"
   ;; Introspection commands' knobs
   "*MAX-COMPLETIONS*" "*PRETTY-MARGIN*" "*MAX-INSPECT-PARTS*"
   ;; The REPL's way back to the editor (lib/dev-repl.lisp)
   "*REPL-SEND*"
   ;; Systems: what an editor's Load System / Test System evaluate
   "LOAD-ASD-SYSTEM" "TEST-ASD-SYSTEM"
   ;; Introspection / extension
   "*COMMANDS*" "DEFINE-COMMAND" "DEFINE-RAW-COMMAND" "*RAW-COMMANDS*"))

(in-package "EXT.DEV")

;;; ================================================================
;;; Return codes
;;;
;;; These are ARexx severity levels, and the values are not arbitrary:
;;; ARexx aborts a macro when a command's return code reaches the FAILAT
;;; threshold, which defaults to 10.  Warnings must therefore stay BELOW
;;; 10 (a style warning should not kill the editor macro) and errors must
;;; reach it (a failed load should).
;;; ================================================================

(defconstant +rc-ok+     0)
(defconstant +rc-warn+   5)
(defconstant +rc-error+ 10)
(defconstant +rc-fatal+ 20)

(defvar *command-package* (find-package "CL-USER")
  "Package that EVAL and READ use for commands.  Set by IN-PACKAGE.
Per-connection state: the REPL's *PACKAGE* is left alone.")

(defvar *max-result-length* 8192
  "Cap on the reply string.  A reply longer than this is truncated on a
line boundary and a marker line is appended.  ARexx result strings are
argstrings and every editor holds the whole thing in memory, so an
unbounded compiler log is not a reasonable thing to send back.")

(defvar *last-result* ""
  "Reply text of the most recent command.  Kept because ARexx only
transmits RESULT when the return code is 0 -- LASTRESULT is how a macro
retrieves the diagnostics that came with a failing rc.")

;;; ================================================================
;;; Diagnostics
;;; ================================================================

(defstruct (diag (:constructor make-diag (severity file line text)))
  severity   ; :ERROR or :WARNING
  file       ; namestring or NIL
  line       ; integer or NIL
  text)      ; the condition report

(defun %condition-text (condition)
  "Report string for CONDITION, never signalling in the process -- a
PRINT-OBJECT method that itself errors must not take down the port."
  (handler-case (princ-to-string condition)
    (error () (format nil "<unprintable ~a condition>" (type-of condition)))))

(defun %own-source-file ()
  "The source file this very file was compiled from, as EXT:BACKTRACE
spells it.  Frame 0 inside a function IS that function, so asking for
one frame here names our own file whether we were loaded from source or
from a FASL."
  (third (first (ext:backtrace 1))))

(defvar *this-file* (%own-source-file)
  "Used to skip our own frames when locating a diagnostic -- see
%ERROR-LOCATION.")

(defmacro %error-location (locate)
  "Source location of the form that is signalling, as (values FILE LINE),
or (values NIL NIL) when LOCATE is false or no location is credible.

EXT:BACKTRACE returns (INDEX NAME FILE LINE) innermost-first.  Two cases
have to be handled differently, which is why this skips rather than just
taking frame 0:

  - Errors raised inside the runtime (undefined function, bad arity) are
    captured at signal time, so frame 0 is already the offending form.
  - Conditions signalled by CL:ERROR run their handlers on top of the
    live stack, so frame 0 is OUR handler, in THIS file.

Skipping leading frames from *THIS-FILE* gives the offending user form in
both cases.  LOCATE is false for commands like EVAL where the form came
from a command string and never had a source location to begin with --
reporting the stale file the compiler last stamped would be worse than
reporting none.

A MACRO, not a function, and that is load-bearing twice over: a function
call would push yet another frame, and the skip depends on the handler
frame being in this file."
  `(if ,locate
       (let ((found nil))
         (dolist (fr (ext:backtrace 8))
           (unless (equal (third fr) *this-file*)
             (setf found fr)
             (return)))
         (if (and found (third found))
             (values (third found) (fourth found))
             (values nil nil)))
       (values nil nil)))

(defun %call-guarded (thunk)
  "Call THUNK, returning (values RESULT ESCAPED-P).  If a condition
unwinds out of THUNK, control lands here instead of continuing outward,
and ESCAPED-P is true.

Deliberately built from CATCH/UNWIND-PROTECT/THROW rather than
HANDLER-CASE: a handler for ERROR anywhere in the dynamic extent wins
the handler search ahead of LOAD's own per-top-level-form recovery, so
the first bad form would abort the file and the editor would see one
diagnostic where there are five.  CATCH establishes no handler, so the
condition system never sees this guard; the cleanup only fires once the
error is already unwinding, and throwing from it abandons that unwind
here.  See tests/test_dev_commands.sh."
  (let ((escaped t) (result nil))
    (catch '%escape
      (unwind-protect
           (progn (setf result (funcall thunk))
                  (setf escaped nil))
        (when escaped (throw '%escape nil))))
    (values result escaped)))

(defun %render-diag (d)
  (let ((file (diag-file d))
        (line (diag-line d)))
    (if (and file line)
        (format nil "~a:~d: ~a: ~a" file line (diag-severity d) (diag-text d))
        (format nil "~a: ~a" (diag-severity d) (diag-text d)))))

(defun %summary (diags)
  (let ((errors 0) (warnings 0))
    (dolist (d diags)
      (if (eq (diag-severity d) :error) (incf errors) (incf warnings)))
    (values errors warnings
            (format nil "~d error(s), ~d warning(s)" errors warnings))))

(defun call-with-diagnostics (thunk &key locate (capture t))
  "Call THUNK, returning (values LOG DIAGS ESCAPED-P): everything it
printed, a DIAG struct for every ERROR and WARNING signalled inside it,
and whether a condition unwound out of THUNK entirely.  With CAPTURE
false nothing is captured -- what THUNK prints goes where the streams
point, which is how the REPL thread streams a LOAD to the editor as it
runs (lib/dev-repl.lisp, REPL-LOAD) -- and LOG is the empty string.

The ERROR handler DECLINES (returns normally) on purpose.  LOAD catches
per top-level form and moves on to the next one, so declining is what
turns `load a file with three broken forms' into three diagnostics
instead of one -- a handler that transferred control would stop at the
first.  WARNING is muffled after recording, since it has already been
turned into a diagnostic line.

Note this relies on LOAD's per-form recovery surviving the UNWIND-PROTECT
that WITH-OUTPUT-TO-STRING expands into; see tests/test_unwind_load.c for
the regression that guards it."
  (let ((diags '()) (escaped nil))
    (flet ((run ()
             (setf escaped
                   (nth-value
                    1
                    (%call-guarded
                     (lambda ()
                       (handler-bind
                           ((warning
                              (lambda (c)
                                (multiple-value-bind (f l) (%error-location locate)
                                  (push (make-diag :warning f l (%condition-text c))
                                        diags))
                                (muffle-warning c)))
                            (error
                              (lambda (c)
                                (multiple-value-bind (f l) (%error-location locate)
                                  (push (make-diag :error f l (%condition-text c))
                                        diags)))))
                         (funcall thunk))))))))
      (let ((log (if capture
                     (with-output-to-string (stream)
                       (let ((*standard-output* stream)
                             (*error-output* stream)
                             (*trace-output* stream))
                         (run)))
                     (progn (run) ""))))
        (values log (nreverse diags) escaped)))))

(defun %reply (diags log escaped &optional prefix &key interrupted)
  "Assemble the reply text and its return code from recorded DIAGS and
captured LOG.  Diagnostics come first so that the machine-readable part
survives truncation; the raw log follows for a human to read.  An
ESCAPED run says it was aborted -- interrupted, with INTERRUPTED."
  (multiple-value-bind (errors warnings summary) (%summary diags)
    (let ((text (with-output-to-string (s)
                  (when prefix
                    (write-string prefix s)
                    (terpri s))
                  (dolist (d diags)
                    (write-string (%render-diag d) s)
                    (terpri s))
                  (write-string summary s)
                  (when escaped
                    ;; A reader error (an unbalanced paren in a half-saved
                    ;; buffer, say) takes the rest of the file with it -- say
                    ;; so, rather than letting the count imply the file was
                    ;; fully processed.
                    (format s "~%; ~:[aborted~;interrupted~] -- the remaining forms were not processed"
                            interrupted))
                  (when (plusp (length log))
                    (format s "~%--- log ---~%~a" log)))))
      (values (cond ((or escaped (plusp errors)) +rc-error+)
                    ((plusp warnings) +rc-warn+)
                    (t +rc-ok+))
              text))))

(defun %truncate (text)
  (if (<= (length text) *max-result-length*)
      text
      (let* ((cut (subseq text 0 *max-result-length*))
             (nl (position #\Newline cut :from-end t)))
        (concatenate 'string
                     (subseq cut 0 (or nl (length cut)))
                     (format nil "~%[truncated at ~d characters]"
                             *max-result-length*)))))

;;; ================================================================
;;; Command table
;;; ================================================================

(defvar *commands* '()
  "Alist of (VERB-STRING . HANDLER).  HANDLER takes the argument string
and returns (values RC TEXT).")

(defmacro define-command (verb (arg) &body body)
  `(let ((entry (assoc ,verb *commands* :test #'string=)))
     (if entry
         (setf (cdr entry) (lambda (,arg) ,@body))
         (push (cons ,verb (lambda (,arg) ,@body)) *commands*))
     ,verb))

(defvar *raw-commands* '()
  "The verbs whose argument is handed over VERBATIM: everything after the
verb and its one separating blank, leading blanks, trailing newlines and
all.  HANDLE-COMMAND trims every other command at both ends, which is
right for `LOAD foo.lisp' and wrong for a chunk of program output.")

(defmacro define-raw-command (verb (arg) &body body)
  "DEFINE-COMMAND for a verb whose argument is text, not syntax: an
editor's `OUTPUT <chunk>' from the REPL thread (lib/dev-repl.lisp), where
an indented line must keep its indentation and a chunk that ends in a
newline must keep it.  `OUTPUT' alone is the empty chunk."
  `(progn
     (pushnew ,verb *raw-commands* :test #'string=)
     (define-command ,verb (,arg) ,@body)))

(defun %raw-verb (command)
  "The verb at the very start of COMMAND, upcased, and the index after it
-- the character there is the separator, when there is one.  Raw verbs
are matched where the REPL thread puts them: at position 0, as a whole
word (OUTPUTS is not OUTPUT)."
  (let ((end (or (position-if (lambda (c) (member c '(#\Space #\Tab #\Newline #\Return)))
                              command)
                 (length command))))
    (values (string-upcase (subseq command 0 end)) end)))

(defun %split-verb (string)
  "Split STRING into (values VERB REST).  VERB is upcased; REST keeps its
internal spacing but is trimmed at both ends."
  (let* ((s (string-trim '(#\Space #\Tab #\Newline #\Return) string))
         (end (or (position-if (lambda (c) (member c '(#\Space #\Tab))) s)
                  (length s))))
    (values (string-upcase (subseq s 0 end))
            (string-trim '(#\Space #\Tab #\Newline #\Return) (subseq s end)))))

(defun %unquote (string)
  "Strip one layer of surrounding double or single quotes -- editors quote
paths that contain spaces (LOAD \"Ram Disk:foo.lisp\")."
  (let ((s (string-trim '(#\Space #\Tab) string)))
    (if (and (>= (length s) 2)
             (or (and (char= (char s 0) #\") (char= (char s (1- (length s))) #\"))
                 (and (char= (char s 0) #\') (char= (char s (1- (length s))) #\'))))
        (subseq s 1 (1- (length s)))
        s)))

;;; ================================================================
;;; The commands
;;; ================================================================

(define-command "PING" (arg)
  (declare (ignore arg))
  (values +rc-ok+ "PONG"))

(define-command "VERSION" (arg)
  (declare (ignore arg))
  (values +rc-ok+
          (format nil "~a ~a on ~a/~a"
                  (lisp-implementation-type)
                  (lisp-implementation-version)
                  (software-type)
                  (machine-type))))

(define-command "LASTRESULT" (arg)
  (declare (ignore arg))
  ;; Deliberately rc 0: this command exists precisely so a macro that just
  ;; got a non-zero rc (and therefore no RESULT) can still read the text.
  (values +rc-ok+ *last-result*))

(define-command "IN-PACKAGE" (arg)
  (let* ((name (string-upcase (%unquote arg)))
         (pkg (find-package name)))
    (if pkg
        (progn (setf *command-package* pkg)
               (values +rc-ok+ (format nil "Package is now ~a" (package-name pkg))))
        (values +rc-error+ (format nil "ERROR: no such package: ~a" name)))))

(define-command "LOAD" (arg)
  (let ((path (%unquote arg)))
    (if (zerop (length path))
        (values +rc-fatal+ "ERROR: LOAD requires a file name")
        (multiple-value-bind (log diags escaped)
            (call-with-diagnostics
             (lambda () (let ((*package* *command-package*)) (load path)))
             :locate t)
          (%reply diags log escaped (format nil "; loading ~a" path))))))

(define-command "COMPILE-FILE" (arg)
  (let ((path (%unquote arg)))
    (if (zerop (length path))
        (values +rc-fatal+ "ERROR: COMPILE-FILE requires a file name")
        (let ((output nil) (failure nil) (warned nil))
          (multiple-value-bind (log diags escaped)
              (call-with-diagnostics
               (lambda ()
                 (let ((*package* *command-package*))
                   (multiple-value-setq (output warned failure)
                     (compile-file path))))
               :locate t)
            (multiple-value-bind (rc text)
                (%reply diags log escaped
                        (format nil "; compiling ~a~@[~%; wrote ~a~]"
                                path (and output (namestring output))))
              ;; COMPILE-FILE reports failure through its own return values
              ;; as well as through signalled conditions (CLHS 3.2.5); a
              ;; failure-p of T with nothing recorded still has to fail.
              (values (cond (failure (max rc +rc-error+))
                            (warned  (max rc +rc-warn+))
                            (t rc))
                      text)))))))

(define-command "EVAL" (arg)
  (if (zerop (length arg))
      (values +rc-fatal+ "ERROR: EVAL requires a form")
      (let ((values-text nil))
        (multiple-value-bind (log diags escaped)
            (call-with-diagnostics
             (lambda ()
               (let ((*package* *command-package*)
                     (eof (list :eof)))
                 (setf values-text
                       (with-output-to-string (out)
                         (with-input-from-string (in arg)
                           (loop for form = (read in nil eof)
                                 until (eq form eof)
                                 do (let ((results (multiple-value-list (eval form))))
                                      (if results
                                          (format out "~{~a~^ ; ~}~%"
                                                  (mapcar #'prin1-to-string results))
                                          (format out "; no values~%"))))))))))
          (multiple-value-bind (rc text) (%reply diags log escaped)
            (values rc
                    (if (and (= rc +rc-ok+) values-text (plusp (length values-text)))
                        (concatenate 'string
                                     (string-right-trim '(#\Newline) values-text)
                                     (string #\Newline) text)
                        text)))))))

;;; ================================================================
;;; Introspection
;;;
;;; The editor's phase-2 commands: what a symbol takes (ARGLIST), what a
;;; prefix could be (COMPLETE), what a symbol is (DESCRIBE, APROPOS), where
;;; it lives (SOURCE-LOCATION) and what a form means (MACROEXPAND).  All
;;; of them resolve names against *COMMAND-PACKAGE* -- the package the
;;; editor selected with IN-PACKAGE -- and never intern: a name that does
;;; not exist is rc 10 with a reason, not a fresh symbol.
;;; ================================================================

(defun %first-token (string)
  "Split STRING into (values FIRST REST): the first whitespace-delimited
token, case preserved, and the trimmed remainder."
  (let* ((s (string-trim '(#\Space #\Tab #\Newline #\Return) string))
         (end (or (position-if (lambda (c) (member c '(#\Space #\Tab))) s)
                  (length s))))
    (values (subseq s 0 end)
            (string-trim '(#\Space #\Tab #\Newline #\Return) (subseq s end)))))

(defun %split-symbol-name (string)
  "Split STRING as the reader would: (values PACKAGE-NAME NAME INTERNAL-P).
PACKAGE-NAME is NIL for an unqualified name, \"KEYWORD\" for :NAME;
INTERNAL-P is true for the PKG::NAME spelling."
  (let ((colon (position #\: string)))
    (cond ((null colon) (values nil string nil))
          ((zerop colon) (values "KEYWORD" (string-left-trim ":" string) t))
          (t (let ((two (and (< (1+ colon) (length string))
                             (char= (char string (1+ colon)) #\:))))
               (values (subseq string 0 colon)
                       (subseq string (+ colon (if two 2 1)))
                       two))))))

(defun %symbol-name-case (name)
  "NAME as the reader would spell it: upcased, unless written |between bars|."
  (if (and (>= (length name) 2)
           (char= (char name 0) #\|)
           (char= (char name (1- (length name))) #\|))
      (subseq name 1 (1- (length name)))
      (string-upcase name)))

(defun %find-symbol-string (string)
  "The existing symbol STRING names, without interning: (values SYMBOL
STATUS) with STATUS as FIND-SYMBOL reports it, or (values NIL NIL REASON)."
  (multiple-value-bind (pkg-name name) (%split-symbol-name (%unquote string))
    (let ((pkg (if pkg-name
                   (find-package (%symbol-name-case pkg-name))
                   *command-package*)))
      (cond ((null pkg)
             (values nil nil (format nil "no such package: ~a" pkg-name)))
            ((zerop (length name))
             (values nil nil "a symbol name is required"))
            (t
             (multiple-value-bind (sym status)
                 (find-symbol (%symbol-name-case name) pkg)
               (if status
                   (values sym status)
                   (values nil nil (format nil "no such symbol: ~a" string)))))))))

(defmacro %with-symbol ((var arg) &body body)
  "Bind VAR to the symbol ARG names and run BODY, or reply rc 10."
  (let ((status (gensym)) (reason (gensym)))
    `(if (zerop (length ,arg))
         (values +rc-fatal+ "ERROR: a symbol name is required")
         (multiple-value-bind (,var ,status ,reason) (%find-symbol-string ,arg)
           (if ,status
               (progn ,@body)
               (values +rc-error+ (format nil "ERROR: ~a" ,reason)))))))

(defmacro %with-reply-printing (&body body)
  "Printer settings for a reply the editor shows as code: symbols relative
to the command package and in lower case, one line unless the command
breaks lines itself."
  `(let ((*package* *command-package*)
         (*print-case* :downcase)
         (*print-pretty* nil)
         (*print-readably* nil)
         (*print-circle* nil)
         (*print-length* nil)
         (*print-level* nil))
     ,@body))

(defun %symbol-string (sym)
  (%with-reply-printing (prin1-to-string sym)))

;;; --- ARGLIST ------------------------------------------------------

(defparameter *special-operator-arglists*
  '((block name &body body) (catch tag &body body)
    (eval-when situations &body body) (flet bindings &body body)
    (function name) (go tag) (if test then &optional else)
    (labels bindings &body body) (let bindings &body body)
    (let* bindings &body body) (load-time-value form &optional read-only-p)
    (locally &body body) (macrolet bindings &body body)
    (multiple-value-call function &rest forms)
    (multiple-value-prog1 first-form &body forms) (progn &body forms)
    (progv symbols values &body body) (quote object)
    (return-from name &optional value) (setq &rest pairs)
    (symbol-macrolet bindings &body body) (tagbody &rest statements)
    (the type form) (throw tag result)
    (unwind-protect protected &body cleanup))
  "Syntax of the standard special operators, which have no function object
to ask -- so the editor's status line has something to show for IF and LET
too.")

(defun %operator-arglist (sym)
  "(values LAMBDA-LIST KIND) for the operator SYM names: KIND is :MACRO,
:GENERIC-FUNCTION, :FUNCTION or :SPECIAL-OPERATOR, and NIL when SYM names
nothing.  LAMBDA-LIST is :NOT-AVAILABLE when nothing was recorded."
  (cond ((special-operator-p sym)
         ;; The table's parameter names are symbols of this package; the
         ;; editor wants them bare, as an uninterned symbol prints.
         (let ((entry (assoc sym *special-operator-arglists*)))
           (values (if entry
                       (mapcar (lambda (x)
                                 (if (member x lambda-list-keywords)
                                     x
                                     (make-symbol (symbol-name x))))
                               (cdr entry))
                       :not-available)
                   :special-operator)))
        ((macro-function sym)
         (values (ext:function-arglist (macro-function sym)) :macro))
        ((fboundp sym)
         (let ((fn (symbol-function sym)))
           (if (typep fn 'generic-function)
               (values (mop:generic-function-lambda-list fn) :generic-function)
               (values (ext:function-arglist fn) :function))))
        (t (values :not-available nil))))

(defun %arglist-string (lambda-list)
  ;; The C builtins' placeholders are uninterned (#:ARG0); an editor's
  ;; status line does not want the #: in front of each.
  (%with-reply-printing
    (let ((*print-gensym* nil))
      (if (null lambda-list) "()" (prin1-to-string lambda-list)))))

(define-command "ARGLIST" (arg)
  (%with-symbol (sym arg)
    (multiple-value-bind (lambda-list kind) (%operator-arglist sym)
      (cond ((null kind)
             (values +rc-error+
                     (format nil "ERROR: ~a names no function, macro or special operator"
                             (%symbol-string sym))))
            ((eq lambda-list :not-available)
             (values +rc-error+
                     (format nil "ERROR: no lambda list recorded for ~a"
                             (%symbol-string sym))))
            (t (values +rc-ok+ (%arglist-string lambda-list)))))))

;;; --- COMPLETE -----------------------------------------------------

(defvar *max-completions* 200
  "Cap on COMPLETE's reply, in candidates.  Past it the editor is better
served by the common prefix than by the list.")

(defun %prefix-p (prefix name)
  (and (<= (length prefix) (length name))
       (string= prefix name :end2 (length prefix))))

(defun %completions (prefix package-name)
  "(values CANDIDATES NIL) or (values NIL REASON).  CANDIDATES are the
symbol names starting with PREFIX -- spelled PKG:NAME / PKG::NAME / :NAME
as PREFIX was -- exported ones first, each group sorted, capped at
*MAX-COMPLETIONS*.  When no name starts with it, the names that contain
it, the same way: `foo-' finds MAKE-FOO-THING for a user who remembers
the middle of a name.  PACKAGE-NAME, when given, is where to look;
otherwise the package part of PREFIX, else *COMMAND-PACKAGE*."
  (multiple-value-bind (pkg-prefix name internal-p) (%split-symbol-name prefix)
    (let* ((pkg-designator (or package-name pkg-prefix))
           (pkg (if pkg-designator
                    (find-package (%symbol-name-case pkg-designator))
                    *command-package*))
           (uname (%symbol-name-case name))
           ;; Only the exported symbols answer to the PKG: spelling.
           (externals-only (and pkg-prefix
                                (not internal-p)
                                (not (string= pkg-prefix ""))
                                (not (string-equal pkg-prefix "KEYWORD"))))
           (spelling (cond ((null pkg-prefix) "")
                           ((string-equal pkg-prefix "KEYWORD")
                            (if (position #\: prefix :end 1) ":" "keyword:"))
                           (t (format nil "~a~a" (string-downcase pkg-prefix)
                                      (if internal-p "::" ":")))))
           (external '())
           (internal '()))
      (if (null pkg)
          (values nil (format nil "no such package: ~a" pkg-designator))
          (progn
            (flet ((collect (matches-p)
                     (do-symbols (s pkg)
                       (when (funcall matches-p uname (symbol-name s))
                         (multiple-value-bind (found status) (find-symbol (symbol-name s) pkg)
                           (declare (ignore found))
                           (if (eq status :internal)
                               (unless externals-only (pushnew s internal))
                               (pushnew s external)))))))
              (collect #'%prefix-p)
              ;; Nothing starts with it: what contains it.
              (when (and (null external) (null internal) (plusp (length uname)))
                (collect (lambda (part name) (and (search part name) t)))))
            (let ((names
                    (%with-reply-printing
                      (let ((*print-gensym* nil))
                        (mapcar (lambda (s)
                                  ;; An uninterned copy prints the bare name
                                  ;; with the reader's escapes, no package.
                                  (concatenate 'string spelling
                                               (prin1-to-string (make-symbol (symbol-name s)))))
                                (append (sort external #'string< :key #'symbol-name)
                                        (sort internal #'string< :key #'symbol-name)))))))
              (values (if (> (length names) *max-completions*)
                          (subseq names 0 *max-completions*)
                          names)
                      nil)))))))

(define-command "COMPLETE" (arg)
  (multiple-value-bind (prefix package-name) (%first-token arg)
    (if (zerop (length prefix))
        (values +rc-fatal+ "ERROR: COMPLETE requires a prefix")
        (multiple-value-bind (names reason)
            (%completions (%unquote prefix)
                          (and (plusp (length package-name)) (%unquote package-name)))
          (if reason
              (values +rc-error+ (format nil "ERROR: ~a" reason))
              (values +rc-ok+ (format nil "~{~a~^~%~}" names)))))))

;;; --- DESCRIBE / APROPOS ---------------------------------------------

(define-command "DESCRIBE" (arg)
  (%with-symbol (sym arg)
    (handler-case
        (values +rc-ok+
                (string-right-trim
                 '(#\Newline)
                 (with-output-to-string (out)
                   (let ((*package* *command-package*))
                     (describe sym out)))))
      (error (e)
        (values +rc-error+ (format nil "ERROR: ~a" (%condition-text e)))))))

(defun %symbol-kinds (sym)
  "What SYM names, as APROPOS tags it: a list of strings."
  (let ((kinds '()))
    (when (boundp sym) (push "variable" kinds))
    (cond ((special-operator-p sym) (push "special-operator" kinds))
          ((macro-function sym) (push "macro" kinds))
          ((fboundp sym) (push "function" kinds)))
    (when (and (fboundp 'find-class) (find-class sym nil))
      (push "class" kinds))
    (nreverse kinds)))

(defun %apropos-lines (string package)
  "One line per matching symbol, NAME then its kind tags: everything
accessible in PACKAGE (APROPOS-LIST), which defaults to the command
package -- the user's own definitions and what their package uses, not
the internals of every package in the image."
  (%with-reply-printing
    (with-output-to-string (out)
      (dolist (s (apropos-list string (or package *command-package*)))
        (format out "~s~{ ~a~}~%" s (%symbol-kinds s))))))

(define-command "APROPOS" (arg)
  (multiple-value-bind (string package-name) (%first-token arg)
    (if (zerop (length string))
        (values +rc-fatal+ "ERROR: APROPOS requires a string")
        (let ((pkg (and (plusp (length package-name))
                        (find-package (%symbol-name-case (%unquote package-name))))))
          (if (and (plusp (length package-name)) (null pkg))
              (values +rc-error+ (format nil "ERROR: no such package: ~a" package-name))
              (values +rc-ok+
                      (string-right-trim '(#\Newline)
                                         (%apropos-lines (%unquote string) pkg))))))))

;;; --- SOURCE-LOCATION ------------------------------------------------

(defun %symbol-source-location (sym)
  "(FILE LINE) for the code SYM names, or NIL.  A macro's expander, a
function's body; a generic function has no body of its own, so the first
method with a recorded location stands in."
  (let ((fn (cond ((macro-function sym))
                  ((fboundp sym) (symbol-function sym)))))
    (flet ((location (f)
             ;; (FILE LINE); LINE is 1-based, so 0 is "not recorded" -- a
             ;; method body compiled at load time from a FASL has the file
             ;; but no line, and a file alone is not somewhere to jump to.
             (let ((loc (and f (ext:function-source-location f))))
               (and (consp loc) (integerp (second loc)) (plusp (second loc)) loc))))
      (cond ((null fn) nil)
            ((typep fn 'generic-function)
             (dolist (m (mop:generic-function-methods fn) nil)
               (let ((loc (location (mop:method-function m))))
                 (when loc (return loc)))))
            (t (location fn))))))

(define-command "SOURCE-LOCATION" (arg)
  (%with-symbol (sym arg)
    (let ((loc (%symbol-source-location sym)))
      (if loc
          (values +rc-ok+ (format nil "~a:~d" (first loc) (second loc)))
          (values +rc-error+
                  (format nil "ERROR: no source location recorded for ~a"
                          (%symbol-string sym)))))))

;;; --- MACROEXPAND ----------------------------------------------------
;;;
;;; The expansion goes into an editor window, so it is laid out the way a
;;; Lisp programmer would write it -- the standard pretty printer fills to
;;; the margin, which is unreadable for code.  Three rules, each tried in
;;; order: a form that fits on the rest of the line stays flat; a known
;;; body operator keeps its distinguished arguments on its own line and
;;; indents the body by two; anything else aligns its arguments under the
;;; first one.

(defvar *pretty-margin* 72
  "Column MACROEXPAND breaks lines at.")

(defparameter *body-operators*
  '((let . 1) (let* . 1) (flet . 1) (labels . 1) (macrolet . 1)
    (symbol-macrolet . 1) (lambda . 1) (when . 1) (unless . 1)
    (defun . 2) (defmacro . 2) (defmethod . 2) (defgeneric . 2)
    (dolist . 1) (dotimes . 1) (do . 2) (do* . 2) (block . 1)
    (return-from . 1) (multiple-value-bind . 2) (destructuring-bind . 2)
    (if . 1) (case . 1) (ecase . 1) (typecase . 1) (etypecase . 1)
    (handler-case . 1) (handler-bind . 1) (restart-case . 1) (catch . 1)
    (unwind-protect . 1) (prog1 . 1) (progn . 0) (cond . 0) (tagbody . 0)
    (locally . 0) (eval-when . 1) (progv . 2) (the . 1)
    (with-open-file . 1) (with-output-to-string . 1)
    (with-input-from-string . 1) (with-slots . 2) (with-accessors . 2)
    (defclass . 2) (defstruct . 1) (deftype . 2) (defvar . 1)
    (defparameter . 1) (defconstant . 1) (define-compiler-macro . 2)
    (define-condition . 2) (loop . :fill))
  "Operator -> how many leading arguments stay on the operator's line
before the body indents by two; :FILL packs the arguments as they come.")

(defun %proper-list-p (x)
  (and (consp x) (ignore-errors (list-length x)) t))

(defun %pp-newline (out col)
  (terpri out)
  (dotimes (i col) (write-char #\Space out)))

(defun %pp-fits-p (form col)
  (<= (+ col (length (prin1-to-string form))) *pretty-margin*))

(defun %pp (form out col)
  "Print FORM to OUT, whose cursor is at column COL."
  (cond ((or (atom form) (not (%proper-list-p form)) (%pp-fits-p form col))
         (prin1 form out))
        ((and (eq (car form) 'quote) (= (length form) 2))
         (write-char #\' out)
         (%pp (second form) out (1+ col)))
        ((and (eq (car form) 'function) (= (length form) 2))
         (write-string "#'" out)
         (%pp (second form) out (+ col 2)))
        (t
         (let* ((head (car form))
                (style (and (symbolp head) (cdr (assoc head *body-operators*))))
                (head-string (and (symbolp head) (prin1-to-string head))))
           (write-char #\( out)
           (cond
             ((integerp style)
              ;; Distinguished arguments on the head's line while they fit
              ;; flat; a bulky one gets its own line, aligned; the body
              ;; indents by two.
              (write-string head-string out)
              (let ((c (+ col 1 (length head-string)))
                    (args (cdr form))
                    (n style))
                (loop while (and args (plusp n))
                      do (let ((a (pop args)))
                           (decf n)
                           (cond ((%pp-fits-p a (1+ c))
                                  (write-char #\Space out)
                                  (prin1 a out)
                                  (incf c (1+ (length (prin1-to-string a)))))
                                 (t
                                  (%pp-newline out (+ col 4))
                                  (%pp a out (+ col 4))
                                  (setq c *pretty-margin*)))))
                (dolist (f args)
                  (%pp-newline out (+ col 2))
                  (%pp f out (+ col 2)))))
             ((eq style :fill)
              ;; LOOP: as many clauses per line as fit, aligned after the
              ;; operator.
              (write-string head-string out)
              (let* ((start (+ col 1 (length head-string) 1))
                     (c start)
                     (first t))
                (dolist (a (cdr form))
                  (let ((flat (prin1-to-string a)))
                    (cond ((and (not first) (> (+ c (length flat)) *pretty-margin*))
                           (%pp-newline out start)
                           (setq c start))
                          ((not first)
                           (write-char #\Space out)
                           (incf c))
                          (t (write-char #\Space out)))
                    (setq first nil)
                    (cond ((%pp-fits-p a c)
                           (write-string flat out)
                           (incf c (length flat)))
                          (t
                           (%pp a out c)
                           (setq c *pretty-margin*)))))))
             (head-string
              ;; Arguments under the first one; a long operator name would
              ;; push them past the middle, so indent by two instead.
              (write-string head-string out)
              (let ((c (+ col 1 (length head-string) 1)))
                (if (> c (floor *pretty-margin* 2))
                    (dolist (f (cdr form))
                      (%pp-newline out (+ col 2))
                      (%pp f out (+ col 2)))
                    (let ((first t))
                      (dolist (f (cdr form))
                        (if first (write-char #\Space out) (%pp-newline out c))
                        (setq first nil)
                        (%pp f out c))))))
             (t
              ;; ((lambda ...) ...) and data: one element per line.
              (let ((first t))
                (dolist (f form)
                  (unless first (%pp-newline out (1+ col)))
                  (setq first nil)
                  (%pp f out (1+ col))))))
           (write-char #\) out)))))

(defun %pretty-string (form)
  (%with-reply-printing
    (with-output-to-string (out) (%pp form out 0))))

(defun %macroexpand-command (verb arg expander)
  (if (zerop (length arg))
      (values +rc-fatal+ (format nil "ERROR: ~a requires a form" verb))
      (handler-case
          (let ((form (let ((*package* *command-package*))
                        (read-from-string arg))))
            (values +rc-ok+ (%pretty-string (funcall expander form))))
        (error (e)
          (values +rc-error+ (format nil "ERROR: ~a" (%condition-text e)))))))

(define-command "MACROEXPAND" (arg)
  (%macroexpand-command "MACROEXPAND" arg #'macroexpand))

(define-command "MACROEXPAND-1" (arg)
  (%macroexpand-command "MACROEXPAND-1" arg #'macroexpand-1))

;;; ================================================================
;;; Rows for a front end's lists
;;;
;;; The inspector's parts, a frame's locals and a restart's report are
;;; shown one per row by the editor, so each value is printed bounded
;;; and on one line: a 10000-element list is a row, not a screen.
;;; ================================================================

(defun %one-line (text limit)
  "TEXT with its newlines blanked, cut to LIMIT characters with a marker."
  (let ((flat (substitute #\Space #\Newline (substitute #\Space #\Return text))))
    (if (> (length flat) limit)
        (concatenate 'string (subseq flat 0 limit) " ...")
        flat)))

(defun %print-bounded (object)
  "OBJECT as one row: PRIN1 with bounded length and depth, on one line,
and never a signal -- a PRINT-OBJECT method that errors gives a marker."
  (let ((*print-length* 10)
        (*print-level* 3)
        (*print-pretty* nil)
        (*print-readably* nil)
        (*print-circle* nil)
        (*package* *command-package*))
    (%one-line (handler-case (prin1-to-string object)
                 (error () (format nil "#<unprintable ~a>" (type-of object))))
               200)))

(defun %parse-index (string)
  "STRING as a non-negative integer, or NIL."
  (let ((n (ignore-errors (parse-integer string :junk-allowed t))))
    (and n (>= n 0) n)))

(defun %split-index (string)
  "(values INDEX REST) for `<n> <rest>': the leading number and what
follows it, trimmed; INDEX is NIL when there is none."
  (let* ((s (string-trim '(#\Space #\Tab) string))
         (end (or (position-if-not #'digit-char-p s) (length s))))
    (if (zerop end)
        (values nil s)
        (values (parse-integer s :end end)
                (string-trim '(#\Space #\Tab) (subseq s end))))))

;;; ================================================================
;;; The inspector
;;;
;;; A non-interactive face over the C inspector's part enumeration
;;; (EXT:INSPECT-PARTS): the editor shows the numbered parts itself,
;;; descends into one with PART and comes back with POP.  The navigation
;;; stack is per connection, like *COMMAND-PACKAGE*, and a fresh INSPECT
;;; starts it over.  The object comes from a form evaluated here, on the
;;; port's handler thread; the REPL thread's *, ** and *** are bound
;;; around that evaluation, so `INSPECT *' looks at the last REPL value.
;;; ================================================================

(defvar *max-inspect-parts* 200
  "How many parts an INSPECT, PART or POP reply lists.  The header line
carries the total, so a front end can say how many more there are.")

(defvar *inspect-stack* '()
  "The objects being inspected, innermost first.")

(defvar *repl-stars* nil
  "The REPL thread's (* ** ***) after its last form, so INSPECT on the
handler thread can see them; lib/dev-repl.lisp keeps it current.")

(defun %inspect-reply ()
  "The reply for the top of *INSPECT-STACK*: a header `<TYPE> <depth>
<count>', the object on one line, then `<n>: <label> = <value>' per part."
  (let ((object (first *inspect-stack*)))
    (multiple-value-bind (parts count)
        (ext:inspect-parts object *max-inspect-parts*)
      (values +rc-ok+
              (with-output-to-string (s)
                (format s "~a ~d ~d~%"
                        (%one-line (let ((*package* *command-package*))
                                     (prin1-to-string (type-of object)))
                                   60)
                        (length *inspect-stack*) count)
                (write-line (%print-bounded object) s)
                (let ((i 0))
                  (dolist (part parts)
                    (format s "~d: ~a = ~a~%" i (car part) (%print-bounded (cdr part)))
                    (incf i))))))))

(define-command "INSPECT" (arg)
  (if (zerop (length arg))
      (values +rc-fatal+ "ERROR: INSPECT requires a form")
      (let ((problem nil) (object nil))
        (handler-case
            (let ((*package* *command-package*))
              (setf object
                    (progv (if *repl-stars* '(* ** ***) '()) *repl-stars*
                      (eval (read-from-string arg)))))
          (error (e) (setf problem (%condition-text e))))
        (if problem
            (values +rc-error+ (format nil "ERROR: ~a" problem))
            (progn (setf *inspect-stack* (list object))
                   (%inspect-reply))))))

(define-command "PART" (arg)
  (let ((n (%parse-index arg)))
    (cond ((null *inspect-stack*)
           (values +rc-error+ "ERROR: nothing is being inspected (INSPECT <form> first)"))
          ((null n)
           (values +rc-fatal+ "ERROR: PART requires a part number"))
          (t
           ;; Only the parts up to N are listed: a big vector costs N
           ;; conses to descend into, not its length.
           (multiple-value-bind (parts count)
               (ext:inspect-parts (first *inspect-stack*) (1+ n))
             (if (< n count)
                 (progn (push (cdr (nth n parts)) *inspect-stack*)
                        (%inspect-reply))
                 (values +rc-error+
                         (format nil "ERROR: no part ~d (the object has ~d)" n count))))))))

(define-command "POP" (arg)
  (declare (ignore arg))
  (cond ((null *inspect-stack*)
         (values +rc-error+ "ERROR: nothing is being inspected (INSPECT <form> first)"))
        ((null (rest *inspect-stack*))
         (values +rc-error+ "ERROR: already at the object INSPECT started from"))
        (t (pop *inspect-stack*)
           (%inspect-reply))))

;;; ================================================================
;;; Systems
;;;
;;; What an editor's `Load System' and `Test System' evaluate, on the
;;; REPL thread: a system is minutes of compiling, which wants its output
;;; as it happens, an interrupt and the debugger -- none of which the
;;; port's handler thread has.  So these are functions, not verbs.
;;;
;;; The system is named by its definition FILE, since that is what an
;;; editor has in a window; the system loaded is the one the file is named
;;; after (foo.asd defines "foo", and "foo/tests" beside it), unless the
;;; caller names one of the file's other systems -- an editor that read
;;; the file's defsystems and asked.  ASDF is loaded on first use -- a
;;; session that never loads a system never pays for it -- so nothing here
;;; may name an ASDF symbol at read time.
;;; ================================================================

(defun %asdf (name &rest args)
  (let ((function (find-symbol name "ASDF")))
    (unless (and function (fboundp function))
      (error "ASDF has no function ~a: is lib/asdf.lisp the one loaded?" name))
    (apply function args)))

(defun %system-file (path)
  "PATH as the truename of a system definition file, or an error that
says what is wrong with it."
  (let ((type (pathname-type (pathname path))))
    (unless (and (stringp type) (string-equal type "asd"))
      (error "Not a system definition file (.asd): ~a" path))
    (or (probe-file path)
        (error "No such system definition file: ~a" path))))

(defun %load-asd (path name)
  "Load the definitions in PATH; the name of the system to act on -- NAME,
or the file's own -- and the file's truename."
  (let ((file (%system-file path)))
    (unless (or (null name) (and (stringp name) (string/= name "")))
      (error "Not a system name: ~s" name))
    (require "asdf")
    (%asdf "LOAD-ASD" file)
    (values (or name (string-downcase (pathname-name file))) file)))

(defun load-asd-system (path &optional name)
  "Load the system definition file PATH and then the system it is named
after -- or NAME, one of the systems the file defines -- with everything
that system depends on.  Returns the system's name."
  (multiple-value-bind (name file) (%load-asd path name)
    (format t "~&; loading system ~a from ~a~%" name (namestring file))
    (%asdf "LOAD-SYSTEM" name)
    name))

(defun test-asd-system (path &optional name)
  "Load the system definition file PATH and run ASDF:TEST-SYSTEM on the
system it is named after, or on NAME, one of the systems the file
defines.  Returns the system's name."
  (multiple-value-bind (name file) (%load-asd path name)
    (format t "~&; testing system ~a from ~a~%" name (namestring file))
    (%asdf "TEST-SYSTEM" name)
    name))

;;; ================================================================
;;; REPL
;;;
;;; The REPL runs on a thread of its own and talks back to the editor's
;;; port; that lives in lib/dev-repl.lisp, which needs gray-streams and
;;; so CLOS.  It is loaded the first time an editor asks for it, so a
;;; session that only loads files never pays for it.
;;; ================================================================

(defvar *repl-send* nil
  "Function of (PORT COMMAND) that delivers COMMAND to the editor's port
PORT, returning (values RC TEXT); an editor that cannot be reached is an
error, which stops the REPL thread.  AMIGA.AREXX installs its SEND (an
ARexx port name), EXT.DEV.TCP its own (a name of the form
tcp:HOST:PORT/TOKEN); each passes a name that is the other's on to the
function it found here, so they load in either order.  The host tests
install a Lisp function that stands in for the editor.")

(define-command "REPL-ATTACH" (arg)
  ;; Loading dev-repl replaces this entry with the real command.
  (let ((problem (handler-case (progn (require "dev-repl") nil)
                   (error (e) (%condition-text e)))))
    (if problem
        (values +rc-fatal+
                (format nil "ERROR: cannot load the REPL support: ~a" problem))
        (funcall (cdr (assoc "REPL-ATTACH" *commands* :test #'string=)) arg))))

;;; ================================================================
;;; Dispatch
;;; ================================================================

(defun handle-command (command)
  "Execute COMMAND, a command string from an editor.  Returns
(values RC TEXT); TEXT is also stashed in *LAST-RESULT*.

A command starting with #\\( is a Lisp form and goes straight to EVAL, so
both `EVAL (room)' and `(room)' work from a macro."
  (let ((rc +rc-fatal+)
        (text ""))
    ;; %CALL-GUARDED, not HANDLER-CASE: handle-command is called from the
    ;; port's handler loop and must always produce a reply, but a handler
    ;; for ERROR established here would sit in the dynamic extent of every
    ;; LOAD below it and pre-empt LOAD's per-form recovery (see
    ;; %call-guarded).  The guard catches escapes without ever entering the
    ;; handler search.
    (let ((escaped
            (nth-value
             1
             (%call-guarded
              (lambda ()
                (let ((trimmed (string-trim '(#\Space #\Tab #\Newline #\Return) command))
                      (raw (and *raw-commands*
                                (multiple-value-bind (verb end) (%raw-verb command)
                                  (and (member verb *raw-commands* :test #'string=)
                                       (cons verb end))))))
                  (cond
                    (raw
                     ;; The argument as it came: after the verb and ONE
                     ;; blank, when there is one.
                     (let* ((end (cdr raw))
                            (rest (if (and (< end (length command))
                                           (char= (char command end) #\Space))
                                      (subseq command (1+ end))
                                      (subseq command end))))
                       (multiple-value-setq (rc text)
                         (funcall (cdr (assoc (car raw) *commands* :test #'string=)) rest))))
                    ((zerop (length trimmed))
                     (setf rc +rc-ok+ text ""))
                    ((char= (char trimmed 0) #\()
                     (multiple-value-setq (rc text)
                       (funcall (cdr (assoc "EVAL" *commands* :test #'string=)) trimmed)))
                    (t
                     (multiple-value-bind (verb rest) (%split-verb trimmed)
                       (let ((entry (assoc verb *commands* :test #'string=)))
                         (if entry
                             (multiple-value-setq (rc text) (funcall (cdr entry) rest))
                             (setf rc +rc-fatal+
                                   text (format nil "ERROR: unknown command: ~a~%Known commands: ~{~a~^ ~}"
                                                verb (sort (mapcar #'car *commands*)
                                                           #'string<))))))))))))))
      (when escaped
        (setf rc +rc-fatal+
              text "ERROR: the command handler was aborted by an unhandled condition")))
    (setf text (%truncate (or text "")))
    (setf *last-result* text)
    (values rc text)))

(provide "dev-commands")
