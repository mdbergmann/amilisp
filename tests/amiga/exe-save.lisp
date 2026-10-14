;;;; exe-save.lisp -- the delivered-executable leg of the Amiga suite.
;;;;
;;;; call-on-ustartup loads this file: it defines a small program and saves
;;;; it with (ext:save-image ... :executable t :toplevel ...), which writes
;;;; build/amiga/exe-test -- a copy of the running amilisp with the image
;;;; appended.  The script then starts exe-test twice:
;;;;
;;;;   from the Shell, in another directory, as
;;;;       exe-test --help --heap "two words"
;;;;     MAIN prints EXECUTABLE-PASSED and ends with (quit 5); verify-amiga
;;;;     wants the marker and "exe rc=5".
;;;;   from Workbench (wbrun: a WBStartup message, no console, an 8000-byte
;;;;     stack), with the tool type ARGS=wb-arg and one project icon.  There
;;;;     is no command name then -- the runtime finds its own file through
;;;;     the WBStartup message -- and no console: the verdict goes to
;;;;     build/amiga/exe-wb.log (EXECUTABLE-WB-PASSED).
;;;;
;;;; tests/test_executable.sh is the host counterpart.

(defpackage :exe-check (:use :cl) (:export #:main))
(in-package :exe-check)

(defvar *trace* '())
(defclass greeter () ((name :initarg :name :reader greeter-name)))
(defgeneric greet (g))
(defmethod greet ((g greeter)) (format nil "hello ~a" (greeter-name g)))
(defun fib (n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))

(defun common-problems ()
  (let ((problems '()))
    (unless (eq ext:*image-restored-p* t)
      (push "*image-restored-p* is not T" problems))
    (unless (equal (reverse *trace*) '(:restore-hook :main))
      (push (format nil "restore hook / toplevel order: ~S" (reverse *trace*))
            problems))
    (unless (and (= (fib 15) 610)
                 (equal (greet (make-instance 'greeter :name "amiga"))
                        "hello amiga"))
      (push "the program's functions did not come back" problems))
    ;; :HEAP-SIZE (* 6 1024 1024) is the arena this process started with.
    (unless (search "/ 6291456 bytes"
                    (with-output-to-string (*standard-output*) (room)))
      (push ":heap-size is not the arena size" problems))
    problems))

(defun shell-main (args)
  (let ((problems (common-problems)))
    ;; Verbatim: --help and --heap are the program's, not amilisp's.
    (unless (equal args '("--help" "--heap" "two words"))
      (push (format nil "arguments: ~S" args) problems))
    (when ext:*workbench-started-p*
      (push "*workbench-started-p* is T on a Shell start" problems))
    (format t "EXECUTABLE ARGS ~S~%" args)
    (if problems
        (format t "EXECUTABLE-FAILED~%~{  ~A~%~}" (reverse problems))
        (format t "EXECUTABLE-PASSED~%"))
    (cl-user::quit 5)))

(defun wb-main (args)
  (with-open-file (s "AmiLisp:build/amiga/exe-wb.log"
                     :direction :output :if-exists :supersede)
    (let ((problems (common-problems))
          (suffix "build/amiga/wb-project.lisp")
          (path (third args)))
      ;; The tool type's ARGS, then --, then every project as a full path.
      (unless (and (= (length args) 3)
                   (equal (first args) "wb-arg")
                   (equal (second args) "--")
                   (stringp path)
                   (>= (length path) (length suffix))
                   (string-equal suffix path
                                 :start2 (- (length path) (length suffix))))
        (push (format nil "arguments: ~S" args) problems))
      (format s "EXECUTABLE-WB ARGS ~S~%" args)
      (if problems
          (format s "EXECUTABLE-WB-FAILED~%~{  ~A~%~}" (reverse problems))
          (format s "EXECUTABLE-WB-PASSED~%")))))

(defun main ()
  (push :main *trace*)
  (if ext:*workbench-started-p*
      (wb-main ext:*command-line-args*)
      (shell-main ext:*command-line-args*)))

(push (lambda () (push :restore-hook *trace*)) ext:*restore-hooks*)

(ext:save-image "AmiLisp:build/amiga/exe-test" :executable t
                :toplevel 'exe-check:main
                :heap-size (* 6 1024 1024) :quit t)
