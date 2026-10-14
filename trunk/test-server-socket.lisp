;;;; trunk/test-server-socket.lisp
;;;;
;;;; Integration test for server-side TCP sockets — ext:socket-listen and
;;;; ext:socket-accept, the foundation for a Sly/SLYNK server.  It drives the
;;;; full Lisp stack: listen, accept, and a bidirectional text round-trip over
;;;; the accepted connection (the same shape SLYNK uses).
;;;;
;;;; Run on the host:
;;;;   ./build/host/amilisp --load trunk/test-server-socket.lisp
;;;;
;;;; Requires a TCP stack.  On AmigaOS that means bsdsocket.library
;;;; (Roadshow/AmiTCP/Miami) must be present; the default FS-UAE config has
;;;; none, so this is host-oriented (also runs on a real Amiga, or a
;;;; TCP-enabled emulator).
;;;;
;;;; The exchange is single-threaded on purpose: a loopback connect()
;;;; completes into the listener's accept backlog, so socket-accept returns
;;;; immediately and the whole round-trip is deterministic — no sleeps, no
;;;; cross-thread socket-table races.

(defvar *pass-count* 0)
(defvar *fail-count* 0)

;; CHECK is a macro so it can catch errors signalled while evaluating ACTUAL
;; (a function call would never see them — the error short-circuits argument
;; evaluation before the call).
(defmacro check (name expected actual)
  (let ((e (gensym "EXPECTED")) (a (gensym "ACTUAL")) (c (gensym "COND")))
    `(handler-case
         (let ((,e ,expected) (,a ,actual))
           (if (equal ,e ,a)
               (progn (incf *pass-count*) (format t "PASS: ~A~%" ,name))
               (progn (incf *fail-count*)
                      (format t "FAIL: ~A - expected ~S got ~S~%" ,name ,e ,a))))
       (error (,c)
         (incf *fail-count*)
         (format t "FAIL: ~A - signaled error: ~A~%" ,name ,c)))))

(defparameter *port* 14550)

(format t "~%=== Server-socket integration test (port ~A) ===~%~%" *port*)

;; Ephemeral port: socket-listen 0 lets the OS pick the port, and
;; socket-local-port is the only way to discover it (what Sly needs to tell
;; Emacs which port to connect to).
(handler-case
    (let ((listener (ext:socket-listen 0 t)))
      (unwind-protect
           (let ((p (ext:socket-local-port listener)))
             (check "ephemeral socket-local-port is a positive integer" t
                    (and (integerp p) (> p 0)))
             ;; round-trip to prove the reported port is the one we listen on
             (let ((client (ext:open-tcp-stream "127.0.0.1" p)))
               (unwind-protect
                    (let ((conn (ext:socket-accept listener)))
                      (unwind-protect
                           (check "ephemeral port accepts a connection" t
                                  (streamp conn))
                        (close conn)))
                 (close client))))
        (close listener)))
  (error (e)
    (incf *fail-count*)
    (format t "FAIL: ephemeral-port setup - ~A~%" e)))

(handler-case
    (let ((listener (ext:socket-listen *port* t)))   ; T => bind 127.0.0.1 only
      (check "socket-listen returns a stream" t (streamp listener))
      (check "socket-local-port matches requested port" *port*
             (ext:socket-local-port listener))
      (unwind-protect
           (let ((client (ext:open-tcp-stream "127.0.0.1" *port*)))
             (unwind-protect
                  ;; connect() above has already landed in the backlog, so this
                  ;; accept returns the new connection without blocking.
                  (let ((conn (ext:socket-accept listener)))
                    (check "socket-accept returns a stream" t (streamp conn))
                    (unwind-protect
                         (progn
                           ;; client -> server
                           (write-line "hello sly" client)
                           (force-output client)
                           (let ((request (read-line conn nil nil)))
                             (check "server receives request" "hello sly" request)
                             ;; server -> client (echo, upcased)
                             (write-line (string-upcase request) conn)
                             (force-output conn))
                           ;; client reads the echo back
                           (check "client receives echo" "HELLO SLY"
                                  (read-line client nil nil))
                           ;; reading past a closed peer yields EOF, not an error
                           (close client)
                           (check "server sees EOF after client close" :eof
                                  (read-line conn nil :eof)))
                      (close conn)))
               ;; CLIENT already closed above inside the body; closing a closed
               ;; stream is a no-op, so this unwind cleanup stays safe.
               (close client)))
        (close listener)))
  (error (e)
    (incf *fail-count*)
    (format t "FAIL: setup - ~A~%" e)
    (format t "  (is port ~A free, and is a TCP stack available?)~%" *port*)))

(format t "~%=== Results ===~%")
(format t "Passed: ~A~%" *pass-count*)
(format t "Failed: ~A~%" *fail-count*)
(if (= *fail-count* 0)
    (format t "~%ALL TESTS PASSED~%")
    (format t "~%SOME TESTS FAILED~%"))
