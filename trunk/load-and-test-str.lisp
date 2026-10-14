;; Load and test str.
;; Works on both host and Amiga.
;;
;; Usage:
;;   ./build/host/amilisp --heap 64M --load trunk/load-and-test-str.lisp

(require "asdf")

;; Make the quicklisp-managed dependencies resolvable through ASDF.
;; All quicklisp handling lives in this helper; this script only uses
;; asdf:load-system.
(load "trunk/load-libs-ql.lisp")

(format t "~%--- Loading str ---~%")
(ensure-ql-lib :str)             ; fetch from dist only if not in local repo
(asdf:load-system :str)

(format t "~%--- Running str tests ---~%")
(asdf:test-system :str)
