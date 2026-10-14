;;; doublebuffer.lisp — V39 double-buffering, attached screens, menu lending.
;;;
;;; Common Lisp port of the NDK 3.1 intuition/doublebuffer.c example
;;; (Examples1/intuition/doublebuffer.c on the Amiga Developer CD): a
;;; face bounces around a HIRES custom screen at the frame rate, drawn
;;; alternately into two ScreenBuffers that ChangeScreenBuffer swaps in
;;; at the vertical blank; a second, attached screen (SA_Parent) below
;;; the canvas holds two GadTools sliders for the speed, and LendMenus
;;; makes the canvas window's menu strip pop up over the control screen
;;; too.
;;;
;;; What it shows: AllocScreenBuffer (SB_SCREEN_BITMAP / SB_COPY_BITMAP),
;;; the dbi_SafeMessage protocol — a buffer may be redrawn only after
;;; the message for its predecessor came back on our port —,
;;; ChangeScreenBuffer's "held off" result and the WaitTOF retry, two
;;; windows sharing one IDCMP UserPort (and the CloseWindowSafely idiom
;;; that needs), an attached screen, LendMenus, GadTools sliders and
;;; menus driven from the raw and curated bindings, and an offscreen
;;; BitMap painted with the area-fill primitives (AreaEllipse over a
;;; TmpRas).
;;;
;;; Keys and menu (right mouse button on either screen): R runs, S steps
;;; one frame, Q quits; 1/2 and 3/4 slow down / speed up the horizontal
;;; and vertical motion, as do the sliders.  Unlike the C, which starts
;;; standing still, the Lisp version starts running.
;;;
;;; Run on AmigaOS 3.1+ (V39 graphics / intuition):
;;;   amilisp --load examples/amiga/gfx/doublebuffer.lisp
;;; Unattended (the examples' harness and the test suite do this): set
;;; amiga.intuition:*event-loop-timeout* to a number of seconds first.
;;; RUN returns a plist of what happened (:frames :held-off :seconds).

(require "amiga/raw/exec")
(require "amiga/raw/dos")
(require "amiga/raw/graphics")
(require "amiga/raw/intuition")
(require "amiga/raw/gadtools")
(require "amiga/intuition")      ; *EVENT-LOOP-TIMEOUT*
(require "amiga/gadtools")       ; NewMenu arrays, VisualInfo, GT_ messages

(defpackage "DOUBLEBUFFER"
  (:use "CL")
  (:local-nicknames ("EXEC"  "AMIGA.RAW.EXEC")
                    ("DOS"   "AMIGA.RAW.DOS")
                    ("GFX"   "AMIGA.RAW.GRAPHICS")
                    ("INTUI" "AMIGA.RAW.INTUITION")
                    ("RAWGT" "AMIGA.RAW.GADTOOLS")
                    ("GT"    "AMIGA.GADTOOLS")))

(in-package "DOUBLEBUFFER")

;;; ---------------------------------------------------------------- constants

;; The animated face
(defconstant +bm-width+  120)
(defconstant +bm-height+ 60)
(defconstant +bm-depth+  2)
(defconstant +max-vectors+ 10)          ; AreaInfo capacity

;; Odd numbers to give a non-repeating bounce
(defconstant +controlsc-top+ 191)

(defconstant +gad-horiz+ 1)
(defconstant +gad-vert+  2)

(defconstant +menu-run+   1)
(defconstant +menu-step+  2)
(defconstant +menu-quit+  3)
(defconstant +menu-hslow+ 4)
(defconstant +menu-hfast+ 5)
(defconstant +menu-vslow+ 6)
(defconstant +menu-vfast+ 7)

(defconstant +ok-redraw+ 1)             ; buffer fully detached, ready for redraw
(defconstant +ok-swapin+ 2)             ; buffer redrawn, ready for swap-in

(defconstant +run-forever+ most-positive-fixnum)   ; the C's count = ~0

;;; ---------------------------------------------------------------- helpers

(defun ptr (value)
  "Struct accessors hand back NIL, an integer or a foreign pointer;
normalise to a foreign pointer or NIL."
  (cond ((null value) nil)
        ((integerp value) (if (zerop value) nil (ffi:make-foreign-pointer value)))
        ((ffi:null-pointer-p value) nil)
        (t value)))

(defun addr (pointer)
  "The address of POINTER, 0 for NIL."
  (if pointer (ffi:foreign-pointer-address pointer) 0))

(defun calloc (size)
  "ALLOC-FOREIGN plus the MEMF_CLEAR the OS structures below expect."
  (let ((p (ffi:alloc-foreign size)))
    (dotimes (i size) (ffi:poke-u8 p 0 i))
    p))

(defun sigmask (port)
  "1 << mp_SigBit."
  (ash 1 (exec:msg-port-sigbit port)))

(defun rassize (width height)
  "graphics/gfx.h RASSIZE: bytes of one bitplane of WIDTH x HEIGHT."
  (* height (logand (ash (+ width 15) -3) #xFFFE)))

(defmacro with-tags ((var &rest pairs) &body body)
  "A TagItem array built from PAIRS for the dynamic extent of BODY.
Values must already be integers or foreign pointers (strings that the
OS keeps a pointer to are allocated by hand and kept in the DEMO)."
  `(let ((,var (amiga.ffi:make-tag-list (list ,@pairs))))
     (unwind-protect (progn ,@body)
       (ffi:free-foreign ,var))))

;;; ---------------------------------------------------------------- state

;; Everything the C keeps in globals: OS objects (foreign pointers, NIL
;; while not yet / no longer allocated), the foreign memory the OS holds
;; pointers into for as long as the objects live, and the animation.
(defstruct demo
  ;; message ports
  dbufport userport
  ;; screens, windows, VisualInfo, menus, gadgets
  canvas control canvaswin controlwin canvasvi controlvi
  menu newmenu newmenu-strings
  glist-cell horizgad vertgad
  ;; the two ScreenBuffers, their RastPorts and their state
  (scbuf   (make-array 2 :initial-element nil))
  (rport   (make-array 2 :initial-element nil))
  (status  (make-array 2 :initial-element 0))
  (prevx   (make-array 2 :initial-element 50))
  (prevy   (make-array 2 :initial-element 50))
  face
  ;; foreign memory referenced by the OS objects above
  textattr fontname vctags pens canvas-title level-format
  horiz-label vert-label
  ;; animation
  (count +run-forever+) (buf-current 0) (buf-nextdraw 1) (buf-nextswap 1)
  (x 50) (y 70) (xstep 1) (xdir 1) (ystep 1) (ydir -1)
  ;; statistics for RUN's result
  (frames 0) (held-off 0))

;;; ---------------------------------------------------------------- init

(defun make-text-attr (d)
  "struct TextAttr Topaz80 = { \"topaz.font\", 8, FS_NORMAL, FPF_ROMFONT|FPF_DESIGNED }"
  (let ((ta (calloc gfx:*text-attr-size*)))
    (setf (demo-fontname d) (ffi:foreign-string "topaz.font")
          (demo-textattr d) ta)
    (setf (gfx:text-attr-name ta)   (demo-fontname d)
          (gfx:text-attr-y-size ta) 8
          (gfx:text-attr-style ta)  gfx:+fs-normal+
          (gfx:text-attr-flags ta)  (logior gfx:+fpf-romfont+ gfx:+fpf-designed+))
    ta))

(defun make-pens (d)
  "The C's UWORD pens[]: DrawInfo pen numbers for both screens, ~0-terminated."
  (let ((pens (calloc 26))
        (values '(0 1 1 2 1 3 1 0 2 1 2 1 #xFFFF)))
    (loop for v in values for i from 0
          do (ffi:poke-u16 pens v (* i 2)))
    (setf (demo-pens d) pens)))

(defun open-screens (d)
  (let ((ta (demo-textattr d))
        (pens (demo-pens d))
        (vctags (setf (demo-vctags d)
                      (amiga.ffi:make-tag-list (list gfx:+vtag-bordersprite-set+ 1)))))
    (setf (demo-canvas-title d)
          (ffi:foreign-string "Intuition double-buffering example"))
    (with-tags (tags intui:+sa-display-id+    gfx:+hires-key+
                     intui:+sa-overscan+      intui:+oscan-text+
                     intui:+sa-depth+         2
                     intui:+sa-auto-scroll+   1
                     intui:+sa-pens+          pens
                     intui:+sa-show-title+    1
                     intui:+sa-title+         (demo-canvas-title d)
                     intui:+sa-video-control+ vctags
                     intui:+sa-font+          ta)
      (setf (demo-canvas d) (intui:open-screen-tag-list 0 tags)))
    (unless (demo-canvas d)
      (error "Couldn't open the canvas screen (HIRES_KEY, OSCAN_TEXT, 2 planes)"))
    (setf (demo-canvasvi d) (gt:get-visual-info (demo-canvas d)))
    ;; The control screen is attached to the canvas (SA_Parent): it moves
    ;; with it and sits at CONTROLSC_TOP, 28 lines high, quiet and not
    ;; draggable.
    (with-tags (tags intui:+sa-display-id+    gfx:+hires-key+
                     intui:+sa-overscan+      intui:+oscan-text+
                     intui:+sa-depth+         2
                     intui:+sa-pens+          pens
                     intui:+sa-top+           +controlsc-top+
                     intui:+sa-height+        28
                     intui:+sa-parent+        (demo-canvas d)
                     intui:+sa-show-title+    0
                     intui:+sa-draggable+     0
                     intui:+sa-video-control+ vctags
                     intui:+sa-quiet+         1
                     intui:+sa-font+          ta)
      (setf (demo-control d) (intui:open-screen-tag-list 0 tags)))
    (unless (demo-control d)
      (error "Couldn't open the control screen (attached, SA_Parent)"))
    (setf (demo-controlvi d) (gt:get-visual-info (demo-control d)))))

(defun open-canvas-window (d)
  "A borderless backdrop window over the whole canvas, for input only.
Opened without WA_IDCMP so Intuition creates no port of its own; the
shared USERPORT is installed afterwards and ModifyIDCMP starts the
message flow to it (the C's canvaswin->UserPort = userport trick)."
  (with-tags (tags intui:+wa-no-care-refresh+ 1
                   intui:+wa-activate+        1
                   intui:+wa-borderless+      1
                   intui:+wa-backdrop+        1
                   intui:+wa-custom-screen+   (demo-canvas d)
                   intui:+wa-new-look-menus+  1)
    (setf (demo-canvaswin d) (intui:open-window-tag-list 0 tags)))
  (unless (demo-canvaswin d)
    (error "Couldn't open the canvas window"))
  (setf (intui:window-user-port (demo-canvaswin d)) (demo-userport d))
  (intui:modify-idcmp (demo-canvaswin d)
                      (logior intui:+idcmp-menupick+ intui:+idcmp-vanillakey+)))

(defun create-menus (d)
  "struct NewMenu demomenu[] -> CreateMenus + LayoutMenus on the canvas's
VisualInfo.  GadTools keeps pointers to the label strings for the life
of the menu strip, so they stay in the DEMO until FreeMenus."
  (multiple-value-bind (newmenu strings)
      (gt:make-new-menu-array
       `((,gt:+nm-title+ "Project")
         (,gt:+nm-item+ "Run"               :commkey "R" :userdata ,+menu-run+)
         (,gt:+nm-item+ "Step"              :commkey "S" :userdata ,+menu-step+)
         :bar
         (,gt:+nm-item+ "Slower Horizontal" :commkey "1" :userdata ,+menu-hslow+)
         (,gt:+nm-item+ "Faster Horizontal" :commkey "2" :userdata ,+menu-hfast+)
         (,gt:+nm-item+ "Slower Vertical"   :commkey "3" :userdata ,+menu-vslow+)
         (,gt:+nm-item+ "Faster Vertical"   :commkey "4" :userdata ,+menu-vfast+)
         :bar
         (,gt:+nm-item+ "Quit"              :commkey "Q" :userdata ,+menu-quit+)))
    (setf (demo-newmenu d) newmenu
          (demo-newmenu-strings d) strings))
  (setf (demo-menu d) (gt:create-menus (demo-newmenu d)))
  (unless (gt:layout-menus (demo-menu d) (demo-canvasvi d)
                           gt:+gtmn-new-look-menus+ 1)
    (error "Couldn't layout menus")))

(defun create-slider (d previous text left id)
  "One SLIDER_KIND gadget, 0..9, through CreateGadgetA with a NewGadget
built by hand.  TEXT is the label as a foreign string: GadTools keeps
the label and GTSL_LevelFormat pointers, so both strings live in the
DEMO until FreeGadgets."
  (let ((ng (calloc rawgt:*new-gadget-size*)))
    (unwind-protect
         (progn
           (setf (rawgt:new-gadget-left-edge ng)   left
                 (rawgt:new-gadget-top-edge ng)    1
                 (rawgt:new-gadget-width ng)       100
                 (rawgt:new-gadget-height ng)      12
                 (rawgt:new-gadget-gadget-text ng) text
                 (rawgt:new-gadget-text-attr ng)   (demo-textattr d)
                 (rawgt:new-gadget-gadget-id ng)   id
                 (rawgt:new-gadget-flags ng)       0
                 (rawgt:new-gadget-visual-info ng) (demo-controlvi d))
           (with-tags (tags rawgt:+gtsl-min+           0
                            rawgt:+gtsl-max+           9
                            rawgt:+gtsl-level+         1
                            rawgt:+gtsl-max-level-len+ 1
                            rawgt:+gtsl-level-format+  (demo-level-format d))
             (rawgt:create-gadget-a rawgt:+slider-kind+ previous ng tags)))
      (ffi:free-foreign ng))))

(defun create-all-gadgets (d)
  "The C's createAllGadgets: a context and two sliders for the control
screen.  The gadget list head lives in a foreign LONG (CreateContext
writes it)."
  (setf (demo-glist-cell d) (calloc 4)
        (demo-level-format d) (ffi:foreign-string "%ld")
        (demo-horiz-label d) (ffi:foreign-string "Horiz:  ")
        (demo-vert-label d) (ffi:foreign-string "Vert:  "))
  (let ((gad (gt:create-context (demo-glist-cell d))))
    (setf gad (create-slider d gad (demo-horiz-label d) 100 +gad-horiz+)
          (demo-horizgad d) gad)
    (unless gad (error "Couldn't create gadgets"))
    (setf gad (create-slider d gad (demo-vert-label d) 300 +gad-vert+)
          (demo-vertgad d) gad)
    (unless gad (error "Couldn't create gadgets"))))

(defun open-control-window (d)
  "A borderless backdrop window on the control screen carrying the
sliders; it shares USERPORT with the canvas window."
  (with-tags (tags intui:+wa-no-care-refresh+ 1
                   intui:+wa-activate+        1
                   intui:+wa-borderless+      1
                   intui:+wa-backdrop+        1
                   intui:+wa-custom-screen+   (demo-control d)
                   intui:+wa-new-look-menus+  1
                   intui:+wa-gadgets+         (ffi:peek-u32 (demo-glist-cell d) 0))
    (setf (demo-controlwin d) (intui:open-window-tag-list 0 tags)))
  (unless (demo-controlwin d)
    (error "Couldn't open the control window"))
  (setf (intui:window-user-port (demo-controlwin d)) (demo-userport d))
  (intui:modify-idcmp (demo-controlwin d)
                      (logior rawgt:+slideridcmp+
                              intui:+idcmp-menupick+ intui:+idcmp-vanillakey+))
  (gt:gt-refresh-window (demo-controlwin d))
  (intui:set-menu-strip (demo-canvaswin d) (demo-menu d))
  ;; The control window shows the canvas window's menus.
  (intui:lend-menus (demo-controlwin d) (demo-canvaswin d)))

(defun alloc-screen-buffers (d)
  "Two ScreenBuffers: the first wraps the screen's own bitmap, the second
is a fresh copy of it.  dbi_UserData1 carries the buffer number so the
SafeMessage can be told apart when it comes back; both start out
ready to draw into."
  (let ((canvas (demo-canvas d)))
    (setf (aref (demo-scbuf d) 0)
          (intui:alloc-screen-buffer canvas 0 intui:+sb-screen-bitmap+))
    (unless (aref (demo-scbuf d) 0) (error "Couldn't allocate ScreenBuffer 1"))
    (setf (aref (demo-scbuf d) 1)
          (intui:alloc-screen-buffer canvas 0 intui:+sb-copy-bitmap+))
    (unless (aref (demo-scbuf d) 1) (error "Couldn't allocate ScreenBuffer 2"))
    (dotimes (i 2)
      (let* ((sb (aref (demo-scbuf d) i))
             (dbi (intui:screen-buffer-d-buf-info sb))
             (rp (calloc gfx:*rastport-size*)))
        (setf (gfx:d-buf-info-user-data1 dbi) i
              (aref (demo-status d) i) +ok-redraw+)
        (gfx:init-rastport rp)
        (setf (gfx:rastport-bitmap rp) (addr (intui:screen-buffer-bitmap sb))
              (aref (demo-rport d) i) rp)))))

(defun make-image-bitmap ()
  "The C's makeImageBM: a crude face, area-filled into a 120x60x2
offscreen BitMap.  AreaEllipse needs an AreaInfo with a vector buffer
and a TmpRas the size of one plane."
  (let ((bm (gfx:alloc-bitmap +bm-width+ +bm-height+ +bm-depth+ gfx:+bmf-clear+ 0)))
    (unless bm (error "Couldn't allocate image bitmap"))
    (let ((plane (gfx:alloc-raster +bm-width+ +bm-height+)))
      (unless plane
        (gfx:free-bitmap bm)
        (error "Couldn't allocate the TmpRas plane"))
      (let ((rp (calloc gfx:*rastport-size*))
            (area (calloc gfx:*area-info-size*))
            (vectors (calloc (* +max-vectors+ 5)))
            (tmpras (calloc gfx:*tmp-ras-size*)))
        (unwind-protect
             (progn
               (gfx:init-rastport rp)
               (setf (gfx:rastport-bitmap rp) (addr bm))
               (gfx:init-area area vectors +max-vectors+)
               (setf (gfx:rastport-area-info rp) (addr area))
               (gfx:init-tmp-ras tmpras plane (rassize +bm-width+ +bm-height+))
               (setf (gfx:rastport-tmp-ras rp) (addr tmpras))
               (gfx:set-ab-pen-dr-md rp 3 0 gfx:+rp-jam1+)
               (gfx:area-ellipse rp (floor +bm-width+ 2) (floor +bm-height+ 2)
                                 (- (floor +bm-width+ 2) 4) (- (floor +bm-height+ 2) 4))
               (gfx:area-end rp)
               (gfx:set-a-pen rp 2)
               (gfx:area-ellipse rp (floor (* 5 +bm-width+) 16) (floor +bm-height+ 4)
                                 (floor +bm-width+ 9) (floor +bm-height+ 9))
               (gfx:area-ellipse rp (floor (* 11 +bm-width+) 16) (floor +bm-height+ 4)
                                 (floor +bm-width+ 9) (floor +bm-height+ 9))
               (gfx:area-end rp)
               (gfx:set-a-pen rp 1)
               (gfx:area-ellipse rp (floor +bm-width+ 2) (floor (* 3 +bm-height+) 4)
                                 (floor +bm-width+ 3) (floor +bm-height+ 9))
               (gfx:area-end rp))
          (gfx:free-raster plane +bm-width+ +bm-height+)
          (ffi:free-foreign tmpras)
          (ffi:free-foreign vectors)
          (ffi:free-foreign area)
          (ffi:free-foreign rp))))
    bm))

(defun init-all (d)
  "The C's init_all, in the same order; every failure signals with the
C's message and CLEANUP undoes whatever was set up."
  (unless (and gfx:*graphics-base* (>= gfx:*graphics-version* 39))
    (error "Couldn't open Gfx V39"))
  (unless (and intui:*intuition-base* (>= intui:*intuition-version* 39))
    (error "Couldn't open Intuition V39"))
  (setf (demo-dbufport d) (exec:create-msg-port))
  (unless (demo-dbufport d) (error "Failed to create port"))
  (setf (demo-userport d) (exec:create-msg-port))
  (unless (demo-userport d) (error "Failed to create port"))
  (make-text-attr d)
  (make-pens d)
  (open-screens d)
  (open-canvas-window d)
  (create-menus d)
  (create-all-gadgets d)
  (open-control-window d)
  (alloc-screen-buffers d)
  (setf (demo-face d) (make-image-bitmap)))

;;; ---------------------------------------------------------------- teardown

(defun strip-intui-messages (port window)
  "Remove and reply every IntuiMessage on PORT that was sent to WINDOW
\(the C's StripIntuiMessages).  Called under Forbid; ln_Succ is read
before the node is replied."
  (when port
    (let ((msg (ptr (exec:list-head (exec:msg-port-msglist port)))))
      (loop
        (let ((succ (ptr (exec:node-succ msg))))
          (unless succ (return))
          (let ((idcmp-window (ptr (intui:intui-message-idcmp-window msg))))
            (when (and idcmp-window
                       (= (addr idcmp-window) (addr window)))
              ;; Intuition is about to free this message: send it back first.
              (exec:remove msg)
              (exec:reply-msg msg)))
          (setf msg succ))))))

(defun close-window-safely (window)
  "Close a window that shares its UserPort with another window: reply
the messages still queued for it, detach the port so Intuition does not
free it, stop the message flow, then close (the C's CloseWindowSafely)."
  (exec:forbid)
  (strip-intui-messages (ptr (intui:window-user-port window)) window)
  (setf (intui:window-user-port window) 0)
  (intui:modify-idcmp window 0)
  (exec:permit)
  (intui:close-window window))

(defun cleanup (d)
  "The C's error_exit, minus the exit: release everything in reverse
order of INIT-ALL, tolerating whatever was never set up."
  (macrolet ((free (accessor form)
               `(when (,accessor d)
                  ,form
                  (setf (,accessor d) nil))))
    (free demo-controlwin (progn (intui:clear-menu-strip (demo-controlwin d))
                                 (close-window-safely (demo-controlwin d))))
    (free demo-canvaswin (progn (intui:clear-menu-strip (demo-canvaswin d))
                                (close-window-safely (demo-canvaswin d))))
    (free demo-control (intui:close-screen (demo-control d)))
    (when (demo-canvas d)
      (dotimes (i 2)
        (let ((sb (aref (demo-scbuf d) (- 1 i))))
          (when sb
            (intui:free-screen-buffer (demo-canvas d) sb)
            (setf (aref (demo-scbuf d) (- 1 i)) nil))))
      (intui:close-screen (demo-canvas d))
      (setf (demo-canvas d) nil))
    (free demo-dbufport (exec:delete-msg-port (demo-dbufport d)))
    (free demo-userport (exec:delete-msg-port (demo-userport d)))
    (free demo-glist-cell (let ((glist (ptr (ffi:peek-u32 (demo-glist-cell d) 0))))
                            (when glist (gt:free-gadgets glist))
                            (ffi:free-foreign (demo-glist-cell d))))
    (free demo-menu (gt:free-menus (demo-menu d)))
    (free demo-newmenu (ffi:free-foreign (demo-newmenu d)))
    (free demo-newmenu-strings (mapc #'ffi:free-foreign (demo-newmenu-strings d)))
    (free demo-canvasvi (gt:free-visual-info (demo-canvasvi d)))
    (free demo-controlvi (gt:free-visual-info (demo-controlvi d)))
    (free demo-face (gfx:free-bitmap (demo-face d)))
    (dotimes (i 2)
      (when (aref (demo-rport d) i)
        (ffi:free-foreign (aref (demo-rport d) i))
        (setf (aref (demo-rport d) i) nil)))
    ;; Foreign memory the OS objects above pointed into — free it last.
    (free demo-horiz-label (ffi:free-foreign (demo-horiz-label d)))
    (free demo-vert-label (ffi:free-foreign (demo-vert-label d)))
    (free demo-level-format (ffi:free-foreign (demo-level-format d)))
    (free demo-canvas-title (ffi:free-foreign (demo-canvas-title d)))
    (free demo-vctags (ffi:free-foreign (demo-vctags d)))
    (free demo-pens (ffi:free-foreign (demo-pens d)))
    (free demo-textattr (ffi:free-foreign (demo-textattr d)))
    (free demo-fontname (ffi:free-foreign (demo-fontname d)))))

;;; ---------------------------------------------------------------- frames

(defun handle-buffer-swap (d)
  "Render into BUF-NEXTDRAW when its SafeMessage has arrived
\(status OK_REDRAW), then swap BUF-NEXTSWAP in when it is rendered
\(status OK_SWAPIN).  Returns T when ChangeScreenBuffer held us off —
the caller then retries after WaitTOF instead of waiting for a signal."
  (let ((held-off nil)
        (nextdraw (demo-buf-nextdraw d)))
    (when (= (aref (demo-status d) nextdraw) +ok-redraw+)
      (let* ((canvas (demo-canvas d))
             (width (intui:screen-width canvas))
             (bar-layer (ptr (intui:screen-bar-layer canvas)))
             (bar-height (if bar-layer (gfx:layer-height bar-layer) 0))
             (x (+ (demo-x d) (* (demo-xstep d) (demo-xdir d))))
             (y (+ (demo-y d) (* (demo-ystep d) (demo-ydir d))))
             (rp (aref (demo-rport d) nextdraw)))
        (cond ((< x 0) (setf x 0 (demo-xdir d) 1))
              ((> x (- width +bm-width+))
               (setf x (- width +bm-width+ 1) (demo-xdir d) -1)))
        (cond ((< y bar-height) (setf y bar-height (demo-ydir d) 1))
              ((>= y (- +controlsc-top+ +bm-height+))
               (setf y (- +controlsc-top+ +bm-height+ 1) (demo-ydir d) -1)))
        (setf (demo-x d) x (demo-y d) y)
        ;; Erase where the face was the last time THIS buffer was shown,
        ;; draw it at the new place.
        (gfx:set-a-pen rp 0)
        (gfx:rect-fill rp (aref (demo-prevx d) nextdraw) (aref (demo-prevy d) nextdraw)
                       (+ (aref (demo-prevx d) nextdraw) +bm-width+ -1)
                       (+ (aref (demo-prevy d) nextdraw) +bm-height+ -1))
        (setf (aref (demo-prevx d) nextdraw) x
              (aref (demo-prevy d) nextdraw) y)
        (gfx:blt-bitmap-rastport (demo-face d) 0 0 rp x y +bm-width+ +bm-height+ #xC0)
        (gfx:wait-blit)                 ; gots to let the BBMRP finish
        (setf (aref (demo-status d) nextdraw) +ok-swapin+
              (demo-buf-nextdraw d) (logxor nextdraw 1))))
    ;; Swap only a fully rendered buffer in.
    (let ((nextswap (demo-buf-nextswap d)))
      (when (= (aref (demo-status d) nextswap) +ok-swapin+)
        (let* ((sb (aref (demo-scbuf d) nextswap))
               (dbi (intui:screen-buffer-d-buf-info sb)))
          ;; The SafeMessage comes back to us on DBUFPORT.
          (setf (exec:message-replyport (gfx:d-buf-info-safe-message dbi))
                (demo-dbufport d))
          (if (/= 0 (intui:change-screen-buffer (demo-canvas d) sb))
              (setf (aref (demo-status d) nextswap) 0
                    (demo-buf-current d) nextswap
                    (demo-buf-nextswap d) (logxor nextswap 1)
                    (demo-count d) (1- (demo-count d))
                    (demo-frames d) (1+ (demo-frames d)))
              (setf held-off t
                    (demo-held-off d) (1+ (demo-held-off d)))))))
    held-off))

(defun handle-dbuf-message (d msg)
  "A dbi_SafeMessage came back: the buffer BEFORE the one it names may be
redrawn.  The buffer number is dbi_UserData1, which follows the
20-byte Message in the DBufInfo (the C's *(APTR *)(dbmsg + 1))."
  (let ((buffer (ffi:peek-u32 msg 20)))
    (setf (aref (demo-status d) (logxor buffer 1)) +ok-redraw+)))

;;; ---------------------------------------------------------------- input

(defun set-xstep (d value)
  "Clamp VALUE to the slider's 0..9, store it and show it on the slider."
  (let ((v (max 0 (min 9 value))))
    (setf (demo-xstep d) v)
    (gt:set-gadget-attrs (demo-horizgad d) (demo-controlwin d) rawgt:+gtsl-level+ v)))

(defun set-ystep (d value)
  (let ((v (max 0 (min 9 value))))
    (setf (demo-ystep d) v)
    (gt:set-gadget-attrs (demo-vertgad d) (demo-controlwin d) rawgt:+gtsl-level+ v)))

(defun handle-menu-pick (d code)
  "Walk the MENUPICK chain; the user data GadTools stored behind each
MenuItem (GTMENUITEM_USERDATA: the LONG after the 34-byte struct) is
our MENU_* number."
  (let ((terminated nil))
    (loop until (= code intui:+menunull+)
          do (let ((item (intui:item-address (demo-menu d) code)))
               (unless item (return))
               (let ((userdata (ffi:peek-u32 item intui:*menu-item-size*)))
                 (cond ((= userdata +menu-run+)   (setf (demo-count d) +run-forever+))
                       ((= userdata +menu-step+)  (setf (demo-count d) 1))
                       ((= userdata +menu-quit+)  (setf (demo-count d) 0 terminated t))
                       ((= userdata +menu-hslow+) (set-xstep d (1- (demo-xstep d))))
                       ((= userdata +menu-hfast+) (set-xstep d (1+ (demo-xstep d))))
                       ((= userdata +menu-vslow+) (set-ystep d (1- (demo-ystep d))))
                       ((= userdata +menu-vfast+) (set-ystep d (1+ (demo-ystep d))))))
               (setf code (logand (intui:menu-item-next-select item) #xFFFF))))
    terminated))

(defun handle-intui-message (d imsg)
  "The C's handleIntuiMessage; returns T when the program should end."
  (let ((class (intui:intui-message-class imsg))
        (code (logand (intui:intui-message-code imsg) #xFFFF)))
    (cond ((or (= class intui:+idcmp-gadgetdown+)
               (= class intui:+idcmp-gadgetup+)
               (= class intui:+idcmp-mousemove+))
           ;; Slider messages: Code is the new level, IAddress the gadget.
           (let ((gadget (ptr (intui:intui-message-i-address imsg))))
             (when gadget
               (let ((id (intui:gadget-gadget-id gadget)))
                 (cond ((= id +gad-horiz+) (setf (demo-xstep d) code))
                       ((= id +gad-vert+)  (setf (demo-ystep d) code))))))
           nil)
          ((= class intui:+idcmp-vanillakey+)
           (case (char-upcase (code-char code))
             (#\S (setf (demo-count d) 1) nil)
             (#\R (setf (demo-count d) +run-forever+) nil)
             (#\Q (setf (demo-count d) 0) t)
             (t nil)))
          ((= class intui:+idcmp-menupick+)
           (handle-menu-pick d code))
          (t nil))))

;;; ---------------------------------------------------------------- main

(defun main-loop (d seconds)
  "The C's main loop.  Handle IntuiMessages and SafeMessages as their
signals say, render/swap while COUNT is non-zero, then either retry
after WaitTOF (held off) or sleep until a signal arrives.  Ctrl-C ends
it, and so does SECONDS (unattended runs; the sleep then polls once a
frame so a stopped animation cannot block past the deadline)."
  (let* ((dbufport (demo-dbufport d))
         (userport (demo-userport d))
         (dbmask (sigmask dbufport))
         (usermask (sigmask userport))
         (waitmask (logior dbmask usermask dos:+sigbreakf-ctrl-c+))
         (deadline (and seconds
                        (+ (get-internal-real-time)
                           (round (* seconds internal-time-units-per-second)))))
         (sigs 0)
         (terminated nil))
    (loop until terminated do
      (when (logtest sigs usermask)
        (loop for imsg = (gt:gt-get-msg userport)
              while imsg
              do (when (handle-intui-message d imsg) (setf terminated t))
                 (gt:gt-reply-msg imsg)))
      ;; SafeMessages are REPLIED to us: never reply them onwards.
      (when (logtest sigs dbmask)
        (loop for msg = (exec:get-msg dbufport)
              while msg
              do (handle-dbuf-message d msg)))
      (when (logtest sigs dos:+sigbreakf-ctrl-c+)
        (setf terminated t))
      (when (and deadline (> (get-internal-real-time) deadline))
        (setf terminated t))
      (unless terminated
        (let ((held-off (and (/= (demo-count d) 0) (handle-buffer-swap d))))
          (cond (held-off (gfx:wait-tof))
                (deadline (gfx:wait-tof)
                          (setf sigs (exec:set-signal 0 waitmask)))
                (t (setf sigs (exec:wait waitmask)))))))))

(defun run (&key (seconds amiga.intuition:*event-loop-timeout*))
  "Open everything, animate until Q / Quit / Ctrl-C — or for SECONDS —
and tear it all down.  Returns (:frames N :held-off M :seconds S)."
  (let ((d (make-demo))
        (start (get-internal-real-time)))
    (unwind-protect
         (progn (init-all d)
                (main-loop d seconds))
      (cleanup d))
    (let ((elapsed (/ (- (get-internal-real-time) start)
                      internal-time-units-per-second)))
      (format t "doublebuffer: ~D frame~:P swapped in ~,1F s~@[ (~,1F fps)~], held off ~D time~:P~%"
              (demo-frames d) elapsed
              (and (plusp elapsed) (/ (demo-frames d) elapsed))
              (demo-held-off d))
      (list :frames (demo-frames d)
            :held-off (demo-held-off d)
            :seconds (float elapsed)))))

(defun available-p ()
  "V39 graphics and intuition — AmigaOS 3.0+ / MorphOS."
  (and gfx:*graphics-base* intui:*intuition-base*
       (>= gfx:*graphics-version* 39) (>= intui:*intuition-version* 39)))

(if (available-p)
    (run)
    (format t "doublebuffer: not available - AmigaOS 3.0+ (V39 graphics/intuition) required~%"))
