; TLS tests (AmigaOS/MorphOS) — loopback client/server through AmiSSL /
; openssl3.library via the socket reactor.  Loaded from run-tests.lisp the
; same way as arexx-tests.lisp (nested LOAD so a failure here costs this
; file, not the suite).
;
; Gated on (ext:tls-available-p): a machine (or emulator config) without
; AmiSSL installed skips cleanly with a note instead of failing — TLS is a
; runtime-optional capability.  Certificates come from AmiLisp:tests/data
; (the suite runs with the repo as current directory).
;
; This mirrors tests/tls-loopback.lisp on the host, scaled down for the
; emulated CPU: one verified handshake (the self-signed test cert is its
; own trust anchor via :ca-file), a character and a binary round-trip,
; LISTEN semantics, peer-certificate introspection, and EOF.

(if (not (ext:tls-available-p))
    (format t "NOTE: TLS provider not installed (AmiSSL) -- TLS tests skipped~%")
    (let* ((cert "tests/data/tls-test-cert.pem")
           (key  "tests/data/tls-test-key.pem")
           (listener (ext:socket-listen 0 t))
           (port (ext:socket-local-port listener))
           (server
             (mp:make-thread
              (lambda ()
                (let ((conn (ext:socket-accept listener)))
                  (ext:socket-start-tls conn :server t
                                             :certificate cert
                                             :key key
                                             :timeout 120)
                  (let ((line (read-line conn)))
                    (write-line (string-upcase line) conn)
                    (finish-output conn))
                  (let ((buf (make-array 64 :element-type '(unsigned-byte 8))))
                    (read-sequence buf conn)
                    (dotimes (i 64)
                      (setf (aref buf i) (mod (* 2 (aref buf i)) 256)))
                    (write-sequence buf conn)
                    (finish-output conn))
                  (close conn)))
              :name "tls-test-server"
              :stack-size 65536)))
      (format t "TLS provider: ~a~%" (ext:tls-version))
      (let ((c (ext:open-tcp-stream "127.0.0.1" port 30)))
        (check "tls: plain before upgrade" nil (ext:socket-tls-p c))
        ;; Slow-CPU headroom: the 68k handshake can take a while.
        (ext:socket-start-tls c :hostname "localhost"
                                :verify t
                                :ca-file cert
                                :timeout 120)
        (check "tls: upgraded" t (ext:socket-tls-p c))
        (write-line "hello over amiga tls" c)
        (finish-output c)
        (check "tls: line echo" "HELLO OVER AMIGA TLS" (read-line c))
        (let ((cert-info (ext:tls-peer-certificate c)))
          (check "tls: peer cert subject" t
                 (not (null (search "CN=localhost"
                                    (getf cert-info :subject ""))))))
        (let ((out (make-array 64 :element-type '(unsigned-byte 8)))
              (in  (make-array 64 :element-type '(unsigned-byte 8))))
          (dotimes (i 64) (setf (aref out i) i))
          (write-sequence out c)
          (finish-output c)
          (read-sequence in c)
          (check "tls: binary echo" t
                 (let ((ok t))
                   (dotimes (i 64 ok)
                     (unless (= (aref in i) (mod (* 2 i) 256))
                       (setq ok nil))))))
        ;; Server closes after the binary echo: READ-CHAR blocks until the
        ;; close_notify arrives, then reports EOF; LISTEN at EOF is NIL.
        (check "tls: eof" :eof (read-char c nil :eof))
        (check "tls: listen at eof" nil (listen c))
        (close c))
      (mp:join-thread server)
      (close listener)

      ;; Regression: a peer that accepts the TCP connection but never
      ;; speaks TLS must not leave the client socket looking
      ;; TLS-upgraded once the handshake times out
      ;; (reactor_expire_deadlines only dropped the half-established SSL
      ;; state for REQ_CONNECT, not REQ_TLS_START -- ext:socket-tls-p
      ;; would then lie, and a retry would fail up front with "socket
      ;; is already TLS-upgraded").
      (let* ((listener2 (ext:socket-listen 0 t))
             (port2 (ext:socket-local-port listener2))
             (server2
               (mp:make-thread
                (lambda ()
                  (let ((conn (ext:socket-accept listener2)))
                    (sleep 5)
                    (ignore-errors (close conn))))
                :name "tls-test-stall-server"
                :stack-size 65536)))
        (let ((c (ext:open-tcp-stream "127.0.0.1" port2 30)))
          (check "tls: stalled handshake times out" t
                 (handler-case
                     (progn (ext:socket-start-tls c :timeout 2) nil)
                   (error () t)))
          (check "tls: not upgraded after timeout" nil (ext:socket-tls-p c))
          (check "tls: retry after timeout not rejected as already-upgraded" t
                 (handler-case
                     (progn (ext:socket-start-tls c :timeout 2) t)
                   (error (e)
                     (not (search "already TLS-upgraded"
                                  (format nil "~a" e))))))
          (ignore-errors (close c)))
        (mp:join-thread server2)
        (close listener2))))
