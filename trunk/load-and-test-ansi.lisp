;; load-and-test-ansi.lisp
;;
;; Bootstrap of the ANSI Common Lisp test suite (Paul Dietz / ansi-test).
;; Loads the rt framework + aux + universe + the CONS, SYMBOLS, NUMBERS and
;; SEQUENCES chapters and the DEFVAR / DEFPARAMETER tests, then runs them all
;; in a single rt:do-tests pass.
;;
;; Usage (host):
;;   ./build/host/amilisp --heap 96M --load trunk/load-and-test-ansi.lisp
;;
;; Notes
;; - Each load is its own top-level form: (in-package :cl-test) needs to
;;   execute *between* loads, not be batched with them under one let body.
;; - Paths are passed absolute to the ansi-test loaders so compile-and-load's
;;   internal merge against *load-pathname* (= this script under trunk/) doesn't
;;   mis-resolve into ansi-test/trunk/foo.lsp.

(defparameter *ansi-test-dir* (truename "third_party/ansi-test/"))
(defparameter *aux-dir* (truename "third_party/ansi-test/auxiliary/"))
(setq *default-pathname-defaults* *ansi-test-dir*)

(format t "~%=== ANSI test bootstrap (rt + aux + universe + cons + symbols + numbers + sequences + defvar/defparameter) ===~%")
(format t "ansi-test dir: ~A~%" *ansi-test-dir*)

;; --- Bootstrap (still in CL-USER) ---
(load (merge-pathnames "compile-and-load.lsp" *ansi-test-dir*))
(load (merge-pathnames "rt-package.lsp" *ansi-test-dir*))
(compile-and-load (merge-pathnames "rt.lsp" *ansi-test-dir*))
(load (merge-pathnames "cl-test-package.lsp" *ansi-test-dir*))

;; ansi-aux-macros.lsp and ansi-aux.lsp have no (in-package :cl-test) at
;; their top — they assume the caller is already in :cl-test (gclload1.lsp
;; switches before loading them).  Compiling them in CL-USER causes the
;; amilisp compiler to runaway (observed: 17 GB host memory, 100% CPU,
;; indefinite hang).  Switch package first.
(in-package :cl-test)

(common-lisp-user::compile-and-load
 (common-lisp:merge-pathnames "ansi-aux-macros.lsp"
                              (common-lisp:symbol-value
                               'common-lisp-user::*aux-dir*)))

;; universe.lsp must come BEFORE ansi-aux.lsp (per gclload1.lsp ordering) —
;; ansi-aux.lsp consumes *universe* / *mini-universe* / *condition-types* etc.
;; Loads in <1s once nconc-on-self-aliasing, copy-tree GC root recursion, and
;; bit-vector :displaced-to are working in amilisp.
(common-lisp:format common-lisp:t "~%--- Loading universe.lsp ---~%")
(common-lisp:load
 (common-lisp:merge-pathnames "universe.lsp"
                              (common-lisp:symbol-value
                               'common-lisp-user::*ansi-test-dir*)))

(common-lisp-user::compile-and-load
 (common-lisp:merge-pathnames "ansi-aux.lsp"
                              (common-lisp:symbol-value
                               'common-lisp-user::*aux-dir*)))

;; random-aux.lsp defines RANDOM-CASE / random list builders used by
;; cons-aux's RANDOM-SET-EXCLUSIVE-OR-TEST and friends.  Not loaded by
;; ansi-aux itself, so we load it here.
(common-lisp-user::compile-and-load
 (common-lisp:merge-pathnames "random-aux.lsp"
                              (common-lisp:symbol-value
                               'common-lisp-user::*aux-dir*)))

;; cl-symbol-names.lsp defines *cl-symbol-names*, *cl-variable-symbols*,
;; *cl-constant-symbols*, *cl-non-variable-constant-symbols* etc.  Several
;; symbol-chapter tests (BOUNDP.5, CL-CONSTANT-SYMBOLS.1, KEYWORD-BEHAVIOR,
;; SYMBOL-NAME.3 via the test fixtures, etc.) loop over these and fail with
;; UNBOUND-VARIABLE without it.  Loaded directly (not compile-and-load) per
;; gclload1.lsp's pattern.
(common-lisp:format common-lisp:t "~%--- Loading cl-symbol-names.lsp ---~%")
(common-lisp:load
 (common-lisp:merge-pathnames "cl-symbol-names.lsp"
                              (common-lisp:symbol-value
                               'common-lisp-user::*ansi-test-dir*)))

;; --- Load cons chapter (paths relative to ansi-test root) ---
(common-lisp:format common-lisp:t "~%--- Loading cons chapter ---~%")
(common-lisp:load
 (common-lisp:merge-pathnames "cons/load.lsp"
                              (common-lisp:symbol-value
                               'common-lisp-user::*ansi-test-dir*)))

;; --- Load symbols chapter ---
(common-lisp:format common-lisp:t "~%--- Loading symbols chapter ---~%")
(common-lisp:load
 (common-lisp:merge-pathnames "symbols/load.lsp"
                              (common-lisp:symbol-value
                               'common-lisp-user::*ansi-test-dir*)))

;; --- Load numbers chapter ---
(common-lisp:format common-lisp:t "~%--- Loading numbers chapter ---~%")
(common-lisp:load
 (common-lisp:merge-pathnames "numbers/load.lsp"
                              (common-lisp:symbol-value
                               'common-lisp-user::*ansi-test-dir*)))

;; --- Load sequences chapter ---
(common-lisp:format common-lisp:t "~%--- Loading sequences chapter ---~%")
(common-lisp:load
 (common-lisp:merge-pathnames "sequences/load.lsp"
                              (common-lisp:symbol-value
                               'common-lisp-user::*ansi-test-dir*)))

;; --- DEFVAR / DEFPARAMETER (two files of the data-and-control-flow chapter,
;; not the whole chapter).  defvar.5 is the CLHS rule that DEFVAR evaluates
;; its initial-value form only when the variable is unbound. ---
(common-lisp:format common-lisp:t "~%--- Loading DEFVAR / DEFPARAMETER tests ---~%")
(common-lisp:load
 (common-lisp:merge-pathnames "data-and-control-flow/defvar.lsp"
                              (common-lisp:symbol-value
                               'common-lisp-user::*ansi-test-dir*)))
(common-lisp:load
 (common-lisp:merge-pathnames "data-and-control-flow/defparameter.lsp"
                              (common-lisp:symbol-value
                               'common-lisp-user::*ansi-test-dir*)))

;; --- Run the tests via rt:do-tests (inherited into :cl-test) ---
(format t "~%--- Running tests via rt:do-tests ---~%")
(do-tests)
(format t "~%=== Summary ===~%")
(format t "passed: ~A~%" (length regression-test::*passed-tests*))
(format t "failed: ~A~%" (length regression-test::*failed-tests*))
(when regression-test::*failed-tests*
  (format t "~%--- Failed tests ---~%")
  (dolist (n regression-test::*failed-tests*)
    (format t "  ~A~%" n)))
