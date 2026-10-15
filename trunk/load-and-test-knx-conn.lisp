;; Load and test knx-conn (KNXnet/IP implementation, system "knx-conn")
;; via ASDF's test-op.
;;
;; knx-conn resolves through ASDF's source registry the same way the other
;; load-and-test-*.lisp scripts do — its .asd is symlinked into ~/common-lisp
;; on the host (and its two AmiLisp-relevant deps, usocket and sento, resolve
;; to the local forks: usocket from quicklisp's local-projects, sento from
;; ~/common-lisp).  Like the sibling scripts this file contains NO quicklisp
;; references: load-libs-ql.lisp sets up the dist + local-projects searchers,
;; ENSURE-QL-LIB fetches the remaining deps from the dist on a cold cache, and
;; ASDF does the rest.
;;
;; The test system's TEST-OP runs (fiveam:run! knx-conn.tests:test-suite).
;; The KNXC-TUNNEL-E2E-TESTS suite is deliberately NOT part of that suite (it
;; talks to a real KNX/IP gateway and is run manually on demand), so it is only
;; compiled here, never executed — no live gateway is required.
;;
;; Usage (host):
;;   ./build/host/amilisp --heap 192M --load trunk/load-and-test-knx-conn.lisp
;;
;; On Amiga: --heap >= 96M and a large stack (e.g. `stack 800000`).

(setq *load-verbose* nil)
(require "asdf")
(load "trunk/load-libs-ql.lisp")

;; babel is a direct knx-conn dependency (utils.lisp uses babel:string-to-octets
;; for the byte-level KNX wire encoding).  It is pure Lisp (alexandria +
;; trivial-features) so it loads fine here; fetch + load it explicitly, mirroring
;; the chipi script, so the load below never trips over a missing transitive dep.
(ensure-ql-lib :babel)
(asdf:load-system :babel)

;; Make knx-conn's remaining quicklisp-managed deps and its test-only deps
;; resolvable from the dist (download-only on a cold cache; ASDF performs the
;; actual compile/load below).  knx-conn itself, usocket and sento resolve to
;; local sources and need no fetch.
(ensure-ql-lib :log4cl)
(ensure-ql-lib :binding-arrows)
(ensure-ql-lib :local-time)
(ensure-ql-lib :knx-conn)
(ensure-ql-lib :fiveam)
(ensure-ql-lib :cl-mock)

;; Load knx-conn (and its sento + log4cl deps) before running the tests.
;;
;; On a WARM FASL cache, sento is loaded from FASL rather than recompiled, so
;; log4cl's logger objects — embedded as literal constants in the compiled
;; (log:debug ...) statements — are reconstructed via log4cl's MAKE-LOAD-FORM
;; method (it returns (%get-logger ...), which re-registers the logger in the
;; live tree).  amilisp's FASL writer now honors MAKE-LOAD-FORM (CLHS 7.6), so
;; the parent (SENTO) logger is registered on load and the tests'
;; (log:config '(sento) :warn) succeeds without any pre-creation workaround.
(asdf:load-system :knx-conn)

(format t "~%--- Running (asdf:test-system :knx-conn) ---~%")
(asdf:test-system :knx-conn)
