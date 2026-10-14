;;; dev-repl-tests.lisp -- the editor's debugger protocol (lib/dev-repl.lisp)
;;; on the Amiga, in-process: no ARexx, a Lisp function stands in for the
;;; editor as in tests/test_dev_commands.sh.  Loaded by run-tests.lisp.
;;;
;;; Why a file of its own on the target: on the host every case here is
;;; green, and the m68k build was not (2026-09-11, the phase-4 leg of
;;; clamacs's drive.rexx) -- a restart back into a nested debugger level,
;;; and CONTINUE on a CERROR, ended the whole form when the function that
;;; signalled had been compiled natively by the JIT.  So each scenario runs
;;; with a natively compiled signaller and with a bytecode-only one, and a
;;; minimal HANDLER-BIND / RESTART-CASE shape without dev-repl runs the same
;;; matrix, so a failure names the layer.

(require "dev-commands")
(require "dev-repl")

(defvar *drt-sent* '())
(defvar *drt-lock* (mp:make-lock))

(setf ext.dev:*repl-send*
      (lambda (port command)
        (declare (ignore port))
        (mp:with-lock-held (*drt-lock*) (push command *drt-sent*))
        (values 0 "")))

(defun drt-prefix-p (prefix s)
  (and (>= (length s) (length prefix)) (string= prefix s :end2 (length prefix))))

(defun drt-sent-p (pred)
  (mp:with-lock-held (*drt-lock*) (and (find-if pred *drt-sent*) t)))

(defun drt-wait (pred)
  (loop repeat 750
        do (when (drt-sent-p pred) (return t))
           (sleep 0.02)))

(defun drt-wait-level (n)
  (let ((want (format nil "DEBUGGER ~d " n)))
    (drt-wait (lambda (c) (drt-prefix-p want c)))))

(defun drt-wait-result ()
  (drt-wait (lambda (c) (drt-prefix-p "RESULT" c))))

(defun drt-clear ()
  (mp:with-lock-held (*drt-lock*) (setf *drt-sent* '())))

(defun drt-last-result ()
  (mp:with-lock-held (*drt-lock*)
    (or (find-if (lambda (c) (drt-prefix-p "RESULT" c)) *drt-sent*) "")))

(defun drt-cmd (c)
  (ext.dev:handle-command c))

;;; The signallers: one the JIT may compile natively, one kept as bytecode.
(defun drt-native-fn (n m)
  (let ((c (* n m)))
    (error "drt native ~a" c)))

(defun drt-native-cerror ()
  (cerror "Go on" "drt native stop")
  :drt-went-on)

(amilisp::%jit-set-active nil)
(defun drt-bytecode-fn (n m)
  (let ((c (* n m)))
    (error "drt bytecode ~a" c)))

(defun drt-bytecode-cerror ()
  (cerror "Go on" "drt bytecode stop")
  :drt-went-on)
(amilisp::%jit-set-active t)

(format t "~&; dev-repl-tests: native signaller has native code: ~a, bytecode one: ~a~%"
        (and (amilisp::%jit-dump-bytes #'drt-native-fn) t)
        (and (amilisp::%jit-dump-bytes #'drt-bytecode-fn) t))
(check "dev-repl: the bytecode signaller has no native code" nil
       (amilisp::%jit-dump-bytes #'drt-bytecode-fn))

;;; ----------------------------------------------------------------
;;; The command layer's guard shape (dev-commands' %CALL-GUARDED: a CATCH
;;; whose UNWIND-PROTECT cleanup THROWs), nested twice with an
;;; UNWIND-PROTECT between.  A C-level error unwind -- no handler takes
;;; the condition -- lands in the inner guard's cleanup, whose THROW
;;; abandons that unwind (CLHS 5.2); the middle UNWIND-PROTECT's epilogue
;;; must then find nothing to re-initiate, and the outer guard sees a
;;; normal return.  The JIT's CATCH landing left the abandoned error
;;; pending (2026-09-14): the middle epilogue re-raised it, the outer
;;; guard answered "aborted by an unhandled condition" (the port's EVAL
;;; of an undefined function came back rc 20 instead of a diagnostic),
;;; and in the port's own cleanup chain the resurrected error killed the
;;; handler thread before it replied.  On a thread of its own, as the
;;; port's handler is, so an escape ends that thread and not this file.
;;; ----------------------------------------------------------------

(defvar *drt-guard-result* :not-run)

(defmacro drt-define-guards (guard outer)
  `(progn
     (defun ,guard (thunk)
       (let ((escaped t) (result nil))
         (catch 'drt-escape
           (unwind-protect
                (progn (setf result (funcall thunk))
                       (setf escaped nil))
             (when escaped (throw 'drt-escape nil))))
         (values result escaped)))
     (defun ,outer (thunk)
       (multiple-value-list
        (,guard (lambda ()
                  (unwind-protect
                       (multiple-value-list (,guard thunk))
                    (push :middle-cleanup *drt-trace*))))))))

(drt-define-guards drt-guard-native drt-outer-guard-native)
(amilisp::%jit-set-active nil)
(drt-define-guards drt-guard-bytecode drt-outer-guard-bytecode)
(amilisp::%jit-set-active t)

(defun drt-guard-case (label outer thunk)
  (setf *drt-guard-result* :not-run
        *drt-trace* '())
  (let ((th (mp:make-thread (lambda () (setf *drt-guard-result* (funcall outer thunk)))
                            :name "drt-guard")))
    (ignore-errors (mp:join-thread th)))
  ;; The inner guard caught it, (NIL T); the middle cleanup ran once; the
  ;; outer guard saw a normal return: ((NIL T) NIL).
  (check (format nil "guarded escape, ~a: result" label) '((nil t) nil) *drt-guard-result*)
  (check (format nil "guarded escape, ~a: middle cleanup ran once" label)
         '(:middle-cleanup) *drt-trace*))

(dolist (guards (list (list "native guards" #'drt-outer-guard-native)
                      (list "bytecode guards" #'drt-outer-guard-bytecode)))
  (dolist (signaller (list (list "native signaller" (lambda () (drt-native-fn 1 2)))
                           (list "bytecode signaller" (lambda () (drt-bytecode-fn 1 2)))
                           (list "undefined function"
                                 (lambda () (funcall (intern "DRT-NO-SUCH-FUNCTION") 1)))))
    (drt-guard-case (format nil "~a, ~a" (first guards) (first signaller))
                    (second guards) (second signaller))))

;;; ----------------------------------------------------------------
;;; The minimal shape, without dev-repl: a handler on top of the erring
;;; frame runs a nested job with its own RESTART-CASE and handler; the
;;; nested handler invokes the nested ABORT; the outer handler then
;;; invokes the outer one.  The trace says where control actually went.
;;; ----------------------------------------------------------------

(defvar *drt-trace* '())
(defvar *drt-park-lock* (mp:make-lock))
(defvar *drt-park-cv* (mp:make-condition-variable))

(defun drt-park ()
  ;; What the REPL thread does between the announcement and the restart:
  ;; wait on a condition variable (here with a timeout).
  (mp:with-lock-held (*drt-park-lock*)
    (mp:condition-wait *drt-park-cv* *drt-park-lock* 0.05)))

(defmacro drt-define-machinery (inner outer)
  `(progn
     (defun ,inner (fn park)
       (restart-case
           (handler-bind ((error (lambda (c)
                                   (push :hook2 *drt-trace*)
                                   (when park (drt-park))
                                   (invoke-restart (find-restart 'abort c)))))
             (funcall fn 1 2))
         (abort ()
           (push :inner-abort *drt-trace*)
           :inner)))
     (defun ,outer (fn park)
       (restart-case
           (handler-bind ((error (lambda (c)
                                   (push :hook1 *drt-trace*)
                                   (when park (drt-park))
                                   (push (list :inner (,inner fn park)) *drt-trace*)
                                   (invoke-restart (find-restart 'abort c)))))
             (funcall fn 3 4))
         (abort ()
           (push :outer-abort *drt-trace*)
           :outer)))))

(drt-define-machinery drt-inner-native drt-outer-native)
(amilisp::%jit-set-active nil)
(drt-define-machinery drt-inner-bytecode drt-outer-bytecode)
(amilisp::%jit-set-active t)

(dolist (machinery (list (list "native machinery" #'drt-outer-native)
                         (list "bytecode machinery" #'drt-outer-bytecode)))
  (dolist (signaller (list (list "native signaller" #'drt-native-fn)
                           (list "bytecode signaller" #'drt-bytecode-fn)))
    (dolist (park '(nil t))
      (let ((label (format nil "nested abort, ~a, ~a~a" (first machinery) (first signaller)
                           (if park ", parked" ""))))
        (setf *drt-trace* '())
        (let ((result (handler-case (funcall (second machinery) (second signaller) park)
                        (error (e) (list :escaped (princ-to-string e))))))
          (check (format nil "~a: trace" label)
                 '(:hook1 :hook2 :inner-abort (:inner :inner) :outer-abort)
                 (reverse *drt-trace*))
          (check (format nil "~a: result" label) :outer result))))))

;;; ----------------------------------------------------------------
;;; The real thing: dev-repl's debugger, driven as the editor drives it.
;;; ----------------------------------------------------------------

;;; A raw verb (what the Lisp Clamacs registers OUTPUT as) gets its argument
;;; verbatim: the leading blanks and the trailing newline survive, where an
;;; ordinary verb's argument is trimmed at both ends.
(defvar *drt-raw* '())
(ext.dev:define-raw-command "DRT-RAW" (arg)
  (push arg *drt-raw*)
  (values ext.dev:+rc-ok+ ""))
(ext.dev:define-command "DRT-TRIMMED" (arg)
  (push arg *drt-raw*)
  (values ext.dev:+rc-ok+ ""))
(drt-cmd (format nil "DRT-RAW   indented~%"))
(drt-cmd "DRT-RAW")
(drt-cmd (format nil "DRT-TRIMMED   both ends  ~%"))
(check "dev-repl: a raw verb keeps blanks and the newline"
       (format nil "  indented~%") (third *drt-raw*))
(check "dev-repl: a raw verb alone is the empty chunk" "" (second *drt-raw*))
(check "dev-repl: an ordinary verb is trimmed" "both ends" (first *drt-raw*))
(check "dev-repl: DRT-RAWS is not DRT-RAW"
       ext.dev:+rc-fatal+ (nth-value 0 (drt-cmd "DRT-RAWS x")))

(multiple-value-bind (rc text) (drt-cmd "REPL-ATTACH DRT DEBUG")
  (check "dev-repl: REPL-ATTACH DEBUG" 0 rc)
  (check "dev-repl: REPL-ATTACH answers the package" "CL-USER" text))

(defun drt-nested-abort-scenario (label fname)
  (drt-clear)
  (drt-cmd (format nil "REPL-EVAL (~a 3 4)" fname))
  (check (format nil "dev-repl ~a: an error opens level 1" label) t (drt-wait-level 1))
  (drt-clear)
  (drt-cmd (format nil "FRAME-EVAL 0 (~a 1 2)" fname))
  (check (format nil "dev-repl ~a: an error in a frame eval opens level 2" label) t
         (drt-wait-level 2))
  (drt-clear)
  (drt-cmd "ABORT")
  (drt-wait (lambda (c) (drt-prefix-p "OUTPUT ; Aborted" c)))
  (sleep 0.3)
  (check (format nil "dev-repl ~a: ABORT from level 2 announces level 1 again" label) t
         (drt-sent-p (lambda (c) (drt-prefix-p "DEBUGGER 1 " c))))
  (multiple-value-bind (rc text) (drt-cmd "RESTARTS")
    (check (format nil "dev-repl ~a: RESTARTS after the abort answers level 1" label) t
           (and (= rc 0) (search "level 1:" text) t))
    (when (/= rc 0) (format t "~&; dev-repl ~a: RESTARTS said ~a; sent so far: ~s~%" label text
                            (mp:with-lock-held (*drt-lock*) (reverse *drt-sent*)))))
  (drt-clear)
  (drt-cmd "ABORT")
  (check (format nil "dev-repl ~a: ABORT from level 1 ends the form" label) t (drt-wait-result))
  (check (format nil "dev-repl ~a: the form's value says it was aborted" label) t
         (and (search "; Aborted" (drt-last-result)) t))
  (multiple-value-bind (rc text) (drt-cmd "RESTARTS")
    (declare (ignore text))
    (check (format nil "dev-repl ~a: no level is left" label) 10 rc)))

(defun drt-continue-scenario (label fname)
  (drt-clear)
  (drt-cmd (format nil "REPL-EVAL (~a)" fname))
  (check (format nil "dev-repl ~a: CERROR opens level 1" label) t (drt-wait-level 1))
  (drt-clear)
  (drt-cmd "CONTINUE")
  (check (format nil "dev-repl ~a: CONTINUE ends the form" label) t (drt-wait-result))
  (check (format nil "dev-repl ~a: with the value after the CERROR" label) t
         (and (search ":DRT-WENT-ON" (drt-last-result)) t))
  (when (not (search ":DRT-WENT-ON" (drt-last-result)))
    (format t "~&; dev-repl ~a: RESULT was ~s~%" label (drt-last-result))))

(drt-nested-abort-scenario "bytecode signaller" "drt-bytecode-fn")
(drt-continue-scenario "bytecode cerror" "drt-bytecode-cerror")
(drt-nested-abort-scenario "native signaller" "drt-native-fn")
(drt-continue-scenario "native cerror" "drt-native-cerror")
;; And once more with the bytecode one, so a fault left behind by the native
;; run shows as such rather than as a flake.
(drt-nested-abort-scenario "bytecode signaller again" "drt-bytecode-fn")

;;; RESULT means idle.  An editor that sends the next form from inside the
;;; send that delivered RESULT -- Clamacs talking to its own image does,
;;; and a fast macro can -- must have it taken, not refused as busy.
(defvar *drt-chained* nil)
(let ((plain ext.dev:*repl-send*))
  (setf ext.dev:*repl-send*
        (lambda (port command)
          (funcall plain port command)
          (when (and (drt-prefix-p "RESULT" command) (null *drt-chained*))
            (setf *drt-chained*
                  (multiple-value-list (drt-cmd "REPL-EVAL (+ 40 2)"))))
          (values 0 "")))
  (drt-clear)
  (drt-cmd "REPL-EVAL (+ 1 1)")
  (drt-wait (lambda (c) (and (drt-prefix-p "RESULT" c) (search "42" c))))
  (setf ext.dev:*repl-send* plain))
(check "dev-repl: a REPL-EVAL sent on RESULT is taken" 0 (first *drt-chained*))
(check "dev-repl: and its form runs" t
       (drt-sent-p (lambda (c) (and (drt-prefix-p "RESULT 0" c) (search "42" c) t))))

;;; Systems (EXT.DEV:LOAD-ASD-SYSTEM): what is refused is refused before
;;; ASDF is asked for, so a wrong pick in the editor costs no memory.  The
;;; load itself is host-tested (tests/test_dev_commands.sh); ASDF is more
;;; than this suite's heap is for.
(let ((asdf-before (and (find-package "ASDF") t)))
  (multiple-value-bind (rc text)
      (drt-cmd "EVAL (ext.dev:load-asd-system \"T:drt-no-system.lisp\")")
    (check "dev-systems: a file that is no .asd is refused" 10 rc)
    (check "dev-systems: and the refusal names it" t
           (and (search "Not a system definition file (.asd): T:drt-no-system.lisp" text) t)))
  (multiple-value-bind (rc text)
      (drt-cmd "EVAL (ext.dev:test-asd-system \"T:drt-no-such-system.asd\")")
    (check "dev-systems: a missing .asd is refused" 10 rc)
    (check "dev-systems: and that refusal names it" t
           (and (search "No such system definition file: T:drt-no-such-system.asd" text) t)))
  (check "dev-systems: a refusal does not load ASDF" asdf-before
         (and (find-package "ASDF") t)))

(multiple-value-bind (rc text) (drt-cmd "REPL-DETACH")
  (declare (ignore text))
  (check "dev-repl: REPL-DETACH" 0 rc))
(setf ext.dev:*repl-send* nil)
