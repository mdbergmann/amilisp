;;; fuelgauge.lisp — fuelgauge.gadget driven from the program.
;;;
;;; Common Lisp port of the NDK 3.2 example Examples/FuelGauge.c: a
;;; window.class window with a horizontal fuelgauge.gadget (0..100 with
;;; percentage display and tick marks), a bevelled label explaining it,
;;; and Down / Up / Quit buttons.  Down and Up animate the gauge level
;;; through SET-GADGET-ATTRS in 5 % steps, with the window's busy pointer
;;; shown meanwhile (WA_BusyPointer through SET-ATTRS on the window
;;; object).
;;;
;;; Run on AmigaOS 3.5+/3.2 or MorphOS (ReAction classes required):
;;;   amilisp --load examples/amiga/reaction/fuelgauge.lisp

(require "amiga/reaction")
(require "amiga/raw/dos")
(require "amiga/raw/intuition")
(require "amiga/raw/classes/window")
(require "amiga/raw/gadgets/layout")
(require "amiga/raw/gadgets/button")
(require "amiga/raw/gadgets/fuelgauge")
(require "amiga/raw/images/label")
(require "amiga/raw/images/bevel")

(defpackage "REACTION-FUELGAUGE"
  (:use "CL")
  (:local-nicknames ("RA"        "AMIGA.REACTION")
                    ("DOS"       "AMIGA.RAW.DOS")
                    ("INTUI"     "AMIGA.RAW.INTUITION")
                    ("WIN"       "AMIGA.RAW.CLASSES.WINDOW")
                    ("LAYOUT"    "AMIGA.RAW.GADGETS.LAYOUT")
                    ("BUTTON"    "AMIGA.RAW.GADGETS.BUTTON")
                    ("FUELGAUGE" "AMIGA.RAW.GADGETS.FUELGAUGE")
                    ("LABEL"     "AMIGA.RAW.IMAGES.LABEL")
                    ("BEVEL"     "AMIGA.RAW.IMAGES.BEVEL")))

(in-package "REACTION-FUELGAUGE")

(defconstant +gid-gauge+ 1)
(defconstant +gid-down+  2)
(defconstant +gid-up+    3)
(defconstant +gid-quit+  4)

(defconstant +fmin+ 0)
(defconstant +fmax+ 100)

(defvar *gauge* nil)

(defun line (text) (format nil "~A~%" text))

(defun make-button (id text)
  (ra:new-object (button:button-get-class)
                 intui:+ga-id+ id
                 intui:+ga-rel-verify+ t
                 intui:+ga-text+ text))

(defun make-window-object ()
  (setf *gauge* (ra:new-object (fuelgauge:fuelgauge-get-class)
                               intui:+ga-id+ +gid-gauge+
                               fuelgauge:+fuelgauge-orientation+ fuelgauge:+fgorient-horiz+
                               fuelgauge:+fuelgauge-min+ +fmin+
                               fuelgauge:+fuelgauge-max+ +fmax+
                               fuelgauge:+fuelgauge-level+ 0
                               fuelgauge:+fuelgauge-percent+ t
                               fuelgauge:+fuelgauge-tick-size+ 5
                               fuelgauge:+fuelgauge-ticks+ 5
                               fuelgauge:+fuelgauge-short-ticks+ t))
  (ra:new-object (win:window-get-class)
    intui:+wa-screen-title+ "ReAction"
    intui:+wa-title+ "ReAction FuelGauge Example"
    intui:+wa-activate+ t
    intui:+wa-depth-gadget+ t
    intui:+wa-drag-bar+ t
    intui:+wa-close-gadget+ t
    intui:+wa-size-gadget+ t
    win:+window-position+ win:+wpos-centermouse+
    win:+window-parent-group+
    (ra:new-object (layout:layout-get-class)
      layout:+layout-orientation+ layout:+layout-vertical+
      layout:+layout-space-outer+ t
      layout:+layout-defer-layout+ t
      layout:+layout-add-child+ *gauge*
      layout:+layout-add-child+
      (ra:new-object (layout:layout-get-class)
                     layout:+layout-orientation+ layout:+layout-vertical+
                     layout:+layout-back-fill+ nil
                     layout:+layout-space-outer+ t
                     layout:+layout-vert-alignment+ layout:+lalign-center+
                     layout:+layout-horiz-alignment+ layout:+lalign-center+
                     layout:+layout-bevel-style+ bevel:+bvs-field+
                     layout:+layout-add-image+
                     (ra:new-object (label:label-get-class)
                                    label:+label-text+ (line "The fuelgauge supports optional tickmarks as")
                                    label:+label-text+ (line "well as vertical and horizontal orientation.")
                                    label:+label-text+ (line " ")
                                    label:+label-text+ (line "You can set the min/max range, as well as")
                                    label:+label-text+ (line "options such as varargs ascii display,")
                                    label:+label-text+ (line "percentage display, and custom pen selection.")))
      layout:+layout-add-child+
      (ra:new-object (layout:layout-get-class)
                     layout:+layout-orientation+ layout:+layout-horizontal+
                     layout:+layout-space-outer+ nil
                     layout:+layout-even-size+ t
                     layout:+layout-add-child+ (make-button +gid-down+ "_Down")
                     layout:+layout-add-child+ (make-button +gid-up+ "_Up")
                     layout:+layout-add-child+ (make-button +gid-quit+ "_Quit"))
      layout:+child-weighted-height+ 0)))

(defun animate (win-obj window from to step)
  "Move the gauge from FROM to TO in STEPs, busy pointer on meanwhile."
  (ra:set-attrs win-obj intui:+wa-busy-pointer+ t)
  (loop for level = from then (+ level step)
        while (if (plusp step) (<= level to) (>= level to))
        do (ra:set-gadget-attrs *gauge* window fuelgauge:+fuelgauge-level+ level)
           (dos:delay 3))
  (ra:set-attrs win-obj intui:+wa-busy-pointer+ nil))

(defun run ()
  (ra:with-foreign-pool ()
    (let ((win-obj (make-window-object)))
      (unwind-protect
           (let ((window (ra:open-window win-obj)))
             (unless window
               (error "fuelgauge: could not open the window"))
             (ra:do-window-events ((result code) win-obj)
               (let ((class (logand result win:+wmhi-classmask+)))
                 (cond ((= class win:+wmhi-closewindow+)
                        (return))
                       ((= class win:+wmhi-gadgetup+)
                        (let ((gid (logand result win:+wmhi-gadgetmask+)))
                          (cond ((= gid +gid-quit+) (return))
                                ((= gid +gid-down+) (animate win-obj window +fmax+ +fmin+ -5))
                                ((= gid +gid-up+)   (animate win-obj window +fmin+ +fmax+ 5)))))))))
        (ra:dispose-object win-obj)
        (setf *gauge* nil)))))

(if (ra:available-p)
    (run)
    (format t "~&; fuelgauge: the ReAction classes are not available on this system (AmigaOS 3.5+/3.2 or MorphOS needed) — nothing to show.~%"))
