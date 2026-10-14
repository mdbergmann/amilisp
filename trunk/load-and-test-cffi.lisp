;; Load and test CFFI (the Common Foreign Function Interface) on cl-amiga.
;;
;; cl-amiga ships a CFFI-SYS backend (cffi-clamiga.lisp) built on the FFI
;; package's libffi-based engine (host: dlopen/dlsym + ffi:call-foreign +
;; ffi:make-callback).  This script loads CFFI and exercises the high-level
;; API with a fiveam suite.
;;
;; NB: the *upstream* cffi-tests system is NOT run here — it depends on
;; cffi-grovel, cffi-libffi (struct-by-value), bordeaux-threads and rt, and
;; builds C test libraries via cffi-toolchain, none of which are available on
;; cl-amiga yet.  This suite drives the same public API against libc/libSystem
;; symbols instead, so it runs unattended on the host.  The foreign-call paths
;; are host-only (POSIX dlopen + libffi); on AmigaOS ffi:symbol-pointer returns
;; NIL and the call/callback tests are skipped.
;;
;; Usage (host):
;;   ./build/host/amilisp --heap 256M --load trunk/load-and-test-cffi.lisp

(setq *load-verbose* nil)
(require "asdf")
(load "trunk/load-libs-ql.lisp")

;; Resolve CFFI + its dependencies (local-projects: cffi, trivial-features;
;; dist: alexandria, babel) and the fiveam test framework.
(dolist (sys '(:trivial-features :alexandria :babel :cffi :fiveam))
  (ensure-ql-lib sys))

(format t "~%--- Loading CFFI ---~%")
(asdf:load-system :cffi)
(asdf:load-system :fiveam)

;; The test definitions below reference the CFFI and FIVEAM packages, which
;; only exist after the loads above — they are read now (a later top-level
;; form is read after the earlier ones have been evaluated).
(load "trunk/cffi-clamiga-tests.lisp")

;; Run the suite; fiveam prints "Pass: N (...) / Fail: N (...)" which the
;; trunk/run-load-and-test-all.sh tally recognizes.
(format t "~%--- Running CFFI amilisp test suite ---~%")
(funcall (find-symbol "RUN-CFFI-CLAMIGA-TESTS" "CFFI-CLAMIGA-TEST"))
