;; Load and test Trivia.
;; Works on both host and Amiga.
;;
;; Usage:
;;   ./build/host/amilisp --heap 24M --load trunk/load-and-test-trivia.lisp

(require "asdf")

(load "trunk/load-libs-ql.lisp")

(format t "~%--- Loading Trivia ---~%")
(ensure-ql-lib :trivia)
(asdf:load-system :trivia)

(format t "~%--- Running Trivia tests ---~%")
(handler-case
    (asdf:test-system :trivia)
  (error (e)
    (format t "~&NOTE: trivia test-op signaled: ~A~%" e)))

(format t "~%=== Trivia done ===~%")
