;;; balancing.lisp — the MUI SDK's Balancing.c: Balance.mui objects
;;; between the children of a group let the user redistribute the room
;;; with the mouse, within the weights the program gave.
;;;
;;; The C program's object tree, one to one: a horizontal group of a
;;; weighted column and a column of six framed rows, every one of them
;;; balanced -- rectangles, buttons, labels and texts alike.  Shift-drag
;;; a balance to size all the balances of its group at once.
;;;
;;; What it shows: BalanceObject with MUIA_CycleChain, MUIA_Weight,
;;; MUIA_ObjectID (a rectangle that remembers its size in the MUI
;;; settings), MUIV_Window_Width_Screen(50) through WINDOW-SIZE-SCREEN,
;;; the return-ID-free "ideal" input loop.
;;;
;;; Run on AmigaOS 3.x with MUI 3.8+ installed, or on MorphOS:
;;;   amilisp --load examples/amiga/mui/balancing.lisp
;;; Unattended (auto-closes after 5 s):
;;;   amilisp --eval '(require "amiga/mui")' \
;;;           --eval '(setf amiga.mui:*event-loop-timeout* 5)' \
;;;           --load examples/amiga/mui/balancing.lisp

(require "amiga/mui")

(when (amiga.mui:available-p)
  (require "amiga/raw/muimaster"))

(defpackage "MUI-BALANCING"
  (:use "CL")
  (:local-nicknames ("MUI" "AMIGA.MUI")))

(in-package "MUI-BALANCING")

(defun m (name)
  "The value of the amiga/raw/muimaster constant NAME -- looked up at run
time, so this file reads on a system where that package does not exist."
  (symbol-value (or (find-symbol name "AMIGA.RAW.MUIMASTER")
                    (error "balancing: amiga.raw.muimaster:~A not found" name))))

;;; The mui.h macros this program uses, as functions.

(defun children (objects)
  (loop for o in objects collect (m "+MUIA-GROUP-CHILD+") collect o))

(defun vgroup (tags &rest objects)
  "VGroup, tags..., Child, ..., End"
  (apply #'mui:new-object :group (append tags (children objects))))

(defun hgroup (tags &rest objects)
  "HGroup: a group with MUIA_Group_Horiz."
  (apply #'vgroup (list* (m "+MUIA-GROUP-HORIZ+") t tags) objects))

(defun group-frame ()
  "GroupFrame"
  (list (m "+MUIA-FRAME+") (m "+MUIV-FRAME-GROUP+")))

(defun rect (&rest tags)
  "RectangleObject, TextFrame, tags... End"
  (apply #'mui:new-object :rectangle
         (m "+MUIA-FRAME+") (m "+MUIV-FRAME-TEXT+") tags))

(defun balance ()
  "BalanceObject, MUIA_CycleChain, 1, End"
  (mui:new-object :balance (m "+MUIA-CYCLE-CHAIN+") 1))

(defun label (text)
  "Label(text): MUI_MakeObject(MUIO_Label, text, 0)"
  (mui:make-object :label text 0))

(defun text (contents)
  (mui:new-object :text (m "+MUIA-TEXT-CONTENTS+") contents))

(defun run ()
  (mui:with-foreign-pool ()
    (let* ((weight (m "+MUIA-WEIGHT+"))
           (win (mui:new-object :window
                  (m "+MUIA-WINDOW-TITLE+")  "Balancing Groups"
                  (m "+MUIA-WINDOW-ID+")     (mui:make-id "BALA")
                  ;; MUIV_Window_Width_Screen(50): half the screen
                  (m "+MUIA-WINDOW-WIDTH+")  (mui:window-size-screen 50)
                  (m "+MUIA-WINDOW-HEIGHT+") (mui:window-size-screen 50)
                  (m "+MUIA-WINDOW-ROOT-OBJECT+")
                  (hgroup '()
                    (vgroup (append (group-frame) (list weight 15))
                      (rect weight 50)
                      (rect weight 100)
                      (balance)
                      (rect weight 200))
                    (balance)
                    (vgroup '()
                      (hgroup (group-frame)
                        (rect (m "+MUIA-OBJECT-ID+") 123)
                        (balance)
                        (rect (m "+MUIA-OBJECT-ID+") 456))
                      (hgroup (group-frame)
                        (rect) (balance) (rect) (balance) (rect) (balance)
                        (rect) (balance) (rect))
                      (hgroup (group-frame)
                        (hgroup '() (rect) (balance) (rect))
                        (balance)
                        (hgroup '() (rect) (balance) (rect)))
                      (hgroup (group-frame)
                        (rect weight 50)
                        (rect weight 100)
                        (balance)
                        (rect weight 200))
                      (hgroup (group-frame)
                        (mui:make-object :button "Also")   (balance)
                        (mui:make-object :button "Try")    (balance)
                        (mui:make-object :button "Sizing") (balance)
                        (mui:make-object :button "With")   (balance)
                        (mui:make-object :button "Shift"))
                      (hgroup (group-frame)
                        (label "Label 1:") (text "data...")
                        (balance)
                        (label "Label 2:") (text "more data..."))))))
           (app (mui:new-object :application
                  (m "+MUIA-APPLICATION-TITLE+")       "BalanceDemo"
                  (m "+MUIA-APPLICATION-VERSION+")     "$VER: BalanceDemo 19.5 (12.02.97)"
                  (m "+MUIA-APPLICATION-COPYRIGHT+")   "(C) 1995, Stefan Stuntz"
                  (m "+MUIA-APPLICATION-AUTHOR+")      "Stefan Stuntz"
                  (m "+MUIA-APPLICATION-DESCRIPTION+") "Show balancing groups"
                  (m "+MUIA-APPLICATION-BASE+")        "BALANCEDEMO"
                  (m "+MUIA-APPLICATION-WINDOW+")      win)))
      (unwind-protect
           (progn
             (mui:notify win (m "+MUIA-WINDOW-CLOSE-REQUEST+") t
                         :application (m "+MUIM-APPLICATION-RETURN-ID+")
                         (m "+MUIV-APPLICATION-RETURN-ID-QUIT+"))
             (mui:set-attrs win (m "+MUIA-WINDOW-OPEN+") t)
             (when (zerop (or (mui:get-attr (m "+MUIA-WINDOW-OPEN+") win) 0))
               (error "balancing: the window would not open"))
             ;; "the ideal input loop for an object oriented MUI application":
             ;; no return IDs, just run until Quit
             (mui:do-application-events ((id) app)))
        (mui:dispose-object app)))))

(if (mui:available-p)
    (run)
    (format t "~&; balancing: MUI is not available on this system (muimaster.library 3.8+ on AmigaOS, or MorphOS, needed) — nothing to show.~%"))
