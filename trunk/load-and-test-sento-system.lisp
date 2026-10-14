;; Load and test Sento via asdf:test-system (matches the documented
;; baseline of 511/513 passing — the 2 fails are timing-flaky
;; ACTOR-OF--LIMIT-QUEUE-SIZE cases).
;;
;; Sibling script `load-and-test-sento.lisp` runs suites one-by-one
;; with a custom runner; this one delegates to the system definition
;; and matches the convention of the other `load-and-test-*` scripts.
;;
;; Usage (host, cold cache):
;;   rm -rf ~/.cache/common-lisp/cl-amiga-*
;;   ./build/host/amilisp --heap 192M --load trunk/load-and-test-sento-system.lisp
;;
;; Usage (host, warm cache):
;;   ./build/host/amilisp --heap 128M --load trunk/load-and-test-sento-system.lisp
;;
;; On Amiga: --heap >= 96M and a large stack (e.g. `stack 800000`).

(setq *load-verbose* nil)
(require "asdf")

;; All quicklisp handling lives in this helper; this script only uses
;; asdf. ENSURE-QL-LIB fetches sento's source into the local repo only if
;; it is not there yet (download only, no load) — so the FASL cache state
;; at start is exactly what the caller arranged: asdf:test-system below
;; still compiles sento + its deps from scratch on a cold cache.
(load "trunk/load-libs-ql.lisp")
(ensure-ql-lib :sento)

(format t "~%--- Running (asdf:test-system :sento) ---~%")
(asdf:test-system :sento)
