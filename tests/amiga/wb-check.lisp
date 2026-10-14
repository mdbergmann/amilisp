;;;; wb-check.lisp -- the Workbench-start leg of the Amiga suite.
;;;;
;;;; call-on-ustartup does not run this file itself: it starts amilisp the
;;;; way Workbench does (verify/realamiga/wbrun.c: a WBStartup message, no
;;;; console, an 8000-byte stack) with the tool icon's tool types
;;;;   ARGS=--no-userinit --no-image --heap 8M --non-interactive
;;;;   WINDOW=CON:0/0/640/100/wb-check/AUTO/CLOSE
;;;; and the project icon of build/amiga/wb-project.lisp carrying
;;;;   ARGS=--load AmiLisp:tests/amiga/wb-check.lisp
;;;; so this file is what the project's ARGS loads, and the project itself
;;;; must arrive as the one argument after -- (never loaded).
;;;;
;;;; There is no console to print to: the verdict goes to a file that the
;;;; startup script types into the results log (WORKBENCH-START-PASSED is
;;;; what verify-amiga greps for).

(with-open-file (s "AmiLisp:build/amiga/wb-start.log"
                   :direction :output :if-exists :supersede)
  (let* ((args ext:*command-line-args*)
         (path (first args))
         (suffix "build/amiga/wb-project.lisp")
         (problems '()))
    (unless ext:*workbench-started-p*
      (push "*workbench-started-p* is not T" problems))
    (unless (= (length args) 1)
      (push (format nil "expected one argument, got ~S" args) problems))
    (unless (and (stringp path)
                 (>= (length path) (length suffix))
                 (string-equal suffix path :start2 (- (length path) (length suffix))))
      (push (format nil "the project is not a full path to ~A: ~S" suffix path) problems))
    (unless (and (stringp path) (probe-file path))
      (push (format nil "the project path does not exist: ~S" path) problems))
    ;; The current directory is the tool's drawer (build/amiga/), where the
    ;; binary and its image live -- so the icon started us there.
    (unless (probe-file "amilisp")
      (push (format nil "cwd is not the tool's drawer: ~A"
                    (namestring *default-pathname-defaults*))
            problems))
    ;; Output() is the WINDOW tool type's console: writing must not signal.
    (handler-case (progn (write-line "wb-check: hello from the WINDOW console")
                         (finish-output))
      (error (e) (push (format nil "console write failed: ~A" e) problems)))
    (format s "WORKBENCH-STARTED-P ~S~%ARGS ~S~%CWD ~A~%"
            ext:*workbench-started-p* args (namestring *default-pathname-defaults*))
    (if problems
        (format s "WORKBENCH-START-FAILED~%~{  ~A~%~}" (reverse problems))
        (format s "WORKBENCH-START-PASSED~%"))))
