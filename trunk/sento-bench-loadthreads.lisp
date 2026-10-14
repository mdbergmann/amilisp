;; Where does the sento pinned/tell cell saturate?  Runs the cell at a
;; sweep of producer counts (:load-threads) with everything else as in
;; trunk/sento-bench-matrix.lisp, so the rate can be read against the
;; number of producers: a rate that grows with the producers is bound by
;; the producer side; one that is flat from 1 producer on is bound by the
;; consumer (the actor thread's VM work) or by what every thread shares
;; (allocation, stop-the-world collections).  The per-cell STATS line
;; carries the GC share so the last two can be told apart.
;;
;; Usage (warm ASDF cache; the cold load wants 192M):
;;
;;   SENTO_LOAD_THREADS="1 2 4 8" ./build/host/amilisp --no-userinit \
;;       --heap 192M --non-interactive --load trunk/sento-bench-loadthreads.lisp
;;
;; ~35 s per producer count.  Throughput of record is trivial-benchmark's
;; AVERAGE column of the MESSAGES-PER-SECOND row.

(setq *load-verbose* nil)
(require "asdf")

(defun env (name)
  (let ((sym (find-symbol "GETENV" :ext)))
    (and sym (fboundp sym) (funcall sym name))))

(load "trunk/load-sento-bench.lisp")

(format t "~%--- lisp-implementation-version: ~A ---~%" (lisp-implementation-version))

(defun parse-counts (s)
  (let ((out nil) (start 0))
    (loop for i from 0 to (length s)
          do (when (or (= i (length s)) (char= (char s i) #\Space))
               (when (> i start)
                 (push (parse-integer s :start start :end i) out))
               (setq start (1+ i))))
    (nreverse out)))

(defparameter *counts*
  (let ((s (env "SENTO_LOAD_THREADS")))
    (if (and s (plusp (length s))) (parse-counts s) '(1 2 4 8))))

(defun run-cell (load-threads)
  (format t "~%===== CELL PINNED/tell load-threads=~D =====~%" load-threads)
  (force-output)
  (let* ((t0 (get-internal-real-time))
         (g0 (ext:%gc-time-stats))
         (ok t))
    (handler-case
        (funcall (find-symbol "RUN-BENCHMARK" :sento.bench)
                 :dispatcher :pinned :with-reply-p nil :async-ask-p nil
                 :num-shared-workers 8
                 :load-threads load-threads
                 :duration 5
                 :num-iterations 6)
      (error (e)
        (setf ok nil)
        (format t "~%--- CELL load-threads=~D ERROR: ~A ---~%" load-threads e)))
    (let* ((t1 (get-internal-real-time))
           (g1 (ext:%gc-time-stats))
           (wall (/ (float (- t1 t0) 1d0) internal-time-units-per-second)))
      ;; %gc-time-stats: (gc-count compact-count stw-s mark-s sweep-s
      ;;                  compact-s stw-stops stw-max-s epoch-skips)
      (destructuring-bind (gc0 cc0 stw0 mark0 sweep0 comp0 stops0 max0 skips0) g0
        (declare (ignore max0 skips0))
        (destructuring-bind (gc1 cc1 stw1 mark1 sweep1 comp1 stops1 max1 skips1) g1
          (declare (ignore skips1))
          (let ((total (+ (- stw1 stw0) (- mark1 mark0) (- sweep1 sweep0) (- comp1 comp0))))
            (format t "~%--- CELL load-threads=~D STATS (~A): wall=~,2Fs gc-count=~D compactions=~D stw=~,3Fs (stops=~D max=~,4Fs) total-gc=~,3Fs gc-share=~,2F% ---~%"
                    load-threads (if ok "OK" "ERROR") wall
                    (- gc1 gc0) (- cc1 cc0) (- stw1 stw0) (- stops1 stops0) max1
                    total (* 100 (/ total wall)))))))
    (force-output)))

(dolist (n *counts*) (run-cell n))

(format t "~%--- SWEEP DONE ---~%")
(force-output)
(uiop:quit 0)
