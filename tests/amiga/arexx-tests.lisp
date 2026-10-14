; ARexx development port tests -- AmigaOS / MorphOS
;
; Loaded by tests/amiga/run-tests.lisp, which defines CHECK, *PASS-COUNT*
; and *FAIL-COUNT*.  It is a separate file because the tests name symbols in
; the AMIGA.AREXX package: the reader resolves those as it reads each form,
; so the REQUIRE that creates the package has to have run in an earlier
; read/eval cycle -- which a nested LOAD gives us and a form in the same
; file cannot.
;
; End-to-end over the real host protocol: AMIGA.AREXX:SEND builds an actual
; RXCOMM message with an argstring in ARG0 and waits for the reply, exactly
; as the ARexx interpreter does -- so this exercises the port without needing
; RexxMast or a second process.  The port name is deliberately NOT "AMILISP":
; a developer may have a real dev port open on the same machine.

(require "amiga/arexx")

(handler-case
    (progn
      (with-open-file (s "T:amilisp-arexx-test.lisp"
                         :direction :output :if-exists :supersede)
        (format s "(defvar *arexx-test-one* t)~%")
        (format s "(error \"arexx test error one\")~%")
        (format s "(arexx-test-undefined-function)~%")
        (format s "(defvar *arexx-test-end* t)~%"))
      (let ((port (amiga.arexx:start :name "CLAMIGATEST")))
        (check "arexx start returns the port name" "CLAMIGATEST" port)
        (check "arexx port-name" "CLAMIGATEST" (amiga.arexx:port-name))
        (check "arexx running-p" t (amiga.arexx:running-p))
        ; START registers STOP as an exit hook, so a process that ends with
        ; the port up takes it down instead of leaving a zombie task on it.
        (check "arexx start registers stop as an exit hook" t
               (and (member 'amiga.arexx:stop ext:*exit-hooks*) t))

        ; PING: the minimal round trip -- message out, reply in, rc 0 with a
        ; RESULT argstring (rc 0 is the only case ARexx transmits one).
        (multiple-value-bind (rc text) (amiga.arexx:send port "PING")
          (check "arexx ping rc" 0 rc)
          (check "arexx ping result" "PONG" text))

        ; Regression: AMIGA:AREXX-SEND used to leak the port-name buffer when
        ; the command argument failed its type check -- the type error itself
        ; longjmp'd past the cleanup.  Must error cleanly and leave the port
        ; serving.
        (check "arexx send rejects a non-string command" t
               (handler-case (progn (amiga.arexx:send port 42) nil)
                 (error () t)))
        (multiple-value-bind (rc text) (amiga.arexx:send port "PING")
          (check "arexx still alive after a bad command" 0 rc)
          (check "arexx still answers after a bad command" "PONG" text))

        (multiple-value-bind (rc text) (amiga.arexx:send port "VERSION")
          (check "arexx version rc" 0 rc)
          (check "arexx version mentions AmiLisp" t
                 (and (search "AmiLisp" text) t)))

        ; A form evaluated in the running image, from the editor's side.
        (multiple-value-bind (rc text) (amiga.arexx:send port "EVAL (+ 40 2)")
          (check "arexx eval rc" 0 rc)
          (check "arexx eval result" t (and (search "42" text) t)))

        ; A form that errors -- an undefined function, the typo case -- must
        ; come back as a diagnostic (rc 10, text via LASTRESULT) and leave
        ; the handler thread on the port: the editor's C-c C-c of
        ; (formt t "hello") must not take amilisp's console debugger or the
        ; port down with it.  Runtime-raised, so the JIT's unwind through
        ; the command layer's guard is what this exercises on the m68k.
        (multiple-value-bind (rc text)
            (amiga.arexx:send port "EVAL (cl-user::arexx-no-such-function 1 2)")
          (check "arexx eval of an undefined function rc" 10 rc)
          (check "arexx eval error drops RESULT" "" text))
        (multiple-value-bind (rc text) (amiga.arexx:send port "LASTRESULT")
          (check "arexx eval error lastresult rc" 0 rc)
          (check "arexx eval error names the function" t
                 (and (search "AREXX-NO-SUCH-FUNCTION" text) t))
          (check "arexx eval error counts one error" t
                 (and (search "1 error(s)" text) t)))
        (multiple-value-bind (rc text) (amiga.arexx:send port "PING")
          (check "arexx port serves after an eval error" 0 rc)
          (check "arexx port answers after an eval error" "PONG" text))
        (multiple-value-bind (rc text) (amiga.arexx:send port "EVAL (* 6 7)")
          (check "arexx eval works after an eval error" 0 rc)
          (check "arexx eval value after an eval error" t (and (search "42" text) t)))

        ; An unknown verb is fatal (rc 20) and, per the protocol, carries no
        ; RESULT -- the text is only reachable through LASTRESULT.
        (multiple-value-bind (rc text) (amiga.arexx:send port "NOSUCHVERB")
          (check "arexx unknown command rc" 20 rc)
          (check "arexx unknown command drops RESULT" "" text))
        (multiple-value-bind (rc text) (amiga.arexx:send port "LASTRESULT")
          (check "arexx lastresult rc" 0 rc)
          (check "arexx lastresult recovers the text" t
                 (and (search "unknown command" text) t)))

        ; The headline case: a file with two bad top-level forms comes back
        ; with a diagnostic for EACH, and the good forms after them still ran.
        (multiple-value-bind (rc text)
            (amiga.arexx:send port "LOAD T:amilisp-arexx-test.lisp")
          (check "arexx load rc is error" 10 rc)
          (check "arexx load error drops RESULT" "" text))
        (multiple-value-bind (rc text) (amiga.arexx:send port "LASTRESULT")
          (check "arexx load diagnostics rc" 0 rc)
          (check "arexx load reports both errors" t
                 (and (search "2 error(s)" text) t))
          (check "arexx load diagnostic has file:line" t
                 (and (search "amilisp-arexx-test.lisp:2" text) t)))
        (check "arexx load ran the forms after the errors" t
               (and (boundp '*arexx-test-end*) t))

        ; Introspection -- the editor's phase-2 commands, over the port.
        ; The definitions come from a file so SOURCE-LOCATION has a line to
        ; report and the docstring went through the compiler's load-time
        ; record.
        (with-open-file (s "T:amilisp-arexx-intro.lisp"
                           :direction :output :if-exists :supersede)
          (format s "(defun arexx-intro-fn (a &optional (b 2))~%")
          (format s "  \"Intro doc.\"~%")
          (format s "  (list a b))~%")
          (format s "(defmacro arexx-intro-mac (x &body body) `(progn ,x ,@body))~%"))
        (multiple-value-bind (rc text)
            (amiga.arexx:send port "LOAD T:amilisp-arexx-intro.lisp")
          (check "arexx intro file loads" 0 rc)
          (check "arexx intro file loads clean" t
                 (and (search "0 error(s)" text) t)))
        (multiple-value-bind (rc text) (amiga.arexx:send port "ARGLIST arexx-intro-fn")
          (check "arexx ARGLIST rc" 0 rc)
          (check "arexx ARGLIST is the written lambda list" "(a &optional (b 2))" text))
        (multiple-value-bind (rc text) (amiga.arexx:send port "ARGLIST arexx-intro-mac")
          (check "arexx ARGLIST of a macro rc" 0 rc)
          (check "arexx ARGLIST of a macro" "(x &body body)" text))
        (multiple-value-bind (rc text) (amiga.arexx:send port "ARGLIST no-such-fn-here")
          (declare (ignore text))
          (check "arexx ARGLIST of an unknown name is rc 10" 10 rc))
        (multiple-value-bind (rc text) (amiga.arexx:send port "COMPLETE arexx-intro-")
          (check "arexx COMPLETE rc" 0 rc)
          (check "arexx COMPLETE lists both, sorted" t
                 (and (search "arexx-intro-fn" text)
                      (search "arexx-intro-mac" text)
                      (< (search "arexx-intro-fn" text) (search "arexx-intro-mac" text))
                      t)))
        ;; No symbol starts with `xx-intro-': the ones that contain it.
        (multiple-value-bind (rc text) (amiga.arexx:send port "COMPLETE xx-intro-")
          (check "arexx COMPLETE fallback rc" 0 rc)
          (check "arexx COMPLETE lists the names containing the text" t
                 (and (search "arexx-intro-fn" text)
                      (search "arexx-intro-mac" text)
                      t)))
        (multiple-value-bind (rc text) (amiga.arexx:send port "DESCRIBE arexx-intro-fn")
          (check "arexx DESCRIBE rc" 0 rc)
          (check "arexx DESCRIBE shows the lambda list" t
                 (and (search "Lambda-list: (A &OPTIONAL (B 2))" text) t))
          (check "arexx DESCRIBE shows the docstring" t
                 (and (search "Documentation: Intro doc." text) t)))
        (multiple-value-bind (rc text) (amiga.arexx:send port "APROPOS arexx-intro")
          (check "arexx APROPOS rc" 0 rc)
          (check "arexx APROPOS tags the kinds" t
                 (and (search "arexx-intro-fn function" text)
                      (search "arexx-intro-mac macro" text)
                      t)))
        (multiple-value-bind (rc text) (amiga.arexx:send port "SOURCE-LOCATION arexx-intro-fn")
          (check "arexx SOURCE-LOCATION rc" 0 rc)
          (check "arexx SOURCE-LOCATION is file:line" t
                 (and (search "amilisp-arexx-intro.lisp:1" text) t)))
        (multiple-value-bind (rc text)
            (amiga.arexx:send port "MACROEXPAND-1 (arexx-intro-mac 1 2 3)")
          (check "arexx MACROEXPAND-1 rc" 0 rc)
          (check "arexx MACROEXPAND-1 expansion" "(progn 1 2 3)" text))
        (delete-file "T:amilisp-arexx-intro.lisp")

        ; The REPL (lib/dev-repl.lisp): a thread of its own that talks back
        ; to the editor's port.  There is one ARexx port per process, so the
        ; editor's port here is our own: the REPL thread's OUTPUT / READLINE
        ; / RESULT commands travel over ARexx as EVALs that record them, and
        ; READLINE is answered with a REPL-INPUT round trip -- the real
        ; protocol in both directions, with the editor played by this file.
        (defvar cl-user::*arexx-repl-sent* '())
        (setf ext.dev:*repl-send*
              (lambda (to command)
                (amiga.arexx:send
                 to (format nil "EVAL (push ~s cl-user::*arexx-repl-sent*)" command))
                (when (string= command "READLINE")
                  (amiga.arexx:send to "REPL-INPUT typed on the amiga"))
                (values 0 "")))
        (flet ((wait-result ()
                 (loop repeat 500
                       until (find-if (lambda (s)
                                        (and (>= (length s) 6)
                                             (string= "RESULT" s :end2 6)))
                                      cl-user::*arexx-repl-sent*)
                       do (sleep 0.02))
                 (prog1 (reverse cl-user::*arexx-repl-sent*)
                   (setf cl-user::*arexx-repl-sent* '())))
               (index-of (prefix sent)
                 (position-if (lambda (s) (and (>= (length s) (length prefix))
                                               (string= prefix s :end2 (length prefix))))
                              sent)))
          (multiple-value-bind (rc text) (amiga.arexx:send port "REPL-ATTACH CLAMIGATEST")
            (check "arexx REPL-ATTACH rc" 0 rc)
            (check "arexx REPL-ATTACH answers the package" "CL-USER" text))
          ; REPL-ATTACH loads lib/dev-repl.lisp on first use, which registers
          ; %repl-stop as an exit hook so the REPL thread cannot outlive the
          ; process (see dev-repl.lisp:326-330).  Newest-hook-first means
          ; %repl-stop, registered here, must run ahead of amiga.arexx:stop,
          ; registered by AMIGA.AREXX:START above -- else the port's exit
          ; hook could tear down the port while the REPL thread is still on
          ; it.
          (check "arexx REPL-ATTACH registers %repl-stop as an exit hook" t
                 (and (member 'ext.dev::%repl-stop ext:*exit-hooks*) t))
          (check "arexx %repl-stop runs ahead of amiga.arexx:stop (newest-first)" t
                 (let ((hooks ext:*exit-hooks*))
                   (and (member 'ext.dev::%repl-stop hooks)
                        (member 'amiga.arexx:stop hooks)
                        (< (position 'ext.dev::%repl-stop hooks)
                           (position 'amiga.arexx:stop hooks))
                        t)))
          (multiple-value-bind (rc text)
              (amiga.arexx:send
               port "REPL-EVAL (progn (princ \"repl says hi\") (terpri) (read-line))")
            (check "arexx REPL-EVAL replies at once" 0 rc)
            (check "arexx REPL-EVAL reply carries no values" "" text))
          (let ((sent (wait-result)))
            (check "arexx REPL streams the output" t
                   (and (index-of "OUTPUT repl says hi" sent) t))
            (check "arexx REPL asks the editor for a line" t
                   (and (index-of "READLINE" sent) t))
            (check "arexx REPL RESULT comes last" t
                   (and (index-of "RESULT 0 CL-USER" sent)
                        (= (index-of "RESULT 0 CL-USER" sent) (1- (length sent)))))
            (check "arexx REPL RESULT carries the line read" t
                   (and (search "\"typed on the amiga\"" (car (last sent))) t))
            (check "arexx REPL output precedes the read" t
                   (and (index-of "OUTPUT repl says hi" sent)
                        (index-of "READLINE" sent)
                        (< (index-of "OUTPUT repl says hi" sent)
                           (index-of "READLINE" sent)))))
          ; A running form keeps the port free, and can be aborted.
          (multiple-value-bind (rc text) (amiga.arexx:send port "REPL-EVAL (loop)")
            (declare (ignore text))
            (check "arexx REPL-EVAL of an endless loop replies" 0 rc))
          (sleep 0.2)
          (multiple-value-bind (rc text) (amiga.arexx:send port "PING")
            (check "arexx port answers while the REPL runs" 0 rc)
            (check "arexx port answers PONG while the REPL runs" "PONG" text))
          (multiple-value-bind (rc text) (amiga.arexx:send port "REPL-EVAL (+ 1 1)")
            (declare (ignore text))
            (check "arexx REPL-EVAL while busy is rc 10" 10 rc))
          (multiple-value-bind (rc text) (amiga.arexx:send port "REPL-INTERRUPT")
            (declare (ignore text))
            (check "arexx REPL-INTERRUPT rc" 0 rc))
          (let ((sent (wait-result)))
            (check "arexx REPL-INTERRUPT aborts the form" t
                   (and (index-of "RESULT 10 CL-USER" sent)
                        (search "Interrupted" (car (last sent)))
                        t)))
          (multiple-value-bind (rc text) (amiga.arexx:send port "REPL-EVAL (+ 40 2)")
            (declare (ignore text))
            (check "arexx REPL-EVAL after the interrupt rc" 0 rc))
          (let ((sent (wait-result)))
            (check "arexx REPL works after the interrupt" t
                   (and (index-of "RESULT 0 CL-USER" sent)
                        (search "42" (car (last sent)))
                        t)))
          (multiple-value-bind (rc text) (amiga.arexx:send port "REPL-DETACH")
            (declare (ignore text))
            (check "arexx REPL-DETACH rc" 0 rc))
          (multiple-value-bind (rc text) (amiga.arexx:send port "REPL-EVAL (+ 1 1)")
            (declare (ignore text))
            (check "arexx REPL-EVAL after detach is rc 10" 10 rc))
          ; The debugger (REPL-ATTACH ... DEBUG): an error parks the REPL
          ; thread and announces DEBUGGER 1 over the port; BACKTRACE and
          ; FRAME are answered while it is parked, RESTART 0 (the REPL's
          ; own ABORT) lets it go, and RESULT follows.
          (multiple-value-bind (rc text) (amiga.arexx:send port "REPL-ATTACH CLAMIGATEST DEBUG")
            (declare (ignore text))
            (check "arexx REPL-ATTACH DEBUG rc" 0 rc))
          (multiple-value-bind (rc text)
              ;; A DEFUN, not an FLET: a local function is inlined and has no
              ;; frame of its own, so FRAME 0 would be the form's.
              (amiga.arexx:send port "REPL-EVAL (progn (defun cl-user::arexx-dbg (n) (error \"dbg ~a\" n)) (cl-user::arexx-dbg 7))")
            (declare (ignore text))
            (check "arexx REPL-EVAL of an error with DEBUG replies" 0 rc))
          (loop repeat 500
                until (index-of "DEBUGGER 1" cl-user::*arexx-repl-sent*)
                do (sleep 0.02))
          (let ((announce (find-if (lambda (s) (and (>= (length s) 10) (string= "DEBUGGER 1" s :end2 10)))
                                   cl-user::*arexx-repl-sent*)))
            (check "arexx an error announces DEBUGGER 1" t (and announce t))
            (check "arexx DEBUGGER carries the condition" t
                   (and announce (search "SIMPLE-ERROR: dbg 7" announce) t))
            (check "arexx DEBUGGER lists the ABORT restart" t
                   (and announce (search "ABORT Return to the REPL" announce) t)))
          (multiple-value-bind (rc text) (amiga.arexx:send port "BACKTRACE")
            (check "arexx BACKTRACE rc" 0 rc)
            (check "arexx BACKTRACE starts at the erring frame" t
                   (and (search "0: " text) t)))
          (multiple-value-bind (rc text) (amiga.arexx:send port "FRAME 0")
            (check "arexx FRAME 0 rc" 0 rc)
            (check "arexx FRAME 0 shows the argument" t
                   (and (search "ARG0 = 7" text) t)))
          (multiple-value-bind (rc text) (amiga.arexx:send port "REPL-EVAL (+ 1 1)")
            (declare (ignore text))
            (check "arexx REPL-EVAL in the debugger is rc 10" 10 rc))
          (multiple-value-bind (rc text) (amiga.arexx:send port "RESTART 0")
            (declare (ignore text))
            (check "arexx RESTART 0 rc" 0 rc))
          (let ((sent (wait-result)))
            (check "arexx leaving the debugger announces DEBUGGER 0" t
                   (and (index-of "DEBUGGER 0" sent) t))
            (check "arexx the aborted form ends with RESULT" t
                   (and (index-of "RESULT 0 CL-USER" sent)
                        (search "Aborted" (car (last sent)))
                        t)))
          (multiple-value-bind (rc text) (amiga.arexx:send port "REPL-DETACH")
            (declare (ignore text))
            (check "arexx REPL-DETACH after the debugger rc" 0 rc)))
        (setf ext.dev:*repl-send* #'amiga.arexx:send)

        ; The inspector: synchronous on the port's thread.
        (multiple-value-bind (rc text) (amiga.arexx:send port "INSPECT (list 1 (list 2 3))")
          (check "arexx INSPECT rc" 0 rc)
          (check "arexx INSPECT header" t (and (search "CONS 1 2" text) t))
          (check "arexx INSPECT parts" t (and (search "1: Cdr = ((2 3))" text) t)))
        (multiple-value-bind (rc text) (amiga.arexx:send port "PART 1")
          (check "arexx PART rc" 0 rc)
          (check "arexx PART descends" t (and (search "CONS 2 2" text) t)))
        (multiple-value-bind (rc text) (amiga.arexx:send port "POP")
          (check "arexx POP rc" 0 rc)
          (check "arexx POP comes back" t (and (search "CONS 1 2" text) t)))
        (multiple-value-bind (rc text) (amiga.arexx:send port "POP")
          (declare (ignore text))
          (check "arexx POP at the root is rc 10" 10 rc))

        ; The port keeps serving after all of that.
        (multiple-value-bind (rc text) (amiga.arexx:send port "PING")
          (check "arexx still alive after failures" 0 rc)
          (check "arexx still answers" "PONG" text))

        (amiga.arexx:stop)
        (check "arexx stop clears running-p" nil (amiga.arexx:running-p))
        (check "arexx stop clears port-name" nil (amiga.arexx:port-name))
        ; Sending to a port that is gone must fail cleanly, not hang.
        (check "arexx send to a closed port errors" t
               (handler-case (progn (amiga.arexx:send port "PING") nil)
                 (error () t)))
        (delete-file "T:amilisp-arexx-test.lisp")))
  (error (e)
    (setq *fail-count* (+ *fail-count* 1))
    (format t "FAIL: ARexx port tests signaled: ~A~%" e)))

; The port closing while other threads' sends are still waiting for their
; replies.  rexxsyslib is shared by the whole process, and the old code
; closed it from whichever side finished first: here the handler closes the
; port (answering B's queued message with rc 20) while B is still parked in
; its send, and B then freed its message through a NULL library base -- the
; "68k exception at 0x4e" in CL-Thread at the end of the Clamacs drive,
; where the editor closes its port under its own REPL thread's send.  Every
; send and the open port hold a reference now; the library closes with the
; last one.
(handler-case
    (let* ((port (amiga.arexx:start :name "CLAMIGATEST2"))
           (slow-rc :none) (slow-text nil) (queued-rc :none)
           (a nil) (b nil))
      (check "arexx rexxsyslib held once by the open port" 1 (amiga::%arexx-lib-users))
      ; A keeps the handler busy for a second ...
      (setf a (mp:make-thread
               (lambda ()
                 (multiple-value-setq (slow-rc slow-text)
                   (amiga.arexx:send port "EVAL (progn (sleep 1) 7)")))))
      (sleep 0.3)
      ; ... so B's message is still queued when STOP closes the port.
      (setf b (mp:make-thread
               (lambda () (setf queued-rc (amiga.arexx:send port "PING")))))
      (sleep 0.3)
      (check "arexx rexxsyslib held by the port and each send in flight" 3
             (amiga::%arexx-lib-users))
      (amiga.arexx:stop)
      (mp:join-thread a)
      (mp:join-thread b)
      (check "arexx the slow command still got its answer" '(0 t)
             (list slow-rc (and (search "7" slow-text) t)))
      (check "arexx a send queued at close is answered rc 20, and survives" 20 queued-rc)
      (check "arexx rexxsyslib closed once the last send is done" 0
             (amiga::%arexx-lib-users))
      ; A send from a process with no port of its own opens and closes it.
      (check "arexx send to a missing port leaves rexxsyslib closed" '(t 0)
             (list (handler-case (progn (amiga.arexx:send "CLAMIGA-NO-SUCH-PORT" "PING") nil)
                     (error () t))
                   (amiga::%arexx-lib-users))))
  (error (e)
    (setq *fail-count* (+ *fail-count* 1))
    (format t "FAIL: ARexx close-under-send tests signaled: ~A~%" e)))
