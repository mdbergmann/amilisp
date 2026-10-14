;;; checkbox.lisp — checkbox.gadget, label.image, an iconifiable window.
;;;
;;; Common Lisp port of the NDK 3.2 example Examples/CheckBox.c: a
;;; window.class window with two checkbox.gadget objects (label to the
;;; right of one, to the left of the other), a bevelled group holding a
;;; multi-line label.image, and a Quit button.  The window has an iconify
;;; gadget: WMHI_ICONIFY turns it into a Workbench icon, double-clicking
;;; the icon (WMHI_UNICONIFY, delivered through the WINDOW_AppPort) opens
;;; it again.  Toggling a checkbox prints its new state — the Code word
;;; of the GADGETUP carries it.
;;;
;;; Run on AmigaOS 3.5+/3.2 or MorphOS (ReAction classes required):
;;;   amilisp --load examples/amiga/reaction/checkbox.lisp

(require "amiga/reaction")
(require "amiga/raw/exec")
(require "amiga/raw/intuition")
(require "amiga/raw/gadtools")          ; PLACETEXT_* (libraries/gadtools.h)
(require "amiga/raw/classes/window")
(require "amiga/raw/gadgets/layout")
(require "amiga/raw/gadgets/button")
(require "amiga/raw/gadgets/checkbox")
(require "amiga/raw/images/label")
(require "amiga/raw/images/bevel")      ; BVS_* bevel styles

(defpackage "REACTION-CHECKBOX"
  (:use "CL")
  (:local-nicknames ("RA"       "AMIGA.REACTION")
                    ("EXEC"     "AMIGA.RAW.EXEC")
                    ("INTUI"    "AMIGA.RAW.INTUITION")
                    ("GT"       "AMIGA.RAW.GADTOOLS")
                    ("WIN"      "AMIGA.RAW.CLASSES.WINDOW")
                    ("LAYOUT"   "AMIGA.RAW.GADGETS.LAYOUT")
                    ("BUTTON"   "AMIGA.RAW.GADGETS.BUTTON")
                    ("CHECKBOX" "AMIGA.RAW.GADGETS.CHECKBOX")
                    ("LABEL"    "AMIGA.RAW.IMAGES.LABEL")
                    ("BEVEL"    "AMIGA.RAW.IMAGES.BEVEL")))

(in-package "REACTION-CHECKBOX")

;; Gadget IDs (the C enum GID_*)
(defconstant +gid-checkbox1+ 1)
(defconstant +gid-checkbox2+ 2)
(defconstant +gid-quit+      3)

(defun vgroup (&rest tags)
  (apply #'ra:new-object (layout:layout-get-class)
         layout:+layout-orientation+ layout:+layout-vertical+ tags))

(defun line (text)
  "TEXT with the newline label.image uses to break lines."
  (format nil "~A~%" text))

(defun make-window-object (app-port)
  (ra:new-object (win:window-get-class)
    intui:+wa-screen-title+ "ReAction"
    intui:+wa-title+ "ReAction CheckBox Example"
    intui:+wa-activate+ t
    intui:+wa-depth-gadget+ t
    intui:+wa-drag-bar+ t
    intui:+wa-close-gadget+ t
    intui:+wa-size-gadget+ t
    win:+window-iconify-gadget+ t
    win:+window-icon-title+ "CheckBox"
    win:+window-app-port+ app-port
    win:+window-position+ win:+wpos-centermouse+
    win:+window-parent-group+
    (vgroup
     layout:+layout-space-outer+ t
     layout:+layout-defer-layout+ t
     layout:+layout-add-child+
     (ra:new-object (checkbox:checkbox-get-class)
                    intui:+ga-id+ +gid-checkbox1+
                    intui:+ga-rel-verify+ t
                    intui:+ga-text+ "CheckBox _1"
                    checkbox:+checkbox-text-place+ gt:+placetext-right+)
     layout:+child-nominal-size+ t
     layout:+layout-add-child+
     (ra:new-object (checkbox:checkbox-get-class)
                    intui:+ga-id+ +gid-checkbox2+
                    intui:+ga-rel-verify+ t
                    intui:+ga-text+ "CheckBox _2"
                    checkbox:+checkbox-text-place+ gt:+placetext-left+)
     layout:+layout-add-child+
     (vgroup
      layout:+layout-back-fill+ nil
      layout:+layout-space-outer+ t
      layout:+layout-vert-alignment+ layout:+lalign-center+
      layout:+layout-horiz-alignment+ layout:+lalign-center+
      layout:+layout-bevel-style+ bevel:+bvs-field+
      ;; LAYOUT_AddImage: an image (not a gadget) as a layout member.
      ;; label.image concatenates every LABEL_Text it is given.
      layout:+layout-add-image+
      (ra:new-object (label:label-get-class)
                     label:+label-text+ (line "The checkbox may have its label placed")
                     label:+label-text+ (line "either on the left or right side.")
                     label:+label-text+ (line " ")
                     label:+label-text+ (line "You may click the label text as well")
                     label:+label-text+ (line "as the check box itself.")))
     layout:+layout-add-child+
     (ra:new-object (button:button-get-class)
                    intui:+ga-id+ +gid-quit+
                    intui:+ga-rel-verify+ t
                    intui:+ga-text+ "_Quit")
     layout:+child-weighted-height+ 0)))

(defun run ()
  (ra:with-foreign-pool ()
    ;; The AppPort receives the messages of the iconified window's icon;
    ;; window.class reads it, we only have to wait on its signal too.
    (let* ((app-port (or (exec:create-msg-port)
                         (error "checkbox: CreateMsgPort failed")))
           (app-signal (ash 1 (exec:msg-port-sigbit app-port)))
           (win-obj (make-window-object app-port)))
      (unwind-protect
           (progn
             (unless (ra:open-window win-obj)
               (error "checkbox: could not open the window"))
             (ra:do-window-events ((result code) win-obj :signals app-signal)
               (let ((class (logand result win:+wmhi-classmask+)))
                 (cond ((= class win:+wmhi-closewindow+)
                        (return))
                       ((= class win:+wmhi-gadgetup+)
                        (let ((gid (logand result win:+wmhi-gadgetmask+)))
                          (cond ((= gid +gid-quit+)
                                 (return))
                                ((or (= gid +gid-checkbox1+) (= gid +gid-checkbox2+))
                                 ;; Code is TRUE/FALSE: the new check state
                                 (format t "~&; checkbox ~D is now ~:[off~;on~]~%"
                                         gid (not (zerop code)))))))
                       ((= class win:+wmhi-iconify+)
                        (ra:iconify win-obj))
                       ((= class win:+wmhi-uniconify+)
                        (unless (ra:open-window win-obj)
                          (error "checkbox: could not re-open the window")))))))
        (ra:dispose-object win-obj)
        (exec:delete-msg-port app-port)))))

(if (ra:available-p)
    (run)
    (format t "~&; checkbox: the ReAction classes are not available on this system (AmigaOS 3.5+/3.2 or MorphOS needed) — nothing to show.~%"))
