;; Load and test chipi (home-automation lib, system "chipi") via ASDF.
;;
;; chipi resolves through ASDF's source registry the same way it does
;; under quicklisp — on the host its .asd is symlinked into ~/common-lisp,
;; so no central-registry push or path hard-coding is needed here.  Like
;; the sibling load-and-test-*.lisp scripts this file contains NO quicklisp
;; references: load-libs-ql.lisp sets up the dist + local-projects searchers
;; (and the :drakma-no-ssl feature cl-amiga needs), ENSURE-QL-LIB fetches
;; each system from the dist on a cold cache, and ASDF does the rest.
;;
;; Usage (host):
;;   ./build/host/amilisp --heap 256M --load trunk/load-and-test-chipi.lisp
;;
;; On Amiga: --heap >= 96M and a large stack (e.g. `stack 800000`).

(setq *load-verbose* nil)
(require "asdf")
(load "trunk/load-libs-ql.lisp")

;; babel is normally pulled in transitively via the cffi chain (cl+ssl);
;; since cl-amiga skips SSL it must be fetched + loaded explicitly.  It is
;; pure Lisp (alexandria + trivial-features) so it loads fine here.
;; chipi's persistence-influx.lisp uses babel:octets-to-string.
(ensure-ql-lib :babel)
(asdf:load-system :babel)

;; Make chipi and its test-only deps resolvable from the quicklisp dist
;; (download-only; ASDF performs the actual load/compile below).
(ensure-ql-lib :chipi)
(ensure-ql-lib :fiveam)
(ensure-ql-lib :cl-mock)

;; chipi's INFLUX-PERSISTENCE-TESTS are integration tests that require a live
;; InfluxDB instance — they hit a hard-coded host (see
;; cl-hab/test/persistence-influx-test.lisp).  When that host is unreachable
;; every test blocks on its AWAIT-COND timeout (and, with no socket connect
;; timeout, can stall far longer), so the suite is unsuitable for an
;; unattended run.  Load the test system, drop that one suite from the tree,
;; then run the remainder.
(asdf:load-system :chipi/tests)

;; A fiveam suite stores its children in BOTH an ordered name list and a
;; hash table (the run loop walks the name list).  fiveam::rem-test clears
;; both, but operates on the *test* bundle special — so bind it to the parent
;; suite's bundle before removing the influx child.
(let ((parent (find-symbol "TEST-SUITE" "CHIPI.TESTS"))
      (influx (find-symbol "INFLUX-PERSISTENCE-TESTS"
                           "CHIPI.INFLUX-PERSISTENCE-TEST")))
  (if (and parent influx (fiveam:get-test parent))
      (let ((fiveam::*test* (fiveam::tests (fiveam:get-test parent))))
        (fiveam::rem-test influx)
        (format t "~&;; Skipping INFLUX-PERSISTENCE-TESTS (needs a live InfluxDB)~%"))
      (format t "~&;; WARNING: could not locate influx suite to skip~%")))

(format t "~%--- (fiveam:run! chipi.tests:test-suite) ---~%")
(fiveam:run! (find-symbol "TEST-SUITE" "CHIPI.TESTS"))
