;;; gc-phase0.lisp -- Phase 0 of specs/gc-backend-immix.md: where does a
;;; classic collection cycle spend its time on THIS machine, and how much
;;; does the allocator pay between cycles for the free list?
;;;
;;;   clamiga --heap 8M --non-interactive --load trunk/gc-phase0.lisp
;;;
;;; Optional, before loading: (defparameter cl-user::*phase0-load* "path")
;;; -- a file to LOAD first (e.g. lib/clamacs/load.lisp) so the live set is
;;; the one the editor carries; the sections then run twice, bare and with
;;; that live set.  Host: CLAMIGA_GENGC=0 selects the classic collector
;;; (gen mode reports no sweep phase at all); a GC=immix build reports its
;;; line sweep in the sweep column and no free-list steps at all.  On the
;;; Amiga, verify/realamiga/run-gc-phase0.sh runs both legs in FS-UAE.
;;;
;;; Everything comes from the runtime's own counters: (ext:%gc-time-stats)
;;; for the phase timers, (ext:%heap-stats) for the free-list probe steps
;;; and the free-list shape.  Output is plain lines so a FS-UAE log and a
;;; real-hardware log read the same.

(defpackage "GC-PHASE0" (:use "CL"))
(in-package "GC-PHASE0")

(defun say (fmt &rest args)
  (apply #'format t fmt args)
  (terpri)
  (finish-output))

;;; ---- counters ---------------------------------------------------------

(defstruct snap gcs compacts mark sweep compact wall steps refills
           bump live free-blocks free-bytes largest)

(defun snapshot ()
  (let ((g (ext:%gc-time-stats))
        (h (ext:%heap-stats)))
    (make-snap :gcs (nth 0 g) :compacts (nth 1 g)
               :mark (nth 3 g) :sweep (nth 4 g) :compact (nth 5 g)
               :wall (/ (get-internal-real-time)
                        (float internal-time-units-per-second 1d0))
               :steps (nth 4 h) :refills (nth 5 h)
               :bump (nth 1 h) :live (nth 2 h)
               :free-blocks (nth 6 h) :free-bytes (nth 7 h) :largest (nth 8 h))))

(defun ms (a b) (* 1000d0 (- b a)))

(defun report-delta (label a b)
  "One line per interval: cycles, phase milliseconds, probe steps."
  (let ((cycles (- (snap-gcs b) (snap-gcs a)))
        (compacts (- (snap-compacts b) (snap-compacts a)))
        (mark (ms (snap-mark a) (snap-mark b)))
        (sweep (ms (snap-sweep a) (snap-sweep b)))
        (compact (ms (snap-compact a) (snap-compact b)))
        (wall (ms (snap-wall a) (snap-wall b)))
        (steps (- (snap-steps b) (snap-steps a))))
    (say "~A: ~D gc (~D compact)  wall ~,1F ms  mark ~,1F ms  sweep ~,1F ms  compact ~,1F ms  freelist-steps ~D"
         label cycles compacts wall mark sweep compact steps)
    (when (and (plusp cycles) (plusp (+ mark sweep)))
      (say "~A:   sweep share of mark+sweep ~,1F%  per-cycle mark ~,1F ms sweep ~,1F ms"
           label (* 100 (/ sweep (+ mark sweep)))
           (/ mark cycles) (/ sweep cycles)))
    (say "~A:   heap: bump ~DK live ~DK  free list: ~D blocks ~DK (largest ~D bytes)"
         label (ash (snap-bump b) -10) (ash (snap-live b) -10)
         (snap-free-blocks b) (ash (snap-free-bytes b) -10) (snap-largest b))))

;;; ---- sections ---------------------------------------------------------

(defun section-explicit (tag)
  "Explicit full collections: the pause with nothing else going on."
  (say "--- ~A: explicit (ext:gc) x4" tag)
  (dotimes (i 4)
    (let ((a (snapshot)))
      (ext:gc)
      (report-delta (format nil "~A gc#~D" tag (1+ i)) a (snapshot)))))

(defun section-compact (tag)
  (say "--- ~A: explicit (ext:gc-compact)" tag)
  (let ((a (snapshot)))
    (ext:gc-compact)
    (report-delta (format nil "~A compact" tag) a (snapshot))))

(defvar *keep* nil)

(defun churn-step (slots i)
  ;; A mixed allocation: 50 conses, a string, a small vector -- about
  ;; 1.5 KB per step.  A fraction stays live in a rotating window so the
  ;; heap carries garbage AND survivors across cycles.
  (let ((l (make-list 50 :initial-element i))
        (s (make-string 64 :initial-element #\x))
        (v (make-array 32 :initial-element i)))
    (setf (svref slots (mod i (length slots))) (list l s v))))

(defun section-churn (tag &key (target-cycles 6) (max-steps 400000))
  "Allocate until TARGET-CYCLES natural collections ran; report each one
as the allocator saw it (free-list probing between cycles included)."
  ;; The live window is sized to the room actually left: a quarter of the
  ;; free heap, so a large loaded live set does not turn this into a
  ;; heap-exhaustion test.
  (let* ((h (ext:%heap-stats))
         (free (max 0 (- (first h) (third h))))
         (nslots (max 50 (min 1500 (floor free (* 4 1536))))))
    (say "--- ~A: churn until ~D natural collections (~D live slots, ~D KB free)"
         tag target-cycles nslots (ash free -10))
  (let* ((slots (make-array nslots :initial-element nil))
         (start (snapshot))
         (last start)
         (seen 0)
         (steps 0))
    (setf *keep* slots)
    (loop while (and (< seen target-cycles) (< steps max-steps)) do
      (dotimes (k 200)
        (churn-step slots (+ steps k)))
      (incf steps 200)
      (let ((now (snapshot)))
        (when (> (snap-gcs now) (snap-gcs last))
          (incf seen (- (snap-gcs now) (snap-gcs last)))
          (report-delta (format nil "~A cycle ~D" tag seen) last now)
          (setf last now))))
    (let* ((end (snapshot))
           (wall (ms (snap-wall start) (snap-wall end)))
           (gc (+ (ms (snap-mark start) (snap-mark end))
                  (ms (snap-sweep start) (snap-sweep end))
                  (ms (snap-compact start) (snap-compact end)))))
      (say "~A churn total: ~D steps (~,1F KB/step) in ~,1F ms; collector ~,1F ms = ~,1F% of wall; ~,1F us/step mutator"
           tag steps 1.5 wall gc (if (plusp wall) (* 100 (/ gc wall)) 0)
           (if (plusp steps) (/ (* 1000 (- wall gc)) steps) 0)))
    (setf *keep* nil))))

(defun run-all (tag)
  (section-explicit tag)
  (section-churn tag)
  (section-compact tag)
  (section-explicit (format nil "~A-after-compact" tag))
  ;; The collector's own view (ROOM's backend lines: the classic free
  ;; list's shape, or the Immix line map, holes claimed, cycles and
  ;; policy compactions) -- the counters above are backend-neutral.
  (say "--- ~A: room" tag)
  (room)
  (finish-output))

;;; ---- main ---------------------------------------------------------------

(say "=== gc-phase0 begin ===")
(say "~A ~A on ~A ~A" (lisp-implementation-type) (lisp-implementation-version)
     (software-type) (machine-type))
(let ((h (ext:%heap-stats)))
  (say "heap ~D KB, bump ~D KB, live ~D KB" (ash (first h) -10) (ash (second h) -10)
       (ash (third h) -10)))
(say "features: ~{~A~^ ~}" (remove-if-not (lambda (f) (member f '(:amigaos :morphos :m68k :ppc :jit :gengc :unix :windows)))
                                          *features*))

(run-all "bare")

(let ((extra (and (boundp 'cl-user::*phase0-load*)
                  (symbol-value 'cl-user::*phase0-load*))))
  (when extra
    (say "--- loading ~A" extra)
    (let ((a (snapshot)))
      (load extra)
      (report-delta "load" a (snapshot)))
    (run-all "loaded")))

(say "=== gc-phase0 end ===")
