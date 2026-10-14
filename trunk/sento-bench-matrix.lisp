;; The docs/sento-bench-results-*.md reply-mode / dispatcher matrix.
;; Loads trunk/load-sento-bench.lisp, then runs the six cells (pinned /
;; shared x tell / ask-s / ask) back-to-back in one session, each with
;; (ext:%gc-time-stats) / (ext:%gengc-stats) snapshotted around it so the
;; per-cell "CELL ... STATS" line carries the GC share of wall time.
;;
;; Usage (cold cache, so the whole dependency stack compiles at the
;; forced speed — cached FASLs bypass the compiler):
;;
;;   rm -rf ~/.cache/common-lisp/amilisp-<version>-fasl<N>
;;   AMILISP_FORCE_SPEED=3 ./build/host/amilisp --no-userinit --heap 192M \
;;       --non-interactive --load trunk/sento-bench-matrix.lisp
;;
;; ATOMICS_DIR=<dir>[:<dir>...]: each directory is pushed onto
;; asdf:*central-registry* before the quickload so an older `atomics`
;; checkout wins over the local-projects fork — needed to run a binary
;; predating mp:compare-and-swap (< 0.7) for an A/B.  The same hook serves
;; a pinned sento checkout, or the bordeaux-threads fork at the commit a
;; base binary needs (its ACQUIRE-LOCK predates the timeout argument).
;; The driver prints which atomics.asd / bordeaux-threads.asd / sento.asd
;; loaded.
;;
;; SENTO_QUEUE_CAP=<n>: passed as :wait-if-queue-larger-than to every cell
;; (the bench's default is 10000).  Binaries before the heap-word locks
;; (04a2a410) need 2000 for the SHARED/ask cell: each in-flight
;; asynchronous ask held three slots of the 16384-entry lock table, and
;; the 0.10 sender side kept more of them in flight than that — see
;; docs/sento-bench-results-0.10.md.
;;
;; Runs ~5 minutes: ~31 s per cell (shared/tell ~50 s, queue drain) plus
;; the cold load.  Throughput of record is trivial-benchmark's AVERAGE
;; column of the MESSAGES-PER-SECOND row.

(setq *load-verbose* nil)
(require "asdf")

(defun env (name)
  (let ((sym (find-symbol "GETENV" :ext)))
    (and sym (fboundp sym) (funcall sym name))))

(let ((dirs (env "ATOMICS_DIR")))
  (when (and dirs (plusp (length dirs)))
    (loop :for start := 0 :then (1+ end)
          :for end := (position #\: dirs :start start)
          :for dir := (subseq dirs start end)
          :do (when (plusp (length dir))
                (push (pathname (if (char= (char dir (1- (length dir))) #\/) dir
                                    (concatenate 'string dir "/")))
                      asdf:*central-registry*)
                (format t "--- ATOMICS_DIR: ~A pushed onto asdf:*central-registry* ---~%" dir))
          :while end)))

(load "trunk/load-sento-bench.lisp")

(format t "~%--- lisp-implementation-version: ~A ---~%" (lisp-implementation-version))
(format t "--- AMILISP_FORCE_SPEED=~A ---~%" (env "AMILISP_FORCE_SPEED"))
(format t "--- atomics loaded from: ~A ---~%" (asdf:system-source-file "atomics"))
(format t "--- bordeaux-threads loaded from: ~A ---~%" (asdf:system-source-file "bordeaux-threads"))
(format t "--- sento loaded from: ~A ---~%" (asdf:system-source-file "sento"))

(defparameter *queue-cap*
  (let ((s (env "SENTO_QUEUE_CAP")))
    (and s (plusp (length s)) (parse-integer s))))
(when *queue-cap*
  (format t "--- SENTO_QUEUE_CAP=~D (:wait-if-queue-larger-than) ---~%" *queue-cap*))

(defun gengc-stats ()
  ;; (enabled-p minor-count minor-seconds promoted-bytes old-top dirty-pages);
  ;; classic-collector builds (no %GENGC-STATS) report zero minors.
  (let ((sym (find-symbol "%GENGC-STATS" :ext)))
    (if (and sym (fboundp sym))
        (funcall sym)
        (list nil 0 0.0d0 0 0 0))))

(defun run-cell (name &rest args)
  (format t "~%===== CELL ~A =====~%" name)
  (force-output)
  (let* ((t0 (get-internal-real-time))
         (g0 (ext:%gc-time-stats))
         (m0 (gengc-stats))
         (ok t))
    (handler-case
        (apply (find-symbol "RUN-BENCHMARK" :sento.bench)
               :num-shared-workers 8
               :load-threads 8
               :duration 5
               :num-iterations 6
               (append args
                       (and *queue-cap*
                            (list :wait-if-queue-larger-than *queue-cap*))))
      (error (e)
        (setf ok nil)
        (format t "~%--- CELL ~A ERROR: ~A ---~%" name e)))
    (let* ((t1 (get-internal-real-time))
           (g1 (ext:%gc-time-stats))
           (m1 (gengc-stats))
           (wall (/ (float (- t1 t0) 1d0) internal-time-units-per-second)))
      ;; %gc-time-stats: (gc-count compact-count stw-s mark-s sweep-s
      ;;                  compact-s stw-stops stw-max-s epoch-skips)
      (destructuring-bind (gc0 cc0 stw0 mark0 sweep0 comp0 stops0 max0 skips0) g0
        (declare (ignore max0))
        (destructuring-bind (gc1 cc1 stw1 mark1 sweep1 comp1 stops1 max1 skips1) g1
          (destructuring-bind (en0 min0 mins0 prom0 top0 dirty0) m0
            (declare (ignore en0 top0 dirty0))
            (destructuring-bind (en1 min1 mins1 prom1 top1 dirty1) m1
              (declare (ignore dirty1))
              (let* ((stw (- stw1 stw0))
                     (mark (- mark1 mark0))
                     (sweep (- sweep1 sweep0))
                     (comp (- comp1 comp0))
                     (minors (- mins1 mins0))
                     (total (+ stw mark sweep comp minors)))
                (format t "~%--- CELL ~A STATS (~A): wall=~,2Fs gc-count=~D compactions=~D stw=~,3Fs (stops=~D max=~,4Fs skips=~D) mark=~,3Fs sweep=~,3Fs compact=~,3Fs minors=~D minor-time=~,3Fs promoted=~D old-top=~D gengc=~A total-gc=~,3Fs gc-share=~,2F% ---~%"
                        name (if ok "OK" "ERROR") wall
                        (- gc1 gc0) (- cc1 cc0)
                        stw (- stops1 stops0) max1 (- skips1 skips0)
                        mark sweep comp
                        (- min1 min0) minors (- prom1 prom0) top1 en1
                        total (* 100 (/ total wall)))))))))
    (force-output)))

(run-cell "PINNED/tell"  :dispatcher :pinned :with-reply-p nil :async-ask-p nil)
(run-cell "PINNED/ask-s" :dispatcher :pinned :with-reply-p t   :async-ask-p nil)
(run-cell "PINNED/ask"   :dispatcher :pinned :with-reply-p t   :async-ask-p t)
(run-cell "SHARED/tell"  :dispatcher :shared :with-reply-p nil :async-ask-p nil)
(run-cell "SHARED/ask-s" :dispatcher :shared :with-reply-p t   :async-ask-p nil)
(run-cell "SHARED/ask"   :dispatcher :shared :with-reply-p t   :async-ask-p t)

(format t "~%--- MATRIX DONE ---~%")
(force-output)
(uiop:quit 0)
