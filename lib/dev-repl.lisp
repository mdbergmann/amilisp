;;; dev-repl.lisp -- the editor's REPL: a dedicated thread behind the port
;;;
;;; Loaded on demand by the REPL-ATTACH command in lib/dev-commands.lisp
;;; (it needs gray-streams, hence CLOS, which the rest of the command layer
;;; does not); (require "dev-repl") works too.
;;;
;;; The EVAL command answers with everything the form printed, after the
;;; fact.  A REPL wants the output as it happens, wants a READ-LINE to ask
;;; the editor, and must not tie up the port's handler thread while a form
;;; runs, or the editor's arglist and completion requests would queue
;;; behind it.  So the REPL is a second thread, and the conversation is
;;; two-way:
;;;
;;;   editor -> amilisp (commands, replied at once)
;;;     REPL-ATTACH <port>   start the REPL thread; PORT is the editor's
;;;                          ARexx port, where the traffic below goes
;;;     REPL-EVAL <forms>    queue the forms; rc 10 while one is running
;;;     REPL-INPUT <line>    answer an outstanding READLINE
;;;     REPL-INTERRUPT       abort the running form
;;;     REPL-DETACH          stop the thread
;;;
;;;   amilisp -> editor (sent from the REPL thread, through *REPL-SEND*)
;;;     OUTPUT <text>        a chunk of output: flushed on newline, on a
;;;                          size threshold and before RESULT
;;;     READLINE             the form is reading a line from standard
;;;                          input; the editor answers with REPL-INPUT
;;;     RESULT <rc> <pkg>    the form is done: rc 0 and the printed values
;;;       <values>           (one per line), or rc 10 and the error text;
;;;                          PKG is the current package for the prompt
;;;
;;; A REPL thread that dies -- of the one error no handler sees, the heap
;;; exhausted -- still ends its form with RESULT: rc 10, the error, and the
;;; news that a new thread took its place (%REPL-THREAD-DIED, called through
;;; MP:*THREAD-DEATH-HOOKS*).  The editor has its prompt back and its next
;;; REPL-EVAL finds a REPL.
;;;
;;; Why the editor never replies to READLINE directly: MUI answers an
;;; application's ARexx command the moment the command hook returns, so the
;;; editor cannot hold the reply until the user has typed the line.  The
;;; line comes back as a command of its own instead, and the REPL thread
;;; parks on a condition variable in between.  Same reason REPL-EVAL does
;;; not carry the values: they arrive with RESULT.
;;;
;;; The debugger (REPL-ATTACH <port> DEBUG) is the same shape once more.
;;; An unhandled error in a form does not end it: the REPL thread, still
;;; on the erring stack with its restarts and locals, sends
;;;
;;;     DEBUGGER <level> <pkg>   level 1, 2, ... for a (nested) debugger,
;;;       <type>: <report>       0 when the form is running or done again;
;;;       <n>: <NAME> <report>   the condition, then one restart per line
;;;
;;; and parks on the condition variable again, taking its next steps from
;;; the port:
;;;
;;;     BACKTRACE                the frames of that level, `<n>: <name>
;;;                              <file>:<line>' (answered from a snapshot)
;;;     FRAME <n>                the locals of frame N, `<name> = <value>'
;;;                              (the REPL thread computes them; the port
;;;                              waits a moment for the answer)
;;;     FRAME-EVAL <n> <forms>   evaluate with frame N's locals bound by
;;;                              their names (ARG0, LOCAL2, ...); replied
;;;                              at once, the values arrive as OUTPUT, an
;;;                              error is a nested level
;;;     RESTART <n>              invoke restart N (interactively: one that
;;;                              asks reads through READLINE)
;;;     ABORT, CONTINUE          the innermost ABORT / CONTINUE restart
;;;
;;; Every REPL form has an ABORT restart ("Return to the REPL"), so a
;;; level can always be left.  Leaving one re-announces the level below
;;; (or 0), and RESULT follows once the form is done.  REPL-INTERRUPT in
;;; the debugger ends the form as it would a running one.
;;;
;;; See tests/test_dev_commands.sh (host, with a Lisp function standing in
;;; for the editor) and tests/amiga/arexx-tests.lisp (the real port).

(eval-when (:compile-toplevel :load-toplevel :execute)
  (require "dev-commands")
  (require "gray-streams"))

(in-package "EXT.DEV")

(defvar *repl-thread-stack-size* (* 256 1024)
  "C stack of the REPL thread: forms compile code, see AMIGA.AREXX:START.")

(defvar *repl-thread-vm-frames* 512)

(defvar *repl-output-chunk* 1024
  "Output is sent to the editor at every newline, or once this many
characters are buffered without one.")

(defvar *repl-stop-timeout-seconds* 5)

;;; Shared between the handler thread (the commands) and the REPL thread,
;;; all under *REPL-LOCK*.
(defvar *repl-lock* (mp:make-lock "repl"))
(defvar *repl-cv* (mp:make-condition-variable "repl"))
(defvar *repl-thread* nil)
(defvar *repl-port* nil "The editor's port name while attached.")
(defvar *repl-job* nil "Form text queued by REPL-EVAL, until the thread takes it.")
(defvar *repl-busy* nil "True from taking a job until RESULT has been sent.")
(defvar *repl-reading* nil "True while a READLINE is outstanding.")
(defvar *repl-input* nil "(LINE) once REPL-INPUT has answered, else NIL.")
(defvar *repl-stop* nil)

;;; Only the REPL thread touches this one: true while a form is being
;;; evaluated inside the handler that turns REPL-INTERRUPT into a RESULT.
;;; The interrupt closure clears it before it signals, so at most one
;;; interrupt reaches each form and none reaches the thread between forms.
(defvar *repl-interruptible* nil)

;;; The debugger.  *REPL-DEBUG* is what REPL-ATTACH asked for; the rest is
;;; shared between the two threads under *REPL-LOCK* like the job above.
(defvar *repl-debug* nil
  "True when the editor asked for DEBUG: an unhandled error parks the REPL
thread in the debugger protocol instead of ending the form.")
(defvar *repl-debug-stack* '()
  "The active debugger levels, innermost first (%DEBUG-LEVEL structs).")
(defvar *repl-debug-job* nil
  "What the handler thread asked the parked REPL thread to do next:
(:FRAME n), (:EVAL n text), (:RESTART n) or (:INTERRUPT).")
(defvar *repl-debug-answer* nil
  "(TEXT) once the REPL thread has answered a :FRAME job.")
(defvar *repl-debug-answer-timeout-seconds* 10)

(defstruct (%debug-level (:constructor %make-debug-level
                             (condition restarts frames depth)))
  condition
  restarts   ; (compute-restarts condition), innermost first: RESTART <n> indexes it
  frames     ; (INDEX NAME FILE LINE LIVE-INDEX) per user frame, innermost first
  depth)     ; how deep the live stack was when FRAMES was taken

(define-condition repl-interrupt (serious-condition) ()
  (:report (lambda (c s) (declare (ignore c)) (write-string "Interrupted" s))))

(defvar *repl-file* (third (first (ext:backtrace 1)))
  "This file as EXT:BACKTRACE spells it, to leave the REPL's own frames
out of a backtrace (see %DEBUG-FRAMES and *THIS-FILE* in dev-commands).")

(defun %prompt-package-name (package)
  "The shortest name PACKAGE answers to -- CL-USER, not COMMON-LISP-USER."
  (let ((best (package-name package)))
    (dolist (n (package-nicknames package) best)
      (when (< (length n) (length best)) (setf best n)))))

(defun %repl-send (command)
  "Send COMMAND to the editor's port.  A transport failure (the editor is
gone) stops the REPL rather than killing its thread: the form runs to its
end with its output dropped, and the thread exits at the next job."
  (unless *repl-stop*
    (handler-case (funcall *repl-send* *repl-port* command)
      (error (e)
        (declare (ignore e))
        (mp:with-lock-held (*repl-lock*)
          (setf *repl-stop* t)
          (mp:condition-broadcast *repl-cv*))))))

;;; ----------------------------------------------------------------
;;; The streams
;;; ----------------------------------------------------------------

(defclass repl-output-stream (gray:fundamental-character-output-stream)
  ((buffer :initform (make-string-output-stream))
   (count :initform 0)
   (column :initform 0)))

(defun %repl-flush (stream)
  (with-slots (buffer count) stream
    (when (plusp count)
      (let ((text (get-output-stream-string buffer)))
        (setf count 0)
        (%repl-send (concatenate 'string "OUTPUT " text))))))

(defmethod gray:stream-write-char ((stream repl-output-stream) char)
  (with-slots (buffer count column) stream
    (write-char char buffer)
    (incf count)
    (if (char= char #\Newline)
        (setf column 0)
        (incf column))
    (when (or (char= char #\Newline) (>= count *repl-output-chunk*))
      (%repl-flush stream)))
  char)

(defmethod gray:stream-write-string ((stream repl-output-stream) string
                                     &optional (start 0) end)
  (let ((end (or end (length string))))
    (if (> (- end start) *repl-output-chunk*)
        ;; One huge PRINC still reaches the editor in pieces it can insert
        ;; as they come.
        (loop for from = start then to
              for to = (min end (+ from *repl-output-chunk*))
              while (< from end)
              do (gray:stream-write-string stream string from to))
        (let ((nl (position #\Newline string :start start :end end :from-end t)))
          (when (< start end)
            (with-slots (buffer count column) stream
              (write-string string buffer :start start :end end)
              (incf count (- end start))
              (setf column (if nl (- end nl 1) (+ column (- end start))))
              (when (or nl (>= count *repl-output-chunk*))
                (%repl-flush stream)))))))
  string)

(defmethod gray:stream-line-column ((stream repl-output-stream))
  (slot-value stream 'column))

(defmethod gray:stream-force-output ((stream repl-output-stream))
  (%repl-flush stream)
  nil)

(defmethod gray:stream-finish-output ((stream repl-output-stream))
  (%repl-flush stream)
  nil)

(defclass repl-input-stream (gray:fundamental-character-input-stream)
  ((line :initform nil)      ; the line being served, without its newline
   (pos :initform 0)
   (pushback :initform nil)  ; an unread character
   (output :initarg :output)))

(defun %repl-request-line (stream)
  "Ask the editor for a line and wait for REPL-INPUT.  Returns the line,
or NIL when the REPL is being stopped."
  (%repl-flush (slot-value stream 'output))   ; the prompt goes out first
  (mp:with-lock-held (*repl-lock*)
    (setf *repl-input* nil
          *repl-reading* t))
  (%repl-send "READLINE")
  (mp:with-lock-held (*repl-lock*)
    (loop until (or *repl-input* *repl-stop*)
          do (mp:condition-wait *repl-cv* *repl-lock*))
    (setf *repl-reading* nil)
    (prog1 (car *repl-input*)
      (setf *repl-input* nil))))

(defmethod gray:stream-read-char ((stream repl-input-stream))
  (with-slots (line pos pushback) stream
    (cond (pushback
           (prog1 pushback (setf pushback nil)))
          ((and line (< pos (length line)))
           (prog1 (char line pos) (incf pos)))
          (line
           ;; The line's terminator; the next read asks for a new line.
           (setf line nil pos 0)
           #\Newline)
          (t
           (let ((got (%repl-request-line stream)))
             (if (null got)
                 :eof
                 (progn (setf line got pos 0)
                        (gray:stream-read-char stream))))))))

(defmethod gray:stream-unread-char ((stream repl-input-stream) char)
  (setf (slot-value stream 'pushback) char)
  nil)

(defmethod gray:stream-listen ((stream repl-input-stream))
  (with-slots (line pushback) stream
    (and (or pushback line) t)))

(defmethod gray:stream-clear-input ((stream repl-input-stream))
  (with-slots (line pos pushback) stream
    (setf line nil pos 0 pushback nil))
  nil)

;;; ----------------------------------------------------------------
;;; The thread
;;; ----------------------------------------------------------------

(defun %repl-values-string (values)
  (if values
      (format nil "~{~a~^~%~}" (mapcar #'prin1-to-string values))
      "; No values"))

(defun %repl-eval (form)
  "EVAL with the listener's history variables kept, as at the console."
  (setf - form)
  (let ((values (multiple-value-list (eval form))))
    (setf +++ ++ ++ + + form
          /// // // / / values
          *** ** ** * * (first values))
    ;; For INSPECT on the handler thread, whose * is not this thread's.
    (setf *repl-stars* (list * ** ***))
    values))

(defun %repl-run (text out in)
  "Evaluate the forms in TEXT, streaming their output, then send RESULT.
The ABORT restart is what the debugger's ABORT (and a REPL-DETACH while
parked there) come back through: the form ends with `; Aborted'."
  (let ((rc +rc-ok+) (result nil) (eof (list :eof)))
    (handler-case
        (restart-case
            (progn
              (setf *repl-interruptible* t)
              (handler-bind ((serious-condition #'%repl-debugger-hook))
                (with-input-from-string (src text)
                  (loop for form = (read src nil eof)
                        until (eq form eof)
                        do (setf result (%repl-values-string (%repl-eval form))))))
              (setf *repl-interruptible* nil))
          (abort ()
            :report "Return to the REPL"
            (setf *repl-interruptible* nil
                  result "; Aborted")))
      (serious-condition (c)
        (setf *repl-interruptible* nil
              rc +rc-error+
              result (format nil "ERROR: ~a" (%condition-text c)))))
    (gray:stream-clear-input in)
    (%repl-flush out)
    (setf *command-package* *package*)
    ;; Idle BEFORE the editor hears RESULT: RESULT puts the prompt back, and
    ;; an editor that sends the next form at once -- a macro, or one that
    ;; delivers in-process from inside this very send -- must not be told
    ;; the REPL is busy.  A REPL-EVAL arriving now waits in *REPL-JOB*.
    (%repl-finish-job)
    (%repl-send (format nil "RESULT ~d ~a~%~a" rc (%prompt-package-name *package*)
                        (%truncate (or result "; No values"))))))

(defun %repl-take-job ()
  "Wait for REPL-EVAL's text, or NIL once the REPL is being stopped."
  (mp:with-lock-held (*repl-lock*)
    (loop until (or *repl-job* *repl-stop*)
          do (mp:condition-wait *repl-cv* *repl-lock*))
    (if *repl-stop*
        nil
        (prog1 *repl-job*
          (setf *repl-job* nil
                *repl-busy* t)))))

(defun %repl-finish-job ()
  ;; An interrupt may have cut a READLINE short: forget it, or the next
  ;; REPL-INPUT would be taken for an answer.
  (mp:with-lock-held (*repl-lock*)
    (setf *repl-busy* nil
          *repl-reading* nil
          *repl-input* nil
          *repl-debug-stack* '()
          *repl-debug-job* nil
          *repl-debug-answer* nil)))

(defun %repl-loop ()
  (let* ((out (make-instance 'repl-output-stream))
         (in (make-instance 'repl-input-stream :output out))
         (*standard-output* out)
         (*error-output* out)
         (*trace-output* out)
         (*standard-input* in)
         (*package* *command-package*)
         ;; Private history for this REPL thread: without a per-thread
         ;; binding, %REPL-EVAL's SETFs would mutate the same global cells
         ;; the physical console's REPL uses (src/core/repl.c's
         ;; cl_repl_update_history), corrupting whichever session runs
         ;; concurrently with this one.
         (- nil) (+ nil) (++ nil) (+++ nil)
         (* nil) (** nil) (*** nil)
         (/ nil) (// nil) (/// nil))
    (unwind-protect
         (loop
           (let ((text (%repl-take-job)))
             (when (null text) (return))
             ;; IN-PACKAGE from the editor since the last form wins; a form's
             ;; own IN-PACKAGE is handed back through *COMMAND-PACKAGE*.
             (setf *package* *command-package*)
             (unwind-protect (%repl-run text out in)
               (%repl-finish-job))))
      (mp:with-lock-held (*repl-lock*)
        (setf *repl-busy* nil
              *repl-reading* nil
              *repl-port* nil)))))

(defun %repl-alive-p ()
  (and *repl-thread* (mp:thread-alive-p *repl-thread*) t))

(defun %repl-interrupt ()
  "Abort the running form, if any.  Safe when none runs: the closure
checks on the REPL thread itself."
  (when (%repl-alive-p)
    ;; IGNORE-ERRORS: the thread may exit between the check and the call.
    (ignore-errors
     (mp:interrupt-thread *repl-thread*
                          (lambda ()
                            (when *repl-interruptible*
                              (setf *repl-interruptible* nil)
                              (error 'repl-interrupt)))))))

(defun %repl-stop ()
  "Stop the REPL thread and wait for it.  Returns true when it is gone."
  (when (%repl-alive-p)
    (mp:with-lock-held (*repl-lock*)
      (setf *repl-stop* t)
      (mp:condition-broadcast *repl-cv*))
    (%repl-interrupt)
    (let ((waited 0))
      (loop while (and (mp:thread-alive-p *repl-thread*)
                       (< waited *repl-stop-timeout-seconds*))
            do (sleep 0.02)
               (incf waited 0.02))))
  (if (%repl-alive-p)
      nil
      (progn (setf *repl-thread* nil *repl-port* nil)
             (when *repl-debug*
               (%repl-jit-frames *repl-jit-frames-before*)
               (setf *repl-debug* nil))
             t)))

;; The REPL thread must not outlive the process either (see the exit hook
;; in AMIGA.AREXX:START for what a leftover thread costs).  This file loads
;; after the port is up, so hooks running newest-first stop the REPL thread
;; before the port that fed it.
(ext:add-exit-hook '%repl-stop)

;;; On the m68k build a function the JIT compiled natively pushes no VM
;;; frame of its own, so EXT:BACKTRACE and EXT:FRAME-LOCALS do not see it
;;; unless the JIT's per-call shadow frames are on (CLAMIGA::%JIT-SET-FRAMES;
;;; a few percent on call-heavy code, which a debugging session accepts;
;;; such a frame shows the arguments, not the LET-bound locals).  Turned on
;;; for a DEBUG attach and put back on detach; a no-op on the host build.
(defvar *repl-jit-frames-before* nil)

(defun %repl-jit-frames (on)
  (let ((setter (find-symbol "%JIT-SET-FRAMES" "CLAMIGA")))
    (when (and setter (fboundp setter))
      (funcall setter on))))

(defun %repl-jit-frames-p ()
  (let ((getter (find-symbol "%JIT-FRAMES-P" "CLAMIGA")))
    (and getter (fboundp getter) (funcall getter))))

(defun %repl-spawn (port debug)
  "A fresh REPL thread for the editor's PORT, the shared state reset."
  (mp:with-lock-held (*repl-lock*)
    (setf *repl-port* port
          *repl-debug* debug
          *repl-job* nil
          *repl-busy* nil
          *repl-reading* nil
          *repl-input* nil
          *repl-stop* nil
          *repl-debug-stack* '()
          *repl-debug-job* nil
          *repl-debug-answer* nil))
  (setf *repl-interruptible* nil)
  (setf *repl-thread*
        (mp:make-thread #'%repl-loop
                        :name "repl"
                        :stack-size *repl-thread-stack-size*
                        :vm-frames *repl-thread-vm-frames*)))

(defun %repl-start (port debug)
  (when debug
    (setf *repl-jit-frames-before* (%repl-jit-frames-p))
    (%repl-jit-frames t))
  (%repl-spawn port debug))

;;; ----------------------------------------------------------------
;;; A REPL thread that died
;;;
;;; Every error a form can signal ends in %REPL-RUN's HANDLER-CASE -- but
;;; an exhausted heap is not signalled: the runtime drops the thread's
;;; handlers and stacks and leaves through the thread's entry, so neither
;;; that handler nor %REPL-LOOP's cleanup runs, and RESULT is never sent.
;;; The editor would wait for it for good.  The runtime calls the hook
;;; below on the dying thread instead, where the form's garbage is
;;; collectable again: it starts a new REPL thread for the same editor
;;; and sends the RESULT the form still owes.
;;;
;;; What this cannot cover: a thread that died holding the transport's
;;; lock (it died inside a send), or a heap with no room left even for the
;;; message.  The hook fails quietly then, the thread is simply gone, and
;;; the editor finds out from "no REPL attached" as before.
;;; ----------------------------------------------------------------

(defun %repl-thread-died (thread message)
  (when (and (eq thread *repl-thread*) *repl-port*)
    (cond (*repl-stop*
           ;; On its way out anyway: what %REPL-LOOP's cleanup would have done.
           (mp:with-lock-held (*repl-lock*)
             (setf *repl-busy* nil
                   *repl-reading* nil
                   *repl-port* nil)))
          (t
           (%repl-spawn *repl-port* *repl-debug*)
           (%repl-send
            (format nil "RESULT ~d ~a~%ERROR: ~a~%; The REPL thread ended with that error; a new one has taken its place"
                    +rc-error+ (%prompt-package-name *command-package*)
                    (%truncate message)))))))

(pushnew '%repl-thread-died mp:*thread-death-hooks*)

;;; ----------------------------------------------------------------
;;; The debugger (on the REPL thread)
;;;
;;; %REPL-DEBUGGER-HOOK is the HANDLER-BIND handler around every form.
;;; It runs on top of the erring stack -- amilisp runs handlers before
;;; unwinding, for runtime errors too -- so the frames, their locals and
;;; the restarts are all still there while the thread parks below.  The
;;; frames are snapshotted for BACKTRACE (the handler thread answers that
;;; one alone); locals are read on demand, mapping a frame's number onto
;;; the live stack, which has grown by the loop's own frames since.
;;; ----------------------------------------------------------------

(defun %own-frame-p (frame)
  (let ((file (third frame)))
    (or (equal file *repl-file*) (equal file *this-file*))))

(defun %debug-frames ()
  "The live stack without the REPL's own frames, innermost first, as
(INDEX NAME FILE LINE LIVE-INDEX) renumbered from 0; and its depth."
  (let* ((all (ext:backtrace))
         (frames '())
         (i 0))
    (dolist (fr all)
      (unless (%own-frame-p fr)
        (push (list i (second fr) (third fr) (fourth fr) (first fr)) frames)
        (incf i)))
    (values (nreverse frames) (length all))))

(defun %debug-frame-locals (level n)
  "The locals of user frame N of LEVEL, as EXT:FRAME-LOCALS gives them, or
:NOT-AVAILABLE.  Frame numbers are relative to the top of the stack, and
the stack is deeper now than when the snapshot was taken: the difference
in depth is the shift.  Measured and read in one function, so both see
the same top."
  (let ((frame (nth n (%debug-level-frames level))))
    (if (null frame)
        :not-available
        (ext:frame-locals (+ (fifth frame)
                             (- (length (ext:backtrace)) (%debug-level-depth level)))))))

(defun %debug-condition-line (condition)
  (%one-line (format nil "~a: ~a" (type-of condition) (%condition-text condition)) 300))

(defun %repl-announce-debugger ()
  "Send DEBUGGER for the innermost level, or DEBUGGER 0 when none is left."
  (let ((message
          (mp:with-lock-held (*repl-lock*)
            (let ((level (first *repl-debug-stack*))
                  (pkg (%prompt-package-name *package*)))
              (if (null level)
                  (format nil "DEBUGGER 0 ~a" pkg)
                  (with-output-to-string (s)
                    (format s "DEBUGGER ~d ~a~%~a" (length *repl-debug-stack*) pkg
                            (%debug-condition-line (%debug-level-condition level)))
                    (let ((i 0))
                      (dolist (r (%debug-level-restarts level))
                        (format s "~%~d: ~a ~a" i (restart-name r)
                                (%one-line (%restart-report r) 120))
                        (incf i)))))))))
    (%repl-send message)))

(defun %restart-report (restart)
  "What RESTART says for itself, or nothing: one without a report
function prints as #<RESTART NAME>, which the row already says."
  (let ((text (%condition-text restart)))
    (if (and (>= (length text) 2) (string= "#<" text :end2 2))
        ""
        text)))

(defun %repl-debug-take-job ()
  "Wait for the handler thread's next job, or NIL once the REPL is being
stopped."
  (mp:with-lock-held (*repl-lock*)
    (loop until (or *repl-debug-job* *repl-stop*)
          do (mp:condition-wait *repl-cv* *repl-lock*))
    (if *repl-stop*
        nil
        (prog1 *repl-debug-job*
          (setf *repl-debug-job* nil)))))

(defun %repl-debug-answer (text)
  (mp:with-lock-held (*repl-lock*)
    (setf *repl-debug-answer* (list text))
    (mp:condition-broadcast *repl-cv*)))

(defun %debug-locals-text (level n)
  (let ((locals (%debug-frame-locals level n)))
    (cond ((not (listp locals)) "; no such frame")
          ((null locals) "; no locals")
          (t (string-right-trim
              '(#\Newline)
              (with-output-to-string (s)
                (dolist (pair locals)
                  (format s "~a = ~a~%" (symbol-name (car pair))
                          (%print-bounded (cdr pair))))))))))

(defun %debug-eval (level n text)
  "FRAME-EVAL: the forms in TEXT with frame N's locals bound under their
placeholder names in the current package, values printed as the REPL
prints them.  An error here is a nested debugger level, whose ABORT
returns here."
  (let* ((locals (%debug-frame-locals level n))
         (names (if (listp locals)
                    (mapcar (lambda (p) (intern (symbol-name (car p)) *package*)) locals)
                    '()))
         (values (if (listp locals) (mapcar #'cdr locals) '()))
         (depth (length *repl-debug-stack*))
         (eof (list :eof)))
    (restart-case
        (handler-bind ((serious-condition #'%repl-debugger-hook))
          (progv names values
            (with-input-from-string (src text)
              (loop for form = (read src nil eof)
                    until (eq form eof)
                    do (format t "~&~a~%"
                               (%repl-values-string (multiple-value-list (eval form))))))))
      (abort ()
        :report (lambda (s) (format s "Return to debugger level ~d" depth))
        (format t "~&; Aborted~%")))
    (force-output)))

(defun %repl-debug-run-job (level job)
  (ecase (first job)
    (:frame
     (%repl-debug-answer (%debug-locals-text level (second job))))
    (:eval
     (%debug-eval level (second job) (third job)))
    (:restart
     (let ((r (nth (second job) (%debug-level-restarts level))))
       (if r
           (invoke-restart-interactively r)
           (format t "~&; no restart ~d~%" (second job)))))
    (:interrupt
     (error 'repl-interrupt))))

(defun %repl-debug (condition)
  "Park in the debugger for CONDITION until a restart takes the thread
elsewhere.  Never returns normally: every way out is a non-local exit
through the UNWIND-PROTECT, which announces the level below."
  (let ((level nil))
    (multiple-value-bind (frames depth) (%debug-frames)
      (setf level (%make-debug-level condition (compute-restarts condition)
                                     frames depth)))
    (force-output)   ; what the form printed so far goes out before DEBUGGER
    (mp:with-lock-held (*repl-lock*)
      (push level *repl-debug-stack*)
      (setf *repl-debug-job* nil))
    (unwind-protect
         (progn
           (%repl-announce-debugger)
           (loop
             (let ((job (%repl-debug-take-job)))
               (if (null job)
                   (abort condition)   ; REPL-DETACH, or the editor is gone
                   (%repl-debug-run-job level job)))))
      (mp:with-lock-held (*repl-lock*)
        (setf *repl-debug-stack* (remove level *repl-debug-stack*)
              *repl-debug-job* nil))
      (%repl-announce-debugger))))

(defun %repl-debugger-hook (condition)
  "The HANDLER-BIND handler around a REPL form.  Declines -- and the form
ends with RESULT 10 as always -- unless DEBUG was asked for; an interrupt
is never debugged, and neither is anything once the REPL is stopping."
  (when (and *repl-debug* (not *repl-stop*)
             (not (typep condition 'repl-interrupt)))
    (%repl-debug condition)))

;;; ----------------------------------------------------------------
;;; The debugger commands (on the handler thread)
;;; ----------------------------------------------------------------

(defun %repl-debug-level ()
  (mp:with-lock-held (*repl-lock*) (first *repl-debug-stack*)))

(defun %repl-debug-post (job)
  "Hand JOB to the parked REPL thread and reply at once."
  (mp:with-lock-held (*repl-lock*)
    (cond ((null *repl-debug-stack*)
           (values +rc-error+ "ERROR: the REPL is not in the debugger"))
          (*repl-debug-job*
           (values +rc-error+ "ERROR: the debugger has not taken the previous command yet"))
          (t
           (setf *repl-debug-job* job
                 *repl-debug-answer* nil)
           (mp:condition-broadcast *repl-cv*)
           (values +rc-ok+ "")))))

(defun %repl-debug-ask (job)
  "Hand JOB to the parked REPL thread and wait for its answer -- briefly:
a thread busy in a FRAME-EVAL answers when that is done, and the port must
not hang on it."
  (mp:with-lock-held (*repl-lock*)
    (cond ((null *repl-debug-stack*)
           (values +rc-error+ "ERROR: the REPL is not in the debugger"))
          (*repl-debug-job*
           (values +rc-error+ "ERROR: the debugger has not taken the previous command yet"))
          (t
           (setf *repl-debug-job* job
                 *repl-debug-answer* nil)
           (mp:condition-broadcast *repl-cv*)
           (let ((waited 0))
             (loop until (or *repl-debug-answer* *repl-stop*
                             (>= waited *repl-debug-answer-timeout-seconds*))
                   do (mp:condition-wait *repl-cv* *repl-lock* 0.5)
                      (incf waited 0.5)))
           (if *repl-debug-answer*
               (values +rc-ok+ (prog1 (first *repl-debug-answer*)
                                 (setf *repl-debug-answer* nil)))
               (values +rc-error+ "ERROR: the REPL thread did not answer (is it running a FRAME-EVAL?)"))))))

(defun %repl-restart-named (name)
  "The index of the innermost restart called NAME at the current level,
or (values NIL REASON)."
  (let ((level (%repl-debug-level)))
    (if (null level)
        (values nil "ERROR: the REPL is not in the debugger")
        (let ((i (position name (%debug-level-restarts level) :key #'restart-name)))
          (if i
              i
              (values nil (format nil "ERROR: no ~a restart at this level" name)))))))

(define-command "BACKTRACE" (arg)
  (declare (ignore arg))
  (let ((level (%repl-debug-level)))
    (if (null level)
        (values +rc-error+ "ERROR: the REPL is not in the debugger")
        (values +rc-ok+
                (string-right-trim
                 '(#\Newline)
                 (with-output-to-string (s)
                   (dolist (fr (%debug-level-frames level))
                     (format s "~d: ~a" (first fr)
                             (if (second fr)
                                 (%with-reply-printing (prin1-to-string (second fr)))
                                 "<anonymous>"))
                     (when (third fr)
                       (format s "  ~a:~d" (third fr) (or (fourth fr) 0)))
                     (terpri s))))))))

(define-command "RESTARTS" (arg)
  ;; The current level's restarts again, as DEBUGGER listed them, with the
  ;; level first: for a macro that missed the announcement.
  (declare (ignore arg))
  (let ((level (%repl-debug-level)))
    (if (null level)
        (values +rc-error+ "ERROR: the REPL is not in the debugger")
        (values +rc-ok+
                (string-right-trim
                 '(#\Newline)
                 (with-output-to-string (s)
                   (format s "level ~d: ~a~%"
                           (mp:with-lock-held (*repl-lock*) (length *repl-debug-stack*))
                           (%debug-condition-line (%debug-level-condition level)))
                   (let ((i 0))
                     (dolist (r (%debug-level-restarts level))
                       (format s "~d: ~a ~a~%" i (restart-name r)
                               (%one-line (%restart-report r) 120))
                       (incf i)))))))))

(define-command "FRAME" (arg)
  (let ((n (%parse-index arg)))
    (if (null n)
        (values +rc-fatal+ "ERROR: FRAME requires a frame number")
        (%repl-debug-ask (list :frame n)))))

(define-command "FRAME-EVAL" (arg)
  (multiple-value-bind (n rest) (%split-index arg)
    (cond ((null n)
           (values +rc-fatal+ "ERROR: FRAME-EVAL requires a frame number and a form"))
          ((zerop (length rest))
           (values +rc-fatal+ "ERROR: FRAME-EVAL requires a form"))
          (t (%repl-debug-post (list :eval n rest))))))

(define-command "RESTART" (arg)
  (let ((n (%parse-index arg))
        (level (%repl-debug-level)))
    (cond ((null n)
           (values +rc-fatal+ "ERROR: RESTART requires a restart number"))
          ((null level)
           (values +rc-error+ "ERROR: the REPL is not in the debugger"))
          ((>= n (length (%debug-level-restarts level)))
           (values +rc-error+ (format nil "ERROR: no restart ~d (this level has ~d)"
                                      n (length (%debug-level-restarts level)))))
          (t (%repl-debug-post (list :restart n))))))

(define-command "ABORT" (arg)
  (declare (ignore arg))
  (multiple-value-bind (i reason) (%repl-restart-named 'abort)
    (if i (%repl-debug-post (list :restart i)) (values +rc-error+ reason))))

(define-command "CONTINUE" (arg)
  (declare (ignore arg))
  (multiple-value-bind (i reason) (%repl-restart-named 'continue)
    (if i (%repl-debug-post (list :restart i)) (values +rc-error+ reason))))

;;; ----------------------------------------------------------------
;;; The commands
;;; ----------------------------------------------------------------

(define-command "REPL-ATTACH" (arg)
  (let* ((text (%unquote arg))
         (space (position #\Space text))
         (port (subseq text 0 space))
         (option (string-upcase
                  (string-trim '(#\Space #\Tab)
                               (subseq text (or space (length text)))))))
    (cond ((zerop (length port))
           (values +rc-fatal+ "ERROR: REPL-ATTACH requires the editor's port name"))
          ((not (or (zerop (length option)) (string= option "DEBUG")))
           (values +rc-fatal+ (format nil "ERROR: REPL-ATTACH <port> [DEBUG], not ~a" option)))
          ((null *repl-send*)
           (values +rc-fatal+ "ERROR: no ARexx transport in this image (is AMIGA.AREXX loaded?)"))
          ((not (%repl-stop))
           (values +rc-error+ "ERROR: the previous REPL thread did not stop"))
          (t
           (%repl-start port (string= option "DEBUG"))
           (values +rc-ok+ (%prompt-package-name *command-package*))))))

(define-command "REPL-DETACH" (arg)
  (declare (ignore arg))
  (if (%repl-stop)
      (values +rc-ok+ "REPL detached")
      (values +rc-error+ "ERROR: the REPL thread did not stop")))

(define-command "REPL-EVAL" (arg)
  (cond ((zerop (length arg))
         (values +rc-fatal+ "ERROR: REPL-EVAL requires a form"))
        ((not (%repl-alive-p))
         (values +rc-error+ "ERROR: no REPL attached (send REPL-ATTACH <port> first)"))
        (t
         (mp:with-lock-held (*repl-lock*)
           (cond (*repl-debug-stack*
                  (values +rc-error+ "ERROR: the REPL is in the debugger (ABORT returns to the prompt)"))
                 ((or *repl-job* *repl-busy*)
                  (values +rc-error+ "ERROR: the REPL is busy (REPL-INTERRUPT aborts the running form)"))
                 (t
                  (setf *repl-job* arg)
                  (mp:condition-broadcast *repl-cv*)
                  (values +rc-ok+ "")))))))

(define-command "REPL-INPUT" (arg)
  (if (not (%repl-alive-p))
      (values +rc-error+ "ERROR: no REPL attached")
      (mp:with-lock-held (*repl-lock*)
        (if *repl-reading*
            (progn
              (setf *repl-input* (list arg))
              (mp:condition-broadcast *repl-cv*)
              (values +rc-ok+ ""))
            (values +rc-error+ "ERROR: the REPL is not reading a line")))))

(define-command "REPL-INTERRUPT" (arg)
  (declare (ignore arg))
  (cond ((not (%repl-alive-p))
         (values +rc-error+ "ERROR: no REPL attached"))
        ((not (mp:with-lock-held (*repl-lock*) *repl-busy*))
         (values +rc-ok+ "the REPL is idle"))
        (t
         ;; Parked in the debugger the thread is in CONDITION-WAIT, not at
         ;; a safepoint: the interrupt goes in as a job too.  Both routes
         ;; end the form with `Interrupted'; the closure checks
         ;; *REPL-INTERRUPTIBLE* so the second one finds nothing to do.
         (mp:with-lock-held (*repl-lock*)
           (when *repl-debug-stack*
             (setf *repl-debug-job* (list :interrupt))
             (mp:condition-broadcast *repl-cv*)))
         (%repl-interrupt)
         (values +rc-ok+ ""))))

(provide "dev-repl")
