;;; layout.lisp — the port of the MUI SDK's Layout.c: a *custom layout
;;; hook*.  A group hands its layout to a MUIA_Group_LayoutHook -- here a
;;; Lisp function -- which MUI calls twice for two different questions:
;;; MUILM_MINMAX ("how big do you want to be?", answered from the sizes
;;; the children already asked for) and MUILM_LAYOUT ("place your
;;; children"), where this one scatters them at random inside the
;;; rectangle it was given.  Any other lm_Type must be answered with
;;; MUILM_UNKNOWN.
;;;
;;; The game the C plays with them is here too: eight buttons whose
;;; MUIA_Pressed notification calls a second Lisp hook through
;;; MUIM_CallHook with the button's number; click them in order and a
;;; hidden ninth button appears (MUIA_ShowMe), click that to quit.  Get
;;; the order wrong and it beeps and starts over.  Overlapping objects
;;; are, as the C's own comment says, usually a bad idea -- a real layout
;;; hook is more sophisticated; this one shows the mechanism.
;;;
;;; What it shows: MUIA_Group_LayoutHook with an AMIGA.MUI:POOL-HOOK,
;;; LAYOUT-MSG-TYPE / LAYOUT-MSG-WIDTH / LAYOUT-MSG-HEIGHT and
;;; LAYOUT-MSG-MIN-MAX with SET-MIN-MAX, LAYOUT-CHILDREN (the
;;; NextObject() walk of lm_Children), AREA-MIN-WIDTH / AREA-MIN-HEIGHT
;;; (mui.h's _minwidth / _minheight), LAYOUT-CHILD (MUI_Layout),
;;; MUI_MAXMAX, MUIM_CallHook with a parameter, MUIA_ShowMe driven from a
;;; return ID.
;;;
;;; The hooks run inside MUI, on its stack: an error in one is caught at
;;; the callback, the hook returns 0, and the condition is re-signaled in
;;; Lisp when the MUI call that invoked it returns -- for the layout hook
;;; that is the SET-ATTRS that opens the window, for the press hook the
;;; APPLICATION-INPUT inside the event loop.
;;;
;;; Run on AmigaOS 3.x with MUI 3.8+ installed, or on MorphOS:
;;;   amilisp --load examples/amiga/mui/layout.lisp
;;; Unattended (auto-closes after 5 s):
;;;   amilisp --eval '(require "amiga/mui")' \
;;;           --eval '(setf amiga.mui:*event-loop-timeout* 5)' \
;;;           --load examples/amiga/mui/layout.lisp

(require "amiga/mui")
(require "amiga/raw/intuition")    ; DisplayBeep

(when (amiga.mui:available-p)
  (require "amiga/raw/muimaster"))

(defpackage "MUI-LAYOUT"
  (:use "CL")
  (:local-nicknames ("MUI" "AMIGA.MUI")
                    ("INTUI" "AMIGA.RAW.INTUITION")))

(in-package "MUI-LAYOUT")

(defun m (name)
  "The value of the amiga/raw/muimaster constant NAME -- looked up at run
time, so this file reads on a system where that package does not exist."
  (symbol-value (or (find-symbol name "AMIGA.RAW.MUIMASTER")
                    (error "layout: amiga.raw.muimaster:~A not found" name))))

(defconstant +id-reward+ 1)             ; ID_REWARD

(defparameter *button-labels*
  '("Click" "me" "in" "correct" "sequence" "to" "be" "rewarded!"))

;;; The mui.h macros this program uses, as functions.

(defun children (objects)
  (loop for o in objects collect (m "+MUIA-GROUP-CHILD+") collect o))

(defun vgroup (tags &rest objects)
  (apply #'mui:new-object :group (append tags (children objects))))

;;; ---------------------------------------------------------------------
;;; The custom layout function
;;; ---------------------------------------------------------------------

(defun layout-min-max (message)
  "MUILM_MINMAX: MUI has already asked our children how big they want to
be, so their AREA-MIN-WIDTH / AREA-MIN-HEIGHT are known and we can size
ourselves from them.  As in the C, we ask for twice the biggest child in
each direction, a default of four times that, and no maximum."
  (let ((maxmax (m "+MUI-MAXMAX+"))
        (max-min-width 0)
        (max-min-height 0))
    (dolist (child (mui:layout-children message))
      (let ((w (mui:area-min-width child))
            (h (mui:area-min-height child)))
        (when (and (< max-min-width maxmax) (> w max-min-width))
          (setf max-min-width w))
        (when (and (< max-min-height maxmax) (> h max-min-height))
          (setf max-min-height h))))
    (mui:set-min-max (mui:layout-msg-min-max message)
                     :min-width  (* 2 max-min-width)
                     :min-height (* 2 max-min-height)
                     :def-width  (* 4 max-min-width)
                     :def-height (* 4 max-min-height)
                     :max-width  maxmax
                     :max-height maxmax))
  0)

(defun layout-place (message)
  "MUILM_LAYOUT: place every child inside the rectangle
\(0, 0, width-1, height-1) MUI gave us.  A group is free to put them
anywhere in there; this one throws them at random positions, at their
minimum size.  Returns T when all of them were placed, NIL on the first
refusal -- errors during layout are hard for MUI to recover from."
  (let ((width  (mui:layout-msg-width message))
        (height (mui:layout-msg-height message)))
    (dolist (child (mui:layout-children message) t)
      (let* ((mw (mui:area-min-width child))
             (mh (mui:area-min-height child))
             (left (random (max 1 (- width mw))))
             (top  (random (max 1 (- height mh)))))
        (unless (mui:layout-child child left top mw mh)
          (return nil))))))

(defun make-layout-hook ()
  "The MUIA_Group_LayoutHook: a struct Hook whose entry is a Lisp
function of (hook object message).  The lm_Types are fetched once,
outside the function MUI calls for every layout pass."
  (let ((minmax-type (m "+MUILM-MINMAX+"))
        (layout-type (m "+MUILM-LAYOUT+"))
        (unknown     (m "+MUILM-UNKNOWN+")))
    (mui:pool-hook
     (lambda (hook object message)
       (declare (ignore hook object))
       (let ((type (mui:layout-msg-type message)))
         (cond ((= type minmax-type) (layout-min-max message))
               ((= type layout-type) (layout-place message))
               (t unknown)))))))

;;; ---------------------------------------------------------------------
;;; The button game — a second hook, called through MUIM_CallHook
;;; ---------------------------------------------------------------------

(defun make-press-hook ()
  "The C's PressFunc, with its `static int lastnum` as a closure
variable: MUIM_CallHook hands us the button's number in the message's
first longword, and the object the method was sent to -- the application,
because that is the notification's destination -- in the hook's second
argument.  The buttons have to arrive in order 0 .. 7; the eighth in a
row queues ID_REWARD for the event loop."
  (let ((last-num -1))
    (mui:pool-hook
     (lambda (hook app message)
       (declare (ignore hook))
       (let ((n (ffi:peek-u32 message 0)))
         (incf last-num)
         (cond ((/= last-num n)
                (intui:display-beep nil)
                (setf last-num -1))
               ((= last-num 7)
                (mui:return-id app +id-reward+)
                (setf last-num -1))))
       0))))

;;; ---------------------------------------------------------------------

(defun run ()
  (setf *random-state* (make-random-state t))   ; the C's srand(time(0))
  (mui:with-foreign-pool ()
    (let* ((layout-hook (make-layout-hook))
           (press-hook  (make-press-hook))
           (buttons (mapcar (lambda (label) (mui:make-object :button label))
                            *button-labels*))
           (yeah (mui:make-object :button
                                  (format nil "Yeah!~%You did it!~%Click to quit!")))
           (win (mui:new-object :window
                  (m "+MUIA-WINDOW-TITLE+") "Custom Layout"
                  (m "+MUIA-WINDOW-ID+")    (mui:make-id "CLS3")
                  (m "+MUIA-WINDOW-ROOT-OBJECT+")
                  (vgroup '()
                    (mui:new-object :text
                      (m "+MUIA-FRAME+") (m "+MUIV-FRAME-TEXT+")
                      (m "+MUIA-BACKGROUND+") (m "+MUII-TEXT-BACK+")
                      (m "+MUIA-TEXT-CONTENTS+")
                      (concatenate 'string (m "+MUIX-C+")
                                   (format nil "Demonstration of a custom layout hook.~%Since it's usually no good idea to have overlapping~%objects, your hooks should be more sophisticated.")))
                    ;; the group whose layout is ours: GroupFrame plus
                    ;; MUIA_Group_LayoutHook, and the nine children
                    (apply #'vgroup
                           (list (m "+MUIA-FRAME+") (m "+MUIV-FRAME-GROUP+")
                                 (m "+MUIA-GROUP-LAYOUT-HOOK+") layout-hook)
                           (append buttons (list yeah))))))
           (app (mui:new-object :application
                  (m "+MUIA-APPLICATION-TITLE+")       "Layout"
                  (m "+MUIA-APPLICATION-VERSION+")     "$VER: Layout 19.5 (12.02.97)"
                  (m "+MUIA-APPLICATION-COPYRIGHT+")   "(C) 1993, Stefan Stuntz"
                  (m "+MUIA-APPLICATION-AUTHOR+")      "Stefan Stuntz"
                  (m "+MUIA-APPLICATION-DESCRIPTION+") "Demonstrate custom layout hooks."
                  (m "+MUIA-APPLICATION-BASE+")        "Layout"
                  (m "+MUIA-APPLICATION-WINDOW+")      win)))
      (unwind-protect
           (progn
             (mui:notify win (m "+MUIA-WINDOW-CLOSE-REQUEST+") t
                         :application (m "+MUIM-APPLICATION-RETURN-ID+")
                         (m "+MUIV-APPLICATION-RETURN-ID-QUIT+"))
             (mui:notify yeah (m "+MUIA-PRESSED+") nil
                         :application (m "+MUIM-APPLICATION-RETURN-ID+")
                         (m "+MUIV-APPLICATION-RETURN-ID-QUIT+"))
             ;; DoMethod(b[i], MUIM_Notify, MUIA_Pressed, FALSE,
             ;;          app, 3, MUIM_CallHook, &PressHook, i)
             (loop for button in buttons
                   for i from 0
                   do (mui:notify button (m "+MUIA-PRESSED+") nil
                                  :application (m "+MUIM-CALL-HOOK+") press-hook i))
             (mui:set-attrs yeah (m "+MUIA-SHOW-ME+") nil)
             (mui:set-attrs win (m "+MUIA-WINDOW-OPEN+") t)
             (when (zerop (or (mui:get-attr (m "+MUIA-WINDOW-OPEN+") win) 0))
               (error "layout: the window would not open"))
             ;; ID_REWARD comes out of the press hook (it queued it with
             ;; MUIM_Application_ReturnID); MUIV_Application_ReturnID_Quit
             ;; ends the loop by itself
             (mui:do-application-events ((id) app)
               (when (= id +id-reward+)
                 (mui:set-attrs yeah (m "+MUIA-SHOW-ME+") t)))
             (mui:set-attrs win (m "+MUIA-WINDOW-OPEN+") nil))
        (mui:dispose-object app)))))          ; then the pool frees the hooks

(if (mui:available-p)
    (run)
    (format t "~&; layout: MUI is not available on this system (muimaster.library 3.8+ on AmigaOS, or MorphOS, needed) — nothing to show.~%"))
