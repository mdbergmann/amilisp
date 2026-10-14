;;; class1.lisp — a MUI custom class written in Lisp: the port of the
;;; MUI SDK's Class1.c, "the simplest possible MUI class".  A subclass of
;;; Area.mui that answers two methods -- MUIM_AskMinMax, where it adds its
;;; own size wishes to what the superclass asked for, and MUIM_Draw, where
;;; it draws a fan of lines into its rectangle with graphics.library --
;;; and passes everything else to its superclass.  One object of the
;;; class fills the window; resize the window to watch it redraw.
;;;
;;; What it shows: AMIGA.MUI:CREATE-CUSTOM-CLASS (MUI_CreateCustomClass
;;; with a Lisp dispatcher, deleted by the foreign pool), METHOD-ID,
;;; DO-SUPER-METHOD (DoSuperMethodA), ADD-MIN-MAX on the MUIM_AskMinMax
;;; message, DRAW-FLAGS / MADF_DRAWOBJECT, the mui.h shortcuts _rp(obj),
;;; _dri(obj), _mleft(obj) ... as AREA-RASTPORT / AREA-DRAW-INFO /
;;; AREA-MLEFT ..., NEW-OBJECT of a private class (CUSTOM-CLASS-CLASS),
;;; and graphics.library calls from inside a MUI method.
;;;
;;; The dispatcher runs inside MUI, on its stack: an error in a method is
;;; caught at the callback, the method returns 0, and the condition is
;;; re-signaled in Lisp when the MUI call that dispatched it returns.
;;;
;;; Run on AmigaOS 3.x with MUI 3.8+ installed, or on MorphOS:
;;;   amilisp --load examples/amiga/mui/class1.lisp
;;; Unattended (auto-closes after 5 s):
;;;   amilisp --eval '(require "amiga/mui")' \
;;;           --eval '(setf amiga.mui:*event-loop-timeout* 5)' \
;;;           --load examples/amiga/mui/class1.lisp

(require "amiga/mui")
(require "amiga/raw/graphics")     ; loads everywhere; SetAPen / Move / Draw
(require "amiga/raw/intuition")    ; struct DrawInfo accessors, TEXTPEN

(when (amiga.mui:available-p)
  (require "amiga/raw/muimaster"))

(defpackage "MUI-CLASS1"
  (:use "CL")
  (:local-nicknames ("MUI" "AMIGA.MUI")
                    ("GFX" "AMIGA.RAW.GRAPHICS")
                    ("INTUI" "AMIGA.RAW.INTUITION")))

(in-package "MUI-CLASS1")

(defun m (name)
  "The value of the amiga/raw/muimaster constant NAME -- looked up at run
time, so this file reads on a system where that package does not exist."
  (symbol-value (or (find-symbol name "AMIGA.RAW.MUIMASTER")
                    (error "class1: amiga.raw.muimaster:~A not found" name))))

;;; The two methods, as the C file has them.

(defun ask-min-max (class object message)
  "MUIM_AskMinMax: MUI asks before the window opens and before layout.
Let the superclass fill in the frame and inner spacing first, then ADD
our own minimum, default and maximum sizes (never set them)."
  (mui:do-super-method class object message)
  (mui:add-min-max message
                   :min-width 100 :def-width 120 :max-width 500
                   :min-height 40 :def-height 90 :max-height 300)
  0)

(defun draw (class object message)
  "MUIM_Draw: called whenever MUI wants us rendered -- after layout, or
for a refresh.  The superclass draws the frame and clears the region;
only MADF_DRAWOBJECT asks for the object itself.  Render strictly inside
_mleft/_mtop/_mwidth/_mheight."
  (mui:do-super-method class object message)
  (when (logtest (mui:draw-flags message) (m "+MADF-DRAWOBJECT+"))
    (let* ((rp     (mui:area-rastport object))                 ; _rp(obj)
           (dri    (mui:area-draw-info object))                ; _dri(obj)
           (pens   (intui:draw-info-pens dri))                 ; dri->dri_Pens
           (textpen (ffi:peek-u16 pens (* 2 intui:+textpen+)))
           (left   (mui:area-mleft object))
           (right  (mui:area-mright object))
           (top    (mui:area-mtop object))
           (bottom (mui:area-mbottom object)))
      (gfx:set-a-pen rp textpen)
      (loop for i from left to right by 5
            do (gfx:move rp left bottom)
               (gfx:draw rp i top)
               (gfx:move rp right bottom)
               (gfx:draw rp i top))))
  0)

(defun make-dispatcher-function ()
  "The dispatcher: MUIM_AskMinMax and MUIM_Draw are ours, every other
method goes to the superclass at once.  The method IDs are fetched once,
outside the function MUI calls for every method."
  (let ((ask-min-max-id (m "+MUIM-ASK-MIN-MAX+"))
        (draw-id        (m "+MUIM-DRAW+")))
    (lambda (class object message)
      (let ((id (mui:method-id message)))
        (cond ((= id ask-min-max-id) (ask-min-max class object message))
              ((= id draw-id)        (draw class object message))
              (t (mui:do-super-method class object message)))))))

(defun run ()
  (mui:with-foreign-pool ()
    ;; MUI_CreateCustomClass(NULL, MUIC_Area, NULL, sizeof(struct MyData),
    ;; MyDispatcher) -- the class is deleted when the pool exits, after the
    ;; application (and so the object) is disposed below.  The C struct
    ;; MyData is one dummy LONG: 4 bytes of instance data nobody reads.
    (let* ((mcc (mui:create-custom-class :area (make-dispatcher-function)
                                         :data-size 4))
           (my-object (mui:new-object (mui:custom-class-class mcc)   ; NewObject(mcc->mcc_Class, NULL, ...)
                        (m "+MUIA-FRAME+") (m "+MUIV-FRAME-TEXT+")     ; TextFrame
                        (m "+MUIA-BACKGROUND+") (m "+MUII-BACKGROUND+")))
           (win (mui:new-object :window
                  (m "+MUIA-WINDOW-TITLE+") "A Simple Custom Class"
                  (m "+MUIA-WINDOW-ID+")    (mui:make-id "CLS1")
                  (m "+MUIA-WINDOW-ROOT-OBJECT+")
                  (mui:new-object :group                                ; VGroup
                    (m "+MUIA-GROUP-CHILD+") my-object)))
           (app (mui:new-object :application
                  (m "+MUIA-APPLICATION-TITLE+")       "Class1"
                  (m "+MUIA-APPLICATION-VERSION+")     "$VER: Class1 1.0"
                  (m "+MUIA-APPLICATION-COPYRIGHT+")   "1993, Stefan Stuntz (port: AmiLisp)"
                  (m "+MUIA-APPLICATION-AUTHOR+")      "Stefan Stuntz"
                  (m "+MUIA-APPLICATION-DESCRIPTION+") "Demonstrate the use of custom classes."
                  (m "+MUIA-APPLICATION-BASE+")        "CLASS1"
                  (m "+MUIA-APPLICATION-WINDOW+")      win)))
      (unwind-protect
           (progn
             (mui:notify win (m "+MUIA-WINDOW-CLOSE-REQUEST+") t
                         :application (m "+MUIM-APPLICATION-RETURN-ID+")
                         (m "+MUIV-APPLICATION-RETURN-ID-QUIT+"))
             (mui:set-attrs win (m "+MUIA-WINDOW-OPEN+") t)
             (when (zerop (or (mui:get-attr (m "+MUIA-WINDOW-OPEN+") win) 0))
               (error "class1: the window would not open"))
             ;; the ideal loop of an object oriented MUI program: nothing
             ;; but the quit check -- the class does the rest
             (mui:do-application-events ((id) app)
               nil)                   ; no return IDs of our own
             (mui:set-attrs win (m "+MUIA-WINDOW-OPEN+") nil))
        (mui:dispose-object app)))))          ; then the pool deletes the class

(if (mui:available-p)
    (run)
    (format t "~&; class1: MUI is not available on this system (muimaster.library 3.8+ on AmigaOS, or MorphOS, needed) — nothing to show.~%"))
