;;; pages.lisp — the MUI SDK's Pages.c: a Register.mui page group whose
;;; tabs switch between four pages of a "character definition" form --
;;; plus a plain MUIA_Group_PageMode group whose page a cycle gadget
;;; selects through a notification alone.
;;;
;;; What it shows: RegisterGroup (MUIA_Register_Titles / _Frame), the
;;; string / cycle / radio / checkmark / slider objects of mui.h's
;;; shortcut macros (StringObject, CycleObject with MUIA_Cycle_Entries
;;; from POOL-STRING-ARRAY, RadioObject, the ImageObject CheckMark,
;;; SliderObject), MUIO_Label_SingleFrame / _DoubleFrame labels, HCenter
;;; and VSpace, and MUIA_Group_PageMode + MUIA_Group_ActivePage set from
;;; MUIA_Cycle_Active with MUIV_TriggerValue.
;;;
;;; Run on AmigaOS 3.x with MUI 3.8+ installed, or on MorphOS:
;;;   amilisp --load examples/amiga/mui/pages.lisp
;;; Unattended (auto-closes after 5 s):
;;;   amilisp --eval '(require "amiga/mui")' \
;;;           --eval '(setf amiga.mui:*event-loop-timeout* 5)' \
;;;           --load examples/amiga/mui/pages.lisp

(require "amiga/mui")

(when (amiga.mui:available-p)
  (require "amiga/raw/muimaster"))

(defpackage "MUI-PAGES"
  (:use "CL")
  (:local-nicknames ("MUI" "AMIGA.MUI")))

(in-package "MUI-PAGES")

(defun m (name)
  "The value of the amiga/raw/muimaster constant NAME -- looked up at run
time, so this file reads on a system where that package does not exist."
  (symbol-value (or (find-symbol name "AMIGA.RAW.MUIMASTER")
                    (error "pages: amiga.raw.muimaster:~A not found" name))))

(defparameter *sex*     '("male" "female"))
(defparameter *pages*   '("Race" "Class" "Armor" "Level"))
(defparameter *races*   '("Human" "Elf" "Dwarf" "Hobbit" "Gnome"))
(defparameter *classes* '("Warrior" "Rogue" "Bard" "Monk" "Magician" "Archmage"))

;;; The mui.h macros this program uses, as functions.

(defun children (objects)
  (loop for o in objects collect (m "+MUIA-GROUP-CHILD+") collect o))

(defun vgroup (tags &rest objects)
  "VGroup, tags..., Child, ..., End"
  (apply #'mui:new-object :group (append tags (children objects))))

(defun hgroup (tags &rest objects)
  (apply #'vgroup (list* (m "+MUIA-GROUP-HORIZ+") t tags) objects))

(defun col-group (columns tags &rest objects)
  (apply #'vgroup (list* (m "+MUIA-GROUP-COLUMNS+") columns tags) objects))

(defun h-space (n) (mui:make-object :h-space n))
(defun v-space (n) (mui:make-object :v-space n))

(defun h-center (object)
  "HCenter(obj): HGroup, GroupSpacing(0), Child, HSpace(0), Child, obj, Child, HSpace(0), End"
  (hgroup (list (m "+MUIA-GROUP-SPACING+") 0) (h-space 0) object (h-space 0)))

(defun label  (text) (mui:make-object :label text 0))
(defun label1 (text) (mui:make-object :label text (m "+MUIO-LABEL-SINGLE-FRAME+")))
(defun label2 (text) (mui:make-object :label text (m "+MUIO-LABEL-DOUBLE-FRAME+")))

(defun string-gadget (contents maxlen)
  "String(contents, maxlen): StringObject, StringFrame, MUIA_String_MaxLen, MUIA_String_Contents"
  (mui:new-object :string
    (m "+MUIA-FRAME+") (m "+MUIV-FRAME-STRING+")
    (m "+MUIA-STRING-MAX-LEN+") maxlen
    (m "+MUIA-STRING-CONTENTS+") contents))

(defun cycle (entries)
  "Cycle(entries): CycleObject, MUIA_Font, MUIV_Font_Button, MUIA_Cycle_Entries, entries"
  (mui:new-object :cycle
    (m "+MUIA-FONT+") (m "+MUIV-FONT-BUTTON+")
    (m "+MUIA-CYCLE-ENTRIES+") (mui:pool-string-array entries)))

(defun radio (entries)
  "Radio(NULL, entries): RadioObject, GroupFrame, MUIA_Radio_Entries"
  (mui:new-object :radio
    (m "+MUIA-FRAME+") (m "+MUIV-FRAME-GROUP+")
    (m "+MUIA-BACKGROUND+") (m "+MUII-GROUP-BACK+")
    (m "+MUIA-RADIO-ENTRIES+") (mui:pool-string-array entries)))

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

(defun slider (min max level)
  "Slider(min, max, level): SliderObject with MUIA_Numeric_Min / Max / Value"
  (mui:new-object :slider
    (m "+MUIA-NUMERIC-MIN+") min
    (m "+MUIA-NUMERIC-MAX+") max
    (m "+MUIA-NUMERIC-VALUE+") level))

(defun text (contents)
  (mui:new-object :text
    (m "+MUIA-FRAME+") (m "+MUIV-FRAME-TEXT+")
    (m "+MUIA-BACKGROUND+") (m "+MUII-TEXT-BACK+")
    (m "+MUIA-TEXT-CONTENTS+") (concatenate 'string (m "+MUIX-C+") contents)))

(defun run ()
  (mui:with-foreign-pool ()
    (let* (page-cycle page-group
           (win (mui:new-object :window
                  (m "+MUIA-WINDOW-TITLE+") "Character Definition"
                  (m "+MUIA-WINDOW-ID+")    (mui:make-id "PAGE")
                  (m "+MUIA-WINDOW-ROOT-OBJECT+")
                  (vgroup '()
                    (col-group 2 '()
                      (label2 "Name:") (string-gadget "Frodo" 32)
                      (label1 "Sex:")  (cycle *sex*))
                    (v-space 2)
                    ;; RegisterGroup(Pages): one child per title, tabs switch them
                    (apply #'mui:new-object :register
                      (m "+MUIA-REGISTER-TITLES+") (mui:pool-string-array *pages*)
                      (m "+MUIA-REGISTER-FRAME+") t
                      (children
                       (list
                        (h-center (radio *races*))
                        (h-center (radio *classes*))
                        (hgroup '()
                          (h-space 0)
                          (col-group 2 '()
                            (label1 "Cloak:")  (checkmark t)
                            (label1 "Shield:") (checkmark t)
                            (label1 "Gloves:") (checkmark t)
                            (label1 "Helmet:") (checkmark t))
                          (h-space 0))
                        (col-group 2 '()
                          (label "Experience:")   (slider 0 100 3)
                          (label "Strength:")     (slider 0 100 42)
                          (label "Dexterity:")    (slider 0 100 24)
                          (label "Condition:")    (slider 0 100 39)
                          (label "Intelligence:") (slider 0 100 74)))))
                    (v-space 2)
                    ;; Beyond Pages.c: a page group without tabs, switched by
                    ;; a cycle gadget -- MUIA_Group_PageMode + MUIA_Group_ActivePage.
                    (vgroup (list (m "+MUIA-FRAME+") (m "+MUIV-FRAME-GROUP+")
                                  (m "+MUIA-FRAME-TITLE+") "MUIA_Group_PageMode driven by a cycle"
                                  (m "+MUIA-BACKGROUND+") (m "+MUII-GROUP-BACK+"))
                      (setf page-cycle
                            ;; MUI_MakeObject(MUIO_Cycle, NULL, entries)
                            (mui:make-object :cycle nil '("Page one" "Page two" "Page three")))
                      (setf page-group
                            (vgroup (list (m "+MUIA-GROUP-PAGE-MODE+") t)
                              (text "The first page.")
                              (text (format nil "The second page --~%two lines tall."))
                              (text "The third page.")))))))
           (app (mui:new-object :application
                  (m "+MUIA-APPLICATION-TITLE+")       "Pages-Demo"
                  (m "+MUIA-APPLICATION-VERSION+")     "$VER: Pages-Demo 19.5 (12.02.97)"
                  (m "+MUIA-APPLICATION-COPYRIGHT+")   "(C) 1992/93, Stefan Stuntz"
                  (m "+MUIA-APPLICATION-AUTHOR+")      "Stefan Stuntz"
                  (m "+MUIA-APPLICATION-DESCRIPTION+") "Show MUIs Page Groups"
                  (m "+MUIA-APPLICATION-BASE+")        "PAGESDEMO"
                  (m "+MUIA-APPLICATION-WINDOW+")      win)))
      (unwind-protect
           (progn
             (mui:notify win (m "+MUIA-WINDOW-CLOSE-REQUEST+") t
                         :application (m "+MUIM-APPLICATION-RETURN-ID+")
                         (m "+MUIV-APPLICATION-RETURN-ID-QUIT+"))
             ;; DoMethod(cycle, MUIM_Notify, MUIA_Cycle_Active, MUIV_EveryTime,
             ;;          pagegroup, 3, MUIM_Set, MUIA_Group_ActivePage, MUIV_TriggerValue)
             (mui:notify page-cycle (m "+MUIA-CYCLE-ACTIVE+") :every-time
                         page-group (m "+MUIM-SET+")
                         (m "+MUIA-GROUP-ACTIVE-PAGE+") :trigger-value)
             (mui:set-attrs win (m "+MUIA-WINDOW-OPEN+") t)
             (when (zerop (or (mui:get-attr (m "+MUIA-WINDOW-OPEN+") win) 0))
               (error "pages: the window would not open"))
             ;; no return IDs: the notification does the page switching
             (mui:do-application-events ((id) app)))
        (mui:dispose-object app)))))

(if (mui:available-p)
    (run)
    (format t "~&; pages: MUI is not available on this system (muimaster.library 3.8+ on AmigaOS, or MorphOS, needed) — nothing to show.~%"))
