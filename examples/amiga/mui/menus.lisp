;;; menus.lisp — the MUI SDK's Menus.c: a window with a menu strip, and
;;; every way of reacting to menus MUI offers -- return IDs from
;;; MUIA_UserData, MUIA_Application_MenuAction notifications, checkmarks
;;; and radio items kept in step with gadgets through MUIM_SetUData /
;;; MUIM_GetUData / MUIM_FindUData, whole menus enabled and disabled,
;;; a menu added and removed while the window is open, titles renamed.
;;;
;;; The C builds its static strip from a gadtools struct NewMenu array
;;; (MUIO_MenustripNM); this program builds the same strip from
;;; Menustrip / Menu / Menuitem objects, which is the other way Menus.c
;;; describes and needs no foreign struct.  The dynamic "Misc" menu uses
;;; MUI_MakeObject(MUIO_Menuitem, ...) as the C does.
;;;
;;; What it shows: MenustripObject / MenuObject / MenuitemObject with
;;; MUIA_Family_Child, MUIA_Menuitem_Shortcut / _Checkit / _Checked /
;;; _Toggle / _Exclude / _Enabled, NM_BARLABEL, MUIA_Window_Menustrip,
;;; MUIA_UserData as the return ID of a selected item,
;;; MUIA_Application_MenuAction, MUIM_FindUData / MUIM_SetUData /
;;; MUIM_GetUData, MUIM_Family_Insert / _Remove, MUIO_Cycle / MUIO_Radio /
;;; MUIO_Checkmark / MUIO_HBar / MUIO_VBar, MUI_Request.
;;;
;;; Run on AmigaOS 3.x with MUI 3.8+ installed, or on MorphOS:
;;;   amilisp --load examples/amiga/mui/menus.lisp
;;; Unattended (auto-closes after 5 s):
;;;   amilisp --eval '(require "amiga/mui")' \
;;;           --eval '(setf amiga.mui:*event-loop-timeout* 5)' \
;;;           --load examples/amiga/mui/menus.lisp

(require "amiga/mui")

(when (amiga.mui:available-p)
  (require "amiga/raw/muimaster"))

(defpackage "MUI-MENUS"
  (:use "CL")
  (:local-nicknames ("MUI" "AMIGA.MUI")))

(in-package "MUI-MENUS")

;;; The C's enum: menu user data doubles as return IDs.
(defconstant +men-project+  1)
(defconstant +men-about+    2)
(defconstant +men-quit+     3)
(defconstant +men-edit+     4)
(defconstant +men-cut+      5)
(defconstant +men-copy+     6)
(defconstant +men-paste+    7)
(defconstant +men-settings+ 8)
(defconstant +men-hardware+ 9)
(defconstant +men-a1000+   10)
(defconstant +men-a2000+   11)
(defconstant +men-a3000+   12)
(defconstant +men-a4000+   13)
(defconstant +men-software+ 14)
(defconstant +men-mui+     15)
(defconstant +men-mfr+     16)
(defconstant +men-mwb+     17)
(defconstant +men-dfa+     18)
(defconstant +id-add+      19)
(defconstant +id-rem+      20)
(defconstant +id-radio+    21)
(defconstant +id-tog+      22)

(defun m (name)
  "The value of the amiga/raw/muimaster constant NAME -- looked up at run
time, so this file reads on a system where that package does not exist."
  (symbol-value (or (find-symbol name "AMIGA.RAW.MUIMASTER")
                    (error "menus: amiga.raw.muimaster:~A not found" name))))

(defun i (name)
  "The value of the amiga/raw/intuition constant NAME (CHECKIT ...)."
  (symbol-value (or (find-symbol name "AMIGA.RAW.INTUITION")
                    (error "menus: amiga.raw.intuition:~A not found" name))))

;;; The mui.h macros this program uses, as functions.

(defun children (objects)
  (loop for o in objects collect (m "+MUIA-GROUP-CHILD+") collect o))

(defun vgroup (tags &rest objects)
  (apply #'mui:new-object :group (append tags (children objects))))

(defun hgroup (tags &rest objects)
  (apply #'vgroup (list* (m "+MUIA-GROUP-HORIZ+") t tags) objects))

(defun col-group (columns tags &rest objects)
  (apply #'vgroup (list* (m "+MUIA-GROUP-COLUMNS+") columns tags) objects))

(defun same-size () (list (m "+MUIA-GROUP-SAME-SIZE+") t))
(defun h-space (n) (mui:make-object :h-space n))
(defun label (text) (mui:make-object :label text 0))

;;; The menu tree.  MUIA_Family_Child attaches menus to the strip, items
;;; to a menu, and sub-items to an item.

(defun family (objects)
  (loop for o in objects collect (m "+MUIA-FAMILY-CHILD+") collect o))

(defun item (title user-data &key shortcut checkit checked toggle (exclude 0) (enabled t) subitems)
  "MenuitemObject: TITLE with the shortcut key SHORTCUT (a string);
USER-DATA is what MUIM_Application_Input returns when it is selected.
CHECKIT makes a checkmark item, TOGGLE one that flips on every selection,
EXCLUDE the bit mask of sibling items unchecked when it is checked (the
gadtools MutualExclude); SUBITEMS make it a sub-menu."
  (apply #'mui:new-object :menuitem
         (append
          (list (m "+MUIA-MENUITEM-TITLE+") title
                (m "+MUIA-USER-DATA+") user-data
                (m "+MUIA-MENUITEM-ENABLED+") enabled)
          (and shortcut (list (m "+MUIA-MENUITEM-SHORTCUT+") shortcut))
          (and checkit (list (m "+MUIA-MENUITEM-CHECKIT+") t
                             (m "+MUIA-MENUITEM-CHECKED+") checked
                             (m "+MUIA-MENUITEM-TOGGLE+") toggle
                             (m "+MUIA-MENUITEM-EXCLUDE+") exclude))
          (family subitems))))

(defun bar-label ()
  "A separator: MUIA_Menuitem_Title NM_BARLABEL, which is (STRPTR)~0."
  (mui:new-object :menuitem (m "+MUIA-MENUITEM-TITLE+") #xFFFFFFFF))

(defun menu (title user-data &key (enabled t) items)
  (apply #'mui:new-object :menu
         (m "+MUIA-MENU-TITLE+") title
         (m "+MUIA-USER-DATA+") user-data
         (m "+MUIA-MENU-ENABLED+") enabled
         (family items)))

(defun make-strip ()
  "The strip of Menus.c's MenuData1, as objects."
  (apply #'mui:new-object :menustrip
         (family
          (list
           (menu "Project" +men-project+
                 :items (list (item "About" +men-about+ :shortcut "?")
                              (bar-label)
                              (item "Quit" +men-quit+ :shortcut "Q")))
           (menu "Edit" +men-edit+
                 :items (list (item "Cut" +men-cut+ :shortcut "X")
                              (item "Copy" +men-copy+ :shortcut "C")
                              (item "Paste" +men-paste+ :shortcut "V")))
           (menu "Settings" +men-settings+ :enabled nil
                 :items
                 (list
                  (item "Hardware" +men-hardware+ :enabled nil
                        :subitems
                        ;; radio items: checking one unchecks the other three
                        (list (item "A1000" +men-a1000+ :shortcut "1" :checkit t :checked t
                                    :exclude (logior 2 4 8))
                              (item "A2000" +men-a2000+ :shortcut "2" :checkit t
                                    :exclude (logior 1 4 8))
                              (item "A3000" +men-a3000+ :shortcut "3" :checkit t
                                    :exclude (logior 1 2 8))
                              (item "A4000" +men-a4000+ :shortcut "4" :checkit t
                                    :exclude (logior 1 2 4))))
                  (item "Software" +men-software+ :enabled nil
                        :subitems
                        ;; toggle items
                        (list (item "MUI"     +men-mui+ :shortcut "M" :checkit t :toggle t)
                              (item "MFR"     +men-mfr+ :shortcut "F" :checkit t :toggle t)
                              (item "MagicWB" +men-mwb+ :shortcut "W" :checkit t :toggle t)
                              (item "DFA"     +men-dfa+ :shortcut "D" :checkit t :toggle t)))))))))

;;; MUIM_FindUData / MUIM_GetUData: the objects below a strip by user data.

(defun find-udata (object user-data)
  "MUIM_FindUData: the object below OBJECT whose MUIA_UserData is USER-DATA."
  (let ((address (mui:do-method object (m "+MUIM-FIND-U-DATA+") user-data)))
    (when (zerop address)
      (error "menus: no object with MUIA_UserData ~D below ~A" user-data object))
    (ffi:make-foreign-pointer address)))

(defun get-udata (object user-data attribute)
  "MUIM_GetUData: ATTRIBUTE of the object below OBJECT whose MUIA_UserData
is USER-DATA, as an unsigned longword."
  (let ((storage (ffi:alloc-foreign 4)))
    (unwind-protect
         (progn
           (ffi:poke-u32 storage 0 0)
           (mui:do-method object (m "+MUIM-GET-U-DATA+") user-data attribute storage)
           (ffi:peek-u32 storage 0))
      (ffi:free-foreign storage))))

(defun run ()
  (mui:with-foreign-pool ()
    (let* ((hardware '("Amiga 1000" "Amiga 2000" "Amiga 3000" "Amiga 4000"))
           (endis1   '("Settings disabled" "Settings enabled"))
           (endis2   '("Hardware disabled" "Hardware enabled"))
           (endis3   '("Software disabled" "Software enabled"))
           (strip (make-strip))
           (cy1   (mui:make-object :cycle nil endis1))
           (cy2   (mui:make-object :cycle nil endis2))
           (cy3   (mui:make-object :cycle nil endis3))
           (radio (mui:make-object :radio nil hardware))
           (cm1   (mui:make-object :checkmark "_MUI"))
           (cm2   (mui:make-object :checkmark "M_FR"))
           (cm3   (mui:make-object :checkmark "Magic_WB"))
           (cm4   (mui:make-object :checkmark "_DFA"))
           (btadd (mui:make-object :button "_Add Misc Menu"))
           (btrem (mui:make-object :button "_Remove Misc Menu"))
           (bttog (mui:make-object :button "_Toggle Misc Titles"))
           (win (mui:new-object :window
                  (m "+MUIA-WINDOW-TITLE+") "Menus"
                  (m "+MUIA-WINDOW-ID+")    (mui:make-id "MEN1")
                  (m "+MUIA-WINDOW-MENUSTRIP+") strip
                  (m "+MUIA-WINDOW-ROOT-OBJECT+")
                  (vgroup '()
                    cy1
                    (hgroup (same-size) cy2 cy3)
                    (mui:make-object :h-bar 4)
                    (hgroup '()
                      (h-space 0)
                      radio
                      (h-space 0)
                      (mui:make-object :v-bar 1)
                      (h-space 0)
                      (col-group 2 '()
                        (label "_MUI")     cm1
                        (label "M_FR")     cm2
                        (label "Magic_WB") cm3
                        (label "_DFA")     cm4)
                      (h-space 0))
                    (mui:make-object :h-bar 4)
                    (hgroup (same-size) btadd btrem bttog))))
           (app (mui:new-object :application
                  (m "+MUIA-APPLICATION-TITLE+")       "Menus"
                  (m "+MUIA-APPLICATION-VERSION+")     "$VER: Menus 19.5 (12.02.97)"
                  (m "+MUIA-APPLICATION-COPYRIGHT+")   "(C) 1994 by Stefan Stuntz"
                  (m "+MUIA-APPLICATION-AUTHOR+")      "Stefan Stuntz"
                  (m "+MUIA-APPLICATION-DESCRIPTION+") "Demonstrate MUI's menu classes."
                  (m "+MUIA-APPLICATION-BASE+")        "MENU"
                  (m "+MUIA-APPLICATION-WINDOW+")      win))
           ;; the dynamic menu, items from MUI_MakeObject(MUIO_Menuitem,
           ;; label, shortcut, flags, data)
           (possi (mui:make-object :menuitem "Possible" nil
                                   (logior (i "+CHECKIT+") (i "+CHECKED+") (i "+MENUTOGGLE+"))
                                   0))
           (menumisc (apply #'mui:new-object :menu
                            (m "+MUIA-MENU-TITLE+") "Misc"
                            (family (list (mui:make-object :menuitem "Dynamic" nil 0 0)
                                          (mui:make-object :menuitem "Adding"  nil 0 0)
                                          (mui:make-object :menuitem "Of"      nil 0 0)
                                          (mui:make-object :menuitem "Items"   nil 0 0)
                                          (mui:make-object :menuitem "Is"      nil 0 0)
                                          possi))))
           (added nil)
           ;; the menu objects the gadgets steer, found by their user data
           (menuset  (find-udata strip +men-settings+))
           (menuedit (find-udata strip +men-edit+))
           (itemhw   (find-udata strip +men-hardware+))
           (itemsw   (find-udata strip +men-software+))
           (disabled (m "+MUIA-DISABLED+")))
      (unwind-protect
           (progn
             ;; the cycles enable / disable a menu and two items
             (mui:notify cy1 (m "+MUIA-CYCLE-ACTIVE+") :every-time
                         menuset (m "+MUIM-SET+") (m "+MUIA-MENU-ENABLED+") :trigger-value)
             (mui:notify cy2 (m "+MUIA-CYCLE-ACTIVE+") :every-time
                         itemhw (m "+MUIM-SET+") (m "+MUIA-MENUITEM-ENABLED+") :trigger-value)
             (mui:notify cy3 (m "+MUIA-CYCLE-ACTIVE+") :every-time
                         itemsw (m "+MUIM-SET+") (m "+MUIA-MENUITEM-ENABLED+") :trigger-value)
             ;; the buttons report through return IDs
             (mui:notify btadd (m "+MUIA-PRESSED+") nil
                         :application (m "+MUIM-APPLICATION-RETURN-ID+") +id-add+)
             (mui:notify btrem (m "+MUIA-PRESSED+") nil
                         :application (m "+MUIM-APPLICATION-RETURN-ID+") +id-rem+)
             (mui:notify bttog (m "+MUIA-PRESSED+") nil
                         :application (m "+MUIM-APPLICATION-RETURN-ID+") +id-tog+)
             ;; a radio menu item selected -> the radio gadget follows:
             ;; MUIA_Application_MenuAction carries the item's user data
             (loop for men in (list +men-a1000+ +men-a2000+ +men-a3000+ +men-a4000+)
                   for n from 0
                   do (mui:notify app (m "+MUIA-APPLICATION-MENU-ACTION+") men
                                  radio (m "+MUIM-SET+") (m "+MUIA-RADIO-ACTIVE+") n))
             ;; a checkmark gadget -> the toggle item with the same user data
             (loop for cm in (list cm1 cm2 cm3 cm4)
                   for men in (list +men-mui+ +men-mfr+ +men-mwb+ +men-dfa+)
                   do (mui:notify cm (m "+MUIA-SELECTED+") :every-time
                                  strip (m "+MUIM-SET-U-DATA+") men
                                  (m "+MUIA-MENUITEM-CHECKED+") :trigger-value)
                      ;; ...and the way back needs the gadget's user data
                      (mui:set-attrs cm (m "+MUIA-USER-DATA+") men))
             (mui:notify radio (m "+MUIA-RADIO-ACTIVE+") :every-time
                         :application (m "+MUIM-APPLICATION-RETURN-ID+") +id-radio+)
             (mui:notify win (m "+MUIA-WINDOW-CLOSE-REQUEST+") t
                         :application (m "+MUIM-APPLICATION-RETURN-ID+")
                         (m "+MUIV-APPLICATION-RETURN-ID-QUIT+"))
             (mui:set-attrs btrem disabled t)
             (mui:set-attrs win (m "+MUIA-WINDOW-OPEN+") t)
             (when (zerop (or (mui:get-attr (m "+MUIA-WINDOW-OPEN+") win) 0))
               (error "menus: the window would not open"))
             (mui:do-application-events ((id) app)
               (cond
                 ((= id +men-quit+)
                  (return))
                 ((= id +men-about+)
                  (mui:request app win nil "OK" "Some little about window."))
                 ((= id +id-add+)
                  (mui:set-attrs btadd disabled t)
                  (mui:set-attrs btrem disabled nil)
                  ;; MUIM_Family_Insert: the Misc menu after the Edit menu
                  (mui:do-method strip (m "+MUIM-FAMILY-INSERT+") menumisc menuedit)
                  (setf added t))
                 ((= id +id-rem+)
                  (mui:set-attrs btadd disabled nil)
                  (mui:set-attrs btrem disabled t)
                  (mui:do-method strip (m "+MUIM-FAMILY-REMOVE+") menumisc)
                  (setf added nil))
                 ((= id +id-tog+)
                  ;; rename the menu and its last item -- titles are read
                  ;; back with GET-ATTR-STRING and set from Lisp strings
                  (let ((title (m "+MUIA-MENU-TITLE+")))
                    (mui:set-attrs menumisc title
                                   (if (string= (mui:get-attr-string title menumisc) "Misc")
                                       "Miscodil" "Misc")))
                  (let ((title (m "+MUIA-MENUITEM-TITLE+")))
                    (mui:set-attrs possi title
                                   (if (string= (mui:get-attr-string title possi) "Possible")
                                       "elbissoP" "Possible"))))
                 ((<= +men-mui+ id +men-dfa+)
                  ;; a toggle item was selected: copy its state to the checkmark
                  (let ((checked (get-udata strip id (m "+MUIA-MENUITEM-CHECKED+"))))
                    (mui:do-method win (m "+MUIM-SET-U-DATA+") id (m "+MUIA-SELECTED+") checked)))
                 ((= id +id-radio+)
                  ;; the radio gadget changed: check the matching item
                  (let ((active (mui:get-attr (m "+MUIA-RADIO-ACTIVE+") radio)))
                    (loop for men in (list +men-a1000+ +men-a2000+ +men-a3000+ +men-a4000+)
                          for n from 0
                          do (mui:do-method itemhw (m "+MUIM-SET-U-DATA+") men
                                            (m "+MUIA-MENUITEM-CHECKED+") (= active n))))))))
        ;; the Misc menu belongs to the strip only while it is inserted
        (unless added
          (mui:dispose-object menumisc))
        (mui:dispose-object app)))))

(if (mui:available-p)
    (run)
    (format t "~&; menus: MUI is not available on this system (muimaster.library 3.8+ on AmigaOS, or MorphOS, needed) — nothing to show.~%"))
