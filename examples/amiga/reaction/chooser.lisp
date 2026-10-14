;;; chooser.lisp — chooser.gadget with a label list.
;;;
;;; Common Lisp port of the NDK 3.2 example Examples/Chooser1.c: a
;;; window.class window with a chooser.gadget ("Baud Rate") whose entries
;;; are an exec list of chooser nodes, a label.image as the child label,
;;; and a Quit button.  Selecting an entry prints it — the GADGETUP's
;;; Code word is the index of the new selection.
;;;
;;; What it shows: building a label list with NEW-LIST, ALLOC-CHOOSER-NODE-A
;;; through WITH-TAGS and ADD-TAIL, CHILD_Label, freeing the nodes after
;;; the gadget is gone (FREE-LIST-NODES).
;;;
;;; Run on AmigaOS 3.5+/3.2 or MorphOS (ReAction classes required):
;;;   amilisp --load examples/amiga/reaction/chooser.lisp

(require "amiga/reaction")
(require "amiga/raw/exec")
(require "amiga/raw/intuition")
(require "amiga/raw/classes/window")
(require "amiga/raw/gadgets/layout")
(require "amiga/raw/gadgets/button")
(require "amiga/raw/gadgets/chooser")
(require "amiga/raw/images/label")

(defpackage "REACTION-CHOOSER"
  (:use "CL")
  (:local-nicknames ("RA"      "AMIGA.REACTION")
                    ("EXEC"    "AMIGA.RAW.EXEC")
                    ("INTUI"   "AMIGA.RAW.INTUITION")
                    ("WIN"     "AMIGA.RAW.CLASSES.WINDOW")
                    ("LAYOUT"  "AMIGA.RAW.GADGETS.LAYOUT")
                    ("BUTTON"  "AMIGA.RAW.GADGETS.BUTTON")
                    ("CHOOSER" "AMIGA.RAW.GADGETS.CHOOSER")
                    ("LABEL"   "AMIGA.RAW.IMAGES.LABEL")))

(in-package "REACTION-CHOOSER")

(defconstant +gid-chooser+ 1)
(defconstant +gid-quit+    2)

(defparameter *baud-rates* '("1200" "2400" "4800" "9600" "19200" "38400" "57600"))

(defun make-chooser-list (labels)
  "An exec list of chooser nodes, one per label (what ChooserLabels()
in the C does).  The node keeps the string pointer, so the strings are
pooled; the nodes are freed by FREE-CHOOSER-LIST-NODES."
  (let ((list (ra:new-list)))
    (dolist (text labels list)
      (ra:with-tags (tags chooser:+cna-text+ text)
        (let ((node (chooser:alloc-chooser-node-a tags)))
          (unless node (error "chooser: AllocChooserNodeA failed"))
          (exec:add-tail list node))))))

(defun free-chooser-list-nodes (list)
  (ra:free-list-nodes list #'chooser:free-chooser-node))

(defun make-window-object (chooser-list)
  (ra:new-object (win:window-get-class)
    intui:+wa-screen-title+ "ReAction"
    intui:+wa-title+ "ReAction Chooser Example 1"
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
      layout:+layout-add-child+
      (ra:new-object (chooser:chooser-get-class)
                     intui:+ga-id+ +gid-chooser+
                     intui:+ga-rel-verify+ t
                     chooser:+chooser-labels+ chooser-list
                     chooser:+chooser-selected+ 0)
      layout:+child-nominal-size+ t
      ;; CHILD_Label: a label.image drawn beside the preceding child
      layout:+child-label+
      (ra:new-object (label:label-get-class)
                     label:+label-text+ "_Baud Rate")
      layout:+layout-add-child+
      (ra:new-object (button:button-get-class)
                     intui:+ga-id+ +gid-quit+
                     intui:+ga-rel-verify+ t
                     intui:+ga-text+ "_Quit")
      layout:+child-weighted-height+ 0)))

(defun run ()
  (ra:with-foreign-pool ()
    (let* ((chooser-list (make-chooser-list *baud-rates*))
           (win-obj (make-window-object chooser-list)))
      (unwind-protect
           (progn
             (unless (ra:open-window win-obj)
               (error "chooser: could not open the window"))
             (ra:do-window-events ((result code) win-obj)
               (let ((class (logand result win:+wmhi-classmask+)))
                 (cond ((= class win:+wmhi-closewindow+)
                        (return))
                       ((= class win:+wmhi-gadgetup+)
                        (let ((gid (logand result win:+wmhi-gadgetmask+)))
                          (cond ((= gid +gid-quit+)
                                 (return))
                                ((= gid +gid-chooser+)
                                 ;; Code is the index of the chosen entry
                                 (format t "~&; baud rate ~A selected (entry ~D)~%"
                                         (nth code *baud-rates*) code)))))))))
        ;; The gadget must be gone before its nodes are freed: dispose
        ;; the window object first.
        (ra:dispose-object win-obj)
        (free-chooser-list-nodes chooser-list)))))

(if (ra:available-p)
    (run)
    (format t "~&; chooser: the ReAction classes are not available on this system (AmigaOS 3.5+/3.2 or MorphOS needed) — nothing to show.~%"))
