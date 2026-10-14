;;;; wb-project.lisp -- the project file of the Workbench-start leg.
;;;;
;;;; call-on-ustartup copies this file to build/amiga/, gives it a project
;;;; icon whose default tool is amilisp, and starts amilisp on it the way
;;;; a double-click would (wbrun).  It must NOT be loaded: a project is an
;;;; argument of the program (EXT:*COMMAND-LINE-ARGS*), not a --load.  If
;;;; it ever is, this form makes the leg fail loudly.
(with-open-file (s "AmiLisp:build/amiga/wb-start.log"
                   :direction :output :if-exists :supersede)
  (format s "WORKBENCH-START-FAILED~%  the project file was LOADED~%"))
