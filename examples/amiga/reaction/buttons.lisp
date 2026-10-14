;;; buttons.lisp — a window full of ReAction buttons.
;;;
;;; Common Lisp port of the NDK 3.2 example Examples/Buttons.c ("ReAction's
;;; speed laying out & rendering 100 buttons"): a window.class window whose
;;; WINDOW_ParentGroup is a horizontal layout.gadget holding ten vertical
;;; groups of ten button.gadget objects.  Where the C source spells out a
;;; hundred ButtonObject blocks, this builds the object tree in a loop.
;;; Every button reports a GADGETUP with its GA_ID; the close gadget or
;;; Ctrl-C ends the program.
;;;
;;; What it shows: the window.class / layout.gadget / button.gadget raw
;;; modules, AMIGA.REACTION:NEW-OBJECT with nested objects and string
;;; tags, RA_OpenWindow / RA_HandleInput as OPEN-WINDOW / DO-WINDOW-EVENTS.
;;;
;;; Run on AmigaOS 3.5+/3.2 or MorphOS (ReAction classes required):
;;;   amilisp --load examples/amiga/reaction/buttons.lisp
;;; Unattended (auto-closes after 5 s):
;;;   amilisp --eval '(require "amiga/reaction")' \
;;;           --eval '(setf amiga.reaction:*event-loop-timeout* 5)' \
;;;           --load examples/amiga/reaction/buttons.lisp

(require "amiga/reaction")
(require "amiga/raw/intuition")
(require "amiga/raw/classes/window")
(require "amiga/raw/gadgets/layout")
(require "amiga/raw/gadgets/button")

(defpackage "REACTION-BUTTONS"
  (:use "CL")
  (:local-nicknames ("RA"     "AMIGA.REACTION")
                    ("INTUI"  "AMIGA.RAW.INTUITION")
                    ("WIN"    "AMIGA.RAW.CLASSES.WINDOW")
                    ("LAYOUT" "AMIGA.RAW.GADGETS.LAYOUT")
                    ("BUTTON" "AMIGA.RAW.GADGETS.BUTTON")))

(in-package "REACTION-BUTTONS")

(defparameter *columns* 10)
(defparameter *rows* 10)

;;; reaction_macros.h spells these VGroupObject / HGroupObject: a
;;; layout.gadget with its orientation set.  LAYOUT_AddChild adds the
;;; children in order.
(defun vgroup (&rest tags)
  (apply #'ra:new-object (layout:layout-get-class)
         layout:+layout-orientation+ layout:+layout-vertical+ tags))

(defun hgroup (&rest tags)
  (apply #'ra:new-object (layout:layout-get-class)
         layout:+layout-orientation+ layout:+layout-horizontal+ tags))

(defun make-button (id)
  ;; GA_RelVerify makes the button report WMHI_GADGETUP on release.
  (ra:new-object (button:button-get-class)
                 intui:+ga-id+ id
                 intui:+ga-rel-verify+ t
                 intui:+ga-text+ "Button"))

(defun make-column (first-id)
  (apply #'vgroup
         (loop for i from 0 below *rows*
               collect layout:+layout-add-child+
               collect (make-button (+ first-id i)))))

(defun make-window-object ()
  (ra:new-object (win:window-get-class)
                 intui:+wa-screen-title+ "ReAction"
                 intui:+wa-title+ (format nil "Benchmark 1 (~D buttons)"
                                          (* *rows* *columns*))
                 intui:+wa-size-gadget+ t
                 intui:+wa-left+ 40
                 intui:+wa-top+ 30
                 intui:+wa-depth-gadget+ t
                 intui:+wa-drag-bar+ t
                 intui:+wa-close-gadget+ t
                 intui:+wa-activate+ t
                 win:+window-parent-group+
                 (apply #'hgroup
                        layout:+layout-space-outer+ t
                        ;; let the application, not input.device, do the
                        ;; (expensive) layout of 100 gadgets
                        layout:+layout-defer-layout+ t
                        (loop for c from 0 below *columns*
                              collect layout:+layout-add-child+
                              collect (make-column (1+ (* c *rows*)))))))

(defun run ()
  ;; The pool keeps the "Button" / title strings alive for as long as the
  ;; objects that reference them exist.
  (ra:with-foreign-pool ()
    (let ((win-obj (make-window-object)))
      (unwind-protect
           (progn
             (unless (ra:open-window win-obj)
               (error "buttons: could not open the window"))
             (ra:do-window-events ((result code) win-obj)
               (let ((class (logand result win:+wmhi-classmask+)))
                 (cond ((= class win:+wmhi-closewindow+)
                        (return))
                       ((= class win:+wmhi-gadgetup+)
                        (format t "~&; button ~D pressed~%"
                                (logand result win:+wmhi-gadgetmask+)))))))
        ;; Disposing the window object closes the window and disposes
        ;; every object attached to it.
        (ra:dispose-object win-obj)))))

(if (ra:available-p)
    (run)
    (format t "~&; buttons: the ReAction classes are not available on this system (AmigaOS 3.5+/3.2 or MorphOS needed) — nothing to show.~%"))
