;;; showhide.lisp — the MUI SDK's ShowHide.c: five checkmarks each show
;;; or hide one button through a notification (MUIA_Selected ->
;;; MUIM_Set MUIA_ShowMe with the trigger value), and, beyond the C,
;;; two buttons that add and remove buttons while the window is open --
;;; the MUIM_Group_InitChange / OM_ADDMEMBER / OM_REMMEMBER /
;;; MUIM_Group_ExitChange protocol for changing a group's children.
;;;
;;; What it shows: MUIA_ShowMe set by MUIV_TriggerValue, MUIA_Weight 0,
;;; the CheckMark ImageObject, MUIM_Group_InitChange / _ExitChange around
;;; OM_ADDMEMBER / OM_REMMEMBER through DO-METHOD, MUI_DisposeObject of a
;;; removed child.
;;;
;;; Run on AmigaOS 3.x with MUI 3.8+ installed, or on MorphOS:
;;;   amilisp --load examples/amiga/mui/showhide.lisp
;;; Unattended (auto-closes after 5 s):
;;;   amilisp --eval '(require "amiga/mui")' \
;;;           --eval '(setf amiga.mui:*event-loop-timeout* 5)' \
;;;           --load examples/amiga/mui/showhide.lisp

(require "amiga/mui")

(when (amiga.mui:available-p)
  (require "amiga/raw/muimaster"))

(defpackage "MUI-SHOWHIDE"
  (:use "CL")
  (:local-nicknames ("MUI" "AMIGA.MUI")))

(in-package "MUI-SHOWHIDE")

(defconstant +id-add+    1)
(defconstant +id-remove+ 2)

(defun m (name)
  "The value of the amiga/raw/muimaster constant NAME -- looked up at run
time, so this file reads on a system where that package does not exist."
  (symbol-value (or (find-symbol name "AMIGA.RAW.MUIMASTER")
                    (error "showhide: amiga.raw.muimaster:~A not found" name))))

(defun i (name)
  "The value of the amiga/raw/intuition constant NAME (OM_ADDMEMBER ...)."
  (symbol-value (or (find-symbol name "AMIGA.RAW.INTUITION")
                    (error "showhide: amiga.raw.intuition:~A not found" name))))

;;; The mui.h macros this program uses, as functions.

(defun children (objects)
  (loop for o in objects collect (m "+MUIA-GROUP-CHILD+") collect o))

(defun vgroup (tags &rest objects)
  (apply #'mui:new-object :group (append tags (children objects))))

(defun hgroup (tags &rest objects)
  (apply #'vgroup (list* (m "+MUIA-GROUP-HORIZ+") t tags) objects))

(defun checkmark (selected)
  "CheckMark(selected): the ImageObject of mui.h with MUII_CheckMark in toggle mode"
  (mui:new-object :image
    (m "+MUIA-FRAME+") (m "+MUIV-FRAME-IMAGE-BUTTON+")
    (m "+MUIA-INPUT-MODE+") (m "+MUIV-INPUT-MODE-TOGGLE+")
    (m "+MUIA-IMAGE-SPEC+") (m "+MUII-CHECK-MARK+")
    (m "+MUIA-IMAGE-FREE-VERT+") t
    (m "+MUIA-SELECTED+") selected
    (m "+MUIA-BACKGROUND+") (m "+MUII-BUTTON-BACK+")
    (m "+MUIA-SHOW-SEL-STATE+") nil))

(defun run ()
  (mui:with-foreign-pool ()
    (let* ((marks   (loop repeat 5 collect (checkmark t)))
           (buttons (loop for n from 1 to 5
                          collect (mui:make-object :button (format nil "Button ~D" n))))
           (add-button    (mui:make-object :button "_Add a button"))
           (remove-button (mui:make-object :button "_Remove the last"))
           (dynamic (apply #'vgroup '()
                           (append buttons (list (mui:make-object :v-space 0)))))
           (win (mui:new-object :window
                  (m "+MUIA-WINDOW-TITLE+") "Show & Hide"
                  (m "+MUIA-WINDOW-ID+")    (mui:make-id "SHHD")
                  (m "+MUIA-WINDOW-ROOT-OBJECT+")
                  (hgroup '()
                    (vgroup (list (m "+MUIA-FRAME+") (m "+MUIV-FRAME-GROUP+"))
                      ;; MUIA_Weight 0: the row of checkmarks takes no extra height
                      (apply #'hgroup (list (m "+MUIA-WEIGHT+") 0) marks)
                      dynamic
                      (hgroup '() add-button remove-button)))))
           (app (mui:new-object :application
                  (m "+MUIA-APPLICATION-TITLE+")       "ShowHide"
                  (m "+MUIA-APPLICATION-VERSION+")     "$VER: ShowHide 19.5 (12.02.97)"
                  (m "+MUIA-APPLICATION-COPYRIGHT+")   "(C) 1992/93, Stefan Stuntz"
                  (m "+MUIA-APPLICATION-AUTHOR+")      "Stefan Stuntz"
                  (m "+MUIA-APPLICATION-DESCRIPTION+") "Show object hiding."
                  (m "+MUIA-APPLICATION-BASE+")        "SHOWHIDE"
                  (m "+MUIA-APPLICATION-WINDOW+")      win))
           (added '())
           (count 5))
      (unwind-protect
           (progn
             (mui:notify win (m "+MUIA-WINDOW-CLOSE-REQUEST+") t
                         :application (m "+MUIM-APPLICATION-RETURN-ID+")
                         (m "+MUIV-APPLICATION-RETURN-ID-QUIT+"))
             ;; DoMethod(cm, MUIM_Notify, MUIA_Selected, MUIV_EveryTime,
             ;;          bt, 3, MUIM_Set, MUIA_ShowMe, MUIV_TriggerValue)
             (loop for mark in marks
                   for button in buttons
                   do (mui:notify mark (m "+MUIA-SELECTED+") :every-time
                                  button (m "+MUIM-SET+")
                                  (m "+MUIA-SHOW-ME+") :trigger-value))
             (mui:notify add-button (m "+MUIA-PRESSED+") nil
                         :application (m "+MUIM-APPLICATION-RETURN-ID+") +id-add+)
             (mui:notify remove-button (m "+MUIA-PRESSED+") nil
                         :application (m "+MUIM-APPLICATION-RETURN-ID+") +id-remove+)
             (mui:set-attrs remove-button (m "+MUIA-DISABLED+") t)
             ;; set(cm3, MUIA_Selected, FALSE): button 3 starts hidden
             (mui:set-attrs (third marks) (m "+MUIA-SELECTED+") nil)
             (mui:set-attrs win (m "+MUIA-WINDOW-OPEN+") t)
             (when (zerop (or (mui:get-attr (m "+MUIA-WINDOW-OPEN+") win) 0))
               (error "showhide: the window would not open"))
             (mui:do-application-events ((id) app)
               (cond
                 ((= id +id-add+)
                  ;; A group's children may only change between
                  ;; MUIM_Group_InitChange and MUIM_Group_ExitChange, which
                  ;; relayouts the window.
                  (let ((button (mui:make-object :button
                                                 (format nil "Button ~D (added)" (incf count)))))
                    (mui:do-method dynamic (m "+MUIM-GROUP-INIT-CHANGE+"))
                    (mui:do-method dynamic (i "+OM-ADDMEMBER+") button)
                    (mui:do-method dynamic (m "+MUIM-GROUP-EXIT-CHANGE+"))
                    (push button added)
                    (mui:set-attrs remove-button (m "+MUIA-DISABLED+") nil)
                    (format t "~&; added Button ~D~%" count)))
                 ((= id +id-remove+)
                  (when added
                    (let ((button (pop added)))
                      (mui:do-method dynamic (m "+MUIM-GROUP-INIT-CHANGE+"))
                      (mui:do-method dynamic (i "+OM-REMMEMBER+") button)
                      (mui:do-method dynamic (m "+MUIM-GROUP-EXIT-CHANGE+"))
                      ;; a removed child is the program's again: dispose it
                      (mui:dispose-object button)
                      (decf count)
                      (format t "~&; removed the last added button~%"))
                    (unless added
                      (mui:set-attrs remove-button (m "+MUIA-DISABLED+") t)))))))
        (mui:dispose-object app)))))

(if (mui:available-p)
    (run)
    (format t "~&; showhide: MUI is not available on this system (muimaster.library 3.8+ on AmigaOS, or MorphOS, needed) — nothing to show.~%"))
