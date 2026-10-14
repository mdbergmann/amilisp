;;; amiga/intuition.lisp — Intuition library abstractions for AmiLisp
;;;
;;; Loaded via (require "amiga/intuition").
;;; Provides high-level window/screen/IDCMP management.

;; compile-time too: COMPILE-FILE (the host builds the lib/amiga FASLs) must see
;; these packages at read time, not only LOAD.
(eval-when (:compile-toplevel :load-toplevel :execute)
  (require "amiga/ffi"))

(defpackage "AMIGA.INTUITION"
  (:use "CL" "FFI" "AMIGA.FFI")
  (:export
   ;; Library
   "*INTUITION-BASE*"
   ;; Window
   "OPEN-WINDOW" "CLOSE-WINDOW" "WITH-WINDOW"
   "WINDOW-RASTPORT" "WINDOW-WIDTH" "WINDOW-HEIGHT"
   "WINDOW-LEFT" "WINDOW-TOP" "WINDOW-TITLE"
   "WINDOW-USER-PORT"
   "WINDOW-BORDER-LEFT" "WINDOW-BORDER-TOP"
   "WINDOW-BORDER-RIGHT" "WINDOW-BORDER-BOTTOM"
   "WINDOW-GZZ-WIDTH" "WINDOW-GZZ-HEIGHT"
   ;; Screen
   "OPEN-SCREEN" "CLOSE-SCREEN" "WITH-SCREEN" "SHOW-TITLE"
   "SCREEN-WIDTH" "SCREEN-HEIGHT" "SCREEN-BAR-HEIGHT" "SCREEN-VIEWPORT"
   ;; IDCMP
   "GET-MSG" "REPLY-MSG" "WAIT-PORT"
   "MSG-CLASS" "MSG-CODE" "MSG-QUALIFIER" "MSG-MOUSE-X" "MSG-MOUSE-Y"
   "MSG-RAW-KEY"
   "EVENT-LOOP" "*EVENT-LOOP-MAX-WAITS*" "*EVENT-LOOP-TIMEOUT*"
   ;; Input event qualifier bits (IntuiMessage->Qualifier)
   "+IEQUALIFIER-LSHIFT+" "+IEQUALIFIER-RSHIFT+" "+IEQUALIFIER-CAPSLOCK+"
   ;; Mouse button codes (IDCMP_MOUSEBUTTONS msg-code values)
   "+SELECTDOWN+" "+SELECTUP+" "+MENUDOWN+" "+MENUUP+"
   ;; Mouse pointer
   "SET-POINTER" "CLEAR-POINTER" "MAKE-POINTER-SPRITE"
   ;; IDCMP class constants
   "+IDCMP-CLOSEWINDOW+" "+IDCMP-GADGETUP+" "+IDCMP-GADGETDOWN+"
   "+IDCMP-MOUSEBUTTONS+" "+IDCMP-MOUSEMOVE+" "+IDCMP-RAWKEY+"
   "+IDCMP-MENUPICK+" "+IDCMP-REFRESHWINDOW+" "+IDCMP-NEWSIZE+"
   "+IDCMP-VANILLAKEY+" "+IDCMP-ACTIVEWINDOW+" "+IDCMP-INACTIVEWINDOW+"
   "+IDCMP-INTUITICKS+"
   ;; Window flag constants
   "+WFLG-CLOSEGADGET+" "+WFLG-DRAGBAR+" "+WFLG-DEPTHGADGET+"
   "+WFLG-SIZEGADGET+" "+WFLG-ACTIVATE+" "+WFLG-SMART-REFRESH+"
   "+WFLG-SIMPLE-REFRESH+" "+WFLG-BACKDROP+" "+WFLG-BORDERLESS+"
   "+WFLG-GIMMEZEROZERO+" "+WFLG-REPORTMOUSE+" "+WFLG-RMBTRAP+"
   ;; Tag constants
   "+WA-LEFT+" "+WA-TOP+" "+WA-WIDTH+" "+WA-HEIGHT+"
   "+WA-TITLE+" "+WA-IDCMP+" "+WA-FLAGS+" "+WA-CUSTOMSCREEN+"
   "+WA-GADGETS+"
   ;; Menu/gadget list
   "SET-MENU-STRIP" "CLEAR-MENU-STRIP"
   "ADD-GADGET-LIST" "REFRESH-GADGET-LIST"
   ;; Public screens
   "LOCK-PUB-SCREEN" "UNLOCK-PUB-SCREEN" "WITH-PUB-SCREEN"
   ;; Display dimensions
   "QUERY-OVERSCAN" "DISPLAY-MODE-HEIGHT"
   "+OSCAN-TEXT+" "+OSCAN-STANDARD+" "+OSCAN-MAX+" "+OSCAN-VIDEO+"))

(in-package "AMIGA.INTUITION")

;;; ================================================================
;;; Intuition library base (opened on load).  NIL on the host build, where
;;; this module is loaded for COMPILE-FILE (the host builds the lib/amiga
;;; FASLs) -- same convention as the generated amiga/raw modules.
;;; ================================================================

(define-library-variable *intuition-base*
  (when (member :amigaos *features*)
    (amiga:open-library "intuition.library" 39)))
(when (and (member :amigaos *features*) (null *intuition-base*))
  (error "Cannot open intuition.library v39"))

;;; ================================================================
;;; LVO offsets (from amiga/intuition_lib.fd)
;;; ================================================================

(defconstant +lvo-open-window+         -204)
(defconstant +lvo-close-window+         -72)
(defconstant +lvo-open-screen+         -198)
(defconstant +lvo-close-screen+         -66)
(defconstant +lvo-open-window-tag-list+ -606)
(defconstant +lvo-open-screen-tag-list+ -612)
(defconstant +lvo-set-menu-strip+      -264)
(defconstant +lvo-clear-menu-strip+     -54)
(defconstant +lvo-add-g-list+          -438)
(defconstant +lvo-refresh-g-list+      -432)
(defconstant +lvo-query-overscan+      -474)
(defconstant +lvo-lock-pub-screen+     -510)
(defconstant +lvo-unlock-pub-screen+   -516)
(defconstant +lvo-show-title+          -282)
(defconstant +lvo-set-pointer+         -270)
(defconstant +lvo-clear-pointer+        -60)
(defconstant +lvo-dos-delay+           -198)  ; dos.library Delay(ticks), d1 = ticks

;;; Exec LVOs for message handling
(define-library-variable *exec-base* (ffi:make-foreign-pointer 4))  ; ExecBase at absolute addr 4
(defconstant +lvo-wait-port+    -384)
(defconstant +lvo-get-msg+      -372)
(defconstant +lvo-reply-msg+    -378)

;;; ================================================================
;;; IDCMP class constants
;;; ================================================================

(defconstant +idcmp-sizeverify+     #x00000001)
(defconstant +idcmp-newsize+        #x00000002)
(defconstant +idcmp-refreshwindow+  #x00000004)
(defconstant +idcmp-mousebuttons+   #x00000008)
(defconstant +idcmp-mousemove+      #x00000010)
(defconstant +idcmp-gadgetdown+     #x00000020)
(defconstant +idcmp-gadgetup+       #x00000040)
(defconstant +idcmp-menupick+       #x00000100)
(defconstant +idcmp-closewindow+    #x00000200)
(defconstant +idcmp-rawkey+         #x00000400)
(defconstant +idcmp-activewindow+   #x00040000)
(defconstant +idcmp-inactivewindow+ #x00080000)
(defconstant +idcmp-vanillakey+     #x00200000)
(defconstant +idcmp-intuiticks+     #x00400000)

;;; IDCMP_MOUSEBUTTONS message codes (devices/inputevent.h IECODE_*):
;;; left button = SELECTDOWN, released = SELECTUP (the UP_PREFIX bit);
;;; the right button only arrives with WFLG_RMBTRAP — menus eat it
;;; otherwise.
(defconstant +selectdown+ #x68)
(defconstant +selectup+   #xE8)
(defconstant +menudown+   #x69)
(defconstant +menuup+     #xE9)

;;; ================================================================
;;; Window flag constants
;;; ================================================================

(defconstant +wflg-sizegadget+      #x0001)
(defconstant +wflg-dragbar+         #x0002)
(defconstant +wflg-depthgadget+     #x0004)
(defconstant +wflg-closegadget+     #x0008)
(defconstant +wflg-smart-refresh+   #x0000)
(defconstant +wflg-simple-refresh+  #x0040)
(defconstant +wflg-backdrop+        #x0100)
(defconstant +wflg-borderless+      #x0800)
(defconstant +wflg-activate+        #x1000)
(defconstant +wflg-gimmezerozero+   #x0400)
(defconstant +wflg-reportmouse+     #x0200)  ; WFLG_REPORTMOUSE (was wrongly 4 = DEPTHGADGET)
(defconstant +wflg-rmbtrap+         #x00010000)

;;; ================================================================
;;; Window tag constants (for OpenWindowTagList)
;;; ================================================================

(defconstant +tag-user+       #x80000000)
(defconstant +wa-dummy+       (+ +tag-user+ 99))   ; 0x80000063
(defconstant +wa-left+        (+ +wa-dummy+ #x01))
(defconstant +wa-top+         (+ +wa-dummy+ #x02))
(defconstant +wa-width+       (+ +wa-dummy+ #x03))
(defconstant +wa-height+      (+ +wa-dummy+ #x04))
(defconstant +wa-idcmp+       (+ +wa-dummy+ #x07))
(defconstant +wa-flags+       (+ +wa-dummy+ #x08))
(defconstant +wa-gadgets+     (+ +wa-dummy+ #x09))
(defconstant +wa-title+       (+ +wa-dummy+ #x0B))
(defconstant +wa-customscreen+ (+ +wa-dummy+ #x0D))

;;; ================================================================
;;; Screen tag constants (for OpenScreenTagList, intuition/screens.h)
;;; ================================================================

(defconstant +sa-dummy+       (+ +tag-user+ 32))
(defconstant +sa-left+        (+ +sa-dummy+ #x01))
(defconstant +sa-top+         (+ +sa-dummy+ #x02))
(defconstant +sa-width+       (+ +sa-dummy+ #x03))
(defconstant +sa-height+      (+ +sa-dummy+ #x04))
(defconstant +sa-depth+       (+ +sa-dummy+ #x05))
(defconstant +sa-title+       (+ +sa-dummy+ #x08))
(defconstant +sa-error-code+  (+ +sa-dummy+ #x0A))
(defconstant +sa-display-id+  (+ +sa-dummy+ #x12))
(defconstant +sa-show-title+  (+ +sa-dummy+ #x16))
(defconstant +sa-quiet+       (+ +sa-dummy+ #x18))

;;; ================================================================
;;; Struct layouts
;;; ================================================================

;;; struct Window (partial — key fields only)
;;; struct Window layout (from intuition/intuition.h):
;;;   0: NextWindow*    4: LeftEdge(W)  6: TopEdge(W)
;;;   8: Width(W)      10: Height(W)   32: Title*
;;;  50: RPort*         54: BorderLeft/Top/Right/Bottom (4 BYTEs)
;;;  86: UserPort*    112: GZZWidth(W) 114: GZZHeight(W)
(ffi:defcstruct window
  (left-edge     :u16  4)
  (top-edge      :u16  6)
  (width         :u16  8)
  (height        :u16 10)
  (rport         :pointer 50)    ; Window->RPort
  (border-left   :u8  54)        ; pixels reserved at left edge
  (border-top    :u8  55)        ; pixels reserved at top edge (incl. title bar)
  (border-right  :u8  56)
  (border-bottom :u8  57)
  (uport         :pointer 86)    ; Window->UserPort (MsgPort for IDCMP)
  (gzz-width     :u16 112)       ; inner width  (only valid with GIMMEZEROZERO)
  (gzz-height    :u16 114)       ; inner height (only valid with GIMMEZEROZERO)
  (title         :pointer 32))

;;; struct IntuiMessage (partial)
(ffi:defcstruct intui-message
  (class       :u32  20)   ; im->Class (IDCMP flags)
  (code        :u16  24)   ; im->Code
  (qualifier   :u16  26)   ; im->Qualifier (IEQUALIFIER_* bits)
  (iaddress    :u32  28)   ; im->IAddress (for key events V36+: the
                           ; originating struct InputEvent — see
                           ; MSG-RAW-KEY)
  (mouse-x     :u16  32)   ; im->MouseX
  (mouse-y     :u16  34))  ; im->MouseY

;;; struct InputEvent (devices/inputevent.h), the fields MSG-RAW-KEY
;;; reads: 0 ie_NextEvent*, 4 ie_Class(UBYTE), 5 ie_SubClass(UBYTE),
;;; 6 ie_Code(UWORD), 8 ie_Qualifier(UWORD).
(ffi:defcstruct input-event
  (ie-class :u8  4)
  (ie-code  :u16 6))

(defconstant +ieclass-rawkey+ #x01)

;;; Input event qualifier bits (devices/inputevent.h) as they appear in
;;; IntuiMessage->Qualifier.  With IDCMP_VANILLAKEY the keymap has
;;; already been applied to Code, so these tell you HOW the character
;;; was produced — e.g. whether an uppercase letter came from a held
;;; Shift or merely from Caps Lock.
(defconstant +iequalifier-lshift+   #x0001)
(defconstant +iequalifier-rshift+   #x0002)
(defconstant +iequalifier-capslock+ #x0004)

;;; struct Screen (partial — from intuition/screens.h):
;;;   0: NextScreen*  4: FirstWindow*  8: LeftEdge(W)  10: TopEdge(W)
;;;  12: Width(W)    14: Height(W)    30: BarHeight(B)
;;;  44: ViewPort (embedded struct — see SCREEN-VIEWPORT below)
(ffi:defcstruct screen
  (width      :u16 12)
  (height     :u16 14)
  (bar-height :u8  30))

(defun screen-viewport (screen)
  "Pointer to SCREEN's embedded ViewPort (offset 44) -- what
AMIGA.GFX:SET-RGB4 wants for setting the screen palette."
  (ffi:make-foreign-pointer (+ (ffi:foreign-pointer-address screen) 44)))

;;; ================================================================
;;; Window management
;;; ================================================================

;;; Intuition keeps the WA_Title/SA_Title pointer for the lifetime of
;;; the window/screen (title redraws read it on demand) — the string
;;; must stay allocated until close, not merely until the Open call
;;; returns.  The table below maps the opened object's address to its
;;; title string; CLOSE-WINDOW/CLOSE-SCREEN free it.
(defvar *title-strings* (make-hash-table))

(defun open-window (&key (title "AmiLisp") (left 0) (top 0)
                         (width 320) (height 200) screen
                         (idcmp +idcmp-closewindow+)
                         (flags (logior +wflg-closegadget+
                                        +wflg-dragbar+
                                        +wflg-depthgadget+
                                        +wflg-sizegadget+
                                        +wflg-activate+)))
  "Open an Intuition window via OpenWindowTagList.
TITLE NIL opens an untitled window -- that matters for borderless
backdrop windows, where a WA_Title still costs a title bar
\(window-border-top stays 0 only without one).
Returns a foreign pointer to the Window struct, or signals an error."
  (let ((title-ptr (and title (ffi:foreign-string title)))
        (opened nil))
    (unwind-protect
        (let ((tag-pairs (list +wa-left+ left
                               +wa-top+ top
                               +wa-width+ width
                               +wa-height+ height
                               +wa-idcmp+ idcmp
                               +wa-flags+ flags)))
          (when title-ptr
            (setf tag-pairs (append tag-pairs (list +wa-title+ title-ptr))))
          (when screen
            (setf tag-pairs (append tag-pairs
                                    (list +wa-customscreen+ screen))))
          (let* ((tags (amiga.ffi:make-tag-list tag-pairs))
                 (result (amiga:call-library *intuition-base*
                                             +lvo-open-window-tag-list+
                                             (list :a0 (ffi:make-foreign-pointer 0)
                                                   :a1 tags))))
            (ffi:free-foreign tags)
            (when (zerop result)
              (error "INTUITION:OPEN-WINDOW failed"))
            (setf opened t)
            (when title-ptr
              (setf (gethash result *title-strings*) title-ptr))
            (ffi:make-foreign-pointer result)))
      (when (and title-ptr (not opened))
        (ffi:free-foreign title-ptr)))))

(defun %free-title-string (object)
  "Free the title string held for the window/screen OBJECT, if any."
  (let* ((addr (ffi:foreign-pointer-address object))
         (title-ptr (gethash addr *title-strings*)))
    (when title-ptr
      (remhash addr *title-strings*)
      (ffi:free-foreign title-ptr))))

(defun close-window (window)
  "Close an Intuition window."
  (amiga:call-library *intuition-base* +lvo-close-window+
                      (list :a0 window))
  (%free-title-string window)
  t)

(defmacro with-window ((var &rest args) &body body)
  "Open a window, bind to VAR, close on exit."
  `(let ((,var (open-window ,@args)))
     (unwind-protect
       (progn ,@body)
       (close-window ,var))))

;;; ================================================================
;;; Custom screens
;;; ================================================================

(defun open-screen (&key (width 640) (height 256) (depth 2)
                         (title "AmiLisp") mode-id show-title)
  "Open a custom Intuition screen via OpenScreenTagList.
MODE-ID non-NIL requests that display mode (SA_DisplayID) -- obtain one
RTG-safely with AMIGA.GFX:BEST-MODE-ID; NIL leaves the mode to
Intuition's default.  SHOW-TITLE non-NIL shows the screen title bar
\(menus work either way).  Returns a foreign pointer to the Screen
struct, or signals an error."
  (let ((title-ptr (ffi:foreign-string title))
        (opened nil))
    (unwind-protect
        (let ((tag-pairs (list +sa-width+ width
                               +sa-height+ height
                               +sa-depth+ depth
                               +sa-title+ title-ptr
                               +sa-show-title+ (if show-title 1 0))))
          (when mode-id
            (setf tag-pairs (append tag-pairs
                                    (list +sa-display-id+ mode-id))))
          (let* ((tags (amiga.ffi:make-tag-list tag-pairs))
                 (result (amiga:call-library *intuition-base*
                                             +lvo-open-screen-tag-list+
                                             (list :a0 (ffi:make-foreign-pointer 0)
                                                   :a1 tags))))
            (ffi:free-foreign tags)
            (when (zerop result)
              (error "INTUITION:OPEN-SCREEN failed (~Dx~Dx~D~@[, mode #x~X~])"
                     width height depth mode-id))
            (setf opened t)
            (setf (gethash result *title-strings*) title-ptr)
            (ffi:make-foreign-pointer result)))
      (unless opened
        (ffi:free-foreign title-ptr)))))

;;; ----------------------------------------------------------------
;;; How large is the display, really?
;;;
;;; A screen's nominal size is what you ASK for; what the monitor can
;;; actually show is a property of the display mode, and the display
;;; database is the only honest source for it — PAL and NTSC differ by
;;; 56 rows, and an RTG mode is whatever its driver says.  Asking
;;; means a program can open a screen the size of the display it
;;; landed on instead of a size it guessed, which is what keeps a
;;; fixed-layout program from being letterboxed or rescaled.

;;; oScanType values (intuition/screens.h)
(defconstant +oscan-text+     1)  ; guaranteed visible on any monitor
(defconstant +oscan-standard+ 2)  ; the mode's nominal display clip
(defconstant +oscan-max+      3)  ; as much as the mode can show
(defconstant +oscan-video+    4)  ; beyond the visible edges

(defun query-overscan (mode-id &optional (oscan-type +oscan-text+))
  "QueryOverscan: the standard overscan rectangle of display mode
MODE-ID, as (VALUES MIN-X MIN-Y MAX-X MAX-Y).  NIL when the display
database does not know the mode.

OSCAN-TYPE picks the region -- +OSCAN-TEXT+ (the default: the area
every monitor of that mode is guaranteed to show), +OSCAN-STANDARD+,
+OSCAN-MAX+ or +OSCAN-VIDEO+.  Get MODE-ID RTG-safely from
AMIGA.GFX:BEST-MODE-ID.  See DISPLAY-MODE-HEIGHT for the common case."
  (let ((rect (ffi:alloc-foreign 8)))   ; struct Rectangle: four WORDs
    (unwind-protect
         ;; QueryOverscan(displayID,rect,oScanType)(a0/a1,d0) — the
         ;; display ID travels in an ADDRESS register even though it is
         ;; a number, the rectangle in A1, the region in D0.  Getting
         ;; this wrong does not crash: the call just reports failure for
         ;; every mode, so DISPLAY-MODE-HEIGHT answers NIL always and a
         ;; caller sizing a screen from it silently keeps its fallback.
         (let ((ok (amiga:call-library *intuition-base* +lvo-query-overscan+
                                       (list :a0 mode-id
                                             :a1 rect
                                             :d0 oscan-type))))
           (unless (zerop ok)
             (values (ffi:peek-i16 rect 0) (ffi:peek-i16 rect 2)
                     (ffi:peek-i16 rect 4) (ffi:peek-i16 rect 6))))
      (ffi:free-foreign rect))))

(defun display-mode-height (mode-id &optional (oscan-type +oscan-text+))
  "Rows display mode MODE-ID can show -- 256 on a PAL chipset mode, 200
on NTSC, whatever the driver reports on RTG.  NIL when the display
database does not know the mode.  A thin reading of QUERY-OVERSCAN:
the rectangle is inclusive at both ends, so the height counts both."
  (multiple-value-bind (min-x min-y max-x max-y)
      (query-overscan mode-id oscan-type)
    (declare (ignore min-x max-x))
    (when min-y (1+ (- max-y min-y)))))

(defun close-screen (screen)
  "Close a custom Intuition screen (all its windows must be closed)."
  (amiga:call-library *intuition-base* +lvo-close-screen+
                      (list :a0 screen))
  (%free-title-string screen)
  t)

(defun show-title (screen show-it)
  "ShowTitle: SHOW-IT non-NIL puts the screen's title bar in front of
backdrop windows, NIL behind them (a full-screen backdrop window then
covers it completely -- how a game hides the OS bar)."
  (amiga:call-library *intuition-base* +lvo-show-title+
                      (list :a0 screen :d0 (if show-it 1 0)))
  t)

(defun set-pointer (window sprite-data height width x-offset y-offset)
  "SetPointer: show a custom mouse pointer on WINDOW while it is
active.  SPRITE-DATA is chip RAM in hardware sprite layout: two
position-control words (0), then two data words per line (low plane
first -- pixel value 1/2/3 shows screen color 17/18/19), then two
trailing words (0).  HEIGHT is the line count, WIDTH at most 16;
X-OFFSET/Y-OFFSET place the hot spot (generally negative).  The
pointer stays until CLEAR-POINTER; SPRITE-DATA must outlive it."
  (amiga:call-library *intuition-base* +lvo-set-pointer+
                      (list :a0 window :a1 sprite-data
                            :d0 height :d1 width
                            :d2 x-offset :d3 y-offset))
  t)

(defun clear-pointer (window)
  "ClearPointer: restore WINDOW's default mouse pointer."
  (amiga:call-library *intuition-base* +lvo-clear-pointer+
                      (list :a0 window))
  t)

(defun make-pointer-sprite (rows)
  "Build the chip-RAM sprite data SET-POINTER wants from ROWS, a list
of (WORD-A WORD-B) pairs -- one pair per pointer line, WORD-A the low
bitplane and WORD-B the high (pixel value 1/2/3 shows screen color
17/18/19).  Frames them with the two position-control words and the
two trailing terminator words the hardware sprite format requires.
Returns (VALUES CHIP HEIGHT), ready for
\(SET-POINTER WINDOW CHIP HEIGHT 16 X-OFFSET Y-OFFSET); the data must
outlive the pointer, and the caller frees it with AMIGA:FREE-CHIP
after the pointer moves off it."
  (let ((chip (amiga:alloc-chip (* 2 (+ 2 (* 2 (length rows)) 2))))
        (off 0))
    (flet ((word (w) (ffi:poke-u16 chip w off) (incf off 2)))
      (word 0) (word 0)                 ; position control
      (dolist (row rows)
        (word (first row))
        (word (second row)))
      (word 0) (word 0))                ; sprite terminator
    (values chip (length rows))))

(defmacro with-screen ((var &rest args) &body body)
  "Open a custom screen, bind to VAR, close on exit."
  `(let ((,var (open-screen ,@args)))
     (unwind-protect
       (progn ,@body)
       (close-screen ,var))))

;;; ================================================================
;;; Window accessors
;;; ================================================================

(defun window-rastport (window)
  "Get the RastPort pointer from a Window struct."
  (ffi:make-foreign-pointer (window-rport window)))

(defun window-left (window)   (window-left-edge window))
(defun window-top (window)    (window-top-edge window))

;;; window-width and window-height are generated by defcstruct

;;; ================================================================
;;; IDCMP message handling
;;; ================================================================

(defun wait-port (port)
  "Wait for a message on a message port (blocking).
Uses exec.library WaitPort."
  ;; Need ExecBase — read from absolute address 4
  (let ((exec (ffi:make-foreign-pointer
               (ffi:peek-u32 (ffi:make-foreign-pointer 4)))))
    (amiga:call-library exec +lvo-wait-port+
                        (list :a0 port))))

(defun get-msg (port)
  "Get next message from a port, or NIL if none available."
  (let* ((exec (ffi:make-foreign-pointer
                (ffi:peek-u32 (ffi:make-foreign-pointer 4))))
         (result (amiga:call-library exec +lvo-get-msg+
                                     (list :a0 port))))
    (if (zerop result) nil (ffi:make-foreign-pointer result))))

(defun reply-msg (msg)
  "Reply to a received message."
  (let ((exec (ffi:make-foreign-pointer
               (ffi:peek-u32 (ffi:make-foreign-pointer 4)))))
    (amiga:call-library exec +lvo-reply-msg+
                        (list :a1 msg))))

(defun dos-delay (ticks)
  "Sleep for TICKS ticks (1/50s each on PAL) via dos.library Delay."
  (with-library (dos "dos.library")
    (amiga:call-library dos +lvo-dos-delay+ (list :d1 ticks))))

(defvar *event-loop-timeout* nil
  "Seconds after which a GUI program's own event loop should return
instead of waiting for the user; NIL = interactive (run until the
window is closed).  Set it before loading a program to run it
unattended -- the examples under examples/amiga/gfx/ honour it, and the
examples' harness and the test suite set it:
     --eval '(require \"amiga/intuition\")'
     --eval '(setf amiga.intuition:*event-loop-timeout* 5)' --load ...
EVENT-LOOP itself is bounded by *EVENT-LOOP-MAX-WAITS*; see also
AMIGA.REACTION:*EVENT-LOOP-TIMEOUT*, the same knob for ReAction
programs.")

(defvar *event-loop-max-waits* nil
  "If non-NIL, bound EVENT-LOOP's wait for the next IDCMP message to
this many polls of GET-MSG (paced one tick apart via dos.library
Delay) instead of blocking on WAIT-PORT/WaitPort forever. Exhausting
the bound signals an error. Intended for unattended test runs, where
a message that never arrives (e.g. a stolen focus in FS-UAE) should
fail that one test cleanly instead of hanging the whole suite until
an external watchdog kills the emulator.")

(defun wait-for-msg (port)
  "Return the next message on PORT, removing it (like GET-MSG would).
Without *EVENT-LOOP-MAX-WAITS* bound, blocks via WAIT-PORT (may wait
forever). With it bound, polls GET-MSG directly instead, pacing one
tick between attempts, and signals an error once the bound is
exhausted with no message received."
  (if (null *event-loop-max-waits*)
      (progn (wait-port port) (get-msg port))
      (dotimes (i *event-loop-max-waits*
                  (error "EVENT-LOOP: no IDCMP message after ~D polls (bounded wait)"
                         *event-loop-max-waits*))
        (let ((msg (get-msg port)))
          (when msg (return msg)))
        (dos-delay 1))))

(defun msg-class (msg)
  "Get the IDCMP class from an IntuiMessage."
  (intui-message-class msg))

(defun msg-code (msg)
  "Get the Code field from an IntuiMessage."
  (intui-message-code msg))

(defun msg-qualifier (msg)
  "Get the Qualifier field from an IntuiMessage (IEQUALIFIER-* bits)."
  (intui-message-qualifier msg))

(defun msg-raw-key (msg)
  "The raw (position) keycode behind a keyboard IntuiMessage, or NIL
when it cannot be told.  With IDCMP_VANILLAKEY the keymap has already
been applied to Code under the qualifiers of the MOMENT -- so a shift
wedged down on the host (the emulator classic: a screenshot chord
steals the key-up) turns the whole digit row into punctuation.  V36+
Intuition hangs the originating struct InputEvent off IAddress; its
ie_Code is the raw key, position-coded and qualifier-blind, valid
until the message is replied (EVENT-LOOP bodies run before the
reply).  IAddress is overloaded per IDCMP class (e.g. it holds a
Gadget* for GADGETUP/DOWN) and is only meaningful for keyboard
messages, so this first checks msg-class is IDCMP_VANILLAKEY or
IDCMP_RAWKEY before trusting it at all.  Guarded further: only a
non-null IAddress whose event reads IECLASS_RAWKEY with a key-down
code is trusted -- anything else answers NIL and the caller falls
back to Code."
  (when (member (msg-class msg) (list +idcmp-vanillakey+ +idcmp-rawkey+))
    (let ((addr (intui-message-iaddress msg)))
      (when (and (integerp addr) (/= addr 0))
        (let ((ev (ffi:make-foreign-pointer addr)))
          (when (= (input-event-ie-class ev) +ieclass-rawkey+)
            (let ((code (input-event-ie-code ev)))
              (when (< code #x80) code))))))))

(defun msg-mouse-x (msg)
  "Get MouseX from an IntuiMessage."
  (intui-message-mouse-x msg))

(defun msg-mouse-y (msg)
  "Get MouseY from an IntuiMessage."
  (intui-message-mouse-y msg))

;;; ================================================================
;;; Event loop
;;; ================================================================

(defun window-user-port (window)
  "Get the UserPort (IDCMP message port) from a Window."
  (ffi:make-foreign-pointer (window-uport window)))

(defmacro event-loop (window &body clauses)
  "Process IDCMP messages for WINDOW until a clause calls (RETURN).
Each clause is (idcmp-class (msg) &body body); the body runs BEFORE the
message is replied, so it may read the message's fields, and (RETURN)
inside a body exits the whole event loop (the message is still replied
on the way out). Waiting for the next message blocks indefinitely
unless *EVENT-LOOP-MAX-WAITS* is bound (see its docstring), in which
case a message that never arrives signals an error instead of hanging.
Example:
  (event-loop win
    (#.+idcmp-closewindow+ (msg) (return))
    (#.+idcmp-rawkey+ (msg)
      (format t \"Key: ~A~%\" (msg-code msg))))"
  (let ((port (gensym "PORT"))
        (msg (gensym "MSG"))
        (first-msg (gensym "FIRST-MSG"))
        (class (gensym "CLASS"))
        (dispatch (gensym "DISPATCH")))
    ;; The clause bodies live in the DISPATCH flet, defined directly
    ;; inside the (BLOCK NIL ...): there a plain (RETURN) exits the
    ;; event loop.  Splicing the bodies into the LOOPs below would break
    ;; that — LOOP establishes its own implicit BLOCK NIL, so (RETURN)
    ;; would silently just drain-continue (a real field bug: quit keys
    ;; and the close gadget did nothing).
    `(let ((,port (window-user-port ,window)))
       (block nil
         (flet ((,dispatch (,class ,msg)
                  (cond
                    ,@(mapcar
                       (lambda (clause)
                         (destructuring-bind (idcmp-val (msg-var) &body body)
                             clause
                           `((eql ,class ,idcmp-val)
                             (let ((,msg-var ,msg))
                               (declare (ignorable ,msg-var))
                               ,@body))))
                       clauses))))
           (loop
             ;; WAIT-FOR-MSG blocks via WAIT-PORT unless
             ;; *EVENT-LOOP-MAX-WAITS* bounds it (see its docstring).
             (let ((,first-msg (wait-for-msg ,port)))
               (unwind-protect
                   (,dispatch (msg-class ,first-msg) ,first-msg)
                 (reply-msg ,first-msg)))
             (loop
               (let ((,msg (get-msg ,port)))
                 (unless ,msg (return))  ; inner loop: no more messages
                 ;; Reply AFTER the body (it reads message fields), and
                 ;; also when the body exits the loop via (RETURN).
                 (unwind-protect
                     (,dispatch (msg-class ,msg) ,msg)
                   (reply-msg ,msg))))))))))

;;; ================================================================
;;; Menu strip and gadget list management
;;; ================================================================

(defun set-menu-strip (window menu)
  "Attach a menu strip to a window."
  (amiga:call-library *intuition-base* +lvo-set-menu-strip+
                      (list :a0 window :a1 menu)))

(defun clear-menu-strip (window)
  "Remove the menu strip from a window."
  (amiga:call-library *intuition-base* +lvo-clear-menu-strip+
                      (list :a0 window)))

(defun add-gadget-list (window gadget-list)
  "Add a gadget list to a window. Returns position."
  (amiga:call-library *intuition-base* +lvo-add-g-list+
                      (list :a0 window :a1 gadget-list
                            :d0 -1  ; position = end
                            :d1 -1  ; numgad = all
                            :a2 (ffi:make-foreign-pointer 0))))  ; requester = NULL

(defun refresh-gadget-list (window gadget-list)
  "Refresh all gadgets in the list."
  (amiga:call-library *intuition-base* +lvo-refresh-g-list+
                      (list :a0 gadget-list :a1 window
                            :a2 (ffi:make-foreign-pointer 0)  ; requester = NULL
                            :d0 -1)))  ; numgad = all

;;; ================================================================
;;; Public screen management
;;; ================================================================

(defun lock-pub-screen (&optional name)
  "Lock a public screen by name (NIL = default/Workbench).
Returns a foreign pointer to the Screen, or NIL."
  (let ((result (amiga:call-library *intuition-base* +lvo-lock-pub-screen+
                                    (list :a0 (if name
                                                  (ffi:foreign-string name)
                                                  (ffi:make-foreign-pointer 0))))))
    (if (zerop result) nil (ffi:make-foreign-pointer result))))

(defun unlock-pub-screen (screen &optional name)
  "Unlock a previously locked public screen."
  (amiga:call-library *intuition-base* +lvo-unlock-pub-screen+
                      (list :a0 (if name
                                    (ffi:foreign-string name)
                                    (ffi:make-foreign-pointer 0))
                            :a1 screen)))

(defmacro with-pub-screen ((var &optional name) &body body)
  "Lock a public screen, bind to VAR, unlock on exit."
  `(let ((,var (lock-pub-screen ,name)))
     (unless ,var
       (error "Cannot lock public screen ~A" ,(or name "default")))
     (unwind-protect
       (progn ,@body)
       (unlock-pub-screen ,var ,name))))

;;; ================================================================
;;; Provide module
;;; ================================================================

(provide "amiga/intuition")
