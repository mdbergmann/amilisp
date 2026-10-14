;; Load and test fiveam.
;; Works on both host and Amiga.
;;
;; Usage:
;;   ./build/host/amilisp --heap 24M --load trunk/load-and-test-5am.lisp

(require "asdf")

;; Make the quicklisp-managed dependencies resolvable through ASDF.
;; All quicklisp handling lives in this helper; this script only uses
;; asdf:load-system.
(load "trunk/load-libs-ql.lisp")

(format t "~%--- Loading fiveam ---~%")
(ensure-ql-lib :fiveam)          ; fetch from dist only if not in local repo
(asdf:load-system :fiveam)

(format t "~%--- Running fiveam self-tests ---~%")
(asdf:test-system :fiveam)
