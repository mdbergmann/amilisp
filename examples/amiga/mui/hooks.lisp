;;; hooks.lisp — struct Hooks written in Lisp, called by MUI: a list whose
;;; rows are formatted by a MUIA_List_DisplayHook (two columns, a title
;;; row), buttons whose MUIA_Pressed notification runs a Lisp hook through
;;; MUIM_CallHook, and a double-click on the list that reports the entry
;;; the same way.  The SDK uses these three idioms in psi.c (display
;;; hooks), Layout.c and AppWindow.c (MUIM_CallHook); this program is
;;; theirs boiled down.
;;;
;;; What it shows: AMIGA.MUI:POOL-HOOK (AMIGA.FFI:MAKE-HOOK, a struct Hook
;;; whose entry is a Lisp function of hook / object / message, freed by
;;; the foreign pool), MUIA_List_DisplayHook filling the STRPTR array MUI
;;; hands it, MUIA_List_ConstructHook / DestructHook with the builtin
;;; string hooks so MUI copies the entries, MUIM_List_InsertSingle,
;;; MUIM_CallHook as a NOTIFY method with a parameter and with
;;; MUIV_TriggerValue, MUIA_Listview_DoubleClick, MUIM_List_GetEntry.
;;;
;;; One MUI rule this program is built around, because getting it wrong is
;;; silent: MUI replaces :TRIGGER-VALUE (MUIV_TriggerValue) in a
;;; notification method *only when the trigger is :EVERY-TIME*.  Notify.mui
;;; says it in one sentence -- "The special value MUIV_EveryTime makes MUI
;;; execute the notification method every time when TrigAttr changes.  In
;;; this case, the special value MUIV_TriggerValue in the notification
;;; method will be replaced with the value that TrigAttr has been set to."
;;; Ask for it under a fixed trigger (..._DoubleClick, TRUE) and no
;;; substitution happens: the hook is handed the raw magic 0x49893131,
;;; which in amilisp is a bignum, not a row index.  So the selection hook
;;; below takes :TRIGGER-VALUE from MUIA_List_Active / :EVERY-TIME, where
;;; it is defined, and the double-click hook asks the list which entry is
;;; active instead -- the idiom every SDK example uses (psi.c, MUI-Demo.c,
;;; WbMan.c, Popup.c all call MUIM_List_GetEntry with
;;; MUIV_List_GetEntry_Active after a MUIA_Listview_DoubleClick, which is
;;; a BOOL and carries TRUE, never an index).
;;;
;;; A hook runs inside MUI, on its stack: an error in it is caught at the
;;; callback, the hook returns 0, and the condition is re-signaled in Lisp
;;; when the MUI call that invoked it returns -- here that is
;;; APPLICATION-INPUT inside the event loop.
;;;
;;; Run on AmigaOS 3.x with MUI 3.8+ installed, or on MorphOS:
;;;   amilisp --load examples/amiga/mui/hooks.lisp
;;; Unattended (auto-closes after 5 s):
;;;   amilisp --eval '(require "amiga/mui")' \
;;;           --eval '(setf amiga.mui:*event-loop-timeout* 5)' \
;;;           --load examples/amiga/mui/hooks.lisp

(require "amiga/mui")

(when (amiga.mui:available-p)
  (require "amiga/raw/muimaster"))

(defpackage "MUI-HOOKS"
  (:use "CL")
  (:local-nicknames ("MUI" "AMIGA.MUI")))

(in-package "MUI-HOOKS")

(defun m (name)
  "The value of the amiga/raw/muimaster constant NAME -- looked up at run
time, so this file reads on a system where that package does not exist."
  (symbol-value (or (find-symbol name "AMIGA.RAW.MUIMASTER")
                    (error "hooks: amiga.raw.muimaster:~A not found" name))))

(defparameter *planets*
  '(("Mercury" 0) ("Venus" 0) ("Earth" 1) ("Mars" 2)
    ("Jupiter" 95) ("Saturn" 146) ("Uranus" 28) ("Neptune" 16)))

(defun planet-at (index)
  "The *PLANETS* row at INDEX, or NIL when there is none.  MUI hands the
index over as an unsigned longword, so MUIV_List_Active_Off (-1) arrives
as #xFFFFFFFF and simply falls outside the list."
  (and (< index (length *planets*)) (nth index *planets*)))

(defun planet-named (name)
  "The *PLANETS* row whose name is NAME -- NIL for NIL, which is what
ACTIVE-ENTRY answers when no entry is active."
  (and name (assoc name *planets* :test #'string=)))

(defun active-entry (list storage)
  "MUIM_List_GetEntry with MUIV_List_GetEntry_Active: the active entry of
LIST -- the string MUI copied at insert time -- as a Lisp string, or NIL
when no entry is active.  STORAGE is a pooled longword the method writes
the entry pointer into, made once so the hook allocates nothing."
  (ffi:poke-u32 storage 0 0)
  (mui:do-method list (m "+MUIM-LIST-GET-ENTRY+")
                 (m "+MUIV-LIST-GET-ENTRY-ACTIVE+") storage)
  (let ((entry (ffi:peek-u32 storage 0)))
    (unless (zerop entry)
      (ffi:foreign-to-string (ffi:make-foreign-pointer entry)))))

(defun children (objects)
  (loop for o in objects collect (m "+MUIA-GROUP-CHILD+") collect o))

(defun vgroup (tags &rest objects)
  (apply #'mui:new-object :group (append tags (children objects))))

(defun hgroup (tags &rest objects)
  (apply #'vgroup (list* (m "+MUIA-GROUP-HORIZ+") t tags) objects))

;;; The display hook.  MUI calls it for every row it renders (and again
;;; for the title row, with a NULL entry) with a2 = the STRPTR array to
;;; fill, one slot per column, and a1 = the entry -- here the string MUI
;;; copied at insert time (the String construct hook).  The strings the
;;; hook hands back must stay valid until the next call: the entry itself
;;; does; the moon count is a pooled string, made once per planet and
;;; cached, so the hook never allocates foreign memory on the fly.

(defun make-display-hook (moons-column)
  "MOONS-COLUMN: planet name -> pooled foreign string of its moon count."
  (let ((title-name  (mui:pool-string "Planet"))
        (title-moons (mui:pool-string "Moons")))
    (mui:pool-hook
     (lambda (hook array entry)
       (declare (ignore hook))
       (if (ffi:null-pointer-p entry)
           (progn                                                 ; the title row
             (ffi:poke-u32 array (ffi:foreign-pointer-address title-name) 0)
             (ffi:poke-u32 array (ffi:foreign-pointer-address title-moons) 4))
           (let ((moons (gethash (ffi:foreign-to-string entry) moons-column)))
             (ffi:poke-u32 array (ffi:foreign-pointer-address entry) 0)
             (ffi:poke-u32 array (ffi:foreign-pointer-address moons) 4)))
       0))))

(defun run ()
  (mui:with-foreign-pool ()
    (let* ((moons-column (make-hash-table :test #'equal))
           (report (mui:new-object :text
                     (m "+MUIA-FRAME+") (m "+MUIV-FRAME-TEXT+")
                     (m "+MUIA-BACKGROUND+") (m "+MUII-TEXT-BACK+")
                     (m "+MUIA-TEXT-SET-MIN+") nil
                     (m "+MUIA-TEXT-CONTENTS+")
                     (concatenate 'string (m "+MUIX-C+")
                                  "Press a button, or click and double-click a planet.")))
           (calls 0)
           (list (mui:new-object :list
                   (m "+MUIA-FRAME+") (m "+MUIV-FRAME-INPUT-LIST+")
                   (m "+MUIA-LIST-FORMAT+") "BAR,"                    ; two columns
                   (m "+MUIA-LIST-TITLE+") t                          ; the hook sees a NULL entry
                   (m "+MUIA-LIST-DISPLAY-HOOK+") (make-display-hook moons-column)
                   (m "+MUIA-LIST-CONSTRUCT-HOOK+") (m "+MUIV-LIST-CONSTRUCT-HOOK-STRING+")
                   (m "+MUIA-LIST-DESTRUCT-HOOK+") (m "+MUIV-LIST-DESTRUCT-HOOK-STRING+")))
           (listview (mui:new-object :listview
                       (m "+MUIA-LISTVIEW-LIST+") list))
           (entry-storage (mui:pool-alloc 4))       ; MUIM_List_GetEntry's APTR *
           ;; MUIM_CallHook's hook: a1 = the parameters after the hook in
           ;; the MUIM_CallHook message, here one longword
           (press-hook (mui:pool-hook
                        (lambda (hook app message)
                          (declare (ignore hook app))
                          (let ((n (ffi:peek-u32 message 0)))
                            (incf calls)
                            (mui:set-attrs report (m "+MUIA-TEXT-CONTENTS+")
                                           (format nil "~AButton ~D pressed -- hook call ~D."
                                                   (m "+MUIX-C+") n calls)))
                          0)))
           ;; the same, with MUIV_TriggerValue -- taken from MUIA_List_Active
           ;; under an :EVERY-TIME trigger, the one place MUI substitutes it
           (select-hook (mui:pool-hook
                         (lambda (hook app message)
                           (declare (ignore hook app))
                           (let ((planet (planet-at (ffi:peek-u32 message 0))))
                             (incf calls)
                             (mui:set-attrs report (m "+MUIA-TEXT-CONTENTS+")
                                            (if planet
                                                (format nil "~A~A selected -- hook call ~D."
                                                        (m "+MUIX-C+") (first planet) calls)
                                                (format nil "~ANothing selected -- hook call ~D."
                                                        (m "+MUIX-C+") calls))))
                           0)))
           ;; the double click carries no index: ask the list for its active
           ;; entry, the way psi.c and MUI-Demo.c do
           (click-hook (mui:pool-hook
                        (lambda (hook app message)
                          (declare (ignore hook app message))
                          (let ((planet (planet-named (active-entry list entry-storage))))
                            (incf calls)
                            (when planet
                              (mui:set-attrs report (m "+MUIA-TEXT-CONTENTS+")
                                             (format nil "~A~A has ~D moon~:P -- hook call ~D."
                                                     (m "+MUIX-C+") (first planet) (second planet)
                                                     calls))))
                          0)))
           (one   (mui:make-object :button "Hook _1"))
           (two   (mui:make-object :button "Hook _2"))
           (three (mui:make-object :button "Hook _3"))
           (quit  (mui:make-object :button "_Quit"))
           (win (mui:new-object :window
                  (m "+MUIA-WINDOW-TITLE+") "Hooks"
                  (m "+MUIA-WINDOW-ID+")    (mui:make-id "HOOK")
                  (m "+MUIA-WINDOW-ROOT-OBJECT+")
                  (vgroup '()
                    (mui:new-object :text
                      (m "+MUIA-FRAME+") (m "+MUIV-FRAME-TEXT+")
                      (m "+MUIA-BACKGROUND+") (m "+MUII-TEXT-BACK+")
                      (m "+MUIA-TEXT-CONTENTS+")
                      (concatenate 'string (m "+MUIX-C+")
                                   (format nil "The list rows come from a Lisp display hook,~%the buttons, the selection and a double-click~%call Lisp hooks through MUIM_CallHook.")))
                    listview
                    report
                    (hgroup (list (m "+MUIA-GROUP-SAME-SIZE+") t) one two three quit))))
           (app (mui:new-object :application
                  (m "+MUIA-APPLICATION-TITLE+")       "Hooks"
                  (m "+MUIA-APPLICATION-VERSION+")     "$VER: Hooks 1.0"
                  (m "+MUIA-APPLICATION-DESCRIPTION+") "Lisp hooks called by MUI"
                  (m "+MUIA-APPLICATION-BASE+")        "HOOKS"
                  (m "+MUIA-APPLICATION-WINDOW+")      win)))
      (unwind-protect
           (progn
             ;; the entries: MUI copies each string (String construct hook),
             ;; the display hook's second column is made once per planet
             (loop for (name moons) in *planets*
                   do (setf (gethash name moons-column)
                            (mui:pool-string (format nil "~D" moons)))
                      (mui:do-method list (m "+MUIM-LIST-INSERT-SINGLE+")
                                     (mui:pool-string name) (m "+MUIV-LIST-INSERT-BOTTOM+")))
             (mui:notify win (m "+MUIA-WINDOW-CLOSE-REQUEST+") t
                         :application (m "+MUIM-APPLICATION-RETURN-ID+")
                         (m "+MUIV-APPLICATION-RETURN-ID-QUIT+"))
             (mui:notify quit (m "+MUIA-PRESSED+") nil
                         :application (m "+MUIM-APPLICATION-RETURN-ID+")
                         (m "+MUIV-APPLICATION-RETURN-ID-QUIT+"))
             ;; DoMethod(button, MUIM_Notify, MUIA_Pressed, FALSE,
             ;;          app, 3, MUIM_CallHook, &hook, i)
             (loop for button in (list one two three)
                   for i from 1
                   do (mui:notify button (m "+MUIA-PRESSED+") nil
                                  :application (m "+MUIM-CALL-HOOK+") press-hook i))
             ;; DoMethod(list, MUIM_Notify, MUIA_List_Active, MUIV_EveryTime,
             ;;          app, 3, MUIM_CallHook, &hook, MUIV_TriggerValue)
             ;; -- an :EVERY-TIME trigger, so MUI substitutes the new value
             (mui:notify list (m "+MUIA-LIST-ACTIVE+") :every-time
                         :application (m "+MUIM-CALL-HOOK+") select-hook :trigger-value)
             ;; a double click carries TRUE, so there is nothing worth
             ;; substituting: the hook reads the active entry itself.  The 0
             ;; is a placeholder parameter, there only so the hook's a1
             ;; points inside the notification message rather than past it.
             (mui:notify listview (m "+MUIA-LISTVIEW-DOUBLE-CLICK+") t
                         :application (m "+MUIM-CALL-HOOK+") click-hook 0)
             (mui:set-attrs win (m "+MUIA-WINDOW-OPEN+") t)
             (when (zerop (or (mui:get-attr (m "+MUIA-WINDOW-OPEN+") win) 0))
               (error "hooks: the window would not open"))
             (mui:do-application-events ((id) app)
               nil)                   ; no return IDs of our own
             (mui:set-attrs win (m "+MUIA-WINDOW-OPEN+") nil)
             (format t "~&; hooks: the Lisp hooks were called ~D time~:P~%" calls))
        (mui:dispose-object app)))))          ; then the pool frees the hooks

(if (mui:available-p)
    (run)
    (format t "~&; hooks: MUI is not available on this system (muimaster.library 3.8+ on AmigaOS, or MorphOS, needed) — nothing to show.~%"))
