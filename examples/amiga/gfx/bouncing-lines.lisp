;;; bouncing-lines.lisp — Bouncing color-cycling lines.
;;;
;;; Common Lisp port of ACE BASIC examples/gfx/bouncing_lines.b.
;;; Five lines bounce off the window edges; pen color is cycled per line.
;;;
;;; Run on Amiga (or in FS-UAE):
;;;   amilisp --load examples/amiga/gfx/bouncing-lines.lisp
;;; Click the close gadget to quit.

(require "amiga/intuition")
(require "amiga/graphics")

(defpackage "BOUNCING-LINES"
  (:use "CL" "AMIGA.INTUITION" "AMIGA.GFX"))

(in-package "BOUNCING-LINES")

(defconstant +num-lines+ 5)

(defstruct line
  start-x start-y end-x end-y
  dx1 dy1 dx2 dy2
  color)

(defun nz-delta ()
  "Random delta in [-3,3], never 0."
  (let ((d (- (random 7) 3)))
    (if (zerop d) 1 d)))

(defun init-lines (w h)
  (loop for i from 0 below +num-lines+
        collect (make-line
                  :start-x (+ 50 (random (max 1 (- w 100))))
                  :start-y (+ 50 (random (max 1 (- h 100))))
                  :end-x   (+ 50 (random (max 1 (- w 100))))
                  :end-y   (+ 50 (random (max 1 (- h 100))))
                  :dx1 (nz-delta) :dy1 (nz-delta)
                  :dx2 (nz-delta) :dy2 (nz-delta)
                  :color (1+ i))))

(defun step-line (ln w h)
  (let ((sx (+ (line-start-x ln) (line-dx1 ln)))
        (sy (+ (line-start-y ln) (line-dy1 ln)))
        (ex (+ (line-end-x   ln) (line-dx2 ln)))
        (ey (+ (line-end-y   ln) (line-dy2 ln)))
        (dx1 (line-dx1 ln))
        (dy1 (line-dy1 ln))
        (dx2 (line-dx2 ln))
        (dy2 (line-dy2 ln)))
    (when (or (<= sx 0) (>= sx w)) (setf dx1 (- dx1)))
    (when (or (<= sy 0) (>= sy h)) (setf dy1 (- dy1)))
    (when (or (<= ex 0) (>= ex w)) (setf dx2 (- dx2)))
    (when (or (<= ey 0) (>= ey h)) (setf dy2 (- dy2)))
    (when (< sx 0) (setf sx 0)) (when (> sx w) (setf sx w))
    (when (< sy 0) (setf sy 0)) (when (> sy h) (setf sy h))
    (when (< ex 0) (setf ex 0)) (when (> ex w) (setf ex w))
    (when (< ey 0) (setf ey 0)) (when (> ey h) (setf ey h))
    (setf (line-start-x ln) sx
          (line-start-y ln) sy
          (line-end-x   ln) ex
          (line-end-y   ln) ey
          (line-dx1 ln) dx1
          (line-dy1 ln) dy1
          (line-dx2 ln) dx2
          (line-dy2 ln) dy2)))

(defun draw-frame (rp lines w h)
  (set-a-pen rp 0)
  (rect-fill rp 0 0 w h)
  (dolist (ln lines)
    (step-line ln w h)
    (set-a-pen rp (line-color ln))
    (draw-line rp
               (line-start-x ln) (line-start-y ln)
               (line-end-x ln)   (line-end-y ln))))

(defun close-requested-p (win)
  "Drain pending IDCMP messages; return T if a CLOSEWINDOW was seen."
  (let ((port (window-user-port win))
        (closed nil))
    (loop for msg = (get-msg port)
          while msg
          do (when (= (msg-class msg) +idcmp-closewindow+)
               (setf closed t))
             (reply-msg msg))
    closed))

(defun draw-fps (rp fps iw)
  "Render an FPS string in the upper-right corner of the inner window."
  (let ((label (format nil "FPS: ~,1F" fps)))
    (set-a-pen rp 0)
    (rect-fill rp (- iw 80) 0 iw 10)
    (set-a-pen rp 1)
    (move-to rp (- iw 78) 8)
    (gfx-text rp label)))

(defun run (&key (width 600) (height 400))
  ;; GIMMEZEROZERO gives us a RastPort whose (0,0) is the inner content
  ;; area — drawing never touches the title bar or window borders.
  (with-window (win :title "Bouncing Color Lines"
                    :left 50 :top 50
                    :width width :height height
                    :idcmp +idcmp-closewindow+
                    :flags (logior +wflg-closegadget+ +wflg-dragbar+
                                   +wflg-depthgadget+ +wflg-sizegadget+
                                   +wflg-activate+ +wflg-gimmezerozero+))
    (let* ((rp (window-rastport win))
           (iw (window-gzz-width win))
           (ih (window-gzz-height win))
           (lines (init-lines iw ih))
           (frames 0)
           (fps 0.0)
           (t0 (get-internal-real-time))
           ;; Unattended runs (the examples' harness, the test suite)
           ;; set *EVENT-LOOP-TIMEOUT*; interactively it is NIL.
           (deadline (and *event-loop-timeout*
                          (+ t0 (round (* *event-loop-timeout*
                                          internal-time-units-per-second))))))
      (loop until (or (close-requested-p win)
                      (and deadline (> (get-internal-real-time) deadline)))
            do
        (draw-frame rp lines iw ih)
        ;; Repaint the FPS overlay every frame — draw-frame's rect-fill
        ;; wipes the inner window each iteration, so a once-per-second
        ;; paint would only flash for ~4 ms out of every second.
        (draw-fps rp fps iw)
        (incf frames)
        (let ((elapsed (/ (- (get-internal-real-time) t0)
                          internal-time-units-per-second)))
          (when (>= elapsed 1)
            (setf fps (float (/ frames elapsed))
                  frames 0
                  t0 (get-internal-real-time))))))))

(if *intuition-base*
    (run)
    (format t "bouncing-lines: not available - AmigaOS/MorphOS with intuition.library required~%"))
