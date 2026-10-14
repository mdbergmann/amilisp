;; Load and test chipi-api (web API for chipi, system "chipi-api") via ASDF.
;;
;; Like the sibling load-and-test-*.lisp scripts this file contains NO
;; quicklisp references: load-libs-ql.lisp sets up the dist + local-projects
;; searchers (and the :drakma-no-ssl feature cl-amiga needs), ENSURE-QL-LIB
;; fetches each system from the dist on a cold cache, and ASDF does the rest.
;;
;; Usage (host):
;;   ./build/host/amilisp --heap 256M --load trunk/load-and-test-chipi-api.lisp
;;
;; On Amiga: --heap >= 96M and a large stack (e.g. `stack 800000`).

(setq *load-verbose* nil)
(require "asdf")
(load "trunk/load-libs-ql.lisp")

;; babel is normally pulled in transitively via the cffi chain (cl+ssl);
;; since cl-amiga skips SSL it must be fetched + loaded explicitly.  It is
;; pure Lisp (alexandria + trivial-features) so it loads fine here.
(ensure-ql-lib :babel)
(asdf:load-system :babel)

;; Make chipi-api and its test-only deps resolvable from the quicklisp dist
;; (download-only; ASDF performs the actual load/compile below).
(ensure-ql-lib :chipi)
(ensure-ql-lib :ironclad)
(ensure-ql-lib :cl-base64)
(ensure-ql-lib :marshal)
(ensure-ql-lib :snooze)
(ensure-ql-lib :fiveam)
(ensure-ql-lib :cl-mock)
(ensure-ql-lib :split-sequence)

(asdf:load-system :chipi-api/tests)

;; chipi-api's integration tests (API-INTEGTESTS) start a real Hunchentoot
;; easy-acceptor and drive it over the loopback with drakma.  Hunchentoot's
;; per-connection SET-TIMEOUTS (and a couple of cl-fad/rfc2388 primitives)
;; have per-Lisp conditionals — these now carry in-source #+cl-amiga branches
;; in the respective forks under quicklisp local-projects, so no separate
;; patch load is needed here.

(format t "~%--- (fiveam:run! chipi-api.tests:test-suite) ---~%")
(fiveam:run! (find-symbol "TEST-SUITE" "CHIPI-API.TESTS"))
