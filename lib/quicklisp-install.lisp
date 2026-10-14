;;; quicklisp-install.lisp — tiny wrapper around lib/quicklisp.lisp
;;;
;;; Just enough to install quicklisp on a fresh AmiLisp system. After
;;; this finishes, use quicklisp the normal way:
;;;
;;;   (load #P"~/quicklisp/setup.lisp")
;;;   (load "lib/quicklisp-compat.lisp")
;;;
;;; Usage on a fresh system (where ~/quicklisp/ does not yet exist):
;;;
;;;   (require :asdf)
;;;   (load "lib/quicklisp-install.lisp")
;;;   (amilisp-ql:install)
;;;
;;; Mechanism: QUICKLISP-QUICKSTART:INSTALL downloads the QL client
;;; into ~/quicklisp/ and finishes by calling (QUICKLISP:SETUP), which
;;; tries to fetch the default dist over the network and fails because
;;; AmiLisp isn't a registered ql-impl. We catch that error, load
;;; lib/quicklisp-compat.lisp (its IN-PACKAGE forms now resolve, since
;;; the ql-* packages were loaded by ASDF before the failure), and
;;; retry (QUICKLISP:SETUP) — which is idempotent and proceeds with
;;; networking patched in.

(defpackage #:amilisp-ql
  (:use #:cl)
  (:export #:install))

(in-package #:amilisp-ql)

(defvar *here*
  (make-pathname :name nil :type nil
                 :defaults (or *load-truename* *load-pathname*)))

(defun maybe-use-http-proxy ()
  "Route quicklisp's fetches through an HTTP proxy when one is set in the
environment (HTTP_PROXY / http_proxy). Quicklisp opens a DIRECT TCP socket and
ignores the proxy env vars on its own, so in a network-isolated sandbox (e.g. the
ai-pipeline egress proxy, the only route out) the install would otherwise hang on
a routeless connection. No-op when unset, so the normal Amiga path is unchanged.
Must run AFTER quicklisp.lisp is loaded -- that's what defines the QL-HTTP package."
  (let ((proxy (or (ext:getenv "HTTP_PROXY") (ext:getenv "http_proxy")))
        (proxy-var (find-symbol "*PROXY-URL*" "QL-HTTP")))
    (when (and proxy proxy-var (plusp (length proxy)))
      (set proxy-var proxy)
      (format t "~&;; amilisp-ql: routing fetches through HTTP proxy ~A~%" proxy))))

(defun install ()
  "Install quicklisp under ~/quicklisp/ on a fresh AmiLisp system."
  (load (merge-pathnames "quicklisp.lisp" *here*))
  (maybe-use-http-proxy)
  (let ((qq-install (find-symbol "INSTALL" "QUICKLISP-QUICKSTART"))
        (compat (merge-pathnames "quicklisp-compat.lisp" *here*)))
    (handler-case
        (funcall qq-install)
      (error (c)
        (let ((ql (find-package "QUICKLISP"))
              (ql-dist (find-package "QL-DIST")))
          (unless (and ql ql-dist)
            (error c))
          (format t "~&;; amilisp-ql: ~A~%~
                     ;;             loading compat shim + retrying dist install~%"
                  c)
          (load compat)
          ;; The failed setup left ~/quicklisp/dists/quicklisp/
          ;; as an empty directory, so QUICKLISP:SETUP's
          ;; idempotency check (DISTS-INITIALIZED-P) now thinks
          ;; the dist is already on disk and skips
          ;; MAYBE-INITIAL-SETUP. Install the dist ourselves with
          ;; :REPLACE T to overwrite the stub, then run SETUP
          ;; again to finish the rest of the init.
          (funcall (find-symbol "INSTALL-DIST" ql-dist)
                   (symbol-value (find-symbol "*INITIAL-DIST-URL*" ql))
                   :prompt nil :replace t)
          (funcall (find-symbol "SETUP" ql))))))
  (format t "~&;; amilisp-ql: install complete. To use:~%~
             ;;   (load #P\"~~/quicklisp/setup.lisp\")~%~
             ;;   (load \"lib/quicklisp-compat.lisp\")~%")
  t)
