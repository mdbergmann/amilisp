;;;; swank.lisp --- Minimal SWANK package shim for AmiLisp.
;;;
;;; Provides the small slice of the SWANK API that quicklisp libraries
;;; reference at read/compile time (chiefly clack's optional remote-debug
;;; server hook).  AmiLisp has no portable SWANK backend, so the stubs
;;; signal a clear error if they are ever actually called.  In practice
;;; clack only calls them when its :swank-port argument is non-NIL, which
;;; defaults to NIL — so loading clog/clack never reaches them.

(defpackage :swank
  (:use :cl)
  (:nicknames :swank-shim)
  (:export #:create-server
           #:stop-server
           #:*communication-style*))

(in-package :swank)

(defvar *communication-style* nil
  "Stub: AmiLisp uses a native SLY/SLYNK transport, not the SWANK backend.")

(defun create-server (&rest args)
  "Stub for SWANK:CREATE-SERVER.  AmiLisp ships no portable SWANK backend;
the real remote-debug server is started through AmiLisp's native SLY/SLYNK
transport instead.  Signalled only if a caller explicitly requests a swank
server (e.g. clack with a non-NIL :swank-port)."
  (declare (ignore args))
  (error "SWANK:CREATE-SERVER is not available on AmiLisp ~
          (no portable SWANK backend; use the native SLY/SLYNK transport)."))

(defun stop-server (&rest args)
  "Stub for SWANK:STOP-SERVER.  See SWANK:CREATE-SERVER."
  (declare (ignore args))
  (error "SWANK:STOP-SERVER is not available on AmiLisp ~
          (no portable SWANK backend; use the native SLY/SLYNK transport)."))
