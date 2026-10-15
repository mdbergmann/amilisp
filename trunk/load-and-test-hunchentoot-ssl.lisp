;; Load and test Hunchentoot as an HTTPS server — the TLS counterpart to
;; trunk/load-and-test-hunchentoot.lisp.
;;
;; AmiLisp is BOTH ends of every connection here: a Hunchentoot
;; EASY-SSL-ACCEPTOR serves https://localhost over the native TLS layer
;; (EXT:SOCKET-START-TLS — OpenSSL on the host, AmiSSL on AmigaOS), reached
;; through the cl+ssl facade (lib/shims/cl+ssl) that hunchentoot's
;; ssl.lisp and drakma's HTTPS path both consume.  Hunchentoot's own
;; confidence suite then drives drakma as the in-process HTTPS client, so a
;; single run exercises the server handshake, the client handshake, and
;; every request/response feature of the plain-HTTP script — all through
;; encrypted loopback connections against the self-signed certificate in
;; tests/data/ (drakma's default :verify NIL accepts it; the verify=on
;; paths are covered by tests/tls-loopback.lisp and the drakma badssl
;; tests).
;;
;; NOTE: like every trunk script, run it with its own FASL cache
;; (AMILISP_FASL_CACHE_DIR) when alternating with the no-ssl hunchentoot
;; script — the :hunchentoot-no-ssl feature changes what hunchentoot.asd
;; compiles (the "No class named SSL-ACCEPTOR" stale-cache trap).
;;
;; Usage (host):
;;   ./build/host/amilisp --heap 256M --load trunk/load-and-test-hunchentoot-ssl.lisp

(setq *load-verbose* nil)
(require "asdf")

;; Make the quicklisp-managed dependencies resolvable through ASDF.
(load "trunk/load-libs-ql.lisp")

;; TLS on BOTH sides: load-libs-ql pins :drakma-no-ssl for the plain-HTTP
;; scripts — drop it, and make sure :hunchentoot-no-ssl is not in effect,
;; BEFORE any .asd is read so the cl+ssl dependency and ssl.lisp stay in.
(setq *features* (remove :drakma-no-ssl *features*))
(setq *features* (remove :hunchentoot-no-ssl *features*))

(unless (ext:tls-available-p)
  (format t "~%No TLS provider available on this host -- skipping.~%")
  (format t "Did 0 checks.~%  Pass: 0 (100%)~%  Fail: 0 (0%)~%")
  (quit))

(format t "~%TLS provider: ~a~%" (ext:tls-version))

(dolist (sys '(:trivial-features :alexandria :babel :cffi :cl+ssl
               :cl-base64 :chunga :flexi-streams :cl-ppcre
               :md5 :rfc2388 :trivial-backtrace
               :usocket :bordeaux-threads :cl-fad :puri
               :cl-who :drakma :hunchentoot))
  (ensure-ql-lib sys))

(format t "~%--- Loading hunchentoot (WITH ssl over the cl+ssl facade) ---~%")
(asdf:load-system :hunchentoot)

;; Guard against the quicklisp-dist cl+ssl sneaking in: the CFFI-based
;; original would "work" on the host and hide facade regressions.  The
;; facade is recognisable by its internal %START-TLS helper.
(unless (find-symbol "%START-TLS" "CL+SSL")
  (error "the loaded cl+ssl is not the AmiLisp facade: lib/shims/cl+ssl ~
          should have been auto-registered on ASDF:*CENTRAL-REGISTRY* when ~
          ASDF loaded (is AMILISP_NO_SHIMS set?)"))

(format t "~%--- Loading :hunchentoot/test ---~%")
(asdf:load-system :hunchentoot/test)

;; Tally adapter — identical to trunk/load-and-test-hunchentoot.lisp (see
;; the rationale there): count every leaf assertion and the failing ones.
(in-package :hunchentoot-test)

(defvar *amilisp-checks* 0)
(defvar *amilisp-fails* 0)

(let ((orig-assert (fdefinition 'http-assert))
      (orig-header  (fdefinition 'http-assert-header)))
  (flet ((counting (orig)
           (lambda (&rest args)
             (incf *amilisp-checks*)
             (handler-bind ((test-failure
                              (lambda (c)
                                (declare (ignore c))
                                (incf *amilisp-fails*))))
               (apply orig args)))))
    (setf (fdefinition 'http-assert)        (counting orig-assert)
          (fdefinition 'http-assert-header) (counting orig-header))))

(in-package :cl-user)

(defparameter *ht-ssl-test-port* 4443)

(format t "~%--- Starting Hunchentoot easy-ssl-acceptor on localhost:~A ---~%"
        *ht-ssl-test-port*)
(let ((server (hunchentoot:start
               (make-instance 'hunchentoot:easy-ssl-acceptor
                              :port *ht-ssl-test-port*
                              :ssl-certificate-file "tests/data/tls-test-cert.pem"
                              :ssl-privatekey-file "tests/data/tls-test-key.pem"
                              :message-log-destination nil
                              :access-log-destination nil))))
  (unwind-protect
       (progn
         ;; Give the acceptor's taskmaster thread a moment to come up.
         (sleep 2)
         (setf hunchentoot-test::*amilisp-checks* 0
               hunchentoot-test::*amilisp-fails*  0)
         (format t "~%--- Running hunchentoot confidence suite over HTTPS ---~%")
         (handler-case
             (hunchentoot-test:test-hunchentoot
              (format nil "https://localhost:~A" *ht-ssl-test-port*))
           (error (e)
             (format t "~&;; confidence suite aborted with an error: ~A~%" e)))
         (let* ((checks hunchentoot-test::*amilisp-checks*)
                (fails  hunchentoot-test::*amilisp-fails*)
                (pass   (max 0 (- checks fails)))
                (total  (max checks (+ pass fails)))
                (ppct   (if (plusp total) (round (* 100 pass) total) 100))
                (fpct   (if (plusp total) (- 100 ppct) 0)))
           (format t "~2&--- hunchentoot HTTPS confidence suite result ---~%")
           (format t "Did ~D check~:P.~%" total)
           (format t "  Pass: ~D (~D%)~%" pass ppct)
           (format t "  Fail: ~D (~D%)~%" fails fpct)))
    (format t "~&--- Stopping server ---~%")
    (ignore-errors (hunchentoot:stop server))
    (ignore-errors (hunchentoot-test::clean-tmp-dir))))
