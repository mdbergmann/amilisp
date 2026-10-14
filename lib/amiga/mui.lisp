;;; amiga/mui.lisp — MUI (Magic User Interface) helpers for AmiLisp
;;;
;;; Loaded via (require "amiga/mui").
;;;
;;; MUI -- 3.8 on AmigaOS 3.x, built into MorphOS (4.x), MUI 5 on OS 4 --
;;; is a BOOPSI toolkit: its classes are created BY NAME through
;;; muimaster.library's MUI_NewObjectA ("Window.mui", "Group.mui" ...)
;;; and driven with SetAttrs / GetAttr / DoMethod like any BOOPSI object.
;;; The toolkit-neutral half of that -- the foreign pool, tag lists from
;;; Lisp values, DO-METHOD, GET-ATTR / SET-ATTRS -- is AMIGA.BOOPSI
;;; (lib/amiga/boopsi.lisp), which this package :USEs and re-exports.
;;; The generated raw module amiga/raw/muimaster provides every function
;;; of muimaster.library and every MUIA_ / MUIM_ / MUIV_ / MUII_ / MUIC_
;;; constant of libraries/mui.h.  What this module adds is what a C MUI
;;; program gets from mui.h's shortcut macros and from amiga.lib:
;;;
;;;   NEW-OBJECT         MUI_NewObjectA with a class NAME (:window or
;;;                      "Window.mui") and a Lisp tag plist -- integers,
;;;                      foreign pointers (nested objects are just values),
;;;                      T/NIL and strings, the strings copied into a
;;;                      WITH-FOREIGN-POOL that outlives the objects
;;;   MAKE-OBJECT        MUI_MakeObjectA, the builtin object collection:
;;;                      (:button "_OK"), (:slider "Volume" 0 100),
;;;                      (:cycle "Mode" '("Fast" "Slow")) ...
;;;   DISPOSE-OBJECT     MUI_DisposeObject
;;;   NOTIFY             the MUIM_Notify idiom with :EVERY-TIME, :SELF /
;;;                      :WINDOW / :APPLICATION / :PARENT, :TRIGGER-VALUE
;;;   RETURN-ID / APPLICATION-INPUT / DO-APPLICATION-EVENTS
;;;                      MUIM_Application_ReturnID / _NewInput and the
;;;                      canonical Wait() loop, with an optional timeout
;;;                      for unattended runs
;;;   REQUEST            MUI_RequestA, the easy requester
;;;   GET-ATTR-STRING    a STRPTR attribute (MUIA_String_Contents) as a
;;;                      Lisp string
;;;   POOL-STRING-ARRAY  a NULL-terminated STRPTR[] for MUIA_Cycle_Entries
;;;                      / MUIA_Radio_Entries
;;;   MAKE-ID, CLASS-ID, WINDOW-SIZE-MINMAX / -VISIBLE / -SCREEN,
;;;   WINDOW-EDGE-DELTA  the remaining function-like macros of mui.h
;;;   CREATE-CUSTOM-CLASS / CUSTOM-CLASS-CLASS
;;;                      MUI_CreateCustomClass with a Lisp dispatcher (a
;;;                      function of class, object, message), deleted by
;;;                      the foreign pool; DO-SUPER-METHOD, INST-DATA,
;;;                      METHOD-ID and the _mleft(obj) family of mui.h
;;;                      shortcuts (AREA-MLEFT ...) for writing its methods;
;;;                      REQUEST-IDCMP / REJECT-IDCMP for a class that
;;;                      answers MUIM_HandleInput
;;;   POOL-HOOK          (AMIGA.BOOPSI) a struct Hook calling a Lisp
;;;                      function, for MUIA_List_DisplayHook, MUIM_CallHook,
;;;                      MUIA_Group_LayoutHook ...
;;;   LAYOUT-CHILDREN / LAYOUT-MSG-* / SET-MIN-MAX / LAYOUT-CHILD
;;;                      what a MUIA_Group_LayoutHook reads out of its
;;;                      struct MUI_LayoutMsg and does with it: the
;;;                      NextObject() walk of lm_Children as a list, the
;;;                      message's fields, and MUI_Layout for each child
;;;
;;; The module loads on any system -- the host, an Amiga without MUI: it
;;; opens muimaster.library on the first call that needs it, and
;;; AVAILABLE-P answers NIL rather than erroring where MUI is missing.
;;; The raw module amiga/raw/muimaster, by contrast, opens the library at
;;; REQUIRE time and fails there with the library's name (the raw-module
;;; policy), so a program that must also run on a MUI-less Amiga tests
;;; AVAILABLE-P before requiring it.
;;;
;;; The few MUIM_ / MUIV_ / MUIO_ values this module needs itself are
;;; duplicated below from amiga/raw/muimaster on purpose (the raw module
;;; cannot be loaded where MUI is absent); tests/test_amiga_curated_vs_raw.sh
;;; requires them to agree with the generated table.
;;;
;;; Hooks and custom classes run Lisp on MUI's stack, inside its C
;;; frames: an error there cannot unwind through MUI, so the runtime
;;; catches it at the callback, returns 0 to MUI and re-signals it on the
;;; Lisp side once the library call that invoked the hook has returned --
;;; typically at NEW-OBJECT, SET-ATTRS or APPLICATION-INPUT.  See the
;;; "Hooks and dispatchers" section of lib/amiga/ffi.lisp.
;;;
;;; See examples/amiga/mui/ and tests/amiga/test-mui.lisp for the
;;; executable specification of this module; tests/test_amiga_mui.sh runs
;;; the portable half on the host.

;; compile-time too: COMPILE-FILE (the host builds the lib/amiga FASLs) must see
;; these packages at read time, not only LOAD.
(eval-when (:compile-toplevel :load-toplevel :execute)
  (require "amiga/ffi")
  (require "amiga/boopsi")
  (require "amiga/raw/exec")
  (require "amiga/raw/dos")
  (require "amiga/raw/utility")
  (require "amiga/raw/intuition"))

;; The :EXPORT list names the AMIGA.BOOPSI symbols too: DEFPACKAGE processes
;; :USE before :EXPORT (CLHS DEFPACKAGE), so exporting an inherited name
;; re-exports that very symbol rather than creating a homonym.  NEW-LIST /
;; FREE-LIST-NODES are not re-exported: MUI has no exec label lists.
(defpackage "AMIGA.MUI"
  (:use "CL" "FFI" "AMIGA.FFI" "AMIGA.BOOPSI")
  (:import-from "AMIGA.BOOPSI" "%ULONG" "%WITH-TAGS")
  (:export
   ;; Availability
   "AVAILABLE-P"
   ;; Methods (AMIGA.BOOPSI, re-exported)
   "DO-METHOD" "OBJECT-CLASS"
   ;; Foreign memory whose lifetime is the GUI's lifetime (AMIGA.BOOPSI, re-exported)
   "WITH-FOREIGN-POOL" "POOL-ALLOC" "POOL-STRING" "POOL-HOOK" "POOL-FINALIZER"
   "WITH-TAGS" "POOL-STRING-ARRAY"
   ;; Objects and attributes (GET-ATTR, GET-ATTR-POINTER, SET-ATTRS: AMIGA.BOOPSI, re-exported)
   "CLASS-ID" "NEW-OBJECT" "MAKE-OBJECT" "DISPOSE-OBJECT"
   "GET-ATTR" "GET-ATTR-POINTER" "GET-ATTR-STRING" "SET-ATTRS"
   ;; Notification and the application loop
   "NOTIFY" "RETURN-ID" "APPLICATION-INPUT" "DO-APPLICATION-EVENTS"
   "*EVENT-LOOP-TIMEOUT*"
   ;; Requesters and the mui.h helper macros
   "REQUEST" "MAKE-ID"
   "WINDOW-SIZE-MINMAX" "WINDOW-SIZE-VISIBLE" "WINDOW-SIZE-SCREEN"
   "WINDOW-EDGE-DELTA"
   ;; Custom classes: MUI_CreateCustomClass with a Lisp dispatcher, and
   ;; what a dispatcher's methods need
   "CREATE-CUSTOM-CLASS" "CUSTOM-CLASS-CLASS" "CUSTOM-CLASS-STATS" "DO-SUPER-METHOD"
   "METHOD-ID" "INST-DATA" "MIN-MAX-INFO" "ADD-MIN-MAX" "SET-MIN-MAX"
   "DRAW-FLAGS" "REQUEST-IDCMP" "REJECT-IDCMP"
   ;; The mui.h shortcuts for custom-class methods: _rp(obj), _mleft(obj) ...
   "AREA-RENDER-INFO" "AREA-RASTPORT" "AREA-WINDOW" "AREA-DRAW-INFO"
   "AREA-SCREEN" "AREA-PENS" "AREA-FONT" "AREA-FLAGS"
   "AREA-LEFT" "AREA-TOP" "AREA-WIDTH" "AREA-HEIGHT" "AREA-RIGHT" "AREA-BOTTOM"
   "AREA-MLEFT" "AREA-MTOP" "AREA-MWIDTH" "AREA-MHEIGHT" "AREA-MRIGHT" "AREA-MBOTTOM"
   "AREA-MIN-WIDTH" "AREA-MIN-HEIGHT"
   ;; Custom layout hooks: the struct MUI_LayoutMsg and MUI_Layout
   "LAYOUT-MSG-TYPE" "LAYOUT-MSG-MIN-MAX" "LAYOUT-MSG-WIDTH" "LAYOUT-MSG-HEIGHT"
   "LAYOUT-CHILDREN" "LAYOUT-CHILD"
   ;; A String's MUIA_String_EditHook that takes keys without running Lisp
   ;; on Intuition's task (MUI 3.8 edits an active String there)
   "MAKE-STRING-KEY-HOOK" "FREE-STRING-KEY-HOOK" "STRING-KEY-HOOK-ENTRY"
   "STRING-KEY-HOOK-STATS"))

(in-package "AMIGA.MUI")

;;; ================================================================
;;; Constants
;;; ================================================================

;;; libraries/mui.h values this module needs itself, duplicated from the
;;; generated amiga/raw/muimaster on purpose: that module opens
;;; muimaster.library at REQUIRE time, and this one must load (and
;;; AVAILABLE-P must answer) on systems without MUI.
;;; tests/test_amiga_curated_vs_raw.sh cross-checks every one of them
;;; against the generated table; tests/amiga/test-mui.lisp again on the
;;; Amiga.

(defconstant +muimaster-vmin+ 11)                         ; MUIMASTER_VMIN

(defconstant +muim-notify+                     #x8042C9CB) ; MUIM_Notify
(defconstant +muim-application-new-input+      #x80423BA6) ; MUIM_Application_NewInput
(defconstant +muim-application-return-id+      #x804276EF) ; MUIM_Application_ReturnID
(defconstant +muim-application-push-method+    #x80429EF8) ; MUIM_Application_PushMethod
(defconstant +muiv-application-return-id-quit+ -1)         ; MUIV_Application_ReturnID_Quit

(defconstant +muiv-every-time+        #x49893131)         ; MUIV_EveryTime
(defconstant +muiv-trigger-value+     #x49893131)         ; MUIV_TriggerValue
(defconstant +muiv-not-trigger-value+ #x49893133)         ; MUIV_NotTriggerValue
(defconstant +muiv-notify-self+        1)                 ; MUIV_Notify_Self
(defconstant +muiv-notify-window+      2)                 ; MUIV_Notify_Window
(defconstant +muiv-notify-application+ 3)                 ; MUIV_Notify_Application
(defconstant +muiv-notify-parent+      4)                 ; MUIV_Notify_Parent

;;; MUI_MakeObjectA object types (mui.h "Object Types for MUI_MakeObject()")
(defconstant +muio-label+          1)   ; STRPTR label, ULONG flags
(defconstant +muio-button+         2)   ; STRPTR label
(defconstant +muio-checkmark+      3)   ; STRPTR label
(defconstant +muio-cycle+          4)   ; STRPTR label, STRPTR *entries
(defconstant +muio-radio+          5)   ; STRPTR label, STRPTR *entries
(defconstant +muio-slider+         6)   ; STRPTR label, LONG min, LONG max
(defconstant +muio-string+         7)   ; STRPTR label, LONG maxlen
(defconstant +muio-pop-button+     8)   ; STRPTR imagespec
(defconstant +muio-h-space+        9)   ; LONG space
(defconstant +muio-v-space+       10)   ; LONG space
(defconstant +muio-h-bar+         11)   ; LONG space
(defconstant +muio-v-bar+         12)   ; LONG space
(defconstant +muio-menustrip-nm+  13)   ; struct NewMenu *nm, ULONG flags
(defconstant +muio-menuitem+      14)   ; STRPTR label, STRPTR shortcut, ULONG flags, ULONG data
(defconstant +muio-bar-title+     15)   ; STRPTR label
(defconstant +muio-numeric-button+ 16)  ; STRPTR label, LONG min, LONG max, STRPTR format

;;; ================================================================
;;; muimaster.library — opened on first use
;;; ================================================================

(define-library-variable *muimaster-base* nil
  "The muimaster.library base once the first MUI call opened it (MUI 3.8
= version 19, MorphOS = 20); NIL before that, and forever on the host or
an Amiga without MUI.  Never closed: the objects a program creates need
the library for as long as the process lives, as in a C MUI program.
NIL again after a heap-image restore, so the first MUI call of the
restored process opens it for that process.")

(defun %open-muimaster ()
  "The library base, opening muimaster.library (version MUIMASTER_VMIN)
on first use; NIL where that is impossible."
  (or *muimaster-base*
      (and (member :amigaos *features*)
           (let ((base (amiga:open-library "muimaster.library" +muimaster-vmin+)))
             (and base (not (ffi:null-pointer-p base))
                  (setf *muimaster-base* base))))))

(defun available-p ()
  "True when MUI can be used on this system: an AmigaOS/MorphOS runtime
whose muimaster.library (version MUIMASTER_VMIN = 11, i.e. MUI 3.8 or
newer) opens.  NIL on the host and on an Amiga without MUI, where
\(require \"amiga/raw/muimaster\") would fail.  A T answer leaves the
library open for the process's lifetime."
  (and (%open-muimaster) t))

(defun %muimaster (who)
  "The library base for the entry point WHO, or a descriptive error."
  (or (%open-muimaster)
      (error "AMIGA.MUI:~A: muimaster.library (MUI 3.8 or newer, version ~D) is not available -- ~:[this is not an AmigaOS/MorphOS runtime~;MUI is not installed on this system, or MUI:Libs is not in the LIBS: path~].  Check AVAILABLE-P before building a MUI GUI."
             who +muimaster-vmin+ (member :amigaos *features*))))

;;; The four library functions this module calls itself (the raw module
;;; has all 26, but cannot be REQUIREd where MUI is absent).  Offsets and
;;; registers from the MUI 3.8 SDK muimaster_lib.fd; checked against the
;;; generated table by tests/test_amiga_curated_vs_raw.sh.

(amiga.ffi:defcfun %new-object-a *muimaster-base* -30
  (:a0 class-name :a1 tags) :result :pointer
  :doc "Object *MUI_NewObjectA(char *class, struct TagItem *tags)")

(amiga.ffi:defcfun %dispose-object *muimaster-base* -36
  (:a0 object) :result :void
  :doc "VOID MUI_DisposeObject(Object *obj)")

(amiga.ffi:defcfun %request-a *muimaster-base* -42
  (:d0 app :d1 window :d2 flags :a0 title :a1 gadgets :a2 format :a3 params)
  :result :signed
  :doc "LONG MUI_RequestA(APTR app, APTR win, LONGBITS flags, char *title, char *gadgets, char *format, APTR params)")

(amiga.ffi:defcfun %make-object-a *muimaster-base* -120
  (:d0 type :a0 params) :result :pointer
  :doc "Object *MUI_MakeObjectA(LONG type, ULONG *params)")

(amiga.ffi:defcfun %create-custom-class *muimaster-base* -108
  (:a0 base :a1 supername :a2 supermcc :d0 datasize :a3 dispatcher) :result :pointer
  :doc "struct MUI_CustomClass *MUI_CreateCustomClass(struct Library *base, char *supername, struct MUI_CustomClass *supermcc, int datasize, APTR dispatcher)")

(amiga.ffi:defcfun %delete-custom-class *muimaster-base* -114
  (:a0 mcc) :result :bool
  :doc "BOOL MUI_DeleteCustomClass(struct MUI_CustomClass *mcc)")

(amiga.ffi:defcfun %request-idcmp *muimaster-base* -90
  (:a0 object :d0 flags) :result :void
  :doc "VOID MUI_RequestIDCMP(Object *obj, ULONG flags)")

(amiga.ffi:defcfun %reject-idcmp *muimaster-base* -96
  (:a0 object :d0 flags) :result :void
  :doc "VOID MUI_RejectIDCMP(Object *obj, ULONG flags)")

(amiga.ffi:defcfun %layout *muimaster-base* -126
  (:a0 object :d0 left :d1 top :d2 width :d3 height :d4 flags) :result :bool
  :doc "BOOL MUI_Layout(Object *obj, LONG l, LONG t, LONG w, LONG h, ULONG flags)")

;;; ================================================================
;;; Foreign memory with the GUI's lifetime — MUI additions
;;; ================================================================

(defun pool-string-array (strings)
  "A NULL-terminated STRPTR[] of foreign copies of STRINGS, everything
allocated in the enclosing WITH-FOREIGN-POOL -- what MUIA_Cycle_Entries,
MUIA_Radio_Entries and the MUIO_Cycle / MUIO_Radio parameters take.
Returns the array as a foreign pointer."
  (let ((array (pool-alloc (* 4 (1+ (length strings))))))   ; zeroed: the terminator
    (loop for s in strings
          for offset from 0 by 4
          do (unless (stringp s)
               (error "AMIGA.MUI:POOL-STRING-ARRAY: ~S is not a string (in ~S) -- the entries of a cycle / radio must all be strings"
                      s strings))
             (ffi:poke-u32 array (ffi:foreign-pointer-address (pool-string s)) offset))
    array))

(defun %build-ulong-array (values pooled)
  "A foreign ULONG[] holding VALUES -- integers, foreign pointers, T/NIL,
strings and (POOLED only) lists of strings.  Strings are copied into the
enclosing WITH-FOREIGN-POOL when POOLED (the receiver keeps the pointers:
MUI_MakeObjectA's labels and entries), or into temporaries otherwise (a
call that only reads them: MUI_RequestA).  Returns the array and the list
of temporaries; the caller frees both (%WITH-ULONG-ARRAY does)."
  (let ((array (ffi:alloc-foreign (* 4 (max 1 (length values)))))
        (temps '())
        (ok nil))
    (unwind-protect
         (progn
           (loop for v in values
                 for offset from 0 by 4
                 do (ffi:poke-u32
                     array
                     (cond ((stringp v)
                            (ffi:foreign-pointer-address
                             (if pooled
                                 (pool-string v)
                                 (let ((s (ffi:foreign-string v)))
                                   (push s temps)
                                   s))))
                           ((and (consp v) (every #'stringp v))
                            (unless pooled
                              (error "AMIGA.MUI: a list of strings ~S is only a parameter of MAKE-OBJECT (the entries of a cycle / radio), not of REQUEST"
                                     v))
                            (ffi:foreign-pointer-address (pool-string-array v)))
                           (t (%ulong v)))
                     offset))
           (setf ok t))
      (unless ok
        (ffi:free-foreign array)
        (dolist (s temps) (ffi:free-foreign s))))
    (values array temps)))

(defmacro %with-ulong-array ((var values &key pooled) &body body)
  (let ((temps (gensym "TEMPS")))
    `(multiple-value-bind (,var ,temps) (%build-ulong-array ,values ,pooled)
       (unwind-protect (progn ,@body)
         (ffi:free-foreign ,var)
         (dolist (s ,temps) (ffi:free-foreign s))))))

;;; ================================================================
;;; Objects
;;; ================================================================

;;; The MUIC_ names the mechanical keyword rule of CLASS-ID cannot
;;; spell: MorphOS's interior capitals and its .mcc aliases.  The
;;; coverage check of tests/test_amiga_mui.sh proves that the rule plus
;;; this list reach every +MUIC-*+ constant of the generated table.
(defparameter *class-id-irregulars*
  '((:fs-protection-bits . "FSProtectionBits.mui")
    (:fsprotectionbits   . "FSProtectionBits.mui")
    (:textinput          . "Textinput.mcc")
    (:textinputscroll    . "Textinputscroll.mcc")))

(defun class-id (class)
  "The MUI class name -- the MUIC_* string MUI_NewObjectA takes -- for
the designator CLASS.  A string is returned as it is (\"Window.mui\",
amiga.raw.muimaster:+muic-window+); a keyword is the MUIC_ name in Lisp
spelling: hyphens dropped, the first letter upper case, the rest lower,
\".mui\" appended -- :WINDOW is \"Window.mui\", :NUMERIC-BUTTON and
:NUMERICBUTTON are \"Numericbutton.mui\".  That rule (plus the handful
of MorphOS irregulars in *CLASS-ID-IRREGULARS* -- :FS-PROTECTION-BITS,
the :TEXTINPUT .mcc aliases) covers every class of mui.h and of the
newer MUIs (tests/test_amiga_mui.sh checks all 107)."
  (cond ((stringp class) class)
        ((keywordp class)
         (let ((irregular (assoc class *class-id-irregulars*)))
           (when irregular (return-from class-id (cdr irregular))))
         (let ((name (remove #\- (symbol-name class))))
           (when (or (zerop (length name)) (notevery #'alphanumericp name))
             (error "AMIGA.MUI:CLASS-ID: ~S does not name a MUI class -- expected a keyword like :WINDOW or :NUMERIC-BUTTON (for MUIC_Window, MUIC_Numericbutton), or the class name string itself (\"Window.mui\")"
                    class))
           (concatenate 'string (string-capitalize (string-downcase name)) ".mui")))
        (t
         (error "AMIGA.MUI:CLASS-ID: ~S is not a MUI class designator -- expected a keyword (:WINDOW) or a class name string (\"Window.mui\")"
                class))))

(defun new-object (class &rest tags)
  "MUI_NewObjectA(CLASS, TAGS): create a MUI object.  CLASS is a class
name -- a keyword (:APPLICATION, :WINDOW, :GROUP, :TEXT, :STRING ...) or a
string (\"Window.mui\", the raw +MUIC-*+ constants), see CLASS-ID -- or a
foreign pointer to a struct IClass (a private class), which goes through
intuition's NewObjectA instead.  TAGS is a plist of tag constant and
value: integer, foreign-pointer (child objects are just values, so
objects nest as in the C macros), T/NIL and string values are accepted,
strings being copied into the enclosing WITH-FOREIGN-POOL, because the
object keeps the pointer.  Returns the object as a foreign pointer, or
signals an error naming the class when MUI returns NULL -- an unknown
class, a NIL child, a missing required attribute, no memory."
  (unless (ffi:foreign-pointer-p class)
    (class-id class))                                       ; validate first: a clear error on any system
  (%muimaster "NEW-OBJECT")
  (or (%with-tags (array tags)
        (if (ffi:foreign-pointer-p class)
            (amiga.raw.intuition:new-object-a class nil array)
            (ffi:with-foreign-string (name (class-id class))
              (%new-object-a name array))))
      (error "AMIGA.MUI:NEW-OBJECT: MUI_NewObjectA returned NULL for class ~S~@[ (~S)~] with ~D tag~:P -- the class is unknown to this MUI (no MUI:Libs/mui/<name>.mui), a child object is NIL, a required attribute is missing or invalid (a Window without MUIA_Window_RootObject, a Window that is no MUIA_Application_Window yet when opened), or memory is exhausted"
             (if (ffi:foreign-pointer-p class) class (class-id class))
             (and (keywordp class) class)
             (floor (length tags) 2))))

(defparameter *make-object-types*
  (list (cons :label +muio-label+) (cons :button +muio-button+)
        (cons :checkmark +muio-checkmark+) (cons :cycle +muio-cycle+)
        (cons :radio +muio-radio+) (cons :slider +muio-slider+)
        (cons :string +muio-string+) (cons :pop-button +muio-pop-button+)
        (cons :h-space +muio-h-space+) (cons :v-space +muio-v-space+)
        (cons :h-bar +muio-h-bar+) (cons :v-bar +muio-v-bar+)
        (cons :menustrip-nm +muio-menustrip-nm+) (cons :menuitem +muio-menuitem+)
        (cons :bar-title +muio-bar-title+) (cons :numeric-button +muio-numeric-button+))
  "MAKE-OBJECT's type keywords -> MUIO_* codes.")

(defun %make-object-type (type)
  (cond ((integerp type) type)
        ((and (keywordp type) (cdr (assoc type *make-object-types*))))
        (t (error "AMIGA.MUI:MAKE-OBJECT: ~S is not a MUI_MakeObject type -- expected one of ~{~S~^, ~} (the MUIO_* codes), or the code itself"
                  type (mapcar #'car *make-object-types*)))))

(defun make-object (type &rest params)
  "MUI_MakeObjectA(TYPE, PARAMS): an object from MUI's builtin collection.
TYPE is a keyword -- :LABEL (label flags), :BUTTON (label), :CHECKMARK
\(label), :CYCLE (label entries), :RADIO (label entries), :SLIDER (label
min max), :STRING (label maxlen), :POP-BUTTON (image), :H-SPACE /
:V-SPACE / :H-BAR / :V-BAR (space), :MENUITEM (label shortcut flags
data), :BAR-TITLE (label), :NUMERIC-BUTTON (label min max format),
:MENUSTRIP-NM (newmenu flags) -- or a MUIO_* code; PARAMS are the
type's parameters as mui.h lists them: integers, foreign pointers,
T/NIL, strings and, for the entries of a cycle / radio, a list of
strings.  Strings are copied into the enclosing WITH-FOREIGN-POOL (the
object keeps them).  Returns the object, or signals when MUI returns
NULL."
  (let ((code (%make-object-type type)))
    (%muimaster "MAKE-OBJECT")
    (or (%with-ulong-array (array params :pooled t)
          (%make-object-a code array))
        (error "AMIGA.MUI:MAKE-OBJECT: MUI_MakeObjectA returned NULL for ~S (MUIO code ~D) with ~D parameter~:P ~S -- the parameters do not fit this type (mui.h: MUIO_Button takes a label, MUIO_Slider label/min/max, MUIO_Cycle label/entries ...), or memory is exhausted"
               type code (length params) params))))

(defun dispose-object (object)
  "MUI_DisposeObject(OBJECT): dispose a MUI object and everything below
it -- disposing the application object takes its windows and all their
children along.  NIL is ignored.  Not intuition's DisposeObject: MUI
objects must go back through muimaster."
  (when object
    (%muimaster "DISPOSE-OBJECT")
    (%dispose-object object))
  nil)

;;; ================================================================
;;; Attributes — MUI additions
;;; ================================================================

(defun get-attr-string (attribute object)
  "GET-ATTR for a STRPTR-valued attribute (MUIA_String_Contents,
MUIA_Text_Contents, MUIA_Window_Title ...): the string it points to as a
fresh Lisp string, or NIL for NULL / an unknown attribute."
  (let ((pointer (get-attr-pointer attribute object)))
    (and pointer (ffi:foreign-to-string pointer))))

;;; ================================================================
;;; Notification
;;; ================================================================

(defun %notify-trigger (trigger)
  (cond ((eq trigger :every-time) +muiv-every-time+)
        ((keywordp trigger)
         (error "AMIGA.MUI:NOTIFY: ~S is not a trigger value -- expected the attribute value that fires the notification (an integer, T/NIL, a foreign pointer) or :EVERY-TIME (MUIV_EveryTime)"
                trigger))
        (t (%ulong trigger))))

(defun %notify-destination (dest)
  (case dest
    (:self        +muiv-notify-self+)
    (:window      +muiv-notify-window+)
    (:application +muiv-notify-application+)
    (:parent      +muiv-notify-parent+)
    (t (if (and (ffi:foreign-pointer-p dest) (not (ffi:null-pointer-p dest)))
           dest
           (error "AMIGA.MUI:NOTIFY: ~S is not a notification destination -- expected the destination object (a foreign pointer) or one of :SELF, :WINDOW, :APPLICATION, :PARENT (MUIV_Notify_*)"
                  dest)))))

(defun %notify-param (param)
  (case param
    (:trigger-value     +muiv-trigger-value+)
    (:not-trigger-value +muiv-not-trigger-value+)
    (t (cond ((stringp param)
              (ffi:foreign-pointer-address (pool-string param)))   ; the notification keeps it
             ((keywordp param)
              (error "AMIGA.MUI:NOTIFY: ~S is not a method parameter -- :TRIGGER-VALUE and :NOT-TRIGGER-VALUE (MUIV_TriggerValue / MUIV_NotTriggerValue) are the only keywords; any other MUIV_ value goes in as its integer"
                     param))
             (t (%ulong param))))))

(defun %notify-args (attribute trigger dest method params)
  "The longwords after MethodID of a MUIP_Notify message: TrigAttr,
TrigVal, DestObj, FollowParams, then the method and its parameters."
  (unless (integerp attribute)
    (error "AMIGA.MUI:NOTIFY: attribute ~S is not an integer -- expected a MUIA_ tag constant (amiga/raw/muimaster)"
           attribute))
  (unless (integerp method)
    (error "AMIGA.MUI:NOTIFY: method ~S is not an integer -- expected a MUIM_ method ID (amiga/raw/muimaster), e.g. MUIM_Application_ReturnID or MUIM_Set"
           method))
  ;; Notify.mui defines the substitution for the MUIV_EveryTime trigger only:
  ;; "MUIV_EveryTime makes MUI execute the notification method every time when
  ;; TrigAttr changes.  In this case, the special value MUIV_TriggerValue in
  ;; the notification method will be replaced with the value that TrigAttr has
  ;; been set to."  Under a fixed trigger MUI leaves the magic longword alone
  ;; and the callee is handed 0x49893131 -- caught here, where the call site
  ;; is still in view, instead of as a nonsense value inside a hook.
  (unless (eq trigger :every-time)
    (let ((special (find-if (lambda (p) (member p '(:trigger-value :not-trigger-value)))
                            params)))
      (when special
        (error "AMIGA.MUI:NOTIFY: ~S needs an :EVERY-TIME trigger -- MUI only substitutes MUIV_TriggerValue when TrigVal is MUIV_EveryTime, so under the trigger ~S the method would be handed the raw magic value.  Notify on the attribute that carries the value you want (MUIA_List_Active rather than MUIA_Listview_DoubleClick, say), or pass the value the trigger already fixes"
               special trigger))))
  (list* attribute
         (%notify-trigger trigger)
         (%notify-destination dest)
         (1+ (length params))
         method
         (mapcar #'%notify-param params)))

(defun notify (object attribute trigger dest method &rest params)
  "MUIM_Notify: when OBJECT's ATTRIBUTE is set to TRIGGER -- a value,
T/NIL, or :EVERY-TIME (MUIV_EveryTime) -- invoke METHOD with PARAMS on
DEST: an object, or :SELF / :WINDOW / :APPLICATION / :PARENT
\(MUIV_Notify_*).  In PARAMS, :TRIGGER-VALUE stands for the new attribute
value (MUIV_TriggerValue) and :NOT-TRIGGER-VALUE for its logical inverse
-- both only under a :EVERY-TIME trigger, the one case MUI substitutes
them (asking for one under a fixed trigger is an error here rather than a
magic longword delivered to the method).  Strings are copied into the
enclosing WITH-FOREIGN-POOL.  FollowParams is computed.  The C idiom
  DoMethod(win, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
           app, 2, MUIM_Application_ReturnID, MUIV_Application_ReturnID_Quit)
is (notify win +muia-window-close-request+ t app
           +muim-application-return-id+ +muiv-application-return-id-quit+).
Returns the method's result."
  (apply #'do-method object +muim-notify+
         (%notify-args attribute trigger dest method params)))

;;; ================================================================
;;; The application loop
;;; ================================================================

(defun return-id (application id)
  "MUIM_Application_ReturnID: queue ID (a LONG) for APPLICATION-INPUT to
return -- what a notification's MUIM_Application_ReturnID does, callable
from Lisp too."
  (do-method application +muim-application-return-id+ id))

(defun %signed32 (unsigned)
  (if (>= unsigned #x80000000) (- unsigned #x100000000) unsigned))

(defun application-input (application)
  "MUIM_Application_NewInput: handle the pending input of APPLICATION and
return two values -- the next queued return ID (:QUIT for
MUIV_Application_ReturnID_Quit, NIL when none is pending, the integer
otherwise) and the signal mask to Wait() on before calling again.  A
zero mask means more input is pending: call again at once (MUI's rule)."
  (let ((storage (ffi:alloc-foreign 4)))
    (unwind-protect
         (progn
           (ffi:poke-u32 storage 0 0)
           (let ((id (%signed32 (do-method application +muim-application-new-input+ storage))))
             (values (cond ((= id +muiv-application-return-id-quit+) :quit)
                           ((zerop id) nil)
                           (t id))
                     (ffi:peek-u32 storage 0))))
      (ffi:free-foreign storage))))

(defvar *event-loop-timeout* nil
  "Default :TIMEOUT of DO-APPLICATION-EVENTS, in seconds; NIL =
interactive (block until the application quits).  Set it before loading
a GUI program to run it unattended -- the examples' harness and the test
suite do:  --eval '(require \"amiga/mui\")'
           --eval '(setf amiga.mui:*event-loop-timeout* 5)' --load ...")

(defmacro do-application-events (((id) application
                                  &key (timeout '*event-loop-timeout*) signals)
                                 &body body)
  "The MUI event loop.  Calls APPLICATION-INPUT, runs BODY once per
return ID with ID bound to it, and Wait()s on the mask MUI hands back
\(plus SIGBREAKF_CTRL_C and the extra SIGNALS mask) until it says :QUIT --
the C idiom
  while (DoMethod(app, MUIM_Application_NewInput, &sigs) != MUIV_Application_ReturnID_Quit)
      if (sigs) { sigs = Wait(sigs | SIGBREAKF_CTRL_C); if (sigs & SIGBREAKF_CTRL_C) break; }
BODY calls (RETURN) to leave the loop; Ctrl-C leaves it too.

With TIMEOUT (seconds; defaults to *EVENT-LOOP-TIMEOUT*) the loop polls
\(Delay(5) between rounds) instead of blocking and returns when the time
is up -- for unattended runs (test suites, screenshots), where nobody
will close the window."
  (let ((app (gensym "APP")) (deadline (gensym "DEADLINE"))
        (extra (gensym "EXTRA")) (sigs (gensym "SIGS")) (got (gensym "GOT")))
    `(let* ((,app ,application)
            (,extra (or ,signals 0))
            (,deadline (let ((timeout ,timeout))
                         (and timeout
                              (+ (get-internal-real-time)
                                 (round (* timeout internal-time-units-per-second)))))))
       (loop
         (multiple-value-bind (,id ,sigs) (application-input ,app)
           (declare (ignorable ,id))
           (when (eq ,id :quit)
             (return))
           (when ,id
             ,@body)
           (when (and ,deadline (>= (get-internal-real-time) ,deadline))
             (return))
           (cond ((zerop ,sigs)
                  ;; MUI: more input pending, NewInput again at once -- but
                  ;; never spin if nothing came of it
                  (unless ,id (amiga.raw.dos:delay 1)))
                 (,deadline
                  (amiga.raw.dos:delay 5))
                 (t
                  ;; AMIGA:WAIT-SIGNALS, not the raw Wait: a task asleep in
                  ;; a plain library call is invisible to a stop-the-world
                  ;; GC another thread starts (a worker thread beside a GUI
                  ;; loop), which would then wait for this loop's next event.
                  (let ((,got (amiga:wait-signals
                               (logior ,sigs ,extra amiga.raw.dos:+sigbreakf-ctrl-c+))))
                    (when (logtest ,got amiga.raw.dos:+sigbreakf-ctrl-c+)
                      (return))))))))))

;;; ================================================================
;;; Requesters and the mui.h helper macros
;;; ================================================================

(defun request (application window title gadgets format &rest params)
  "MUI_RequestA: the easy requester.  APPLICATION (NIL = a system
requester instead) and WINDOW (NIL = not centered on a window) are
objects; TITLE a string or NIL (the application's title); GADGETS the
answers, \"_Save|_Use|*_Cancel\" style ('_' marks the shortcut, '*' the
default); FORMAT a C printf-style format whose PARAMS are longwords --
%ld for integers, %s for strings (foreign or Lisp -- the copies live
for the call only, no pool needed).  Blocks until the user answers;
returns the number of the gadget chosen, 1 = the leftmost, 0 = the
rightmost (the cancel position)."
  (%muimaster "REQUEST")
  (let ((ftitle nil) (fgadgets nil) (fformat nil))
    (unwind-protect
         (progn
           (setf ftitle   (and title (ffi:foreign-string title))
                 fgadgets (ffi:foreign-string gadgets)
                 fformat  (ffi:foreign-string format))
           (%with-ulong-array (array params)
             (%request-a application window 0 ftitle fgadgets fformat array)))
      (when fformat (ffi:free-foreign fformat))
      (when fgadgets (ffi:free-foreign fgadgets))
      (when ftitle (ffi:free-foreign ftitle)))))

(defun make-id (string)
  "MAKE_ID('M','A','I','N'): the four characters of STRING packed into a
longword, for MUIA_Window_ID and the other IDs MUI keeps its settings
under."
  (unless (and (stringp string) (= (length string) 4)
               (every (lambda (c) (< (char-code c) 256)) string))
    (error "AMIGA.MUI:MAKE-ID: ~S is not a four-character ID -- MAKE_ID('M','A','I','N') is (make-id \"MAIN\")"
           string))
  (logior (ash (char-code (char string 0)) 24)
          (ash (char-code (char string 1)) 16)
          (ash (char-code (char string 2)) 8)
          (char-code (char string 3))))

;;; The function-like MUIV_Window_* macros of mui.h.  Width, Height,
;;; AltWidth and AltHeight share the three formulas, TopEdge and
;;; AltTopEdge the fourth.

(defun window-size-minmax (p)
  "MUIV_Window_Width_MinMax(p) / Height / AltWidth / AltHeight: P percent
of the way from the object's minimum to its maximum size."
  (- p))

(defun window-size-visible (p)
  "MUIV_Window_Width_Visible(p) / Height / AltWidth / AltHeight: P
percent of the visible screen size."
  (- -100 p))

(defun window-size-screen (p)
  "MUIV_Window_Width_Screen(p) / Height / AltWidth / AltHeight: P
percent of the screen size."
  (- -200 p))

(defun window-edge-delta (p)
  "MUIV_Window_TopEdge_Delta(p) / AltTopEdge: P pixels below the screen's
title bar."
  (- -3 p))

;;; ================================================================
;;; Custom classes
;;; ================================================================

;;; A MUI custom class is a BOOPSI class whose dispatcher is a Lisp
;;; function (AMIGA.FFI:MAKE-DISPATCHER): MUI enters it with the class,
;;; the object and the message for every method sent to an object of the
;;; class -- OM_NEW, MUIM_AskMinMax, MUIM_Draw, OM_SET ... -- and the
;;; function handles what it wants and passes the rest to the superclass
;;; with DO-SUPER-METHOD, exactly as the C examples of the MUI SDK do
;;; (Class1.c: examples/amiga/mui/class1.lisp).  The message is a foreign
;;; pointer to the method's struct (MethodID first, METHOD-ID); the
;;; object's instance data is INST-DATA; the shortcut macros of mui.h
;;; that a MUIM_Draw method uses (_rp, _mleft, _mwidth ...) are the AREA-*
;;; functions below.  MUI_CreateCustomClass builds the dispatcher Hook
;;; itself and reserves its h_Data (mui.h), so per-class state lives in
;;; Lisp closures or in cl_UserData.
;;;
;;; The dispatcher runs inside MUI: an error in a method is caught at the
;;; callback, the method returns 0 and the condition is re-signaled on the
;;; Lisp side when the call into MUI that dispatched the method returns.

;; struct MUI_CustomClass address -> its dispatcher entry, for
;; CUSTOM-CLASS-STATS; an entry lives as long as the class.
(defvar *class-dispatchers* (make-hash-table))

(defun create-custom-class (superclass function &key (data-size 0)
                                                   (methods nil methods-p))
  "MUI_CreateCustomClass(NULL, SUPERCLASS, ..., DATA-SIZE, dispatcher): a
new private class over SUPERCLASS -- a class name (:AREA, \"Area.mui\", see
CLASS-ID) or another custom class (a foreign pointer from this function)
-- whose dispatcher calls FUNCTION with the class, the object and the
message (foreign pointers).  FUNCTION returns the method's result
\(CALLBACK-ULONG): for the methods it does not handle, what
\(DO-SUPER-METHOD class object message) returns.  DATA-SIZE bytes of
zeroed instance data per object are reachable with INST-DATA.  With
METHODS, a list of method ids, FUNCTION is called for those methods only
and every other message goes to SUPERCLASS natively, without entering
Lisp (AMIGA.FFI:MAKE-DISPATCHER) -- list what the class handles when its
objects sit on an interactive path; CUSTOM-CLASS-STATS counts.  Returns
the struct MUI_CustomClass as a foreign pointer; NEW-OBJECT takes its
CUSTOM-CLASS-CLASS.  The class is deleted (MUI_DeleteCustomClass) and
the dispatcher released when the enclosing WITH-FOREIGN-POOL exits --
after BODY has disposed the objects of the class -- so it must be called
inside one."
  (unless (functionp function)
    (error "AMIGA.MUI:CREATE-CUSTOM-CLASS: ~S is not a function -- expected the dispatcher, a function of (class object message)"
           function))
  (unless (and (integerp data-size) (<= 0 data-size 65535))
    (error "AMIGA.MUI:CREATE-CUSTOM-CLASS: DATA-SIZE ~S is not an integer between 0 and 65535"
           data-size))
  (unless (ffi:foreign-pointer-p superclass)
    (class-id superclass))                                    ; validate first
  (unless amiga.boopsi::*foreign-pool*
    (error "AMIGA.MUI:CREATE-CUSTOM-CLASS: called outside WITH-FOREIGN-POOL -- the pool deletes the class after the objects are gone; wrap the GUI in (WITH-FOREIGN-POOL () ...)"))
  (%muimaster "CREATE-CUSTOM-CLASS")
  (let ((dispatcher (if methods-p
                        (amiga.ffi:make-dispatcher function :methods methods)
                        (amiga.ffi:make-dispatcher function)))
        (mcc nil))
    (unwind-protect
         (setf mcc
               (if (ffi:foreign-pointer-p superclass)
                   (%create-custom-class nil nil superclass data-size dispatcher)
                   (ffi:with-foreign-string (name (class-id superclass))
                     (%create-custom-class nil name nil data-size dispatcher))))
      (unless mcc
        (amiga.ffi:free-dispatcher dispatcher)))
    (unless mcc
      (error "AMIGA.MUI:CREATE-CUSTOM-CLASS: MUI_CreateCustomClass returned NULL for superclass ~S -- the superclass is unknown to this MUI, or memory is exhausted"
             (if (ffi:foreign-pointer-p superclass) superclass (class-id superclass))))
    (setf (gethash (ffi:foreign-pointer-address mcc) *class-dispatchers*) dispatcher)
    (pool-finalizer (lambda ()
                      (%delete-custom-class mcc)
                      (remhash (ffi:foreign-pointer-address mcc) *class-dispatchers*)
                      (amiga.ffi:free-dispatcher dispatcher)))
    mcc))

(defun custom-class-stats (mcc)
  "Three values for a CREATE-CUSTOM-CLASS class made with METHODS: how
many messages its dispatcher was sent, how many of them went to the
superclass without entering Lisp, and how many method ids it lists.  NIL
for a class made without METHODS."
  (let ((dispatcher (gethash (ffi:foreign-pointer-address mcc) *class-dispatchers*)))
    (unless dispatcher
      (error "AMIGA.MUI:CUSTOM-CLASS-STATS: ~S is not a live CREATE-CUSTOM-CLASS class" mcc))
    (amiga.ffi:dispatcher-stats dispatcher)))

;;; ----------------------------------------------------------------
;;; Struct layouts.  The custom-class support reads MUI's and intuition's
;;; structs by hand: this module cannot require the generated
;;; amiga/raw/muimaster (it opens muimaster.library at load, and AMIGA.MUI
;;; must load where MUI is absent).  So every offset and size below is a
;;; constant named after the generated accessor or size --
;;; +MUI-AREA-DATA-FLAGS-OFFSET+ is the offset of
;;; AMIGA.RAW.MUIMASTER:MUI-AREA-DATA-FLAGS, +MUI-NOTIFY-DATA-SIZE+ its
;;; *MUI-NOTIFY-DATA-SIZE* -- and tests/test_amiga_curated_vs_raw.lisp
;;; requires each to agree with the generated struct row (check 4).  The
;;; layouts are mui.h 3.8's and intuition/classes.h's, the same on MorphOS
;;; (its headers pack(2) to the 68k layout).
;;; ----------------------------------------------------------------

;; struct MUI_CustomClass (mui.h)
(defconstant +mui-custom-class-class-offset+ 24)         ; mcc_Class
;; struct IClass (intuition/classes.h)
(defconstant +iclass-dispatcher-offset+ 0)               ; cl_Dispatcher, a struct Hook
(defconstant +iclass-super-offset+ 24)                   ; cl_Super
(defconstant +iclass-inst-offset-offset+ 32)             ; cl_InstOffset (UWORD)
;; the custom-class method messages
(defconstant +muip-ask-min-max-min-max-info-offset+ 4)   ; struct MUIP_AskMinMax.MinMaxInfo
(defconstant +muip-draw-flags-offset+ 4)                 ; struct MUIP_Draw.flags
;; struct MUI_MinMax: six WORDs
(defconstant +mui-min-max-size+ 12)
(defconstant +mui-min-max-min-width-offset+ 0)
(defconstant +mui-min-max-min-height-offset+ 2)
(defconstant +mui-min-max-max-width-offset+ 4)
(defconstant +mui-min-max-max-height-offset+ 6)
(defconstant +mui-min-max-def-width-offset+ 8)
(defconstant +mui-min-max-def-height-offset+ 10)
;; struct MUI_LayoutMsg: what a MUIA_Group_LayoutHook is called with.
;; lm_MinMax is embedded (not a pointer, as in MUIP_AskMinMax), and
;; lm_Layout's Width/Height are its 20/24 (the generator flattens them).
(defconstant +mui-layout-msg-size+ 36)
(defconstant +mui-layout-msg-type-offset+ 0)
(defconstant +mui-layout-msg-children-offset+ 4)
(defconstant +mui-layout-msg-min-max-offset+ 8)
(defconstant +mui-layout-msg-width-offset+ 20)
(defconstant +mui-layout-msg-height-offset+ 24)
;; an Object * points at its MUI_NotifyData, followed by the MUI_AreaData
;; (mui.h's struct __dummyXFC2__): an area field sits at
;; +MUI-NOTIFY-DATA-SIZE+ plus its MUI_AreaData offset
(defconstant +mui-notify-data-size+ 28)
(defconstant +mui-area-data-render-info-offset+ 0)       ; mad_RenderInfo
(defconstant +mui-area-data-font-offset+ 8)              ; mad_Font
(defconstant +mui-area-data-min-max-offset+ 12)          ; mad_MinMax, a struct MUI_MinMax
(defconstant +mui-area-data-box-offset+ 24)              ; mad_Box, a struct IBox
(defconstant +mui-area-data-addleft-offset+ 32)          ; mad_addleft .. mad_subheight, BYTEs
(defconstant +mui-area-data-addtop-offset+ 33)
(defconstant +mui-area-data-subwidth-offset+ 34)
(defconstant +mui-area-data-subheight-offset+ 35)
(defconstant +mui-area-data-flags-offset+ 36)            ; mad_Flags
;; struct IBox (intuition.h): four WORDs
(defconstant +i-box-left-offset+ 0)
(defconstant +i-box-top-offset+ 2)
(defconstant +i-box-width-offset+ 4)
(defconstant +i-box-height-offset+ 6)
;; struct MUI_RenderInfo
(defconstant +mui-render-info-screen-offset+ 4)          ; mri_Screen
(defconstant +mui-render-info-draw-info-offset+ 8)       ; mri_DrawInfo
(defconstant +mui-render-info-pens-offset+ 12)           ; mri_Pens
(defconstant +mui-render-info-window-offset+ 16)         ; mri_Window
(defconstant +mui-render-info-rastport-offset+ 20)       ; mri_RastPort

(defun custom-class-class (mcc)
  "The struct IClass of a CREATE-CUSTOM-CLASS class (mcc->mcc_Class) as a
foreign pointer -- what NEW-OBJECT takes to make an object of it."
  (ffi:make-foreign-pointer (ffi:peek-u32 mcc +mui-custom-class-class-offset+)))

(defun method-id (message)
  "The MethodID of a method MESSAGE (its first longword), as an unsigned
integer -- compare against the MUIM_ / OM_ constants."
  (ffi:peek-u32 message 0))

(defun do-super-method (class object message)
  "DoSuperMethodA(CLASS, OBJECT, MESSAGE): pass MESSAGE on to the
superclass of CLASS, from inside a dispatcher.  Returns the result as an
unsigned integer (the object for OM_NEW, 0 on failure)."
  ;; struct IClass.cl_Super -> its cl_Dispatcher hook
  (amiga.raw.utility:call-hook-pkt
   (ffi:make-foreign-pointer (+ (ffi:peek-u32 class +iclass-super-offset+)
                                +iclass-dispatcher-offset+))
   object message))

(defun inst-data (class object)
  "INST_DATA(CLASS, OBJECT): the instance data of OBJECT that CLASS's
DATA-SIZE reserved, as a foreign pointer (zeroed at OM_NEW)."
  (ffi:pointer+ object (ffi:peek-u16 class +iclass-inst-offset-offset+)))

(defun min-max-info (message)
  "The struct MUI_MinMax a MUIM_AskMinMax MESSAGE points at, as a foreign
pointer: six WORDs -- MinWidth, MinHeight, MaxWidth, MaxHeight, DefWidth,
DefHeight -- at offsets 0, 2, 4, 6, 8, 10."
  (ffi:make-foreign-pointer (ffi:peek-u32 message +muip-ask-min-max-min-max-info-offset+)))

(defun %min-max-offsets ()
  "The six WORD offsets of a struct MUI_MinMax, in the keyword order the
functions below take them."
  (list +mui-min-max-min-width-offset+ +mui-min-max-min-height-offset+
        +mui-min-max-max-width-offset+ +mui-min-max-max-height-offset+
        +mui-min-max-def-width-offset+ +mui-min-max-def-height-offset+))

(defun add-min-max (message &key (min-width 0) (min-height 0) (max-width 0)
                                 (max-height 0) (def-width 0) (def-height 0))
  "Add the given amounts to the MUI_MinMax of a MUIM_AskMinMax MESSAGE --
what every AskMinMax method does after DO-SUPER-METHOD has filled in the
superclass's frame and spacing (the values must be ADDED, not set).
Returns NIL."
  (let ((info (min-max-info message)))
    (loop for delta in (list min-width min-height max-width max-height def-width def-height)
          for offset in (%min-max-offsets)
          unless (zerop delta)
            do (ffi:poke-i16 info (+ (ffi:peek-i16 info offset) delta) offset)))
  nil)

(defun set-min-max (info &key min-width min-height max-width max-height
                              def-width def-height)
  "Set the given fields of the struct MUI_MinMax INFO -- a foreign
pointer, from MIN-MAX-INFO (a MUIM_AskMinMax message) or from
LAYOUT-MSG-MIN-MAX (a MUILM_MINMAX layout message).  NIL leaves a field
as it is.  A custom layout hook SETs the six words, because it computes
the whole group's size itself; an MUIM_AskMinMax method ADDs to what its
superclass filled in instead -- that is ADD-MIN-MAX, which takes the
message directly.  Returns NIL."
  (loop for value in (list min-width min-height max-width max-height def-width def-height)
        for offset in (%min-max-offsets)
        when value
          do (unless (integerp value)
               (error "AMIGA.MUI:SET-MIN-MAX: ~S is not an integer -- the fields of a MUI_MinMax are WORDs (MUI_MAXMAX = 10000 means unlimited)"
                      value))
             (ffi:poke-i16 info value offset))
  nil)

(defun draw-flags (message)
  "The flags of a MUIM_Draw MESSAGE (struct MUIP_Draw.flags): test
MADF_DRAWOBJECT before rendering, MADF_DRAWUPDATE for an update."
  (ffi:peek-u32 message +muip-draw-flags-offset+))

;;; The mui.h shortcuts for custom-class methods, over the MUI_AreaData
;;; that follows the object's MUI_NotifyData (the layout constants above).
;;; Validity is mui.h's: the render info between MUIM_Setup and
;;; MUIM_Cleanup, the window and rastport between MUIM_Show and MUIM_Hide,
;;; the box and its margins during MUIM_Draw.

(defun %area-offset (field-offset)
  "The offset of a MUI_AreaData field from the object."
  (+ +mui-notify-data-size+ field-offset))

(defun area-render-info (object)
  "muiRenderInfo(obj): the struct MUI_RenderInfo of OBJECT (valid between
MUIM_Setup and MUIM_Cleanup), as a foreign pointer."
  (ffi:make-foreign-pointer
   (ffi:peek-u32 object (%area-offset +mui-area-data-render-info-offset+))))

(defun %render-info-field (object offset)
  (ffi:make-foreign-pointer (ffi:peek-u32 (area-render-info object) offset)))

(defun area-window (object)    "_window(obj): the struct Window (MUIM_Show..MUIM_Hide)."  (%render-info-field object +mui-render-info-window-offset+))
(defun area-rastport (object)  "_rp(obj): the struct RastPort to draw into (MUIM_Show..MUIM_Hide)." (%render-info-field object +mui-render-info-rastport-offset+))
(defun area-draw-info (object) "_dri(obj): the screen's struct DrawInfo (MUIM_Setup..MUIM_Cleanup)." (%render-info-field object +mui-render-info-draw-info-offset+))
(defun area-screen (object)    "_screen(obj): the struct Screen (MUIM_Setup..MUIM_Cleanup)." (%render-info-field object +mui-render-info-screen-offset+))
(defun area-pens (object)      "_pens(obj): MUI's pen array, UWORD[] (MUIM_Setup..MUIM_Cleanup)." (%render-info-field object +mui-render-info-pens-offset+))
(defun area-font (object)      "_font(obj): the object's struct TextFont (MUIM_Setup..MUIM_Cleanup)." (ffi:make-foreign-pointer (ffi:peek-u32 object (%area-offset +mui-area-data-font-offset+))))
(defun area-flags (object)     "mad_Flags of OBJECT (MADF_*)." (ffi:peek-u32 object (%area-offset +mui-area-data-flags-offset+)))

(defun %box-field (object box-offset)
  "A WORD of the object's mad_Box (a struct IBox)."
  (ffi:peek-i16 object (%area-offset (+ +mui-area-data-box-offset+ box-offset))))

(defun area-left (object)   "_left(obj): the object's left edge (MUIM_Draw)."  (%box-field object +i-box-left-offset+))
(defun area-top (object)    "_top(obj): the object's top edge (MUIM_Draw)."    (%box-field object +i-box-top-offset+))
(defun area-width (object)  "_width(obj): the object's width (MUIM_Draw)."     (%box-field object +i-box-width-offset+))
(defun area-height (object) "_height(obj): the object's height (MUIM_Draw)."   (%box-field object +i-box-height-offset+))
(defun area-right (object)  "_right(obj): left + width - 1."   (+ (area-left object) (area-width object) -1))
(defun area-bottom (object) "_bottom(obj): top + height - 1."  (+ (area-top object) (area-height object) -1))

(defun %margin (object field-offset)
  "One of the four BYTE margins of the MUI_AreaData."
  (ffi:peek-i8 object (%area-offset field-offset)))

(defun area-mleft (object)   "_mleft(obj): the left edge inside the frame (MUIM_Draw)."   (+ (area-left object) (%margin object +mui-area-data-addleft-offset+)))
(defun area-mtop (object)    "_mtop(obj): the top edge inside the frame (MUIM_Draw)."     (+ (area-top object) (%margin object +mui-area-data-addtop-offset+)))
(defun area-mwidth (object)  "_mwidth(obj): the width inside the frame (MUIM_Draw)."      (- (area-width object) (%margin object +mui-area-data-subwidth-offset+)))
(defun area-mheight (object) "_mheight(obj): the height inside the frame (MUIM_Draw)."    (- (area-height object) (%margin object +mui-area-data-subheight-offset+)))
(defun area-mright (object)  "_mright(obj): mleft + mwidth - 1."   (+ (area-mleft object) (area-mwidth object) -1))
(defun area-mbottom (object) "_mbottom(obj): mtop + mheight - 1."  (+ (area-mtop object) (area-mheight object) -1))

(defun %area-min-max (object offset)
  "A WORD of the object's mad_MinMax (a struct MUI_MinMax)."
  (ffi:peek-i16 object (%area-offset (+ +mui-area-data-min-max-offset+ offset))))

(defun area-min-width (object)
  "_minwidth(obj): the minimum width OBJECT settled on when MUI asked it
\(valid between MUIM_Show and MUIM_Hide) -- what a custom layout hook
reads from each of its children before placing them."
  (%area-min-max object +mui-min-max-min-width-offset+))

(defun area-min-height (object)
  "_minheight(obj): the minimum height OBJECT settled on (MUIM_Show..MUIM_Hide)."
  (%area-min-max object +mui-min-max-min-height-offset+))

;;; ================================================================
;;; Custom layout hooks
;;; ================================================================

;;; A group whose MUIA_Group_LayoutHook is a POOL-HOOK lays its children
;;; out itself.  MUI calls the hook with a struct MUI_LayoutMsg and one of
;;; two lm_Types: MUILM_MINMAX, where the hook reports the group's own
;;; minimum / maximum / default size (its children have already been
;;; asked, so AREA-MIN-WIDTH / AREA-MIN-HEIGHT of each is known), and
;;; MUILM_LAYOUT, where it calls LAYOUT-CHILD for every child inside the
;;; rectangle (0, 0, LAYOUT-MSG-WIDTH - 1, LAYOUT-MSG-HEIGHT - 1).  Any
;;; other lm_Type must be answered with MUILM_UNKNOWN.  The port of the
;;; MUI SDK's Layout.c is examples/amiga/mui/layout.lisp.

(defun layout-msg-type (message)
  "lm_Type of a MUI_LayoutMsg: MUILM_MINMAX or MUILM_LAYOUT (a layout
hook returns MUILM_UNKNOWN for anything else)."
  (ffi:peek-u32 message +mui-layout-msg-type-offset+))

(defun layout-msg-min-max (message)
  "The struct MUI_MinMax embedded in a MUI_LayoutMsg (lm_MinMax), as a
foreign pointer -- what a MUILM_MINMAX hook fills in with SET-MIN-MAX."
  (ffi:pointer+ message +mui-layout-msg-min-max-offset+))

(defun layout-msg-width (message)
  "lm_Layout.Width: the width MUI gives the group for a MUILM_LAYOUT.
SETF-able -- a virtual group writes back the width it really needs."
  (ffi:peek-i32 message +mui-layout-msg-width-offset+))

(defun (setf layout-msg-width) (value message)
  (ffi:poke-i32 message value +mui-layout-msg-width-offset+)
  value)

(defun layout-msg-height (message)
  "lm_Layout.Height: the height MUI gives the group for a MUILM_LAYOUT.
SETF-able, like LAYOUT-MSG-WIDTH."
  (ffi:peek-i32 message +mui-layout-msg-height-offset+))

(defun (setf layout-msg-height) (value message)
  (ffi:poke-i32 message value +mui-layout-msg-height-offset+)
  value)

(defun layout-children (message)
  "The children of the group being laid out, as a list of objects
\(foreign pointers): the NextObject() walk of a MUI_LayoutMsg's
lm_Children that every custom layout hook starts with.  The order is the
order the children were added in."
  (let ((list (ffi:make-foreign-pointer
               (ffi:peek-u32 message +mui-layout-msg-children-offset+))))
    (if (ffi:null-pointer-p list)
        '()
        (let ((cursor (ffi:alloc-foreign 4)))
          (unwind-protect
               (progn
                 (ffi:poke-u32 cursor (ffi:peek-u32 list 0) 0)   ; mlh_Head
                 (loop for child = (amiga.raw.intuition:next-object cursor)
                       while child
                       collect child))
            (ffi:free-foreign cursor))))))

(defun layout-child (child left top width height &optional (flags 0))
  "MUI_Layout(CHILD, LEFT, TOP, WIDTH, HEIGHT, FLAGS): place CHILD at
LEFT/TOP with WIDTH x HEIGHT, in coordinates relative to the group's own
rectangle -- what a custom layout hook calls for each of LAYOUT-CHILDREN
on a MUILM_LAYOUT message.  Returns T when MUI accepted the placement;
a hook that gets NIL should stop and return FALSE (errors during layout
are hard for MUI to recover from)."
  (%muimaster "LAYOUT-CHILD")
  (%layout child left top width height flags))

;;; MUI_RequestIDCMP / MUI_RejectIDCMP — what a custom class that answers
;;; MUIM_HandleInput asks for in MUIM_Setup and gives back in MUIM_Cleanup.

(defun request-idcmp (object flags)
  "MUI_RequestIDCMP(OBJECT, FLAGS): ask the window OBJECT lives in for
the IDCMP classes FLAGS (IDCMP_MOUSEMOVE, IDCMP_INTUITICKS ...), so
MUI sends OBJECT a MUIM_HandleInput for them.  A custom class does this
in its MUIM_Setup method and REJECT-IDCMP with the same flags in
MUIM_Cleanup.  Returns NIL."
  (%muimaster "REQUEST-IDCMP")
  (%request-idcmp object flags)
  nil)

(defun reject-idcmp (object flags)
  "MUI_RejectIDCMP(OBJECT, FLAGS): give back the IDCMP classes
REQUEST-IDCMP asked for, in MUIM_Cleanup.  Returns NIL."
  (%muimaster "REJECT-IDCMP")
  (%reject-idcmp object flags)
  nil)

;;; ================================================================
;;; The string-key hook: keys taken from an active String
;;; ================================================================
;;;
;;; An active MUI 3.8 String is an Intuition string gadget, and Intuition
;;; runs its edit hook -- the Hook that MUIA_String_EditHook installs --
;;; on input.device's task, not the application's.  A Lisp hook there
;;; is answered with 0 without running (specs/mui-bindings.md §10.3.2:
;;; no VM, an unknown stack), so the key it meant to take goes to the
;;; gadget: TAB moves the focus on, C-g types nothing.  (MUI 4 never
;;; calls the hook at all and hands the keys to MUIM_HandleEvent.)
;;;
;;; MAKE-STRING-KEY-HOOK builds a hook that is C from end to end: it
;;; matches the key's raw code and qualifiers against ENTRIES, and for
;;; a match it makes the key a no-op for the gadget and queues the
;;; entry's VALUE to OBJECT as METHOD's one argument through
;;; MUIM_Application_PushMethod -- so a Lisp method (a custom class's
;;; dispatcher) runs on the application's task, after the gadget is done
;;; with the event.  Which keys to take is decided in Lisp when the hook
;;; is made, from the keymap (MapRawKey over the raw codes: which code
;;; and qualifiers give `g' with Control, say); the hook itself never
;;; decodes.

(defun make-string-key-hook (app object method entries)
  "A struct Hook for a String's MUIA_String_EditHook that takes the key
presses in ENTRIES away from the gadget and pushes them to OBJECT.
ENTRIES is a list of (CODE QUAL-MASK QUAL-VALUE VALUE): a press whose
raw code (ie_Code, bit 7 clear) is CODE and whose qualifiers, masked
with QUAL-MASK, equal QUAL-VALUE is taken -- the right Shift and Alt
count as the left ones before the mask is applied -- and VALUE (an
unsigned 32-bit integer, e.g. the key's own encoding) is queued as
\(DoMethod APP MUIM_Application_PushMethod OBJECT 2 METHOD VALUE), which
the application's input loop turns into (DoMethod OBJECT METHOD VALUE).
Every other key stays the gadget's.  Returns the hook as a foreign
pointer, the value of MUIA_String_EditHook; it must outlive the String
and is released with FREE-STRING-KEY-HOOK.  STRING-KEY-HOOK-STATS says
how often it was called and how often it matched."
  (unless (and (listp entries) (every (lambda (e) (and (listp e) (= (length e) 4))) entries))
    (error "AMIGA.MUI:MAKE-STRING-KEY-HOOK: ENTRIES must be a list of (code qual-mask qual-value value) lists, got ~S"
           entries))
  (amiga::%make-string-key-hook app object +muim-application-push-method+ method entries))

(defun free-string-key-hook (hook)
  "Release a MAKE-STRING-KEY-HOOK hook -- only after every String that
holds it is disposed, since its entry code goes with it.  NIL is ignored."
  (amiga::%free-string-key-hook hook)
  nil)

(defun string-key-hook-entry (hook)
  "The hook's h_Entry as a foreign pointer -- what the OS calls, exact on
the host too, where it is a C function of (hook, sgwork, message) that
tests call themselves."
  (amiga::%string-key-hook-entry hook))

(defun string-key-hook-stats (hook)
  "Three values: how often the OS called HOOK, how many key presses it
took, and how many entries it holds.  `never called' and `never matched'
are different bugs."
  (values-list (amiga::%string-key-hook-stats hook)))

(provide "amiga/mui")
