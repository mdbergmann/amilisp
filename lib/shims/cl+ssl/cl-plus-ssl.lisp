;;;; cl-plus-ssl.lisp --- cl+ssl-compatible facade over AmiLisp native TLS.
;;;
;;; Implements the slice of the CL+SSL API that drakma, hunchentoot and
;;; websocket-driver's client actually use (see cl+ssl.asd for the
;;; background) on top of the runtime's EXT:SOCKET-START-TLS.  Key
;;; behavioural differences from the real cl+ssl, all invisible to those
;;; clients:
;;;
;;;  - TLS upgrades the socket stream IN PLACE: MAKE-SSL-CLIENT-STREAM /
;;;    MAKE-SSL-SERVER-STREAM return the same stream object they were given
;;;    (the real cl+ssl wraps it in a Gray ssl-stream).  Wrappers such as
;;;    chunga and flexi-streams sit on top either way.
;;;  - Certificate verification (chain + hostname) happens during the
;;;    handshake in C; a failure signals CL+SSL:SSL-ERROR from the
;;;    MAKE-SSL-*-STREAM call, which is also where the real cl+ssl signals.
;;;  - SSL-STREAM-X509-CERTIFICATE returns a plist
;;;    (:subject ... :issuer ... :not-before ... :not-after ...) rather than
;;;    a foreign X509 pointer.  Hunchentoot passes the value through to user
;;;    code untouched.

(defpackage :cl+ssl
  (:use :cl)
  (:export #:+ssl-verify-none+
           #:+ssl-verify-peer+
           #:*make-ssl-client-stream-verify-default*
           #:ensure-initialized
           #:ssl-check-verify-p
           #:ssl-error
           #:ssl-error-message
           #:make-context
           #:with-global-context
           #:make-ssl-client-stream
           #:make-ssl-server-stream
           #:ssl-stream-x509-certificate))

(in-package :cl+ssl)

(defconstant +ssl-verify-none+ 0)
(defconstant +ssl-verify-peer+ 1)

(defvar *make-ssl-client-stream-verify-default* :required
  "Default for MAKE-SSL-CLIENT-STREAM's :VERIFY (NIL, :OPTIONAL or :REQUIRED).")

(define-condition ssl-error (error)
  ((message :initarg :message :initform "" :reader ssl-error-message))
  (:report (lambda (condition stream)
             (format stream "SSL error: ~a" (ssl-error-message condition)))))

;;; Initialisation.  The real cl+ssl loads libssl/libcrypto and seeds the
;;; RNG here; the native runtime brings TLS up by itself, so this exists for
;;; callers that insist on it (websocket-driver's client calls it before
;;; every wss:// connect).
(defun ensure-initialized (&key method rand-seed)
  "cl+ssl compatibility: TLS needs no explicit initialisation on AmiLisp.
METHOD and RAND-SEED are accepted and ignored."
  (declare (ignore method rand-seed))
  t)

;;; The deprecated global verification switch.  cl+ssl keeps it around as the
;;; fallback for MAKE-SSL-CLIENT-STREAM's :VERIFY: once set, an unspecified
;;; :VERIFY means :OPTIONAL; never set, *MAKE-SSL-CLIENT-STREAM-VERIFY-DEFAULT*
;;; decides.  Same three-state variable and accessor pair as the original.
(defvar *ssl-check-verify-p* :unspecified
  ":UNSPECIFIED until (SETF SSL-CHECK-VERIFY-P) is called, then T or NIL.")

(defun ssl-check-verify-p ()
  "True when the deprecated global peer-verification switch has been turned
on with (SETF SSL-CHECK-VERIFY-P)."
  (and *ssl-check-verify-p*
       (not (eq *ssl-check-verify-p* :unspecified))))

(defun (setf ssl-check-verify-p) (check-verify-p)
  (setf *ssl-check-verify-p* (not (null check-verify-p))))

(defun %client-verify-default ()
  "MAKE-SSL-CLIENT-STREAM's :VERIFY when the caller passes none: :OPTIONAL
once the deprecated switch is on, else *MAKE-SSL-CLIENT-STREAM-VERIFY-DEFAULT*
-- the same rule the original cl+ssl applies."
  (if (ssl-check-verify-p)
      :optional
      *make-ssl-client-stream-verify-default*))

;;; Contexts.  The real cl+ssl wraps a foreign SSL_CTX; here a context is a
;;; plain bag of options that MAKE-SSL-*-STREAM folds into the native
;;; handshake call, so there is nothing foreign to free and :AUTO-FREE-P is
;;; honoured trivially.
(defstruct (ssl-context (:constructor %make-ssl-context))
  ca-file
  ca-path
  verify-mode
  cert-file
  key-file
  key-password)

(defvar *ssl-global-context* nil)

(defun %directory-location-p (loc)
  "True when LOC names an existing directory (AmiLisp's PROBE-FILE returns
directory truenames with a trailing slash)."
  (let ((probed (ignore-errors (probe-file loc))))
    (and probed
         (let ((name (namestring probed)))
           (and (plusp (length name))
                (char= (char name (1- (length name))) #\/))))))

(defun %parse-verify-location (location)
  "Split cl+ssl's :VERIFY-LOCATION designator -- :DEFAULT, a file, a
directory, or a list of files/directories -- into (values ca-file ca-path).
:DEFAULT means the TLS provider's default trust store (NIL/NIL)."
  (let ((ca-file nil)
        (ca-path nil))
    (dolist (loc (cond ((eq location :default) '())
                       ((listp location) location)
                       (t (list location))))
      (let ((name (namestring loc)))
        (if (%directory-location-p name)
            (if ca-path
                (error 'ssl-error :message
                       (format nil "verify-location lists at most one CA directory (got ~a and ~a)"
                               ca-path name))
                (setf ca-path name))
            (if ca-file
                (error 'ssl-error :message
                       (format nil "verify-location lists at most one CA file (got ~a and ~a)"
                               ca-file name))
                (setf ca-file name)))))
    (values ca-file ca-path)))

(defun make-context (&key (verify-location :default)
                          (verify-depth 100)
                          (verify-mode +ssl-verify-peer+)
                          verify-callback
                          certificate-chain-file
                          private-key-file
                          private-key-password
                          &allow-other-keys)
  "cl+ssl-compatible context constructor.  VERIFY-DEPTH is accepted for
compatibility; VERIFY-CALLBACK must be NIL (Lisp verify callbacks cannot run
inside the native handshake)."
  (declare (ignore verify-depth))
  (when verify-callback
    (error 'ssl-error :message
           "custom :verify-callback functions are not supported by the AmiLisp cl+ssl facade"))
  (multiple-value-bind (ca-file ca-path)
      (%parse-verify-location verify-location)
    (%make-ssl-context
     :ca-file ca-file
     :ca-path ca-path
     :verify-mode verify-mode
     :cert-file (and certificate-chain-file (namestring certificate-chain-file))
     :key-file (and private-key-file (namestring private-key-file))
     :key-password private-key-password)))

(defmacro with-global-context ((context &key auto-free-p) &body body)
  "Bind CONTEXT as the global context for the extent of BODY.  There is no
foreign state to release, so :AUTO-FREE-P needs no action."
  (declare (ignore auto-free-p))
  `(let ((*ssl-global-context* ,context))
     ,@body))

(defun %start-tls (socket &rest args)
  "Run EXT:SOCKET-START-TLS, translating failures into SSL-ERROR."
  (handler-case
      (apply #'ext:socket-start-tls socket args)
    (ssl-error (e) (error e))
    (error (e)
      (error 'ssl-error :message (format nil "~a" e)))))

(defun %namestring (x)
  (and x (namestring x)))

(defun make-ssl-client-stream (socket &key certificate key password
                                           (verify (%client-verify-default))
                                           hostname
                                           &allow-other-keys)
  "Upgrade the connected socket stream SOCKET to client-side TLS and return
it.  VERIFY is NIL, :OPTIONAL or :REQUIRED (cl+ssl semantics; the default
follows the deprecated SSL-CHECK-VERIFY-P switch exactly like the original);
with a non-NIL VERIFY the peer chain is checked against the global context's
verify-location (or the provider's default trust store), plus HOSTNAME when
given.  CERTIFICATE/KEY/PASSWORD configure an optional client certificate."
  (check-type verify (member nil :optional :required))
  (let ((context *ssl-global-context*))
    (%start-tls socket
                :server nil
                :hostname hostname
                :verify (and verify t)
                :certificate (or (%namestring certificate)
                                 (and context (ssl-context-cert-file context)))
                :key (or (%namestring key)
                         (and context (ssl-context-key-file context)))
                :key-password (or password
                                  (and context (ssl-context-key-password context)))
                :ca-file (and context (ssl-context-ca-file context))
                :ca-path (and context (ssl-context-ca-path context)))))

(defun make-ssl-server-stream (socket &key certificate key password
                                           &allow-other-keys)
  "Upgrade the connected socket stream SOCKET to server-side TLS and return
it.  CERTIFICATE (PEM, may contain the chain) is required; KEY defaults to
CERTIFICATE; PASSWORD unlocks an encrypted key."
  (%start-tls socket
              :server t
              :certificate (%namestring certificate)
              :key (%namestring key)
              :key-password password))

(defun ssl-stream-x509-certificate (ssl-stream)
  "Peer certificate of a TLS stream as a plist (:subject :issuer
:not-before :not-after), or NIL when the peer presented none."
  (ext:tls-peer-certificate ssl-stream))
