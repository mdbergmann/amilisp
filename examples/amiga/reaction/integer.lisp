;;; integer.lisp — integer.gadget: numeric entry with and without arrows.
;;;
;;; Common Lisp port of the NDK 3.2 example Examples/Integer.c: a
;;; window.class window with two integer.gadget fields — one with
;;; increment arrows and a -32..32 range, one plain with 0..100000 — each
;;; with a label.image, and a Quit button.  Tab cycles between the
;;; fields; the first one is activated when the window opens
;;; (ActivateLayoutGadget).  Leaving a field prints its value, read back
;;; with GET-ATTR INTEGER_Number.
;;;
;;; Run on AmigaOS 3.5+/3.2 or MorphOS (ReAction classes required):
;;;   amilisp --load examples/amiga/reaction/integer.lisp

(require "amiga/reaction")
(require "amiga/raw/intuition")
(require "amiga/raw/classes/window")
(require "amiga/raw/gadgets/layout")
(require "amiga/raw/gadgets/button")
(require "amiga/raw/gadgets/integer")
(require "amiga/raw/images/label")

(defpackage "REACTION-INTEGER"
  (:use "CL")
  (:local-nicknames ("RA"      "AMIGA.REACTION")
                    ("INTUI"   "AMIGA.RAW.INTUITION")
                    ("WIN"     "AMIGA.RAW.CLASSES.WINDOW")
                    ("LAYOUT"  "AMIGA.RAW.GADGETS.LAYOUT")
                    ("BUTTON"  "AMIGA.RAW.GADGETS.BUTTON")
                    ("INTEGER" "AMIGA.RAW.GADGETS.INTEGER")
                    ("LABEL"   "AMIGA.RAW.IMAGES.LABEL")))

(in-package "REACTION-INTEGER")

(defconstant +gid-integer1+ 1)
(defconstant +gid-integer2+ 2)
(defconstant +gid-quit+     3)

(defvar *main-group* nil "The WINDOW_ParentGroup layout.")
(defvar *integer1* nil "The first integer gadget (activated on open).")
(defvar *integer2* nil)

(defun make-label (text)
  (ra:new-object (label:label-get-class) label:+label-text+ text))

(defun make-window-object ()
  (setf *integer1* (ra:new-object (integer:integer-get-class)
                                  intui:+ga-id+ +gid-integer1+
                                  intui:+ga-rel-verify+ t
                                  intui:+ga-tab-cycle+ t
                                  integer:+integer-arrows+ t
                                  integer:+integer-max-chars+ 3
                                  integer:+integer-minimum+ -32
                                  integer:+integer-maximum+ 32
                                  integer:+integer-number+ 0)
        *integer2* (ra:new-object (integer:integer-get-class)
                                  intui:+ga-id+ +gid-integer2+
                                  intui:+ga-rel-verify+ t
                                  intui:+ga-tab-cycle+ t
                                  integer:+integer-arrows+ nil
                                  integer:+integer-max-chars+ 6
                                  integer:+integer-minimum+ 0
                                  integer:+integer-maximum+ 100000
                                  integer:+integer-number+ 100)
        *main-group* (ra:new-object (layout:layout-get-class)
                                    layout:+layout-orientation+ layout:+layout-vertical+
                                    layout:+layout-space-outer+ t
                                    layout:+layout-defer-layout+ t
                                    layout:+layout-add-child+ *integer1*
                                    layout:+child-nominal-size+ t
                                    layout:+child-label+ (make-label "Integer _1")
                                    layout:+layout-add-child+ *integer2*
                                    layout:+child-label+ (make-label "Integer _2")
                                    layout:+layout-add-child+
                                    (ra:new-object (button:button-get-class)
                                                   intui:+ga-id+ +gid-quit+
                                                   intui:+ga-rel-verify+ t
                                                   intui:+ga-text+ "_Quit")
                                    layout:+child-weighted-height+ 0))
  (ra:new-object (win:window-get-class)
                 intui:+wa-screen-title+ "ReAction"
                 intui:+wa-title+ "ReAction Integer Example"
                 intui:+wa-activate+ t
                 intui:+wa-depth-gadget+ t
                 intui:+wa-drag-bar+ t
                 intui:+wa-close-gadget+ t
                 intui:+wa-size-gadget+ t
                 win:+window-position+ win:+wpos-centermouse+
                 win:+window-parent-group+ *main-group*))

(defun signed32 (n)
  "INTEGER_Number is a LONG; GET-ATTR hands it back as an unsigned longword."
  (if (logbitp 31 n) (- n #x100000000) n))

(defun run ()
  (ra:with-foreign-pool ()
    (let ((win-obj (make-window-object)))
      (unwind-protect
           (let ((window (ra:open-window win-obj)))
             (unless window
               (error "integer: could not open the window"))
             ;; Put the cursor into the first field right away.
             (layout:activate-layout-gadget *main-group* window nil *integer1*)
             (ra:do-window-events ((result code) win-obj)
               (let ((class (logand result win:+wmhi-classmask+)))
                 (cond ((= class win:+wmhi-closewindow+)
                        (return))
                       ((= class win:+wmhi-gadgetup+)
                        (let ((gid (logand result win:+wmhi-gadgetmask+)))
                          (cond ((= gid +gid-quit+)
                                 (return))
                                ((= gid +gid-integer1+)
                                 (format t "~&; integer 1 = ~D~%"
                                         (signed32 (ra:get-attr integer:+integer-number+ *integer1*))))
                                ((= gid +gid-integer2+)
                                 (format t "~&; integer 2 = ~D~%"
                                         (signed32 (ra:get-attr integer:+integer-number+ *integer2*)))))))))))
        (ra:dispose-object win-obj)
        (setf *main-group* nil *integer1* nil *integer2* nil)))))

(if (ra:available-p)
    (run)
    (format t "~&; integer: the ReAction classes are not available on this system (AmigaOS 3.5+/3.2 or MorphOS needed) — nothing to show.~%"))
