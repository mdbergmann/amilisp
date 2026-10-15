;; Load and test Hunchentoot — the HTTP server — running as a real SERVER on
;; AmiLisp over the usocket AmiLisp backend.
;;
;; This is the server-side counterpart to trunk/load-and-test-drakma.lisp
;; (which exercises drakma as an HTTP/HTTPS *client*).  Here AmiLisp itself
;; IS the web server: we start a Hunchentoot easy-acceptor over
;; usocket/backend/clamiga.lisp (ext:socket-listen / ext:socket-accept on a
;; per-connection taskmaster thread) and then run Hunchentoot's own built-in
;; confidence suite against it.  The suite drives drakma over the loopback
;; interface and asserts on the responses — so a single run exercises:
;;
;;   - the usocket AmiLisp *server* path (listen / accept / wake-for-shutdown),
;;   - AmiLisp MP threads + bordeaux-threads (the acceptor's taskmaster),
;;   - Hunchentoot request/response, cookies, sessions, multipart uploads,
;;     range requests, static-file + folder dispatchers, basic auth, and
;;     latin-1/utf-8 parameter decoding (cl-who renders the test pages), and
;;   - drakma as the in-process client hitting all of the above.
;;
;; SCOPE: plain HTTP only.  The confidence suite talks to http://localhost, so
;; we push :hunchentoot-no-ssl (skip cl+ssl in Hunchentoot) and leave
;; load-libs-ql's :drakma-no-ssl in place (plain-HTTP drakma client).  HTTPS is
;; already covered by the drakma script; there is nothing SSL-specific here.
;;
;; HOST-ONLY: the suite needs a working loopback TCP/IP stack, which the
;; Amiga/FS-UAE test harness does not have — so, like the drakma script, there
;; is no Amiga counterpart.
;;
;; Usage (host):
;;   ./build/host/amilisp --heap 256M --load trunk/load-and-test-hunchentoot.lisp

(setq *load-verbose* nil)
(require "asdf")

;; Make the quicklisp-managed dependencies resolvable through ASDF.
;; (Also pushes :drakma-no-ssl — kept: our client only ever does plain HTTP.)
(load "trunk/load-libs-ql.lisp")

;; The confidence suite is a plain-HTTP test, so Hunchentoot needs no TLS.
;; Push :hunchentoot-no-ssl BEFORE hunchentoot.asd is read so its ssl.lisp
;; component and the cl+ssl dependency are both dropped.
(pushnew :hunchentoot-no-ssl *features*)

;; Resolve hunchentoot + its transitive deps and the test-only deps from the
;; quicklisp dist (download-only; ASDF performs the actual compile/load).
;; cl-who renders the test pages; drakma is the in-process client the suite
;; uses to hit the running acceptor.
(dolist (sys '(:trivial-features :alexandria :babel :cffi
               :cl-base64 :chunga :flexi-streams :cl-ppcre
               :md5 :rfc2388 :trivial-backtrace
               :usocket :bordeaux-threads :cl-fad :puri
               :cl-who :drakma :hunchentoot))
  (ensure-ql-lib sys))

(format t "~%--- Loading hunchentoot (plain HTTP; :hunchentoot-no-ssl) ---~%")
(asdf:load-system :hunchentoot)

;; The AmiLisp Hunchentoot portability (SET-TIMEOUTS) now ships as an in-source
;; #+cl-amiga branch in the hunchentoot fork (local-projects), so loading the
;; system above already installs it — no separate patch load needed.

;; Load Hunchentoot's own test system: the example handlers (test-handlers),
;; the assertion/script engine (script-engine), and the confidence script
;; (script.lisp's HUNCHENTOOT-TEST:TEST-HUNCHENTOOT).  Pulls in cl-who + drakma.
(format t "~%--- Loading :hunchentoot/test ---~%")
(asdf:load-system :hunchentoot/test)

;; ---------------------------------------------------------------------------
;; Tally adapter.  Hunchentoot's script engine SIGNALs a TEST-FAILURE for each
;; failed assertion and merely prints a "N assertions FAILED" line at the end —
;; it never reports a per-check pass/fail count.  trunk/run-load-and-test-all.sh
;; tallies on the fiveam-style "Pass: N (...) / Fail: N (...)" format, so we
;; wrap the two leaf assertion entry points (HTTP-ASSERT and HTTP-ASSERT-HEADER;
;; HTTP-ASSERT-BODY funnels through HTTP-ASSERT) to count every check and, via a
;; per-call HANDLER-BIND, the ones that signal a failure.  Both wrappers decline
;; the condition, so the engine's own handler still runs and prints its detail.
;; ---------------------------------------------------------------------------
(in-package :hunchentoot-test)

(defvar *amilisp-checks* 0)
(defvar *amilisp-fails* 0)

(let ((orig-assert (fdefinition 'http-assert))
      (orig-header  (fdefinition 'http-assert-header)))
  (flet ((counting (orig)
           (lambda (&rest args)
             (incf *amilisp-checks*)
             (handler-bind ((test-failure
                              (lambda (c)
                                (declare (ignore c))
                                (incf *amilisp-fails*))))
               (apply orig args)))))
    (setf (fdefinition 'http-assert)        (counting orig-assert)
          (fdefinition 'http-assert-header) (counting orig-header))))

;; ---------------------------------------------------------------------------
;; Start the acceptor, run the confidence suite against it, print the tally.
;; ---------------------------------------------------------------------------
(in-package :cl-user)

(defparameter *ht-test-port* 4242)

(format t "~%--- Starting Hunchentoot easy-acceptor on localhost:~A ---~%"
        *ht-test-port*)
(let ((server (hunchentoot:start
               (make-instance 'hunchentoot:easy-acceptor
                              :port *ht-test-port*
                              :message-log-destination nil
                              :access-log-destination nil))))
  (unwind-protect
       (progn
         ;; Give the acceptor's taskmaster thread a moment to come up.
         (sleep 2)
         (setf hunchentoot-test::*amilisp-checks* 0
               hunchentoot-test::*amilisp-fails*  0)
         (format t "~%--- Running hunchentoot confidence suite (HTTP server + drakma client) ---~%")
         (handler-case
             (hunchentoot-test:test-hunchentoot
              (format nil "http://localhost:~A" *ht-test-port*))
           (error (e)
             (format t "~&;; confidence suite aborted with an error: ~A~%" e)))
         (let* ((checks hunchentoot-test::*amilisp-checks*)
                (fails  hunchentoot-test::*amilisp-fails*)
                (pass   (max 0 (- checks fails)))
                (total  (max checks (+ pass fails)))
                (ppct   (if (plusp total) (round (* 100 pass) total) 100))
                (fpct   (if (plusp total) (- 100 ppct) 0)))
           (format t "~2&--- hunchentoot confidence suite result ---~%")
           (format t "Did ~D check~:P.~%" total)
           (format t "  Pass: ~D (~D%)~%" pass ppct)
           (format t "  Fail: ~D (~D%)~%" fails fpct)))
    (format t "~&--- Stopping server ---~%")
    (ignore-errors (hunchentoot:stop server))
    (ignore-errors (hunchentoot-test::clean-tmp-dir))))
