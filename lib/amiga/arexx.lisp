;;; arexx.lisp -- ARexx development port for AmiLisp (AmigaOS / MorphOS)
;;;
;;; Loaded via (require "amiga/arexx").
;;;
;;;   (amiga.arexx:start)          ; => "AMILISP"
;;;
;;; From an editor macro (CygnusEd, GoldED, ...):
;;;
;;;   OPTIONS RESULTS
;;;   OPTIONS FAILAT 21                     /* let us see rc 10 ourselves */
;;;   ADDRESS AMILISP 'LOAD Work:src/foo.lisp'
;;;   IF RC = 0 THEN SAY RESULT
;;;   ELSE DO
;;;       ADDRESS AMILISP 'LASTRESULT'      /* rc /= 0 means ARexx dropped */
;;;       SAY RESULT                        /* RESULT -- fetch it back */
;;;   END
;;;
;;; See examples/amiga/arexx/ for complete, runnable editor macros.
;;;
;;; This file is the TRANSPORT half only: a handler thread that owns the
;;; public message port, hands each command string to EXT.DEV:HANDLE-COMMAND
;;; and ships the reply back.  Every decision about what a command means
;;; lives in lib/dev-commands.lisp, which is portable and host-tested.
;;;
;;; Concurrency: commands run on the handler thread, concurrently with
;;; whatever the REPL is doing.  That is what makes the port answer while
;;; the main thread is busy -- and it means an editor-triggered LOAD and a
;;; REPL-triggered one can compile at the same time.  Drive one at a time.

;; compile-time too: COMPILE-FILE (the host builds the lib/amiga FASLs) must see
;; these packages at read time, not only LOAD.
(eval-when (:compile-toplevel :load-toplevel :execute)
  (require "dev-commands"))

(defpackage "AMIGA.AREXX"
  (:use "CL")
  (:export
   "START" "STOP" "RUNNING-P" "PORT-NAME" "SEND"
   "*DEFAULT-PORT-NAME*" "*HANDLER-THREAD*"))

(in-package "AMIGA.AREXX")

(defvar *default-port-name* "AMILISP"
  "Base name claimed by START.  ARexx upcases the name in ADDRESS, so the
port is always registered upcased; if the name is taken (a second amilisp
is already running) the port becomes AMILISP.1, AMILISP.2, ...")

(defvar *handler-thread* nil)
(defvar *port-name* nil)
(defvar *start-error* nil)

(defconstant +startup-timeout-seconds+ 5)
(defconstant +poll-interval+ 0.02)

(defun port-name ()
  "Name the port is registered under, or NIL when it is not running."
  *port-name*)

(defun running-p ()
  (and *handler-thread*
       (mp:thread-alive-p *handler-thread*)
       *port-name*
       t))

(defun %serve-one (command)
  "Run one COMMAND and reply.  The reply is issued from an UNWIND-PROTECT
cleanup so that even a command that blows up past HANDLE-COMMAND's own
guard cannot leave the ARexx sender blocked forever -- an unanswered
message is a hung editor, not a lost result."
  (let ((rc ext.dev:+rc-fatal+)
        (text "ERROR: the command was aborted before it produced a reply"))
    (unwind-protect
         (multiple-value-setq (rc text) (ext.dev:handle-command command))
      (amiga:arexx-reply rc text))))

(defun %handler-loop (basename)
  "Body of the handler thread.  Opens the port HERE, not in START: exec
binds a message port's signal to the task that creates it, so only this
thread can wait on it."
  (unwind-protect
       (progn
         (handler-case
             (setf *port-name* (amiga:arexx-open basename))
           (error (c)
             (setf *start-error* (princ-to-string c))
             (return-from %handler-loop nil)))
         (loop
           (let ((command (amiga:arexx-wait)))
             ;; NIL means STOP woke us rather than a message arriving.
             (when (null command) (return))
             (%serve-one command))))
    (setf *port-name* nil)
    (amiga:arexx-close)))

(defun start (&key (name *default-port-name*)
                   (stack-size (* 256 1024))
                   (vm-frames 512))
  "Open the ARexx port and start its handler thread.  Returns the port
name actually claimed.

STACK-SIZE and VM-FRAMES size the handler thread: commands compile code,
and compiling the GUI/game load path needs far more than the AmigaOS
default stack (see the Amiga Stack Requirements notes in CLAUDE.md).
Raise them if a LOAD over the port reports a stack-exhaustion error that
the same LOAD does not produce at the REPL."
  (when (running-p)
    (error "AMIGA.AREXX: port ~a is already running" *port-name*))
  (setf *port-name* nil
        *start-error* nil)
  ;; Take the port down when the process exits.  Without this a
  ;; --non-interactive run (or a (quit)) that ends with the handler thread
  ;; alive leaves an AmigaOS task behind that still owns the public port
  ;; but whose VM has been torn down: the next message to it crashes the
  ;; task, and the sender waits forever for a reply.  Exit hooks run before
  ;; any teardown, and the registration is idempotent.
  (ext:add-exit-hook 'stop)
  (setf *handler-thread*
        (mp:make-thread (lambda () (%handler-loop name))
                        :name "arexx-port"
                        :stack-size stack-size
                        :vm-frames vm-frames))
  ;; The port is opened by the thread, so START has to wait for it to come
  ;; up before it can honestly report the name (or the failure).
  (let ((waited 0))
    (loop until (or *port-name*
                    *start-error*
                    (not (mp:thread-alive-p *handler-thread*))
                    (>= waited +startup-timeout-seconds+))
          do (sleep +poll-interval+)
             (incf waited +poll-interval+)))
  (cond (*port-name* *port-name*)
        (*start-error*
         (setf *handler-thread* nil)
         (error "AMIGA.AREXX: could not open port ~a: ~a" name *start-error*))
        (t
         (setf *handler-thread* nil)
         (error "AMIGA.AREXX: the handler thread did not come up within ~d seconds"
                +startup-timeout-seconds+))))

(defun stop ()
  "Remove the port and stop the handler thread.  Messages still queued are
answered with a failure code rather than dropped."
  (when *handler-thread*
    (amiga:arexx-request-stop)
    (let ((waited 0))
      (loop while (and (mp:thread-alive-p *handler-thread*)
                       (< waited +startup-timeout-seconds+))
            do (sleep +poll-interval+)
               (incf waited +poll-interval+)))
    ;; If the thread is still alive here, the wake signal did not get it out
    ;; of whatever it was doing (e.g. mid-LOAD) within the timeout -- the OS
    ;; port and its task are still live.  Clearing *port-name*/*handler-thread*
    ;; anyway would make RUNNING-P/PORT-NAME lie, and a later START with the
    ;; same base name would race the still-open port.  Signal instead of
    ;; silently forgetting about it.
    (if (mp:thread-alive-p *handler-thread*)
        (error "AMIGA.AREXX: handler thread did not stop within ~d seconds; port ~a may still be open"
               +startup-timeout-seconds+ *port-name*)
        (setf *handler-thread* nil
              *port-name* nil)))
  t)

(defun send (port command &key (result-size 8192))
  "Send COMMAND to the public ARexx port PORT and wait for the reply.
Returns (values RC RESULT-STRING).

The sending half of the same protocol: it drives any Amiga application's
ARexx port from Lisp, and it is what the test suite uses to exercise our
own port end to end without needing RexxMast."
  (amiga:arexx-send port command result-size))

;; The editor's REPL (lib/dev-repl.lisp) sends its OUTPUT / READLINE /
;; RESULT traffic to the editor's own port with this.  A port name that is
;; another transport's (`tcp:HOST:PORT/TOKEN', lib/dev-tcp.lisp) goes on
;; to the sender that was installed before, so the two files may load in
;; either order; installed once, however often this file is loaded.
(defvar *previous-repl-send* nil)
(defvar *send-installed* nil)

(unless *send-installed*
  (setf *previous-repl-send* ext.dev:*repl-send*
        *send-installed* t
        ext.dev:*repl-send*
        (lambda (port command)
          (if (and *previous-repl-send*
                   (> (length port) 4)
                   (string-equal port "tcp:" :end1 4))
              (funcall *previous-repl-send* port command)
              (send port command)))))

(provide "amiga/arexx")
