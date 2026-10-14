;;; gc-mark-bench.lisp -- the mark phase alone: build a live set of a known
;;; size and shape, then time explicit collections on it.  The number that
;;; comes out is the idle pause (docs/gc-phase0-results.md: on a quiet heap
;;; the pause is 85-95 % mark, which neither the free list nor Immix's line
;;; sweep touches), as milliseconds per cycle and per live megabyte.
;;;
;;;   clamiga --heap 16M --non-interactive --load trunk/gc-mark-bench.lisp
;;;
;;; Optional, before loading:
;;;   (defparameter cl-user::*mark-bench-mb* 4)   ; live set to build (MB, default 2)
;;;   (defparameter cl-user::*mark-bench-load* "path")  ; a file to LOAD first
;;;     (lib/clamacs/load.lisp: the editor's live set instead of a synthetic one)
;;;
;;; The synthetic live set is a mix in the proportions a loaded heap shows:
;;; lists of fixnums (cons-heavy, the common case), lists of small strings
;;; (every cons reaches a leaf object), small simple-vectors of conses, and
;;; a hash table.  Everything is reachable from one special variable.
;;; Host: CLAMIGA_GENGC=0 selects the classic collector (gen mode routes
;;; (ext:gc) to a compaction and reports no mark phase).  On the Amiga,
;;; verify/realamiga/run-gc-phase0.sh SCRIPT=trunk/gc-mark-bench.lisp runs
;;; it in FS-UAE.

(defpackage "GC-MARK-BENCH" (:use "CL"))
(in-package "GC-MARK-BENCH")

(defun say (fmt &rest args)
  (apply #'format t fmt args)
  (terpri)
  (finish-output))

(defvar *live* nil)

(defun live-kb ()
  (ash (nth 2 (ext:%heap-stats)) -10))

(defun mark-seconds ()
  (nth 3 (ext:%gc-time-stats)))

(defun gc-count ()
  (nth 0 (ext:%gc-time-stats)))

(defun build-live-set (target-mb)
  "Grow *LIVE* until the heap carries TARGET-MB of live data (as the
collector sees it after a cycle)."
  (let ((target-kb (* 1024 target-mb))
        (ht (make-hash-table :test 'equal))
        (i 0))
    (setf *live* (list ht))
    (loop
      ;; one unit: ~4 KB of mixed objects
      (push (make-list 100 :initial-element i) *live*)
      (push (loop for k below 20 collect (make-string 8 :initial-element #\a)) *live*)
      (push (let ((v (make-array 16)))
              (dotimes (k 16) (setf (svref v k) (cons k i)))
              v)
            *live*)
      (setf (gethash (format nil "key~D" i) ht) i)
      (incf i)
      (when (zerop (mod i 64))
        (ext:gc)
        (when (>= (live-kb) target-kb)
          (return))))
    (say "live set: ~D units, ~D KB live after a cycle" i (live-kb))))

;;; ---- main ---------------------------------------------------------------

(let ((load-file (and (boundp 'cl-user::*mark-bench-load*)
                      (symbol-value 'cl-user::*mark-bench-load*)))
      (target-mb (if (boundp 'cl-user::*mark-bench-mb*)
                     (symbol-value 'cl-user::*mark-bench-mb*)
                     2)))
  (say "=== gc-mark-bench start: ~A ~A" (lisp-implementation-type) (lisp-implementation-version))
  (cond (load-file
         (say "loading ~A" load-file)
         (load load-file)
         (ext:gc)
         (say "live set: ~A, ~D KB live after a cycle" load-file (live-kb)))
        (t
         (build-live-set target-mb)))
  (let ((live-kb (live-kb))
        (total 0d0)
        (n 5))
    (say "--- explicit (ext:gc) x~D on ~D KB live" n live-kb)
    (dotimes (i n)
      (let ((m0 (mark-seconds))
            (g0 (gc-count)))
        (ext:gc)
        (let ((ms (* 1000d0 (- (mark-seconds) m0))))
          (incf total ms)
          (say "gc#~D: ~D cycle(s)  mark ~,1F ms" (1+ i) (- (gc-count) g0) ms))))
    (say "mark: ~,1F ms per cycle, ~,1F ms per live MB"
         (/ total n) (/ (/ total n) (/ live-kb 1024d0))))
  (setf *live* nil)
  (say "=== gc-mark-bench end ==="))
