;;; test-mui.lisp — Amiga-side tests of lib/amiga/mui.lisp (AMIGA.MUI,
;;; the MUI helpers over AMIGA.BOOPSI and muimaster.library).
;;;
;;; Loaded from run-tests.lisp via (load ...) inside #+amigaos, like
;;; test-reaction.lisp: CHECK comes from run-tests.lisp.  The portable
;;; half (CLASS-ID, MAKE-ID, the message packing, the diagnostics) is
;;; mirrored on the host by tests/test_amiga_mui.sh and only spot-checked
;;; here on real 32-bit addresses; the object half needs MUI (3.8 in the
;;; FS-UAE Workbench, MUI 4 built into MorphOS) and is skipped -- with an
;;; assertion that AVAILABLE-P says so -- where it is absent.  Nothing
;;; here needs a user: the notification test fires MUIM_Notify by setting
;;; the attribute from Lisp, the loop test relies on the timeout.
;;; MUI_RequestA is interactive and covered by the example only.

(require "amiga/mui")
(format t "; mui: amiga/mui loaded~%")
(finish-output)

;;; --- portable half, on real 32-bit addresses ----------------------------

;; the same symbols as AMIGA.BOOPSI, not homonyms
(check "mui-shares-boopsi-symbols" t
  (let ((ok t))
    (dolist (name '("DO-METHOD" "OBJECT-CLASS" "WITH-FOREIGN-POOL" "POOL-ALLOC"
                    "POOL-STRING" "WITH-TAGS" "GET-ATTR" "GET-ATTR-POINTER" "SET-ATTRS"
                    "%ULONG" "%WITH-TAGS"))
      (unless (eq (find-symbol name "AMIGA.MUI") (find-symbol name "AMIGA.BOOPSI"))
        (setf ok nil)))
    ok))

(check "mui-class-id" '("Window.mui" "Numericbutton.mui" "Group.mui")
  (mapcar #'amiga.mui:class-id '(:window :numeric-button "Group.mui")))

(check "mui-make-id" #x4D41494E (amiga.mui:make-id "MAIN"))

;; POOL-STRING-ARRAY: the STRPTR[] reads back, NULL-terminated
(check "mui-pool-string-array-reads-back" '("Fast" "Slow" 0)
  (amiga.mui:with-foreign-pool ()
    (let ((a (amiga.mui:pool-string-array '("Fast" "Slow"))))
      (list (ffi:foreign-to-string (ffi:make-foreign-pointer (ffi:peek-u32 a 0)))
            (ffi:foreign-to-string (ffi:make-foreign-pointer (ffi:peek-u32 a 4)))
            (ffi:peek-u32 a 8)))))

;; the MUI_MakeObject parameter array: a pooled label, a pooled STRPTR[]
(check "mui-ulong-array-strings-read-back" '(7 "Mode" "a" "b" 0)
  (amiga.mui:with-foreign-pool ()
    (amiga.mui::%with-ulong-array (a '(7 "Mode" ("a" "b")) :pooled t)
      (let ((entries (ffi:make-foreign-pointer (ffi:peek-u32 a 8))))
        (list (ffi:peek-u32 a 0)
              (ffi:foreign-to-string (ffi:make-foreign-pointer (ffi:peek-u32 a 4)))
              (ffi:foreign-to-string (ffi:make-foreign-pointer (ffi:peek-u32 entries 0)))
              (ffi:foreign-to-string (ffi:make-foreign-pointer (ffi:peek-u32 entries 4)))
              (ffi:peek-u32 entries 8))))))

(check "mui-notify-rejects-bad-destination" t
  (handler-case (progn (amiga.mui:notify nil 1 t :nowhere 2) nil)
    (error (e) (and (search ":NOWHERE" (format nil "~A" e)) t))))

;;; --- MUI half -------------------------------------------------------------

(defvar *mui-p* (amiga.mui:available-p))
(format t "; mui: muimaster.library ~:[absent — skipping the object checks~;present~]~%" *mui-p*)
(finish-output)

;; AVAILABLE-P agrees with the OS: muimaster.library >= MUIMASTER_VMIN opens
(check "mui-available-p-matches-muimaster" t
  (eq (if *mui-p* t nil)
      (let ((b (amiga:open-library "muimaster.library" 11)))
        (when b (amiga:close-library b))
        (if b t nil))))

;; ... and a T answer left the module's base open (a real library base:
;; lib_Version is at least MUIMASTER_VMIN)
(check "mui-available-p-opened-the-base" t
  (if *mui-p*
      (and amiga.mui::*muimaster-base*
           (>= (amiga.ffi:library-version amiga.mui::*muimaster-base*) 11))
      (null amiga.mui::*muimaster-base*)))

;; The raw module is only REQUIREd where MUI exists, so nothing in this
;; file may spell an amiga.raw.muimaster: symbol -- the reader would need
;; the package before the REQUIRE has run (it did not exist in a MUI-only
;; run on MorphOS).  Every constant goes through %M instead.
(defun %mui-sym (name)
  (or (find-symbol name "AMIGA.RAW.MUIMASTER")
      (error "AMIGA.RAW.MUIMASTER::~A not found" name)))
(defun %m (name) (symbol-value (%mui-sym name)))

(when *mui-p*
  (require "amiga/raw/muimaster")
  (format t "; mui: raw module loaded (muimaster ~D)~%" (%m "*MUIMASTER-VERSION*"))
  (finish-output))

;; the module's private copies of the mui.h values must agree with the
;; generated ones (the host cross-checks all of them; here the ones the
;; loop and NOTIFY depend on, against the library actually running)
(check "mui-constants-match-generated" t
  (if *mui-p*
      (and (= amiga.mui::+muim-notify+ (%m "+MUIM-NOTIFY+"))
           (= amiga.mui::+muim-application-new-input+ (%m "+MUIM-APPLICATION-NEW-INPUT+"))
           (= amiga.mui::+muim-application-return-id+ (%m "+MUIM-APPLICATION-RETURN-ID+"))
           (= amiga.mui::+muiv-application-return-id-quit+ (%m "+MUIV-APPLICATION-RETURN-ID-QUIT+"))
           (= amiga.mui::+muiv-every-time+ (%m "+MUIV-EVERY-TIME+"))
           (= amiga.mui::+muiv-notify-application+ (%m "+MUIV-NOTIFY-APPLICATION+"))
           (= amiga.mui::+muio-button+ (%m "+MUIO-BUTTON+"))
           (= amiga.mui::+muimaster-vmin+ (%m "+MUIMASTER-VMIN+"))
           t)
      t))

;; CLASS-ID's keyword rule against the raw MUIC_ strings, on the Amiga too
(check "mui-class-id-matches-raw-muic" t
  (if *mui-p*
      (and (string= (amiga.mui:class-id :window) (%m "+MUIC-WINDOW+"))
           (string= (amiga.mui:class-id :application) (%m "+MUIC-APPLICATION+"))
           (string= (amiga.mui:class-id :numeric-button) (%m "+MUIC-NUMERICBUTTON+"))
           (string= (amiga.mui:class-id :scrollgroup) (%m "+MUIC-SCROLLGROUP+")))
      t))

;; NEW-OBJECT by keyword and by the raw MUIC_ string: a Rectangle, a Text
;; with a pooled string tag; OBJECT-CLASS is a class; DISPOSE-OBJECT
(check "mui-new-object-rectangle-text-dispose" '(t t t t nil)
  (if *mui-p*
      (amiga.mui:with-foreign-pool ()
        (let ((rect (amiga.mui:new-object :rectangle))
              (text (amiga.mui:new-object (%m "+MUIC-TEXT+")
                                          (%m "+MUIA-TEXT-CONTENTS+") "Hello")))
          (list (and rect (ffi:foreign-pointer-p rect) (not (ffi:null-pointer-p rect)))
                (and text (ffi:foreign-pointer-p text) t)
                (let ((cls (amiga.mui:object-class rect)))
                  (and cls (ffi:foreign-pointer-p cls) (not (ffi:null-pointer-p cls))))
                ;; MUIA_Text_Contents reads back through GET-ATTR-STRING
                (equal "Hello" (amiga.mui:get-attr-string (%m "+MUIA-TEXT-CONTENTS+") text))
                (progn (amiga.mui:dispose-object text)
                       (amiga.mui:dispose-object rect)))))
      '(t t t t nil)))

;; an unknown class -> MUI_NewObjectA returns NULL -> an error naming the
;; class (and the keyword it came from).  (Not "Nonexistent.mui": a real
;; MorphOS box turned out to have a class of that name in MUI:Libs/mui.)
(check "mui-new-object-unknown-class-is-error" '(t t)
  (if *mui-p*
      (list (handler-case (progn (amiga.mui:new-object "Clamigabogus.mui") nil)
              (error (e) (and (search "Clamigabogus.mui" (format nil "~A" e)) t)))
            (handler-case (progn (amiga.mui:new-object :no-such-class) nil)
              (error (e) (let ((m (format nil "~A" e)))
                           (and (search "Nosuchclass.mui" m) (search ":NO-SUCH-CLASS" m) t)))))
      '(t t)))

;; MAKE-OBJECT: MUI_MakeObject's builtin button, a slider with min/max, a
;; cycle whose entries are a list of strings
(check "mui-make-object-button-slider-cycle" '(t t t)
  (if *mui-p*
      (amiga.mui:with-foreign-pool ()
        (let ((btn (amiga.mui:make-object :button "_OK"))
              (sld (amiga.mui:make-object :slider "Volume" 0 64))
              (cyc (amiga.mui:make-object :cycle "Mode" '("Fast" "Slow"))))
          (prog1 (list (and btn (ffi:foreign-pointer-p btn) t)
                       (and sld (ffi:foreign-pointer-p sld) t)
                       (and cyc (ffi:foreign-pointer-p cyc) t))
            (amiga.mui:dispose-object cyc)
            (amiga.mui:dispose-object sld)
            (amiga.mui:dispose-object btn))))
      '(t t t)))

;; a String object: SET-ATTRS of MUIA_String_Contents (a pooled string),
;; read back with GET-ATTR-STRING; GET-ATTR of an unknown attribute is NIL
(check "mui-string-contents-round-trip" '("" "typed" nil)
  (if *mui-p*
      (amiga.mui:with-foreign-pool ()
        (let ((str (amiga.mui:new-object :string
                                         (%m "+MUIA-STRING-MAX-LEN+") 32)))
          (unwind-protect
               (list (amiga.mui:get-attr-string (%m "+MUIA-STRING-CONTENTS+") str)
                     (progn (amiga.mui:set-attrs str (%m "+MUIA-STRING-CONTENTS+") "typed")
                            (amiga.mui:get-attr-string (%m "+MUIA-STRING-CONTENTS+") str))
                     (amiga.mui:get-attr #x8EADBEEF str))
            (amiga.mui:dispose-object str))))
      '("" "typed" nil)))

;;; The full life of an application: Application / Window / Group / String
;;; / Text tree, notifications, the return-ID queue, open / close, the
;;; bounded event loop, disposal of everything through the application.

(defun %mui-make-app (string-cell)
  "An Application owning one Window; the String object is stored in
STRING-CELL for the notification test.  Returns the application."
  (let* ((str (amiga.mui:new-object :string
                                    (%m "+MUIA-STRING-MAX-LEN+") 64
                                    (%m "+MUIA-STRING-CONTENTS+") "initial"))
         (win (amiga.mui:new-object :window
                                    (%m "+MUIA-WINDOW-TITLE+") "AMIGA.MUI test"
                                    (%m "+MUIA-WINDOW-ID+") (amiga.mui:make-id "TEST")
                                    (%m "+MUIA-WINDOW-ROOT-OBJECT+")
                                    (amiga.mui:new-object :group
                                                          (%m "+MUIA-GROUP-CHILD+")
                                                          (amiga.mui:new-object :text
                                                                                (%m "+MUIA-TEXT-CONTENTS+") "Testing MUI from Lisp")
                                                          (%m "+MUIA-GROUP-CHILD+") str)))
         (app (amiga.mui:new-object :application
                                    (%m "+MUIA-APPLICATION-TITLE+") "AmiLispMUITest"
                                    (%m "+MUIA-APPLICATION-BASE+") "CLAMIGAMUITEST"
                                    (%m "+MUIA-APPLICATION-WINDOW+") win)))
    (setf (car string-cell) str)
    (values app win)))

;; RETURN-ID queues an ID that APPLICATION-INPUT hands back, with a signal
;; mask to wait on; the queue empties to NIL
(check "mui-return-id-then-application-input" '(7 t nil)
  (if *mui-p*
      (amiga.mui:with-foreign-pool ()
        (let ((cell (list nil)))
          (let ((app (%mui-make-app cell)))
            (unwind-protect
                 (progn
                   (amiga.mui:return-id app 7)
                   (multiple-value-bind (id sigs) (amiga.mui:application-input app)
                     (list id
                           (integerp sigs)
                           (amiga.mui:application-input app))))
              (amiga.mui:dispose-object app)))))
      '(7 t nil)))

;; MUIV_Application_ReturnID_Quit comes back as :QUIT
(check "mui-return-id-quit-is-keyword" :quit
  (if *mui-p*
      (amiga.mui:with-foreign-pool ()
        (let* ((cell (list nil))
               (app (%mui-make-app cell)))
          (unwind-protect
               (progn
                 (amiga.mui:return-id app (%m "+MUIV-APPLICATION-RETURN-ID-QUIT+"))
                 (amiga.mui:application-input app))
            (amiga.mui:dispose-object app))))
      :quit))

;; NOTIFY without a user: MUIA_String_Contents :EVERY-TIME -> the
;; application's MUIM_Application_ReturnID 42; setting the contents from
;; Lisp fires it, and APPLICATION-INPUT returns 42.  Both destination
;; spellings -- :APPLICATION (MUIV_Notify_Application) and the object.
;;
;; The window is OPENED first, on purpose: :APPLICATION resolves through
;; the object's application pointer, which MUI 3.8 propagates down the
;; tree when the window is attached to the application, while MorphOS's
;; MUI (muimaster 22) sets it at window setup -- from an object in a
;; never-opened window the notification fires on 3.8 and is silently
;; dropped on MorphOS.  The object destination works on both regardless.
;; A program that sets its notifications up before opening its windows,
;; as every C MUI program does, is fine either way: they fire later.
(check "mui-notify-fires-on-set-attrs" '(42 43)
  (if *mui-p*
      (amiga.mui:with-foreign-pool ()
        (let ((cell (list nil)))
          (multiple-value-bind (app win) (%mui-make-app cell)
            (let ((str (car cell)))
              (unwind-protect
                   (progn
                     (amiga.mui:set-attrs win (%m "+MUIA-WINDOW-OPEN+") t)
                     (amiga.mui:notify str (%m "+MUIA-STRING-CONTENTS+") :every-time
                                       :application (%m "+MUIM-APPLICATION-RETURN-ID+") 42)
                     (amiga.mui:set-attrs str (%m "+MUIA-STRING-CONTENTS+") "changed")
                     (let ((first (amiga.mui:application-input app)))
                       (amiga.mui:notify str (%m "+MUIA-STRING-CONTENTS+") :every-time
                                         app (%m "+MUIM-APPLICATION-RETURN-ID+") 43)
                       (amiga.mui:set-attrs str (%m "+MUIA-STRING-CONTENTS+") "again")
                       ;; the first notification fires 42 again, then 43
                       (let ((again (amiga.mui:application-input app)))
                         (list first
                               (if (eql again 42) (amiga.mui:application-input app) again)))))
                (amiga.mui:set-attrs win (%m "+MUIA-WINDOW-OPEN+") nil)
                (amiga.mui:dispose-object app))))))
      '(42 43)))

;; MUIV_TriggerValue is substituted only under a MUIV_EveryTime trigger
;; (Notify.mui).  Under a fixed trigger MUI hands the method the raw
;; 0x49893131 -- refused here, at the call site.  Pure argument checking, so
;; it runs whether or not muimaster.library is around.
(check "mui-notify-trigger-value-needs-every-time" '(t (#x49893131))
  (list (handler-case
            (progn (amiga.mui::%notify-args 1 t :application 2 '(:trigger-value)) nil)
          (error (c) (let ((s (format nil "~A" c)))
                       (and (search ":EVERY-TIME" s) (search ":TRIGGER-VALUE" s) t))))
        (subseq (amiga.mui::%notify-args 1 :every-time :self 2 '(:trigger-value)) 5)))

;; a notification whose trigger value does not match stays silent
;; (window open, see above)
(check "mui-notify-trigger-value-selects" '(nil 5)
  (if *mui-p*
      (amiga.mui:with-foreign-pool ()
        (let ((cell (list nil)))
          (multiple-value-bind (app win) (%mui-make-app cell)
            (let ((str (car cell)))
              (unwind-protect
                   (let ((disabled (%m "+MUIA-DISABLED+")))
                     (amiga.mui:set-attrs win (%m "+MUIA-WINDOW-OPEN+") t)
                     (amiga.mui:notify str disabled t
                                       :application (%m "+MUIM-APPLICATION-RETURN-ID+") 5)
                     (amiga.mui:set-attrs str disabled nil)
                     (list (amiga.mui:application-input app)
                           (progn (amiga.mui:set-attrs str disabled t)
                                  (amiga.mui:application-input app))))
                (amiga.mui:set-attrs win (%m "+MUIA-WINDOW-OPEN+") nil)
                (amiga.mui:dispose-object app))))))
      '(nil 5)))

;; the window: MUIA_Window_Open set and read back, the window's struct
;; Window through GET-ATTR-POINTER, the bounded loop returning on its
;; timeout, close, and the application disposing the whole tree
(check "mui-window-open-loop-close" '(0 1 t :looped 0)
  (if *mui-p*
      (amiga.mui:with-foreign-pool ()
        (let ((cell (list nil)))
          (multiple-value-bind (app win) (%mui-make-app cell)
            (unwind-protect
                 (let ((open (%m "+MUIA-WINDOW-OPEN+")))
                   (list (amiga.mui:get-attr open win)
                         (progn (amiga.mui:set-attrs win open t)
                                (amiga.mui:get-attr open win))
                         (let ((w (amiga.mui:get-attr-pointer (%m "+MUIA-WINDOW-WINDOW+") win)))
                           (and w (ffi:foreign-pointer-p w) t))
                         (let ((looped :looped))
                           ;; nobody clicks: NewInput yields no ID, the
                           ;; loop polls until the timeout
                           (amiga.mui:do-application-events ((id) app :timeout 1)
                             (setq looped (list :unexpected-id id)))
                           looped)
                         (progn (amiga.mui:set-attrs win open nil)
                                (amiga.mui:get-attr open win))))
              (amiga.mui:dispose-object app)))))
      '(0 1 t :looped 0)))

;; the loop leaves on :QUIT from the queue, running the body for the IDs
;; before it.  MUI's return-ID queue is FIFO and Quit is just an entry in
;; it: an ID queued after Quit is NOT dropped -- the next loop over the
;; same application sees it first (MUI 3.8 and 4 alike).  And (RETURN)
;; from the body leaves the loop with the rest still queued.
(check "mui-do-application-events-ids-and-quit" '((1 2) (9) (3))
  (if *mui-p*
      (amiga.mui:with-foreign-pool ()
        (let* ((cell (list nil))
               (app (%mui-make-app cell))
               (seen '()))
          (unwind-protect
               (flet ((collect (stop)
                        (setf seen '())
                        (amiga.mui:do-application-events ((id) app :timeout 2)
                          (push id seen)
                          (when stop (return)))
                        (reverse seen)))
                 (amiga.mui:return-id app 1)
                 (amiga.mui:return-id app 2)
                 (amiga.mui:return-id app (%m "+MUIV-APPLICATION-RETURN-ID-QUIT+"))
                 (amiga.mui:return-id app 9)          ; survives the Quit
                 (let* ((first (collect nil))          ; 1, 2, then Quit ends it
                        (leftover (collect t))         ; 9 was still queued
                        (third (progn (amiga.mui:return-id app 3)
                                      (amiga.mui:return-id app 4)
                                      (collect t))))   ; 3, then (RETURN)
                   (list first leftover third)))
            (amiga.mui:dispose-object app))))
      '((1 2) (9) (3))))

;;; --- hooks and custom classes ------------------------------------------------
;;; Lisp functions MUI calls (specs/mui-bindings.md section 4.5, unlocked
;;; by the callback runtime of section 10): a MUIA_List_DisplayHook that
;;; formats the rows of a List, MUIM_CallHook driven by a notification,
;;; and a custom class -- a subclass of Area.mui whose dispatcher answers
;;; MUIM_AskMinMax and MUIM_Draw from Lisp -- shown in an opened window so
;;; MUI really dispatches to it.  The custom class is that of
;;; examples/amiga/mui/class1.lisp.

;; MUIM_CallHook: a notification on a String's contents calls a Lisp hook
;; with a parameter; the hook's a2 is the object the method was sent to
(check "mui-callhook-from-notification" '(1 t 77)
  (if *mui-p*
      (amiga.mui:with-foreign-pool ()
        (let* ((calls '())
               (hook (amiga.mui:pool-hook
                      (lambda (h o m) h
                        (push (list (ffi:foreign-pointer-address o) (ffi:peek-u32 m 0)) calls)
                        0)))
               (cell (list nil)))
          (multiple-value-bind (app win) (%mui-make-app cell)
            (let ((str (car cell)))
              (unwind-protect
                   (progn
                     (amiga.mui:set-attrs win (%m "+MUIA-WINDOW-OPEN+") t)
                     ;; DoMethod(str, MUIM_Notify, MUIA_String_Contents, MUIV_EveryTime,
                     ;;          app, 3, MUIM_CallHook, &hook, 77)
                     (amiga.mui:notify str (%m "+MUIA-STRING-CONTENTS+") :every-time
                                       app (%m "+MUIM-CALL-HOOK+") hook 77)
                     (amiga.mui:set-attrs str (%m "+MUIA-STRING-CONTENTS+") "fire")
                     (list (length calls)
                           (= (first (first calls)) (ffi:foreign-pointer-address app))
                           (second (first calls))))
                (amiga.mui:set-attrs win (%m "+MUIA-WINDOW-OPEN+") nil)
                (amiga.mui:dispose-object app))))))
      '(1 t 77)))

;; an error inside a hook MUI calls is re-signaled at the Lisp call that
;; made MUI call it -- here SET-ATTRS -- and the program goes on
(check "mui-hook-error-surfaces-at-the-mui-call" '("hook failed" t)
  (if *mui-p*
      (amiga.mui:with-foreign-pool ()
        (let* ((hook (amiga.mui:pool-hook (lambda (h o m) h o m (error "hook failed"))))
               (cell (list nil)))
          (multiple-value-bind (app win) (%mui-make-app cell)
            (let ((str (car cell)))
              (unwind-protect
                   (progn
                     (amiga.mui:set-attrs win (%m "+MUIA-WINDOW-OPEN+") t)
                     (amiga.mui:notify str (%m "+MUIA-STRING-CONTENTS+") :every-time
                                       app (%m "+MUIM-CALL-HOOK+") hook 1)
                     (list (handler-case
                               (progn (amiga.mui:set-attrs str (%m "+MUIA-STRING-CONTENTS+") "x")
                                      :no-error)
                             (simple-error (e) (format nil "~A" e)))
                           ;; the object is intact and MUI is not
                           (equal "x" (amiga.mui:get-attr-string (%m "+MUIA-STRING-CONTENTS+") str))))
                (amiga.mui:set-attrs win (%m "+MUIA-WINDOW-OPEN+") nil)
                (amiga.mui:dispose-object app))))))
      '("hook failed" t)))

;; MUIA_List_DisplayHook: MUI calls the hook with the STRPTR array (a2)
;; and the entry (a1, NULL for the title row) while rendering the opened
;; window; the hook writes the columns.  Two columns, a title, two entries
;; copied by the builtin String construct hook.
(check "mui-list-display-hook-is-called" '(t t t)
  (if *mui-p*
      (amiga.mui:with-foreign-pool ()
        (let* ((rows '())
               (title-a (amiga.mui:pool-string "Name"))
               (title-b (amiga.mui:pool-string "Len"))
               (lens (make-hash-table :test #'equal))
               (hook (amiga.mui:pool-hook
                      (lambda (h array entry) h
                        (if (ffi:null-pointer-p entry)
                            (progn (push :title rows)
                                   (ffi:poke-u32 array (ffi:foreign-pointer-address title-a) 0)
                                   (ffi:poke-u32 array (ffi:foreign-pointer-address title-b) 4))
                            (let ((s (ffi:foreign-to-string entry)))
                              (push s rows)
                              (ffi:poke-u32 array (ffi:foreign-pointer-address entry) 0)
                              (ffi:poke-u32 array (ffi:foreign-pointer-address (gethash s lens)) 4)))
                        0)))
               (list (amiga.mui:new-object :list
                                           (%m "+MUIA-LIST-FORMAT+") "BAR,"
                                           (%m "+MUIA-LIST-TITLE+") t
                                           (%m "+MUIA-LIST-DISPLAY-HOOK+") hook
                                           (%m "+MUIA-LIST-CONSTRUCT-HOOK+") (%m "+MUIV-LIST-CONSTRUCT-HOOK-STRING+")
                                           (%m "+MUIA-LIST-DESTRUCT-HOOK+") (%m "+MUIV-LIST-DESTRUCT-HOOK-STRING+")))
               (win (amiga.mui:new-object :window
                                          (%m "+MUIA-WINDOW-TITLE+") "AMIGA.MUI display hook"
                                          (%m "+MUIA-WINDOW-ROOT-OBJECT+")
                                          (amiga.mui:new-object :listview (%m "+MUIA-LISTVIEW-LIST+") list)))
               (app (amiga.mui:new-object :application
                                          (%m "+MUIA-APPLICATION-BASE+") "CLAMIGAMUIHOOK"
                                          (%m "+MUIA-APPLICATION-WINDOW+") win)))
          (unwind-protect
               (progn
                 (dolist (s '("alpha" "be"))
                   (setf (gethash s lens) (amiga.mui:pool-string (format nil "~D" (length s))))
                   (amiga.mui:do-method list (%m "+MUIM-LIST-INSERT-SINGLE+")
                                        (amiga.mui:pool-string s) (%m "+MUIV-LIST-INSERT-BOTTOM+")))
                 (amiga.mui:set-attrs win (%m "+MUIA-WINDOW-OPEN+") t)
                 ;; let MUI render (the loop polls; nobody clicks)
                 (amiga.mui:do-application-events ((id) app :timeout 1) id)
                 (list (and (member :title rows) t)
                       (and (member "alpha" rows :test #'equal) t)
                       (and (member "be" rows :test #'equal) t)))
            (amiga.mui:set-attrs win (%m "+MUIA-WINDOW-OPEN+") nil)
            (amiga.mui:dispose-object app))))
      '(t t t)))

;; a custom class: MUI dispatches MUIM_AskMinMax and MUIM_Draw to the
;; Lisp dispatcher of an object in an opened window; DO-SUPER-METHOD
;; reaches Area.mui (the object gets a size), ADD-MIN-MAX is honoured
;; (the object is at least 100 pixels wide), the mui.h shortcuts read the
;; live MUI_AreaData, INST-DATA is zeroed instance data of the requested
;; size, the class is deleted by the pool -- and the generated struct
;; accessors of the raw module (mui.h's C structs laid out by the
;; generator: MUI_AreaData behind the MUI_NotifyData, its IBox, its
;; MUI_RenderInfo, the MUIP_AskMinMax message) read the same live values
;; as the curated shortcuts
(when *mui-p*
  (require "amiga/raw/intuition"))                ; struct IBox's accessors
(defun %raw-area-agrees-p (object message)
  "During MUIM_Draw: the raw accessors against AMIGA.MUI's area-* shortcuts."
  (let* ((mad (ffi:pointer+ object (%m "*MUI-NOTIFY-DATA-SIZE*")))
         (box (funcall (%mui-sym "MUI-AREA-DATA-BOX") mad))
         (mri (funcall (%mui-sym "MUI-AREA-DATA-RENDER-INFO") mad))
         (i-box-width (find-symbol "I-BOX-WIDTH" "AMIGA.RAW.INTUITION"))
         (i-box-left (find-symbol "I-BOX-LEFT" "AMIGA.RAW.INTUITION")))
    (and (= (funcall i-box-width box) (amiga.mui:area-width object))
         (= (funcall i-box-left box) (amiga.mui:area-left object))
         (= (funcall (%mui-sym "MUI-AREA-DATA-FLAGS") mad) (amiga.mui:area-flags object))
         (= (- (amiga.mui:area-width object) (funcall (%mui-sym "MUI-AREA-DATA-SUBWIDTH") mad))
            (amiga.mui:area-mwidth object))
         (= (ffi:foreign-pointer-address (funcall (%mui-sym "MUI-RENDER-INFO-RASTPORT") mri))
            (ffi:foreign-pointer-address (amiga.mui:area-rastport object)))
         (= (ffi:foreign-pointer-address (funcall (%mui-sym "MUI-RENDER-INFO-WINDOW") mri))
            (ffi:foreign-pointer-address (amiga.mui:area-window object)))
         (= (ffi:foreign-pointer-address (funcall (%mui-sym "MUI-AREA-DATA-FONT") mad))
            (ffi:foreign-pointer-address (amiga.mui:area-font object)))
         (= (funcall (%mui-sym "MUIP-DRAW-FLAGS") message) (amiga.mui:draw-flags message))
         t)))
(check "mui-custom-class-dispatches-from-lisp" '(t t t t t t t)
  (if *mui-p*
      (let ((methods '()) (draw-width nil) (inst-ok nil) (rp-ok nil) (raw-ok nil))
        (amiga.mui:with-foreign-pool ()
          (let* ((ask-id (%m "+MUIM-ASK-MIN-MAX+"))
                 (draw-id (%m "+MUIM-DRAW+"))
                 (mcc (amiga.mui:create-custom-class
                       :area
                       (lambda (class object message)
                         (let ((id (amiga.mui:method-id message)))
                           (push id methods)
                           (cond ((= id ask-id)
                                  (amiga.mui:do-super-method class object message)
                                  (amiga.mui:add-min-max message :min-width 100 :def-width 120 :max-width 500
                                                                 :min-height 40 :def-height 90 :max-height 300)
                                  ;; the raw MUIP_AskMinMax / MUI_MinMax accessors see the addition
                                  (let ((info (funcall (%mui-sym "MUIP-ASK-MIN-MAX-MIN-MAX-INFO") message)))
                                    (setf raw-ok
                                          (and (= (ffi:foreign-pointer-address info)
                                                  (ffi:foreign-pointer-address (amiga.mui:min-max-info message)))
                                               (>= (funcall (%mui-sym "MUI-MIN-MAX-MIN-WIDTH") info) 100)
                                               (>= (funcall (%mui-sym "MUI-MIN-MAX-DEF-HEIGHT") info) 90))))
                                  0)
                                 ((= id draw-id)
                                  (amiga.mui:do-super-method class object message)
                                  (setf draw-width (amiga.mui:area-mwidth object)
                                        rp-ok (not (ffi:null-pointer-p (amiga.mui:area-rastport object)))
                                        raw-ok (and raw-ok (%raw-area-agrees-p object message)))
                                  (let ((data (amiga.mui:inst-data class object)))
                                    (setf inst-ok (and (zerop (ffi:peek-u32 data 0))
                                                       (zerop (ffi:peek-u32 data 4)))))
                                  0)
                                 (t (amiga.mui:do-super-method class object message)))))
                       :data-size 8))
                 (obj (amiga.mui:new-object (amiga.mui:custom-class-class mcc)
                                            (%m "+MUIA-FRAME+") (%m "+MUIV-FRAME-TEXT+")))
                 (win (amiga.mui:new-object :window
                                            (%m "+MUIA-WINDOW-TITLE+") "AMIGA.MUI custom class"
                                            (%m "+MUIA-WINDOW-ROOT-OBJECT+")
                                            (amiga.mui:new-object :group (%m "+MUIA-GROUP-CHILD+") obj)))
                 (app (amiga.mui:new-object :application
                                            (%m "+MUIA-APPLICATION-BASE+") "CLAMIGAMUICLASS"
                                            (%m "+MUIA-APPLICATION-WINDOW+") win)))
            (unwind-protect
                 (progn
                   (amiga.mui:set-attrs win (%m "+MUIA-WINDOW-OPEN+") t)
                   (amiga.mui:do-application-events ((id) app :timeout 1) id))
              (amiga.mui:set-attrs win (%m "+MUIA-WINDOW-OPEN+") nil)
              (amiga.mui:dispose-object app))
            (list (and (member ask-id methods) t)          ; MUI asked for sizes
                  (and (member draw-id methods) t)         ; and had it drawn
                  (and (integerp draw-width) (>= draw-width 100))   ; ADD-MIN-MAX honoured
                  rp-ok                                    ; _rp(obj) live during Draw
                  inst-ok                                  ; zeroed instance data
                  (> (length methods) 2)                   ; OM_NEW, Setup, Show ... forwarded
                  (and raw-ok t)))))                       ; the generated struct rows agree
      '(t t t t t t t)))

;; the same class with a method list (CREATE-CUSTOM-CLASS :METHODS): the
;; Lisp function is entered for MUIM_AskMinMax and MUIM_Draw alone, and
;; everything else MUI sends the object -- OM_NEW, MUIM_Setup, MUIM_Show,
;; OM_GET ... -- reaches Area natively, so the object still comes up and
;; draws at the size the Lisp method asked for.
(check "mui-custom-class-method-list-keeps-other-methods-out-of-lisp" '(t t t t t)
  (if *mui-p*
      (let ((methods '()) (draw-width nil) (stats nil))
        (amiga.mui:with-foreign-pool ()
          (let* ((ask-id (%m "+MUIM-ASK-MIN-MAX+"))
                 (draw-id (%m "+MUIM-DRAW+"))
                 (mcc (amiga.mui:create-custom-class
                       :area
                       (lambda (class object message)
                         (let ((id (amiga.mui:method-id message)))
                           (pushnew id methods)
                           (cond ((= id ask-id)
                                  (amiga.mui:do-super-method class object message)
                                  (amiga.mui:add-min-max message :min-width 100 :def-width 120 :max-width 500
                                                                 :min-height 40 :def-height 90 :max-height 300)
                                  0)
                                 (t
                                  (amiga.mui:do-super-method class object message)
                                  (setf draw-width (amiga.mui:area-mwidth object))
                                  0))))
                       :methods (list ask-id draw-id)))
                 (obj (amiga.mui:new-object (amiga.mui:custom-class-class mcc)
                                            (%m "+MUIA-FRAME+") (%m "+MUIV-FRAME-TEXT+")))
                 (win (amiga.mui:new-object :window
                                            (%m "+MUIA-WINDOW-TITLE+") "AMIGA.MUI method list"
                                            (%m "+MUIA-WINDOW-ROOT-OBJECT+")
                                            (amiga.mui:new-object :group (%m "+MUIA-GROUP-CHILD+") obj)))
                 (app (amiga.mui:new-object :application
                                            (%m "+MUIA-APPLICATION-BASE+") "CLAMIGAMUIMETHODS"
                                            (%m "+MUIA-APPLICATION-WINDOW+") win)))
            (unwind-protect
                 (progn
                   (amiga.mui:set-attrs win (%m "+MUIA-WINDOW-OPEN+") t)
                   (amiga.mui:do-application-events ((id) app :timeout 1) id))
              (amiga.mui:set-attrs win (%m "+MUIA-WINDOW-OPEN+") nil)
              (amiga.mui:dispose-object app))
            (setf stats (multiple-value-list (amiga.mui:custom-class-stats mcc)))
            (list (and (not (ffi:null-pointer-p obj)) t)    ; OM_NEW reached Area natively
                  (null (set-difference methods (list ask-id draw-id)))  ; nothing else entered Lisp
                  (and (member ask-id methods) (member draw-id methods) t)
                  (and (integerp draw-width) (>= draw-width 100))        ; ADD-MIN-MAX honoured
                  ;; more messages were passed on than the two kinds handled
                  (and (= (third stats) 2) (> (second stats) 2)
                       (> (first stats) (second stats)))))))
      '(t t t t t)))

;; a custom layout hook: MUIA_Group_LayoutHook makes MUI ask a Lisp
;; function both of its layout questions.  MUILM_MINMAX is answered with
;; SET-MIN-MAX over the children's AREA-MIN-WIDTH / AREA-MIN-HEIGHT (the
;; group must then really be at least that wide), MUILM_LAYOUT places
;; every child of LAYOUT-CHILDREN with LAYOUT-CHILD -- and the placement
;; is read back from the children's own _left/_top/_width afterwards.
;; This is the mechanism of examples/amiga/mui/layout.lisp.
(check "mui-custom-layout-hook-lays-the-group-out" '(t t 3 t t t)
  (if *mui-p*
      (let ((types '()) (child-count 0) (min-width nil)
            (placed t) (positions '()))
        (amiga.mui:with-foreign-pool ()
          (let* ((minmax-type (%m "+MUILM-MINMAX+"))
                 (layout-type (%m "+MUILM-LAYOUT+"))
                 (maxmax      (%m "+MUI-MAXMAX+"))
                 (hook (amiga.mui:pool-hook
                        (lambda (hook object message)
                          (declare (ignore hook object))
                          (let ((type (amiga.mui:layout-msg-type message))
                                (kids (amiga.mui:layout-children message)))
                            (pushnew type types)
                            (cond
                              ((= type minmax-type)
                               (setf child-count (length kids))
                               (let ((w 0) (h 0))
                                 (dolist (child kids)
                                   (setf w (max w (amiga.mui:area-min-width child))
                                         h (max h (amiga.mui:area-min-height child))))
                                 (setf min-width w)
                                 (amiga.mui:set-min-max
                                  (amiga.mui:layout-msg-min-max message)
                                  :min-width (* 2 w) :min-height (* 2 h)
                                  :def-width (* 3 w) :def-height (* 3 h)
                                  :max-width maxmax :max-height maxmax))
                               0)
                              ((= type layout-type)
                               ;; a column of the children, each at its
                               ;; minimum size, top to bottom
                               (let ((top 0))
                                 (dolist (child kids)
                                   (let ((mw (amiga.mui:area-min-width child))
                                         (mh (amiga.mui:area-min-height child)))
                                     (unless (amiga.mui:layout-child child 0 top mw mh)
                                       (setf placed nil))
                                     (incf top mh))))
                               t)
                              (t (%m "+MUILM-UNKNOWN+")))))))
                 (kids (list (amiga.mui:make-object :button "One")
                             (amiga.mui:make-object :button "Two")
                             (amiga.mui:make-object :button "Three")))
                 (group (apply #'amiga.mui:new-object :group
                               (%m "+MUIA-GROUP-LAYOUT-HOOK+") hook
                               (loop for k in kids
                                     collect (%m "+MUIA-GROUP-CHILD+") collect k)))
                 (win (amiga.mui:new-object :window
                                            (%m "+MUIA-WINDOW-TITLE+") "AMIGA.MUI layout hook"
                                            (%m "+MUIA-WINDOW-ROOT-OBJECT+") group))
                 (app (amiga.mui:new-object :application
                                            (%m "+MUIA-APPLICATION-BASE+") "CLAMIGAMUILAYOUT"
                                            (%m "+MUIA-APPLICATION-WINDOW+") win)))
            (unwind-protect
                 (progn
                   (amiga.mui:set-attrs win (%m "+MUIA-WINDOW-OPEN+") t)
                   (amiga.mui:do-application-events ((id) app :timeout 1) id)
                   ;; the hook put them in a column at their minimum size
                   (setf positions
                         (loop for k in kids
                               collect (list (amiga.mui:area-width k)
                                             (amiga.mui:area-min-width k)))))
              (amiga.mui:set-attrs win (%m "+MUIA-WINDOW-OPEN+") nil)
              (amiga.mui:dispose-object app))
            (list (and (member minmax-type types) t)   ; MUI asked for the size
                  (and (member layout-type types) t)   ; and had the children placed
                  child-count                          ; LAYOUT-CHILDREN found all three
                  placed                               ; every LAYOUT-CHILD succeeded
                  (and (integerp min-width) (> min-width 0))  ; _minwidth was live
                  ;; each child ended up exactly its minimum width
                  (and (every (lambda (p) (= (first p) (second p))) positions) t)))))
      '(t t 3 t t t)))

;; a custom class that handles input: MUIM_Setup asks for IDCMP classes
;; with REQUEST-IDCMP and MUIM_Cleanup gives them back with REJECT-IDCMP
;; -- both run just from opening and closing the window, with no user.
;; The same class overrides MUIM_Numeric_Stringify (a Slider.mui
;; subclass), returning a STRPTR out of its own instance data: MUI calls
;; it while sizing and drawing the slider.  Both idioms are
;; examples/amiga/mui/slidorama.lisp's.
(check "mui-custom-class-input-and-stringify" '(t t t t t)
  (if *mui-p*
      (let ((setup nil) (cleanup nil) (stringified nil) (seen-values '()) (failed nil))
        (amiga.mui:with-foreign-pool ()
          (let* ((setup-id     (%m "+MUIM-SETUP+"))
                 (cleanup-id   (%m "+MUIM-CLEANUP+"))
                 (stringify-id (%m "+MUIM-NUMERIC-STRINGIFY+"))
                 (value-of     (symbol-function (%mui-sym "MUIP-NUMERIC-STRINGIFY-VALUE")))
                 (idcmp (logior amiga.raw.intuition:+idcmp-mousemove+
                                amiga.raw.intuition:+idcmp-intuiticks+))
                 (mcc (amiga.mui:create-custom-class
                       :slider
                       (lambda (class object message)
                         (let ((id (amiga.mui:method-id message)))
                           (cond
                             ((= id setup-id)
                              (if (zerop (amiga.mui:do-super-method class object message))
                                  nil
                                  (progn (amiga.mui:request-idcmp object idcmp)
                                         (setf setup t)
                                         t)))
                             ((= id cleanup-id)
                              (amiga.mui:reject-idcmp object idcmp)
                              (setf cleanup t)
                              (amiga.mui:do-super-method class object message))
                             ((= id stringify-id)
                              ;; MUI asks for several values while sizing
                              ;; the slider (it needs the widest label),
                              ;; so collect them rather than keep the last
                              (setf stringified t)
                              (pushnew (funcall value-of message) seen-values)
                              ;; a STRPTR out of our own instance data
                              (let ((buffer (amiga.mui:inst-data class object))
                                    (text "amilisp"))
                                (ffi:poke-bytes buffer text 0 0 (length text))
                                (ffi:poke-u8 buffer 0 (length text))
                                buffer))
                             (t (amiga.mui:do-super-method class object message)))))
                       :data-size 16))
                 (slider (amiga.mui:new-object (amiga.mui:custom-class-class mcc)
                                               (%m "+MUIA-NUMERIC-MIN+") 0
                                               (%m "+MUIA-NUMERIC-MAX+") 100
                                               (%m "+MUIA-NUMERIC-VALUE+") 42))
                 (win (amiga.mui:new-object :window
                                            (%m "+MUIA-WINDOW-TITLE+") "AMIGA.MUI input class"
                                            (%m "+MUIA-WINDOW-ROOT-OBJECT+")
                                            (amiga.mui:new-object :group
                                                                  (%m "+MUIA-GROUP-CHILD+") slider)))
                 (app (amiga.mui:new-object :application
                                            (%m "+MUIA-APPLICATION-BASE+") "CLAMIGAMUIINPUT"
                                            (%m "+MUIA-APPLICATION-WINDOW+") win)))
            (unwind-protect
                 (handler-case
                     (progn
                       (amiga.mui:set-attrs win (%m "+MUIA-WINDOW-OPEN+") t)
                       (amiga.mui:do-application-events ((id) app :timeout 1) id)
                       (amiga.mui:set-attrs win (%m "+MUIA-WINDOW-OPEN+") nil))
                   (error (e) (setf failed (format nil "~A" e))))
              (amiga.mui:dispose-object app))
            (list setup                                  ; MUIM_Setup ran, IDCMP asked for
                  cleanup                                ; MUIM_Cleanup gave them back
                  stringified                            ; Stringify was overridden
                  (and (member 42 seen-values) t)        ; the slider's own value among them
                  (null failed)))))                      ; no deferred hook error
      '(t t t t t)))

;; The string-key hook: a String's MUIA_String_EditHook that is C from
;; end to end, since MUI 3.8 runs it on input.device's task where Lisp
;; cannot run.  Called here as Intuition would (CallHookPkt with a
;; hand-built SGWork), it must rewrite the event into a no-op for the
;; gadget and queue the entry's value to the object through
;; MUIM_Application_PushMethod -- which the application's input loop
;; then delivers to the custom class's Lisp dispatcher.  The m68k stub
;; entry, the CallHookPkt register convention and the real push are what
;; this checks beyond the host's tests/test_amiga_mui.lisp.
(check "mui-string-key-hook-pushes-through-the-application" '(t #xFFFFFFFF (#xFF 0 0 1) (1 1) 4242 (1 0))
  (if *mui-p*
      (let ((got '()) (rc nil) (rewritten nil) (stats nil) (after nil) (missed nil))
        (amiga.mui:with-foreign-pool ()
          (let* ((method #x81000042)
                 (mcc (amiga.mui:create-custom-class
                       :area
                       (lambda (class object message)
                         (if (= (amiga.mui:method-id message) method)
                             (progn (push (ffi:peek-u32 message 4) got) 1)
                             (amiga.mui:do-super-method class object message)))))
                 (obj (amiga.mui:new-object (amiga.mui:custom-class-class mcc)))
                 (app (amiga.mui:new-object :application
                                            (%m "+MUIA-APPLICATION-BASE+") "CLAMIGAMUIKEYHOOK"))
                 (hook (amiga.mui:make-string-key-hook app obj method
                                                       '((#x42 #x19 #x00 4242))))  ; TAB
                 (sgw (ffi:alloc-foreign 44))
                 (ie (ffi:alloc-foreign 22))
                 (msg (ffi:alloc-foreign 4)))
            (unwind-protect
                 (progn
                   (ffi:poke-u8 ie 1 4)                       ; IECLASS_RAWKEY
                   (ffi:poke-u16 ie #x42 6)                   ; TAB pressed
                   (ffi:poke-u16 ie 0 8)
                   (ffi:poke-pointer sgw ie 20)               ; IEvent
                   (ffi:poke-u16 sgw 9 24)                    ; Code: the TAB character
                   (ffi:poke-u32 sgw #x23 30)                 ; SGA_USE|SGA_END|SGA_NEXTACTIVE
                   (ffi:poke-u16 sgw 8 42)                    ; EO_INSERTCHAR
                   (ffi:poke-u32 msg 1 0)                     ; SGH_KEY
                   (setf rc (amiga.raw.utility:call-hook-pkt hook sgw msg))
                   (setf rewritten (list (ffi:peek-u16 ie 6) (ffi:peek-u16 sgw 24)
                                         (ffi:peek-u32 sgw 30) (ffi:peek-u16 sgw 42)))
                   (setf stats (multiple-value-bind (c m) (amiga.mui:string-key-hook-stats hook)
                                 (list c m)))
                   ;; the queued method runs from the input loop
                   (amiga.mui:application-input app)
                   (setf after (car got))
                   ;; an unlisted key is left alone and pushes nothing
                   (ffi:poke-u16 ie #x20 6)                   ; `a'
                   (let ((n (length got)))
                     (amiga.raw.utility:call-hook-pkt hook sgw msg)
                     (amiga.mui:application-input app)
                     (setf missed (list (- (length got) n)
                                        (multiple-value-bind (c m) (amiga.mui:string-key-hook-stats hook)
                                          (- m 1))))))
              (ffi:free-foreign msg) (ffi:free-foreign ie) (ffi:free-foreign sgw)
              (amiga.mui:dispose-object obj)
              (amiga.mui:dispose-object app)
              (amiga.mui:free-string-key-hook hook))))
        (list t rc rewritten stats after (list (- 1 (first missed)) (second missed))))
      '(t #xFFFFFFFF (#xFF 0 0 1) (1 1) 4242 (1 0))))

;; the module's copy of MUIM_Application_PushMethod, which the hook uses
(check "mui-push-method-constant-matches-generated" t
  (if *mui-p*
      (= amiga.mui::+muim-application-push-method+ (%m "+MUIM-APPLICATION-PUSH-METHOD+"))
      t))

(format t "; mui: done~%")
(finish-output)
