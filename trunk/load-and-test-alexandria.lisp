;; Load and test Alexandria.
;; Works on both host and Amiga.
;;
;; Usage:
;;   ./build/host/amilisp --heap 24M --load trunk/load-and-test-alexandria.lisp

(require "asdf")

;; Make the quicklisp-managed dependencies resolvable through ASDF.
;; All quicklisp handling lives in this helper; this script only uses
;; asdf:load-system.
(load "trunk/load-libs-ql.lisp")

(format t "~%--- Loading Alexandria ---~%")
(ensure-ql-lib :alexandria)      ; fetch from dist only if not in local repo
(asdf:load-system :alexandria)

(format t "~%--- Running Alexandria tests ---~%")
;; ASDF's TEST-OP for alexandria runs the MIT-RT suite and, by design, SIGNALS
;; an error when any test fails (so `asdf:test-system` is non-zero in CI).  We
;; still want a machine-readable pass/fail tally afterwards either way, so the
;; error is caught here and the counts are printed from the RT registry below.
(handler-case
    (asdf:test-system :alexandria)
  (error (e)
    (format t "~&NOTE: alexandria test-op signaled (expected when tests fail): ~A~%" e)))

;; Emit a summary the trunk/run-load-and-test-all.sh aggregator recognizes
;; (the `passed:` / `failed:` "rt" format, same as load-and-test-ansi.lisp).
;; Alexandria's tests use the MIT regression tester (package REGRESSION-TEST,
;; nicknames RTEST/RT): every test is an entry in *ENTRIES*, and PENDING-TESTS
;; returns the ones that did not pass.  Looked up via FIND-SYMBOL so this form
;; carries no read-time dependency on the package existing.
(let* ((rt (find-package "REGRESSION-TEST"))
       (entries-sym (and rt (find-symbol "*ENTRIES*" rt)))
       (pending-sym (and rt (find-symbol "PENDING-TESTS" rt)))
       (total (if (and entries-sym (boundp entries-sym))
                  (length (cdr (symbol-value entries-sym)))
                  0))
       (failed (if (and pending-sym (fboundp pending-sym))
                   (length (funcall pending-sym))
                   0))
       (passed (- total failed)))
  (format t "~%=== Alexandria Summary ===~%")
  (format t "passed: ~A~%" passed)
  (format t "failed: ~A~%" failed))
