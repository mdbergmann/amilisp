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

;; The test-op loads trivia.test, whose own dist dependencies (optima,
;; trivia.quasiquote -> fare-quasiquote-readtable, ...) are not reachable
;; from trivia's dependency list: fetch them before ASDF needs them.
(ensure-ql-lib :trivia.test)

(format t "~%--- Running Trivia tests ---~%")
(handler-case
    (asdf:test-system :trivia)
  (error (e)
    (format t "~&NOTE: trivia test-op signaled: ~A~%" e)))

(format t "~%=== Trivia done ===~%")
