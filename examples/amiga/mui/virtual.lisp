;;; virtual.lisp — the MUI SDK's Virtual.c: four scroll groups, each
;;; showing a virtual group through a viewport smaller than its contents
;;; -- a long text, a sampler of MUI's builtin images and backgrounds,
;;; input objects (radios, cycles, a list, a field of buttons) and the
;;; "completely crazy" page: a virtual group inside a virtual group, arrow
;;; buttons that scroll it by notification, and a cycle-driven page group.
;;;
;;; What it shows: ScrollgroupObject with MUIA_Scrollgroup_Contents and
;;; MUIA_Scrollgroup_UseWinBorder, VirtgroupObject (VGroupV / ColGroupV)
;;; with VirtualFrame, MUIA_Virtgroup_Left / _Top set by MUIM_Notify,
;;; ImageObject with MUIA_Image_Spec for the MUII_* images, the MUII_*
;;; backgrounds, GaugeObject and ScaleObject, ListviewObject / ListObject
;;; filled by MUIM_List_Insert from a POOL-STRING-ARRAY, KeyLabel /
;;; KeyCycle control characters, MUIA_Text_SetMax, InnerSpacing,
;;; MUIM_Window_SetCycleChain, the window's border scrollers.
;;;
;;; Run on AmigaOS 3.x with MUI 3.8+ installed, or on MorphOS:
;;;   amilisp --load examples/amiga/mui/virtual.lisp
;;; Unattended (auto-closes after 5 s):
;;;   amilisp --eval '(require "amiga/mui")' \
;;;           --eval '(setf amiga.mui:*event-loop-timeout* 5)' \
;;;           --load examples/amiga/mui/virtual.lisp

(require "amiga/mui")

(when (amiga.mui:available-p)
  (require "amiga/raw/muimaster"))

(defpackage "MUI-VIRTUAL"
  (:use "CL")
  (:local-nicknames ("MUI" "AMIGA.MUI")))

(in-package "MUI-VIRTUAL")

(defun m (name)
  "The value of the amiga/raw/muimaster constant NAME -- looked up at run
time, so this file reads on a system where that package does not exist."
  (symbol-value (or (find-symbol name "AMIGA.RAW.MUIMASTER")
                    (error "virtual: amiga.raw.muimaster:~A not found" name))))

(defparameter *computers* '("Amiga 500" "Amiga 600" "Amiga 1000 :)" "Amiga 1200" "Amiga 2000"
                            "Amiga 3000" "Amiga 4000" "Amiga 4000T" "Atari ST :("))
(defparameter *printers*  '("HP Deskjet" "NEC P6" "Okimate 20"))
(defparameter *displays*  '("A1081" "NEC 3D" "A2024" "Eizo T660i"))

(defparameter *x4-pages*   '("Race" "Class" "Armors" "Weapons" "Levels"))
(defparameter *x4-races*   '("Human" "Elf" "Dwarf" "Hobbit" "Gnome"))
(defparameter *x4-classes* '("Warrior" "Rogue" "Bard" "Monk" "Magician" "Archmage"))
(defparameter *x4-weapons* '("Staff" "Dagger" "Sword" "Axe" "Grenade"))

(defvar *cycle-chain* '()
  "The input objects of page 3 in TAB order, for MUIM_Window_SetCycleChain.")

;;; The mui.h macros this program uses, as functions.

(defun children (objects)
  (loop for o in objects collect (m "+MUIA-GROUP-CHILD+") collect o))

(defun vgroup (tags &rest objects)
  (apply #'mui:new-object :group (append tags (children objects))))

(defun hgroup (tags &rest objects)
  (apply #'vgroup (list* (m "+MUIA-GROUP-HORIZ+") t tags) objects))

(defun col-group (columns tags &rest objects)
  (apply #'vgroup (list* (m "+MUIA-GROUP-COLUMNS+") columns tags) objects))

(defun vgroup-v (tags &rest objects)
  "VGroupV: a Virtgroup.mui -- a group that may be larger than it is shown"
  (apply #'mui:new-object :virtgroup (append tags (children objects))))

(defun col-group-v (columns tags &rest objects)
  "ColGroupV(n)"
  (apply #'vgroup-v (list* (m "+MUIA-GROUP-COLUMNS+") columns tags) objects))

(defun scrollgroup (contents &rest tags)
  "ScrollgroupObject, MUIA_Scrollgroup_Contents, contents, tags... End"
  (apply #'mui:new-object :scrollgroup
         (m "+MUIA-SCROLLGROUP-CONTENTS+") contents tags))

(defun frame (name) (list (m "+MUIA-FRAME+") (m (format nil "+MUIV-FRAME-~A+" name))))
(defun virtual-frame () (frame "VIRTUAL"))
(defun group-frame ()   (frame "GROUP"))
(defun text-frame ()    (frame "TEXT"))

(defun group-frame-t (title)
  "GroupFrameT(title)"
  (append (group-frame)
          (list (m "+MUIA-FRAME-TITLE+") title
                (m "+MUIA-BACKGROUND+") (m "+MUII-GROUP-BACK+"))))

(defun group-spacing (n) (list (m "+MUIA-GROUP-SPACING+") n))

(defun inner-spacing (h v)
  "InnerSpacing(h, v): MUIA_InnerLeft / Right = h, MUIA_InnerTop / Bottom = v"
  (list (m "+MUIA-INNER-LEFT+") h (m "+MUIA-INNER-RIGHT+") h
        (m "+MUIA-INNER-TOP+") v (m "+MUIA-INNER-BOTTOM+") v))

(defun h-space (n) (mui:make-object :h-space n))
(defun v-space (n) (mui:make-object :v-space n))
(defun hv-space () (mui:new-object :rectangle))

(defun h-center (object)
  (hgroup (group-spacing 0) (h-space 0) object (h-space 0)))

(defun label  (text) (mui:make-object :label text 0))
(defun label1 (text) (mui:make-object :label text (m "+MUIO-LABEL-SINGLE-FRAME+")))

(defun key-label1 (text key)
  "KeyLabel1(label, key): the control character is or-ed into the flags"
  (mui:make-object :label text (logior (m "+MUIO-LABEL-SINGLE-FRAME+") (char-code key))))

(defun x (name)
  "The text engine escape MUIX_<NAME> (\"\\33i\" ...)."
  (m (format nil "+MUIX-~A+" name)))

(defun text (contents &rest tags)
  (apply #'mui:new-object :text (m "+MUIA-TEXT-CONTENTS+") contents tags))

(defun centered-text (contents)
  "mytxt(txt): a centred text that never grows past its contents"
  (text (concatenate 'string (x "C") contents) (m "+MUIA-TEXT-SET-MAX+") t))

(defun image (spec)
  "ImageObject, MUIA_Image_Spec, spec, End"
  (mui:new-object :image (m "+MUIA-IMAGE-SPEC+") (m (format nil "+MUII-~A+" spec))))

(defun image-button (spec)
  "ibt(i): an image that acts as a button"
  (mui:new-object :image
    (m "+MUIA-FRAME+") (m "+MUIV-FRAME-IMAGE-BUTTON+")
    (m "+MUIA-BACKGROUND+") (m "+MUII-BUTTON-BACK+")
    (m "+MUIA-INPUT-MODE+") (m "+MUIV-INPUT-MODE-REL-VERIFY+")
    (m "+MUIA-IMAGE-SPEC+") (m (format nil "+MUII-~A+" spec))))

(defun background-rect (spec)
  "RectangleObject, TextFrame, MUIA_Background, MUII_<spec>, MUIA_FixWidth, 30"
  (apply #'mui:new-object :rectangle
         (m "+MUIA-BACKGROUND+") (m (format nil "+MUII-~A+" spec))
         (m "+MUIA-FIX-WIDTH+") 30
         (text-frame)))

(defun cycle (entries &optional key)
  "Cycle(entries) / KeyCycle(entries, key)"
  (apply #'mui:new-object :cycle
         (m "+MUIA-FONT+") (m "+MUIV-FONT-BUTTON+")
         (m "+MUIA-CYCLE-ENTRIES+") (mui:pool-string-array entries)
         (and key (list (m "+MUIA-CONTROL-CHAR+") (char-code key)))))

(defun radio (name entries)
  "Radio(name, array): RadioObject, GroupFrameT(name), MUIA_Radio_Entries"
  (apply #'mui:new-object :radio
         (m "+MUIA-RADIO-ENTRIES+") (mui:pool-string-array entries)
         (if name (group-frame-t name) (group-frame))))

(defun checkmark (selected)
  "CheckMark(selected)"
  (mui:new-object :image
    (m "+MUIA-FRAME+") (m "+MUIV-FRAME-IMAGE-BUTTON+")
    (m "+MUIA-INPUT-MODE+") (m "+MUIV-INPUT-MODE-TOGGLE+")
    (m "+MUIA-IMAGE-SPEC+") (m "+MUII-CHECK-MARK+")
    (m "+MUIA-IMAGE-FREE-VERT+") t
    (m "+MUIA-SELECTED+") selected
    (m "+MUIA-BACKGROUND+") (m "+MUII-BUTTON-BACK+")
    (m "+MUIA-SHOW-SEL-STATE+") nil))

(defun slider (min max level)
  (mui:new-object :slider
    (m "+MUIA-NUMERIC-MIN+") min
    (m "+MUIA-NUMERIC-MAX+") max
    (m "+MUIA-NUMERIC-VALUE+") level))

(defun button (label) (mui:make-object :button label))

;;; Page 1: one long text in a virtual group, scrolled by the window's
;;; border scrollers.

(defun text1 ()
  (let ((i (x "I")) (n (x "N")) (u (x "U")) (c (x "C")))
    (format nil "~AHello User !~A~%~%This could be a very long text and you are looking~%at it through a ~Avirtual group~A. Please use the~%scrollbars at the right and bottom of the group to~%move the visible area either vertically or~%horizontally. While holding down the small arrow~%button between both scrollbars, the display will~%follow your mouse moves.~%~%If you click somewhere into a ~Avirtual group~A and~%move the mouse across one of its borders, the group will~%start scrolling. If you are lucky and own a middle mouse~%button, you may also want to press it and try moving.~%~%When the surrounding window is big enough for the~%complete virtual group to fit, the scrollers and~%the move button get disabled.~%~%Since this ~Avirtual group~A does only contain a~%single text object, it's a rather simple example.~%In fact, virtual groups are a lot more powerful,~%they can contain any objects you like.~%~%Note to 7MHz/68000 users: Sorry if you find this~%thingy a bit slow. Clipping in virtual groups can~%get quite complicated. Please don't blame me,~%blame your 'out of date' machine! :-)~%~%~A~AHave fun, Stefan.~A"
            i n u n u n u n i c n)))

(defun make-page-1 ()
  (scrollgroup
   (vgroup-v (virtual-frame)
     (text (text1) (m "+MUIA-BACKGROUND+") (m "+MUII-TEXT-BACK+")))
   ;; the scrollers go into the window's border (MUIA_Window_Use*BorderScroller)
   (m "+MUIA-SCROLLGROUP-USE-WIN-BORDER+") t))

;;; Page 2: the standard images, some backgrounds, a gauge with a scale.

(defun text2 ()
  (format nil "~AAs you can see, this virtual group contains a~%lot of different objects. The (virtual) width~%and height of the virtual group are automatically~%calculated from the default width and height of~%the virtual groups contents."
          (x "C")))

(defun make-page-2 ()
  (scrollgroup
   (vgroup-v (virtual-frame)
     (apply #'text (text2) (m "+MUIA-BACKGROUND+") (m "+MUII-TEXT-BACK+") (text-frame))
     (hgroup '()
       (col-group 2 (group-frame-t "Standard Images")
         (label "ArrowUp:")     (image "ARROW-UP")
         (label "ArrowDown:")   (image "ARROW-DOWN")
         (label "ArrowLeft:")   (image "ARROW-LEFT")
         (label "ArrowRight:")  (image "ARROW-RIGHT")
         (label "RadioButton:") (image "RADIO-BUTTON")
         (label "File:")        (image "POP-FILE")
         (label "HardDisk:")    (image "HARD-DISK")
         (label "Disk:")        (image "DISK")
         (label "Chip:")        (image "CHIP")
         (label "Drawer:")      (image "DRAWER"))
       (vgroup (group-frame-t "Some Backgrounds")
         (hgroup '() (background-rect "BACKGROUND") (background-rect "FILL")
                     (background-rect "SHADOW"))
         (hgroup '() (background-rect "SHADOWBACK") (background-rect "SHADOWFILL")
                     (background-rect "SHADOWSHINE"))
         (hgroup '() (background-rect "FILLBACK") (background-rect "SHINEBACK")
                     (background-rect "FILLSHINE"))))
     (col-group 2 (group-frame)
       (label1 "Gauge:")
       (apply #'mui:new-object :gauge
              (m "+MUIA-GAUGE-CURRENT+") 66
              (m "+MUIA-GAUGE-HORIZ+") t
              (frame "GAUGE"))
       (v-space 0)
       (mui:new-object :scale)))))

;;; Page 3: input objects -- radios, cycles with keys, a list, buttons.

(defun text3 ()
  (format nil "~AThe above pages only showed 'read only' groups,~%no user actions within them were possible. Of course,~%handling user actions in a virtual group is not a~%problem for MUI. As I promised on the first page,~%you can use virtual groups with whatever objects~%you want. Here's a small example...~%~%Note: Due to some limitations of the operating system,~%it is not possible to clip gadgets depending on~%intuition.library correctly. This affects the appearence~%of string and proportional objects in virtual groups.~%You will only be able to use these gadgets when they~%are completely visible.~%~%PS: Also try TAB cycling here!"
          (x "C")))

(defun make-page-3 ()
  (let* ((mt-computer (radio "Computer:" *computers*))
         (mt-printer  (radio "Printer:" *printers*))
         (mt-display  (radio "Display:" *displays*))
         (cy-computer (cycle *computers* #\c))
         (cy-printer  (cycle *printers* #\p))
         (cy-display  (cycle *displays* #\d))
         (list (mui:new-object :list (m "+MUIA-FRAME+") (m "+MUIV-FRAME-INPUT-LIST+")))
         (listview (mui:new-object :listview
                     (m "+MUIA-LISTVIEW-SCROLLER-POS+") (m "+MUIV-LISTVIEW-SCROLLER-POS-LEFT+")
                     (m "+MUIA-LISTVIEW-INPUT+") t
                     (m "+MUIA-LISTVIEW-LIST+") list))
         (buttons (loop repeat 12 collect (button "Button")))
         (page (scrollgroup
                (vgroup-v (virtual-frame)
                  (apply #'text (text3) (m "+MUIA-BACKGROUND+") (m "+MUII-TEXT-BACK+") (text-frame))
                  (vgroup '()
                    (hgroup '()
                      mt-computer
                      (vgroup '() mt-printer (v-space 0) mt-display)
                      (vgroup '()
                        (col-group 2 (group-frame-t "Cycle Gadgets")
                          (key-label1 "Computer:" #\c) cy-computer
                          (key-label1 "Printer:" #\p)  cy-printer
                          (key-label1 "Display:" #\d)  cy-display)
                        listview))
                    (apply #'col-group 4 (group-frame-t "Button Field") buttons))))))
    ;; DoMethod(list, MUIM_List_Insert, array, -1, MUIV_List_Insert_Bottom):
    ;; -1 = the array is NULL-terminated; the list copies nothing, the
    ;; strings stay in the pool
    (mui:do-method list (m "+MUIM-LIST-INSERT+") (mui:pool-string-array *computers*)
                   -1 (m "+MUIV-LIST-INSERT-BOTTOM+"))
    (setf *cycle-chain*
          (append (list mt-computer mt-printer mt-display cy-computer cy-printer cy-display listview)
                  buttons))
    page))

;;; Page 4: a virtual group inside a virtual group, arrow buttons that
;;; scroll it, and a cycle-driven page group inside a third one.

(defparameter *crazy-words*
  '((:button "One") (:button "Two") (:button "Three") (:button "Four") (:button "Five")
    (:button "Six") (:button "Eighteen") (:text "The") (:text "red") (:text "brown")
    (:text "fox") (:button "Seven") (:button "Seventeen") (:text "dog.") (:button "Nineteen")
    (:button "Twenty") (:text "jumps") (:button "Eight") (:button "Sixteen") (:text "lazy")
    (:text "the") (:text "over") (:text "quickly") (:button "Nine") (:button "Fifteen")
    (:button "Fourteen") (:button "Thirteen") (:button "Twelve") (:button "Eleven")
    (:button "Ten")))

(defun make-page-4 ()
  (let* ((bt-up    (image-button "ARROW-UP"))
         (bt-left  (image-button "ARROW-LEFT"))
         (bt-right (image-button "ARROW-RIGHT"))
         (bt-down  (image-button "ARROW-DOWN"))
         (inner (apply #'vgroup-v (virtual-frame)
                       (list
                        (apply #'col-group 6 (list (m "+MUIA-GROUP-SAME-SIZE+") t)
                               (loop for (kind word) in *crazy-words*
                                     collect (if (eq kind :button)
                                                 (button word)
                                                 (centered-text word)))))))
         (page-cycle (cycle *x4-pages*))
         (page-group (vgroup (list (m "+MUIA-GROUP-PAGE-MODE+") t)
                       (h-center (radio nil *x4-races*))
                       (h-center (radio nil *x4-classes*))
                       (hgroup '()
                         (h-space 0)
                         (col-group 2 '()
                           (label1 "Cloak:")  (checkmark t)
                           (label1 "Shield:") (checkmark t)
                           (label1 "Gloves:") (checkmark t)
                           (label1 "Helm:")   (checkmark t))
                         (h-space 0))
                       (h-center (radio nil *x4-weapons*))
                       (col-group 2 '()
                         (label "Experience:")   (slider 0 100 3)
                         (label "Strength:")     (slider 0 100 42)
                         (label "Dexterity:")    (slider 0 100 24)
                         (label "Condition:")    (slider 0 100 39)
                         (label "Intelligence:") (slider 0 100 74))))
         (page (scrollgroup
                (col-group-v 3 (append (virtual-frame) (list (m "+MUIA-GROUP-SPACING+") 10))
                  (vgroup (group-frame)
                    (hgroup '() (h-space 0) bt-up (h-space 0))
                    (hgroup '() bt-left bt-right)
                    (hgroup '() (h-space 0) bt-down (h-space 0)))
                  (centered-text (format nil "Ever wanted to see~%a virtual group in~%a virtual group?"))
                  (hv-space)
                  (centered-text "Here it is!")
                  (scrollgroup inner)
                  (centered-text "Do you like it? I hope...")
                  (hv-space)
                  (centered-text (format nil "I admit, it's a~% bit crazy... :-)~%But it demonstrates~%the power of~%~Aobject oriented~A~%GUI design."
                                         (x "B") (x "N")))
                  (scrollgroup
                   (vgroup-v (append (virtual-frame) (inner-spacing 4 4))
                     (vgroup '() page-cycle page-group)))))))
    ;; the arrows scroll the inner virtual group to its edges
    (mui:notify bt-up    (m "+MUIA-PRESSED+") nil inner (m "+MUIM-SET+") (m "+MUIA-VIRTGROUP-TOP+") 0)
    (mui:notify bt-left  (m "+MUIA-PRESSED+") nil inner (m "+MUIM-SET+") (m "+MUIA-VIRTGROUP-LEFT+") 0)
    (mui:notify bt-right (m "+MUIA-PRESSED+") nil inner (m "+MUIM-SET+") (m "+MUIA-VIRTGROUP-LEFT+") 9999)
    (mui:notify bt-down  (m "+MUIA-PRESSED+") nil inner (m "+MUIM-SET+") (m "+MUIA-VIRTGROUP-TOP+") 9999)
    ;; the cycle picks the page
    (mui:notify page-cycle (m "+MUIA-CYCLE-ACTIVE+") :every-time
                page-group (m "+MUIM-SET+") (m "+MUIA-GROUP-ACTIVE-PAGE+") :trigger-value)
    page))

(defun run ()
  (mui:with-foreign-pool ()
    (let* ((*cycle-chain* '())
           (win (mui:new-object :window
                  (m "+MUIA-WINDOW-TITLE+") "Virtual Groups"
                  (m "+MUIA-WINDOW-ID+")    (mui:make-id "VIRT")
                  (m "+MUIA-WINDOW-USE-RIGHT-BORDER-SCROLLER+") t
                  (m "+MUIA-WINDOW-USE-BOTTOM-BORDER-SCROLLER+") t
                  (m "+MUIA-WINDOW-ROOT-OBJECT+")
                  (col-group 2 (group-spacing 8)
                    (make-page-1)
                    (make-page-2)
                    (make-page-3)
                    (make-page-4))))
           (app (mui:new-object :application
                  (m "+MUIA-APPLICATION-TITLE+")       "VirtualDemo"
                  (m "+MUIA-APPLICATION-VERSION+")     "$VER: VirtualDemo 19.5 (12.02.97)"
                  (m "+MUIA-APPLICATION-COPYRIGHT+")   "(C) 1993, Stefan Stuntz"
                  (m "+MUIA-APPLICATION-AUTHOR+")      "Stefan Stuntz"
                  (m "+MUIA-APPLICATION-DESCRIPTION+") "Show virtual groups."
                  (m "+MUIA-APPLICATION-BASE+")        "VIRTUALDEMO"
                  (m "+MUIA-APPLICATION-WINDOW+")      win)))
      (unwind-protect
           (progn
             (mui:notify win (m "+MUIA-WINDOW-CLOSE-REQUEST+") t
                         :application (m "+MUIM-APPLICATION-RETURN-ID+")
                         (m "+MUIV-APPLICATION-RETURN-ID-QUIT+"))
             ;; DoMethod(window, MUIM_Window_SetCycleChain, obj, ..., NULL)
             (apply #'mui:do-method win (m "+MUIM-WINDOW-SET-CYCLE-CHAIN+")
                    (append *cycle-chain* (list nil)))
             (mui:set-attrs win (m "+MUIA-WINDOW-OPEN+") t)
             (when (zerop (or (mui:get-attr (m "+MUIA-WINDOW-OPEN+") win) 0))
               (error "virtual: the window would not open"))
             (mui:do-application-events ((id) app)))
        (mui:dispose-object app)))))

(if (mui:available-p)
    (run)
    (format t "~&; virtual: MUI is not available on this system (muimaster.library 3.8+ on AmigaOS, or MorphOS, needed) — nothing to show.~%"))
