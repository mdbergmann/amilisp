;;; dev-tcp-tests.lisp -- the development port over TCP (lib/dev-tcp.lisp)
;;; on the Amiga, over loopback: bsdsocket.library in the emulator, the
;;; same server a Mac Clamacs drives an Amiga amilisp through over the LAN.
;;; Loaded by run-tests.lisp, which defines CHECK and the counters.
;;;
;;; Why on the target: the frames, the AUTH gate and the REPL's way back are
;;; host-tested in full by tests/test_dev_tcp.sh; what this file pins is
;;; that the same code serves on bsdsocket -- the named bind, the read
;;; timeouts a connection thread lives by, a REPL thread sending through a
;;; `tcp:' name -- on a 68k build with its JIT.  The LAN leg (a client on
;;; another machine) is checked by hand once per release.

(require "dev-commands")
(require "dev-tcp")

(defvar cl-user::*dtt-served-before* 0)

(defun dtt-reply (stream line)
  "rc and the text with newlines as ~, or GONE."
  (multiple-value-bind (rc text) (ext.dev.tcp:request stream line)
    (if rc (format nil "rc=~d ~a" rc (substitute #\~ #\Newline text)) "GONE")))

(defun dtt-closed-after-p (stream)
  (setf (ext:socket-stream-timeout stream :input) 5)
  (handler-case (null (read-char stream nil nil)) (error () nil)))

(let ((server (handler-case (ext.dev.tcp:start :port 0 :token "amiga-token")
                (error (e)
                  (format t "NOTE: dev-tcp tests skipped: ~a~%" e)
                  nil))))
  (when server
    (unwind-protect
         (let ((port (ext.dev.tcp:port server)))
           (check "dev-tcp: start listens on loopback" "127.0.0.1" (ext.dev.tcp:host server))
           (check "dev-tcp: start registers the exit hook" t
                  (and (member 'ext.dev.tcp::stop-all ext:*exit-hooks*) t))
           ;; The right token is served.
           (let ((c (ext.dev.tcp:connect "127.0.0.1" port "amiga-token")))
             (check "dev-tcp: a right token connects" t (and c t))
             (when c
               (check "dev-tcp: PING" "rc=0 PONG" (dtt-reply c "PING"))
               (check "dev-tcp: EVAL answers the value" t
                      (and (search "rc=0 42" (dtt-reply c "EVAL (* 6 7)")) t))
               (check "dev-tcp: an unknown verb is rc 20" t
                      (and (search "rc=20 ERROR: unknown command" (dtt-reply c "NOSUCHVERB")) t))
               ;; A character above 127 is one in <n> and comes back whole.
               (let ((e-acute (string (code-char 233))))
                 (ext.dev.tcp:write-request c (concatenate 'string "EVAL (length \"" e-acute "x\")"))
                 (multiple-value-bind (rc text) (ext.dev.tcp:read-reply c)
                   (check "dev-tcp: <n> counts characters" t
                          (and (eql rc 0) (eql (search "2" text) 0)))))
               (close c)))
           ;; Refused: no AUTH, a wrong token.
           (setq cl-user::*dtt-served-before* (ext.dev.tcp::server-served server))
           (let ((r (ext:open-tcp-stream "127.0.0.1" port 5)))
             (ext.dev.tcp:write-request r "EVAL (setq cl-user::*dtt-probe* :ran)")
             (multiple-value-bind (rc text) (ext.dev.tcp:read-reply r)
               (check "dev-tcp: a connection that skips AUTH is refused" '(20 "authentication required")
                      (list rc text)))
             (check "dev-tcp: and closed" t (dtt-closed-after-p r))
             (close r))
           (sleep 0.2)
           (check "dev-tcp: the refused command did not run" nil (boundp 'cl-user::*dtt-probe*))
           (let ((r (ext:open-tcp-stream "127.0.0.1" port 5)))
             (ext.dev.tcp:write-request r "AUTH wrong")
             (multiple-value-bind (rc text) (ext.dev.tcp:read-reply r)
               (check "dev-tcp: a wrong token is refused" '(20 "authentication required")
                      (list rc text)))
             (close r))
           (check "dev-tcp: nothing was served to a refused connection"
                  cl-user::*dtt-served-before* (ext.dev.tcp::server-served server))
           ;; A wildcard host and an address this machine does not have.
           (check "dev-tcp: a wildcard :host is refused" t
                  (handler-case (progn (ext.dev.tcp:stop (ext.dev.tcp:start :port 0 :host "0.0.0.0" :token "t")) nil)
                    (error (e) (and (search "wildcard" (princ-to-string e)) t))))
           ;; bsdsocket's parser (platform_amiga.c) takes leading zeros too, so
           ;; an all-zero address is INADDR_ANY however it is spelled.
           (check "dev-tcp: a wildcard spelled with leading zeros is refused" t
                  (handler-case (progn (ext.dev.tcp:stop (ext.dev.tcp:start :port 0 :host "00.0.0.0" :token "t")) nil)
                    (error (e) (and (search "wildcard" (princ-to-string e)) t))))
           (check "dev-tcp: an address no interface has is refused" t
                  (handler-case (progn (ext.dev.tcp:stop (ext.dev.tcp:start :port 0 :host "203.0.113.1" :token "t")) nil)
                    (error (e) (and (search "203.0.113.1" (princ-to-string e)) t))))
           ;; The REPL's way back: a second server plays the editor.
           (let* ((sent '())
                  (lock (mp:make-lock))
                  (editor (ext.dev.tcp:start :port 0 :token "editor-token"))
                  (name (ext.dev.tcp:port-name "127.0.0.1" (ext.dev.tcp:port editor) "editor-token")))
             (ext.dev:define-raw-command "OUTPUT" (arg)
               (mp:with-lock-held (lock) (push (concatenate 'string "OUTPUT " arg) sent))
               (values 0 ""))
             (ext.dev:define-raw-command "RESULT" (arg)
               (mp:with-lock-held (lock) (push (concatenate 'string "RESULT " arg) sent))
               (values 0 ""))
             (ext.dev:define-raw-command "READLINE" (arg)
               (declare (ignore arg))
               (mp:with-lock-held (lock) (push "READLINE" sent))
               (ext.dev:handle-command "REPL-INPUT typed line")
               (values 0 ""))
             (ext.dev:define-raw-command "DEBUGGER" (arg)
               (mp:with-lock-held (lock) (push (concatenate 'string "DEBUGGER " arg) sent))
               (values 0 ""))
             (flet ((wait-for (prefix)
                      (loop repeat 750
                            do (when (mp:with-lock-held (lock)
                                       (find-if (lambda (s) (and (>= (length s) (length prefix))
                                                                 (string= prefix s :end2 (length prefix))))
                                                sent))
                                 (return t))
                               (sleep 0.02)))
                    (taken ()
                      (mp:with-lock-held (lock)
                        (prog1 (reverse sent) (setf sent '())))))
               (let ((c (ext.dev.tcp:connect "127.0.0.1" port "amiga-token")))
                 (check "dev-tcp: REPL-ATTACH takes a tcp: name" t
                        (and (search "rc=0" (dtt-reply c (concatenate 'string "REPL-ATTACH " name))) t))
                 (dtt-reply c "REPL-EVAL (progn (princ \"hi\") (terpri) (+ 2 3))")
                 (check "dev-tcp: RESULT reaches the editor over TCP" t (wait-for "RESULT"))
                 (let ((got (taken)))
                   (check "dev-tcp: OUTPUT came first, line by line" t
                          (and (>= (length got) 2)
                               (string= (first got) (format nil "OUTPUT hi~%"))
                               (search "RESULT 0 CL-USER" (car (last got)))
                               (search "5" (car (last got)))
                               t)))
                 (dtt-reply c "REPL-EVAL (read-line)")
                 (check "dev-tcp: READLINE is answered through REPL-INPUT" t
                        (and (wait-for "RESULT")
                             (find-if (lambda (s) (search "typed line" s)) (taken))
                             t))
                 ;; The editor goes: the REPL thread stops itself.
                 (ext.dev.tcp:stop editor)
                 (sleep 0.3)
                 (dtt-reply c "REPL-EVAL (+ 1 1)")
                 (loop repeat 250
                       while (and ext.dev::*repl-thread* (mp:thread-alive-p ext.dev::*repl-thread*))
                       do (sleep 0.02))
                 (check "dev-tcp: an editor that went stops the REPL" nil
                        (and ext.dev::*repl-thread* (mp:thread-alive-p ext.dev::*repl-thread*) t))
                 (check "dev-tcp: and its connection is dropped" 0 (length ext.dev.tcp::*clients*))
                 (close c))))
           ;; STOP over the port itself: the reply arrives, then the port is gone.
           (let ((c (ext.dev.tcp:connect "127.0.0.1" port "amiga-token")))
             (check "dev-tcp: EVAL (ext.dev.tcp:stop) still answers" t
                    (and (search "rc=0" (dtt-reply c "EVAL (ext.dev.tcp:stop)")) t))
             (check "dev-tcp: after which the connection is closed" t (dtt-closed-after-p c))
             (close c)
             (check "dev-tcp: and the listener refuses" :refused
                    (handler-case (progn (close (ext:open-tcp-stream "127.0.0.1" port 1)) :accepted)
                      (error () :refused)))
             (check "dev-tcp: running-p is off" nil (ext.dev.tcp:running-p server))))
      (ext.dev.tcp:stop server))))
