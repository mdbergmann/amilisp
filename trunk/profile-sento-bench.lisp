;; Profiling-friendly variant of run-sento-bench.lisp.  Same setup, but
;; bumps duration / iterations / load-threads so the bench actually
;; steady-states long enough for `sample` (or instruments / dtrace) to
;; gather meaningful stack data.
;;
;; Usage (host, macOS):
;;   ./build/host/amilisp --heap 192M --load trunk/profile-sento-bench.lisp &
;;   PID=$(pgrep -f "amilisp.*profile-sento-bench")
;;   # Wait for "--- BENCH STEADY STATE BEGIN ---" in the log, then:
;;   sample $PID 30 -file /tmp/sento-bench.sample
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

;; Marker so the profiler driver knows the steady-state run is starting.
(format t "~%--- BENCH STEADY STATE BEGIN ---~%")
(force-output)

(let ((rc 0))
  (handler-case
      (progn
        (funcall (find-symbol "RUN-BENCHMARK" :sento.bench)
                 :dispatcher :pinned
                 :duration 15
                 :num-iterations 8
                 :load-threads 4
                 :num-shared-workers 4)
        (format t "~%--- BENCH OK ---~%"))
    (error (e)
      (format t "~%--- BENCH ERROR: ~A ---~%" e)
      (setf rc 1)))
  (uiop:quit rc))
