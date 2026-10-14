;;; examples.lisp — run every GUI program under examples/amiga/ unattended,
;;; one after the other, in this one amilisp process.
;;;
;;; Each example opens its window or screen, sits in its event loop for
;;; *EVENT-LOOP-TIMEOUT* seconds (nobody clicks in an unattended run),
;;; closes and returns; a failure to load or run is reported per example
;;; and does not stop the others.  The host side (run-examples.sh)
;;; greps the EXAMPLE-OK / EXAMPLE-FAIL / EXAMPLE-SKIP / EXAMPLES-DONE
;;; lines.  The graphics examples honour AMIGA.INTUITION's timeout, the
;;; ReAction ones AMIGA.REACTION's, the MUI ones AMIGA.MUI's; the latter
;;; two are skipped where the ReAction classes / MUI are absent.
;;;
;;; Not loaded by the Amiga test suite — it has its own focused checks
;;; (tests/amiga/test-gfx-examples.lisp, test-reaction.lisp); this is
;;; the examples' smoke run, photographed by screen-grab.lisp.

(require "amiga/intuition")
(require "amiga/reaction")
(require "amiga/mui")

(defparameter *examples*
  '(("gfx" "bouncing-lines")
    ("gfx" "doublebuffer")
    ("gfx" "sprite")
    ("reaction" "buttons") ("reaction" "checkbox") ("reaction" "chooser")
    ("reaction" "clicktab") ("reaction" "fuelgauge") ("reaction" "integer")
    ("reaction" "listbrowser") ("reaction" "requester")
    ("mui" "hello") ("mui" "layout") ("mui" "group-layout") ("mui" "balancing")
    ("mui" "pages") ("mui" "menus") ("mui" "showhide") ("mui" "slidorama")
    ("mui" "numeric") ("mui" "virtual") ("mui" "requester") ("mui" "class1")
    ("mui" "hooks")))

(setf amiga.intuition:*event-loop-timeout* 6
      amiga.reaction:*event-loop-timeout* 6
      amiga.mui:*event-loop-timeout* 6)

;; screen-grab.lisp runs detached beside us and writes T:screen-grab-ready
;; once it is watching; give it up to 60 s so the first window is not up
;; before it looks (the harness deletes the file before starting us).
(let ((deadline (+ (get-internal-real-time) (* 60 internal-time-units-per-second))))
  (loop until (or (probe-file "T:screen-grab-ready")
                  (> (get-internal-real-time) deadline))
        do (sleep 0.5))
  (format t "~&; examples: screen-grab ~:[not ready after 60 s, going ahead~;ready~]~%"
          (probe-file "T:screen-grab-ready")))

(let ((ok 0) (failed 0) (skipped 0)
      (reaction-p (amiga.reaction:available-p))
      (mui-p (amiga.mui:available-p)))
  (unless reaction-p
    (format t "~&; ReAction classes not available on this system - reaction/ examples skipped~%"))
  (unless mui-p
    (format t "~&; MUI not available on this system - mui/ examples skipped~%"))
  (dolist (example *examples*)
    (destructuring-bind (dir name) example
      (format t "~&=== example: ~A/~A ===~%" dir name)
      (finish-output)
      (cond
        ((and (string= dir "reaction") (not reaction-p))
         (incf skipped)
         (format t "~&EXAMPLE-SKIP ~A/~A: no ReAction classes~%" dir name))
        ((and (string= dir "mui") (not mui-p))
         (incf skipped)
         (format t "~&EXAMPLE-SKIP ~A/~A: no MUI~%" dir name))
        (t
          (let ((start (get-internal-real-time)))
            (handler-case
                (progn
                  (load (format nil "examples/amiga/~A/~A.lisp" dir name))
                  (incf ok)
                  (format t "~&EXAMPLE-OK ~A/~A (~,1F s)~%" dir name
                          (/ (- (get-internal-real-time) start)
                             internal-time-units-per-second)))
              (error (e)
                (incf failed)
                (format t "~&EXAMPLE-FAIL ~A/~A: ~A~%" dir name e))))))
      (finish-output)))
  (format t "~&EXAMPLES-DONE ~D ok, ~D failed, ~D skipped~%" ok failed skipped)
  (finish-output))
