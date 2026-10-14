;; Smoke-run sento.bench::run-benchmark to verify the actor pipeline.
;; Loads the user's local cl-gserver checkout if present, otherwise falls
;; back to the quicklisp-installed copy.  Tiny defaults so it finishes in
;; a few seconds — bump :duration / :num-iterations / :load-threads for
;; an actual perf measurement.
;;
;; Usage:
;;   ./build/host/amilisp --heap 192M --load trunk/run-sento-bench.lisp
;;
;; Cold cache wants ~192M (cl-unicode + serapeum).  After the cache is
;; warm, --heap 64M is plenty.

(setq *load-verbose* nil)
(require "asdf")

#+amigaos (load "S:quicklisp/setup.lisp")
#-amigaos (load (merge-pathnames "quicklisp/setup.lisp" (user-homedir-pathname)))

(load "lib/quicklisp-compat.lisp")

(format t "~%--- quickload :sento + bench deps ---~%")
(let ((*standard-output* (make-broadcast-stream)))
  (ql:quickload :sento)
  (ql:quickload :serapeum)
  (ql:quickload :alexandria)
  (ql:quickload :trivial-benchmark)
  (ql:quickload :log4cl))

(defparameter *bench-file*
  (let ((local "/Users/mbergmann/Development/MySources/cl-gserver/bench.lisp"))
    (if (probe-file local)
        local
        (merge-pathnames "dists/quicklisp/software/cl-gserver-20260101-git/bench.lisp"
                         (merge-pathnames "quicklisp/" (user-homedir-pathname))))))

(format t "--- loading bench.lisp from ~A ---~%" *bench-file*)
(load *bench-file*)
