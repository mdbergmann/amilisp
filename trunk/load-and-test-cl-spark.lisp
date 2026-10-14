;; Load and test cl-spark.
;; Works on both host and Amiga.
;;
;; Usage:
;;   ./build/host/amilisp --heap 24M --load trunk/load-and-test-cl-spark.lisp

(require "asdf")

;; Make the quicklisp-managed dependencies resolvable through ASDF.
;; All quicklisp handling lives in this helper; this script only uses
;; asdf:load-system.
(load "trunk/load-libs-ql.lisp")

(format t "~%--- Loading cl-spark-test ---~%")
(ensure-ql-lib :cl-spark)        ; fetch from dist only if not in local repo
(asdf:load-system :cl-spark-test)

(format t "~%--- Running cl-spark tests ---~%")
(let ((ok (cl-spark-test:run-tests)))
  (format t "~%=== cl-spark tests ~A ===~%" (if ok "PASSED" "FAILED")))
