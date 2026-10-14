;;; group-layout.lisp — what MUI's own layout engine does with the group
;;; attributes: orientation, columns, weights, same-size, frames and
;;; backgrounds.
;;;
;;; This is the other side of layout.lisp, the port of the MUI SDK's
;;; Layout.c: there a program takes the layout over with a custom hook,
;;; here it states its intentions with attributes and lets MUI do all of
;;; it.  The window is resizable -- drag its corner to watch the weights
;;; and columns follow.
;;;
;;; What it shows: MUIA_Group_Horiz / MUIA_Group_Columns /
;;; MUIA_Group_SameSize, MUIA_Weight, MUIA_FixHeight, every MUIV_Frame_*
;;; on a text object, MUIA_Background with the MUII_* pens and patterns,
;;; MUIA_FrameTitle (the C's GroupFrameT), MUIO_Label flags.
;;;
;;; Run on AmigaOS 3.x with MUI 3.8+ installed, or on MorphOS:
;;;   amilisp --load examples/amiga/mui/group-layout.lisp
;;; Unattended (auto-closes after 5 s):
;;;   amilisp --eval '(require "amiga/mui")' \
;;;           --eval '(setf amiga.mui:*event-loop-timeout* 5)' \
;;;           --load examples/amiga/mui/group-layout.lisp

(require "amiga/mui")

(when (amiga.mui:available-p)
  (require "amiga/raw/muimaster"))

(defpackage "MUI-GROUP-LAYOUT"
  (:use "CL")
  (:local-nicknames ("MUI" "AMIGA.MUI")))

(in-package "MUI-GROUP-LAYOUT")

(defun m (name)
  "The value of the amiga/raw/muimaster constant NAME -- looked up at run
time, so this file reads on a system where that package does not exist."
  (symbol-value (or (find-symbol name "AMIGA.RAW.MUIMASTER")
                    (error "group-layout: amiga.raw.muimaster:~A not found" name))))

;;; The mui.h macros this program uses, as functions.

(defun children (objects)
  "Child, obj, Child, obj ... -- MUIA_Group_Child before every object."
  (loop for o in objects collect (m "+MUIA-GROUP-CHILD+") collect o))

(defun vgroup (tags &rest objects)
  "VGroup, tags..., Child, ..., End"
  (apply #'mui:new-object :group (append tags (children objects))))

(defun hgroup (tags &rest objects)
  "HGroup: a group with MUIA_Group_Horiz."
  (apply #'vgroup (list* (m "+MUIA-GROUP-HORIZ+") t tags) objects))

(defun col-group (columns tags &rest objects)
  "ColGroup(n): MUIA_Group_Columns lays the children out row by row."
  (apply #'vgroup (list* (m "+MUIA-GROUP-COLUMNS+") columns tags) objects))

(defun group-frame-t (title)
  "GroupFrameT(title): a group frame with a title on the group background."
  (list (m "+MUIA-FRAME+") (m "+MUIV-FRAME-GROUP+")
        (m "+MUIA-FRAME-TITLE+") title
        (m "+MUIA-BACKGROUND+") (m "+MUII-GROUP-BACK+")))

(defun centered (text)
  "MUIX_C -- the text engine's centre escape -- in front of TEXT."
  (concatenate 'string (m "+MUIX-C+") text))

(defun text (contents &rest tags)
  "TextObject, TextFrame, MUIA_Text_Contents, ... End"
  (apply #'mui:new-object :text
         (m "+MUIA-FRAME+") (m "+MUIV-FRAME-TEXT+")
         (m "+MUIA-TEXT-CONTENTS+") (centered contents)
         tags))

(defun framed (frame-name label)
  "A text object wearing the frame MUIV_Frame_<FRAME-NAME>."
  (mui:new-object :text
    (m "+MUIA-FRAME+") (m (format nil "+MUIV-FRAME-~A+" frame-name))
    (m "+MUIA-TEXT-CONTENTS+") (centered label)))

(defun swatch (image-name)
  "A rectangle painted with the background MUII_<IMAGE-NAME>."
  (mui:new-object :rectangle
    (m "+MUIA-FRAME+") (m "+MUIV-FRAME-TEXT+")
    (m "+MUIA-BACKGROUND+") (m (format nil "+MUII-~A+" image-name))
    (m "+MUIA-FIX-HEIGHT+") 16))

(defun c-label (label)
  "CLabel(label): MUI_MakeObject(MUIO_Label, label, MUIO_Label_Centered)"
  (mui:make-object :label label (m "+MUIO-LABEL-CENTERED+")))

(defun hv-space ()
  "HVSpace: an empty rectangle that takes whatever room is left."
  (mui:new-object :rectangle))

(defun run ()
  (mui:with-foreign-pool ()
    (let* ((quit (mui:make-object :button "_Quit"))
           (win (mui:new-object :window
                  (m "+MUIA-WINDOW-TITLE+") "Group Layout"
                  (m "+MUIA-WINDOW-ID+")    (mui:make-id "GLAY")
                  (m "+MUIA-WINDOW-ROOT-OBJECT+")
                  (vgroup '()
                    (mui:new-object :text
                      (m "+MUIA-FRAME+") (m "+MUIV-FRAME-TEXT+")
                      (m "+MUIA-BACKGROUND+") (m "+MUII-TEXT-BACK+")
                      (m "+MUIA-TEXT-CONTENTS+")
                      (centered (format nil "MUI lays the objects out itself: groups nest, weights share the room,~%columns align, frames and backgrounds come from the user's MUI preferences.~%Resize the window to watch it work.")))
                    ;; the room a horizontal group has is shared by MUIA_Weight
                    (hgroup (group-frame-t "MUIA_Group_Horiz with MUIA_Weight 50 / 100 / 200")
                      (text "weight 50"  (m "+MUIA-WEIGHT+") 50)
                      (text "weight 100" (m "+MUIA-WEIGHT+") 100)
                      (text "weight 200" (m "+MUIA-WEIGHT+") 200))
                    ;; MUIA_Group_Columns: children fill the rows left to right
                    (col-group 3 (group-frame-t "MUIA_Group_Columns 3")
                      (mui:make-object :button "One")   (mui:make-object :button "Two")
                      (mui:make-object :button "Three") (mui:make-object :button "Four")
                      (mui:make-object :button "Five")  (mui:make-object :button "Six"))
                    ;; MUIA_Group_SameSize: every child as wide as the widest
                    (hgroup (append (group-frame-t "MUIA_Group_SameSize")
                                    (list (m "+MUIA-GROUP-SAME-SIZE+") t))
                      (mui:make-object :button "Short")
                      (mui:make-object :button "A much longer label")
                      (mui:make-object :button "Mid"))
                    ;; MUIV_Frame_*: the frames of mui.h, each on a text object
                    (hgroup (group-frame-t "MUIA_Frame, MUIV_Frame_*")
                      (framed "BUTTON" "Button")   (framed "TEXT" "Text")
                      (framed "STRING" "String")   (framed "GROUP" "Group")
                      (framed "READ-LIST" "ReadList") (framed "GAUGE" "Gauge")
                      (framed "VIRTUAL" "Virtual"))
                    ;; MUIA_Background: the pens and patterns of MUII_*
                    (col-group 8 (group-frame-t "MUIA_Background, MUII_*")
                      (c-label "Background") (c-label "Shadow") (c-label "Shine")
                      (c-label "Fill") (c-label "ShadowBack") (c-label "ShineBack")
                      (c-label "FillBack") (c-label "TextBack")
                      (swatch "BACKGROUND") (swatch "SHADOW") (swatch "SHINE")
                      (swatch "FILL") (swatch "SHADOWBACK") (swatch "SHINEBACK")
                      (swatch "FILLBACK") (swatch "TEXT-BACK"))
                    ;; the quit button, centred by two elastic rectangles
                    (hgroup '() (hv-space) quit (hv-space)))))
           (app (mui:new-object :application
                  (m "+MUIA-APPLICATION-TITLE+")       "GroupLayout"
                  (m "+MUIA-APPLICATION-VERSION+")     "$VER: GroupLayout 1.0"
                  (m "+MUIA-APPLICATION-DESCRIPTION+") "Show MUI's group layout"
                  (m "+MUIA-APPLICATION-BASE+")        "GROUPLAYOUT"
                  (m "+MUIA-APPLICATION-WINDOW+")      win)))
      (unwind-protect
           (progn
             (mui:notify win (m "+MUIA-WINDOW-CLOSE-REQUEST+") t
                         :application (m "+MUIM-APPLICATION-RETURN-ID+")
                         (m "+MUIV-APPLICATION-RETURN-ID-QUIT+"))
             (mui:notify quit (m "+MUIA-PRESSED+") nil
                         :application (m "+MUIM-APPLICATION-RETURN-ID+")
                         (m "+MUIV-APPLICATION-RETURN-ID-QUIT+"))
             (mui:set-attrs win (m "+MUIA-WINDOW-OPEN+") t)
             (when (zerop (or (mui:get-attr (m "+MUIA-WINDOW-OPEN+") win) 0))
               (error "group-layout: the window would not open"))
             ;; nothing to do per return ID: the layout is all MUI's
             (mui:do-application-events ((id) app)))
        (mui:dispose-object app)))))

(if (mui:available-p)
    (run)
    (format t "~&; group-layout: MUI is not available on this system (muimaster.library 3.8+ on AmigaOS, or MorphOS, needed) — nothing to show.~%"))
