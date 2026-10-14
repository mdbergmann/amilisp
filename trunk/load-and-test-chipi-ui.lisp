;; Load and test chipi-ui (CLOG web UI for chipi, system "chipi-ui") via ASDF.
;;
;; chipi-ui depends on chipi-api (REST/SSE web API) and clog (websocket GUI),
;; so this script is the union of the chipi-api and clog loaders:
;;   * push :hunchentoot-no-ssl + :websocket-driver-no-ssl BEFORE any cl+ssl
;;     .asd is read (clog is server-only)
;;   * load-libs-ql.lisp sets up the dist + local-projects searchers and the
;;     :drakma-no-ssl feature; ENSURE-QL-LIB fetches releases on a cold cache
;;
;; The chipi-ui/tests suite is browser-free: the rendering tests stub the CLOG
;; connection (clog-connection:execute/query) and assert on the generated DOM
;; commands, so no websocket/Hunchentoot acceptor is started here — and the
;; cl-amiga hunchentoot acceptor portability is therefore not exercised.
;;
;; Usage (host):
;;   ./build/host/amilisp --heap 256M --load trunk/load-and-test-chipi-ui.lisp
;;
;; On Amiga: --heap >= 96M and a large stack (e.g. `stack 800000`).

(setq *load-verbose* nil)

;; Skip the optional cl+ssl deps BEFORE any .asd that mentions them is read.
(pushnew :hunchentoot-no-ssl *features*)
(pushnew :websocket-driver-no-ssl *features*)

(require "asdf")
(load "trunk/load-libs-ql.lisp")

;; babel is normally pulled in transitively via the cffi/cl+ssl chain; since
;; cl-amiga skips SSL it is fetched + loaded explicitly (pure Lisp).
(ensure-ql-lib :babel)
(asdf:load-system :babel)

;; Make chipi-ui's full dependency closure resolvable from the quicklisp dist
;; (download-only; ASDF performs the actual load/compile below).  ENSURE-QL-LIB
;; is idempotent and only touches the network for releases not already present.
(dolist (sys '(;; chipi core + api + its test-only deps
               :chipi :ironclad :cl-base64 :marshal :snooze
               :fiveam :cl-mock :split-sequence
               ;; clog dependency closure (foundations first)
               :alexandria :trivial-features :iterate
               :named-readtables :pythonic-string-reader :trivial-utf-8
               :trivial-gray-streams :trivial-backtrace :trivial-indent
               :global-vars :cl-ppcre :parse-float :documentation-utils
               :static-vectors :cffi
               :fast-io :xsubseq :proc-parse :smart-buffer :cl-utilities
               :fast-http :fast-websocket :event-emitter :sha1
               :circular-streams :websocket-driver
               :chunga :flexi-streams :trivial-mimes :trivial-rfc-1123
               :http-body :quri :idna :clack :lack
               :mgl-pax :cl-template :md5 :cl-isaac :cl-pass
               :yason :cl-dbi :sqlite
               :clog
               ;; the target test system
               :chipi-ui))
  (ensure-ql-lib sys))

(format t "~%--- Loading chipi-ui/tests ---~%")
(asdf:load-system :chipi-ui/tests)

(format t "~%--- (fiveam:run! chipi-ui.tests:test-suite) ---~%")
(let* ((suite (find-symbol "TEST-SUITE" "CHIPI-UI.TESTS"))
       (results (fiveam:run suite)))
  (fiveam:explain! results)
  (if (fiveam:results-status results)
      (format t "~&--- chipi-ui tests: all passed ---~%")
      (progn
        (format t "~&--- chipi-ui tests: FAILURES (see report above) ---~%")
        (error "chipi-ui tests failed"))))
