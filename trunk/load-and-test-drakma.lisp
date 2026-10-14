;; Load and test drakma — the full-featured HTTP/HTTPS client — WITH TLS.
;;
;; This is the SSL-enabled counterpart to the other trunk scripts.  Unlike
;; chipi (which sets :drakma-no-ssl and stays on plain HTTP), this script loads
;; drakma together with cl+ssl so the HTTPS path is exercised end to end.  It
;; brings three cl-amiga capabilities together:
;;
;;   - the host FFI engine + CFFI backend (cffi-clamiga),
;;   - cl+ssl loading against the host's OpenSSL — enabled by the :darwin /
;;     :linux + arch features cl-amiga now exposes, which let cl+ssl's
;;     define-foreign-library pick the right library names and locations, and
;;   - the usocket cl-amiga backend (usocket/backend/clamiga.lisp) that maps
;;     usocket onto ext:open-tcp-stream / ext:socket-listen / ext:socket-accept.
;;
;; The upstream drakma test-suite (ASDF system :drakma-test, run via fiveam)
;; makes live requests to google.com / httpbin.org / badssl.com, so it needs
;; network access and is HOST-ONLY: the Amiga/FS-UAE target has no TCP/IP stack
;; in the test harness, so there is no Amiga counterpart to this script.
;;
;; SCOPE: cl-amiga runs drakma as an HTTP/HTTPS *client* over the usocket
;; cl-amiga backend + cl+ssl.  The tests that exercise that — plain HTTP and
;; HTTPS, GET and POST, streamed responses, and cl+ssl certificate
;; verification (the badssl.com VERIFY.* tests) — pass reliably.  Tests that
;; instead need a local hunchentoot SERVER, chipz streaming decompression, or
;; the (flaky, rate-limited) httpbin.org service are skipped with documented
;; reasons; see trunk/drakma-skip-tests.lisp.
;;
;; Usage (host):
;;   ./build/host/amilisp --heap 256M --load trunk/load-and-test-drakma.lisp

(setq *load-verbose* nil)
(require "asdf")

;; Make the quicklisp-managed dependencies resolvable through ASDF.
(load "trunk/load-libs-ql.lisp")

;; load-libs-ql.lisp pushes :drakma-no-ssl for the SSL-less scripts.  This is
;; the one script that WANTS SSL, so remove it before drakma's .asd is read —
;; that restores :cl+ssl to drakma's dependency list.
(setq *features* (remove :drakma-no-ssl *features*))

;; Resolve drakma + its transitive and test-only dependencies from the
;; quicklisp dist (download-only; ASDF performs the actual compile/load).
;; babel is normally pulled in via the cffi chain but is listed explicitly so
;; a cold cache fetches it regardless.  hunchentoot/easy-routes/fiveam are the
;; drakma-test deps; routes (cl-routes) is easy-routes' router.
(dolist (sys '(:trivial-features :alexandria :babel :cffi :cl+ssl
               :puri :cl-base64 :chunga :flexi-streams :cl-ppcre :chipz
               :usocket :bordeaux-threads :cl-fad :md5 :rfc2388
               :trivial-backtrace :routes :drakma
               :hunchentoot :easy-routes :fiveam))
  (ensure-ql-lib sys))

(format t "~%--- Loading drakma (with TLS via cl+ssl) ---~%")
(asdf:load-system :drakma)

;; Load the test system (defines drakma's fiveam suite :drakma and pulls in
;; hunchentoot/easy-routes, which the test file references at read time).
(format t "~%--- Loading :drakma-test ---~%")
(asdf:load-system :drakma-test)

;; The form-POST tests run drakma against a LOCAL hunchentoot server over the
;; usocket cl-amiga backend.  The cl-amiga Hunchentoot/cl-fad/rfc2388
;; portability now lives as in-source #+cl-amiga branches in those forks
;; (local-projects), installed when the systems load above.

;; Redefine the out-of-scope tests as skips (see file header for the three
;; groups + reasons).  Loaded as a file so its top (in-package :drakma-test)
;; takes effect before the skip forms are read — DRAKMA-TEST does not exist
;; when THIS script is read.
(load "trunk/drakma-skip-tests.lisp")

;; Run the suite.  fiveam:run! prints "Pass: N (...) / Fail: N (...)" which the
;; trunk/run-load-and-test-all.sh tally recognizes.
(format t "~%--- Running drakma test-suite (HTTP/HTTPS client + cl+ssl; out-of-scope tests skipped) ---~%")
(funcall (find-symbol "RUN!" "FIVEAM") :drakma)
