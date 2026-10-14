;;; tls-loopback.lisp — end-to-end TLS test over loopback.
;;;
;;; Exercises the whole EXT TLS surface against itself: an MP server thread
;;; accepts on an ephemeral loopback port and upgrades with :SERVER T; the
;;; main thread connects, upgrades as a client, and the two sides exchange
;;; data through the encrypted stream — characters, binary chunks, LISTEN,
;;; peer-certificate introspection, verification success AND failure, and
;;; EOF on close.  Driven by tests/test_tls_loopback.sh (and, under
;;; CLAMIGA_GC_STRESS=1, by the gc-stress suite); also runnable directly:
;;;
;;;   ./build/host/amilisp --non-interactive --load tests/tls-loopback.lisp
;;;
;;; Skips cleanly (still printing the ALL-PASSED marker) when no TLS
;;; provider is installed, so the suite stays green on hosts without
;;; OpenSSL — EXT:TLS-AVAILABLE-P is the honest gate.

(defvar *passes* 0)
(defvar *fails* 0)

(defun check (name ok)
  (if ok
      (progn (incf *passes*) (format t "ok    ~a~%" name))
      (progn (incf *fails*) (format t "FAIL  ~a~%" name))))

(defvar *cert* "tests/data/tls-test-cert.pem")
(defvar *key*  "tests/data/tls-test-key.pem")

(if (not (ext:tls-available-p))
    (progn
      (format t "TLS provider not available -- skipping TLS loopback tests~%")
      (format t "TLS-LOOPBACK ALL-PASSED~%"))
    (progn
      (format t "TLS provider: ~a~%" (ext:tls-version))

      ;; -- Server: accept one connection, upgrade, echo lines/bytes ------
      (let* ((listener (ext:socket-listen 0 t))
             (port (ext:socket-local-port listener))
             (server
               (mp:make-thread
                (lambda ()
                  (let ((conn (ext:socket-accept listener)))
                    (ext:socket-start-tls conn :server t
                                               :certificate *cert*
                                               :key *key*)
                    ;; Echo one line back, upcased.
                    (let ((line (read-line conn)))
                      (write-line (string-upcase line) conn)
                      (finish-output conn))
                    ;; Echo 256 bytes back doubled mod 256.
                    (let ((buf (make-array 256 :element-type '(unsigned-byte 8))))
                      (read-sequence buf conn)
                      (dotimes (i 256)
                        (setf (aref buf i) (mod (* 2 (aref buf i)) 256)))
                      (write-sequence buf conn)
                      (finish-output conn))
                    ;; Hold the connection briefly so the client can probe
                    ;; LISTEN with no data pending, then close (EOF test).
                    (sleep 0.3)
                    (close conn)))
                :name "tls-server")))

        ;; -- Client ------------------------------------------------------
        (let ((c (ext:open-tcp-stream "127.0.0.1" port 5)))
          (check "connect" (streamp c))
          (check "not yet TLS" (not (ext:socket-tls-p c)))

          ;; Verified handshake against the self-signed cert: trust it via
          ;; :ca-file and verify :hostname localhost.
          (ext:socket-start-tls c :hostname "localhost"
                                  :verify t
                                  :ca-file *cert*
                                  :timeout 10)
          (check "TLS active" (ext:socket-tls-p c))

          ;; Character round-trip through the encrypted stream.
          (write-line "hello over tls" c)
          (finish-output c)
          (check "line echo" (string= (read-line c) "HELLO OVER TLS"))

          ;; Peer certificate plist.
          (let ((cert (ext:tls-peer-certificate c)))
            (check "peer cert subject"
                   (and cert (search "CN=localhost"
                                     (getf cert :subject ""))))
            (check "peer cert issuer"
                   (and cert (search "CL-Amiga TLS test"
                                     (getf cert :issuer "")))))

          ;; Binary round-trip.
          (let ((out (make-array 256 :element-type '(unsigned-byte 8)))
                (in  (make-array 256 :element-type '(unsigned-byte 8))))
            (dotimes (i 256) (setf (aref out i) i))
            (write-sequence out c)
            (finish-output c)
            (read-sequence in c)
            (check "binary echo"
                   (let ((ok t))
                     (dotimes (i 256 ok)
                       (unless (= (aref in i) (mod (* 2 i) 256))
                         (setq ok nil))))))

          ;; LISTEN on an idle TLS stream: no data buffered, must be NIL
          ;; (and must not block or steal bytes).
          (check "listen idle" (not (listen c)))

          ;; EOF after the server closes: READ-CHAR blocks until the
          ;; close_notify arrives and then returns the eof value; LISTEN at
          ;; EOF answers NIL (CLHS: "at end of file, LISTEN returns false").
          (check "eof" (eq (read-char c nil :eof) :eof))
          (check "listen at eof" (not (listen c)))
          (close c))

        (mp:join-thread server))

      ;; -- Verification failure: self-signed cert without a trust anchor
      ;; must be rejected by the client handshake. ------------------------
      (let* ((listener (ext:socket-listen 0 t))
             (port (ext:socket-local-port listener))
             (server
               (mp:make-thread
                (lambda ()
                  (let ((conn (ext:socket-accept listener)))
                    ;; The client aborts mid-handshake; both outcomes
                    ;; (error signalled or clean return) are fine.
                    (ignore-errors
                      (ext:socket-start-tls conn :server t
                                                 :certificate *cert*
                                                 :key *key*))
                    (ignore-errors (close conn))))
                :name "tls-badserver")))
        (let ((c (ext:open-tcp-stream "127.0.0.1" port 5)))
          (check "verify rejects self-signed"
                 (handler-case
                     (progn (ext:socket-start-tls c :hostname "localhost"
                                                    :verify t
                                                    :timeout 10)
                            nil)
                   (error (e)
                     (search "certificate" (format nil "~a" e)))))
          (ignore-errors (close c)))
        (mp:join-thread server)
        (close listener))

      ;; -- Error surface: TLS on a non-socket must signal cleanly. -------
      (check "reject non-socket"
             (handler-case
                 (progn (ext:socket-start-tls *standard-output*) nil)
               (error () t)))

      (format t "~%TLS loopback: ~d passed, ~d failed~%" *passes* *fails*)
      (if (zerop *fails*)
          (format t "TLS-LOOPBACK ALL-PASSED~%")
          (format t "TLS-LOOPBACK FAILED~%"))))
