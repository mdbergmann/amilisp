;;; sprite.lisp — a hardware sprite moved across a custom screen.
;;;
;;; Common Lisp port of the RKM Companion ssprite.c example
;;; (RKM_Companion_v2.04/Graphics_Libraries/Sprites_Bobs/ssprite.c on
;;; the Amiga Developer CD, the "Simple Sprite" listing of the ROM
;;; Kernel Reference Manual: Libraries): GetSprite claims one of the
;;; eight hardware sprites, its three colours are set in the colour
;;; registers that sprite pair owns, ChangeSprite installs the image
;;; and MoveSprite walks it diagonally back and forth across the screen,
;;; one step per WaitTOF; on the way sprite DMA is switched off and on
;;; again (the OFF_SPRITE / ON_SPRITE macros) to show what that does.
;;;
;;; What it shows: struct SimpleSprite and the sprite image format (a
;;; position-control word pair, two data words per line, a terminating
;;; zero pair) in CHIP memory, the sprite colour register arithmetic
;;; \(16 + 4 * pair), SetRGB4, and a direct custom-chip register write
;;; from Lisp.
;;;
;;; Hardware sprites exist on the native chipset only: the example opens
;;; its own LORES custom screen (a sprite is invisible over an RTG
;;; screen) and reports when no sprite is available.
;;;
;;; Run on AmigaOS 3+ (an Amiga chipset, real or emulated):
;;;   amilisp --load examples/amiga/gfx/sprite.lisp
;;; The walk takes 6 x 100 frames (12 s at 50 Hz); (run :passes 2
;;; :frames 50) shortens it, and amiga.intuition:*event-loop-timeout*
;;; (the unattended-run knob of the other examples) cuts it off after
;;; that many seconds.  RUN returns a plist of what happened (:sprite
;;; :color-reg :x :y :frames).

(require "amiga/raw/exec")
(require "amiga/raw/graphics")
(require "amiga/raw/intuition")
(require "amiga/raw/hardware/custom")
(require "amiga/raw/hardware/dmabits")
(require "amiga/intuition")      ; *EVENT-LOOP-TIMEOUT*

(defpackage "SIMPLE-SPRITE"
  (:use "CL")
  (:local-nicknames ("EXEC"   "AMIGA.RAW.EXEC")
                    ("GFX"    "AMIGA.RAW.GRAPHICS")
                    ("INTUI"  "AMIGA.RAW.INTUITION")
                    ("CUSTOM" "AMIGA.RAW.HARDWARE.CUSTOM")
                    ("DMA"    "AMIGA.RAW.HARDWARE.DMABITS")))

(in-package "SIMPLE-SPRITE")

;;; Real boring sprite data: 9 lines, 16 pixels wide.  Two words per
;;; line -- bit n of the first and second word select the colour of
;;; pixel n: 00 transparent, 01 colour 1, 10 colour 2, 11 colour 3.
(defparameter *sprite-data*
  '(#x0000 #x0000                        ; position control (set by MoveSprite)
    #xffff #x0000                        ; line 1, colour 1
    #xffff #x0000                        ; line 2, colour 1
    #x0000 #xffff                        ; line 3, colour 2
    #x0000 #xffff                        ; line 4, colour 2
    #x0000 #x0000                        ; line 5, transparent
    #x0000 #xffff                        ; line 6, colour 2
    #x0000 #xffff                        ; line 7, colour 2
    #xffff #xffff                        ; line 8, colour 3
    #xffff #xffff                        ; line 9, colour 3
    #x0000 #x0000))                      ; reserved, must be 0 0

(defconstant +sprite-height+ 9)

;;; The custom chip register block.  OFF_SPRITE / ON_SPRITE from
;;; graphics/gfxmacros.h write DMACON directly; that is a chipset
;;; register, so it exists on an Amiga (or emulator) only.
(defconstant +custom-base+ #xDFF000)

(defun custom-chips-p ()
  "Only classic AmigaOS talks to the chipset; MorphOS has none."
  (and (member :amigaos *features*) (not (member :morphos *features*))))

(defun sprite-dma (on)
  "ON_SPRITE / OFF_SPRITE: set or clear DMAF_SPRITE in DMACON."
  (when (custom-chips-p)
    (ffi:poke-u16 (ffi:make-foreign-pointer +custom-base+)
                  (if on
                      (logior dma:+dmaf-setclr+ dma:+dmaf-sprite+)
                      dma:+dmaf-sprite+)
                  custom:+dmacon+)))

(defun make-sprite-image ()
  "The image words in CHIP memory, where the sprite DMA can fetch them."
  (let* ((size (* 2 (length *sprite-data*)))
         (mem (exec:alloc-vec size (logior exec:+memf-chip+ exec:+memf-clear+))))
    (unless mem (error "Couldn't allocate ~D bytes of CHIP memory" size))
    (loop for word in *sprite-data* for i from 0
          do (ffi:poke-u16 mem word (* i 2)))
    mem))

(defun open-sprite-screen (title)
  "A LORES screen -- the C's OpenScreenTagList(NULL, NULL) default -- so
the sprite is over a native-chipset ViewPort."
  (let ((tags (amiga.ffi:make-tag-list (list intui:+sa-display-id+ gfx:+lores-key+
                                             intui:+sa-depth+      2
                                             intui:+sa-title+      title
                                             intui:+sa-show-title+ 1))))
    (unwind-protect (intui:open-screen-tag-list 0 tags)
      (ffi:free-foreign tags))))

(defun run (&key (passes 6) (frames 100)
                 (seconds amiga.intuition:*event-loop-timeout*))
  "Claim sprite 2, colour it, walk it PASSES times FRAMES steps diagonally,
alternating direction each pass -- or until SECONDS are over.  Returns
\(:sprite N :color-reg R :x X :y Y :frames F), :sprite -1 when no
hardware sprite could be had."
  (let ((title (ffi:foreign-string "Simple sprite"))
        (screen nil)
        (sprite (ffi:alloc-foreign gfx:*simple-sprite-size*))
        (image nil)
        (sprite-num -1)
        (color-reg nil)
        (moved 0)
        (final-x 0)
        (final-y 0)
        (deadline (and seconds
                       (+ (get-internal-real-time)
                          (round (* seconds internal-time-units-per-second))))))
    (dotimes (i gfx:*simple-sprite-size*) (ffi:poke-u8 sprite 0 i))
    (unwind-protect
         (progn
           (setf screen (open-sprite-screen title))
           (unless screen
             (error "Couldn't open a LORES custom screen for the sprite"))
           (let ((viewport (intui:screen-view-port screen)))
             (setf sprite-num (gfx:get-sprite sprite 2))
             (if (= sprite-num -1)
                 (format t "sprite: GetSprite failed - no hardware sprite available~%")
                 (progn
                   ;; Sprites come in pairs sharing three colour registers:
                   ;; 17-19 for sprites 0/1, 21-23 for 2/3, and so on.
                   (setf color-reg (+ 16 (ash (logand sprite-num 6) 1)))
                   (format t "sprite: got sprite ~D, color_reg=~D~%" sprite-num color-reg)
                   (gfx:set-rgb4 viewport (+ color-reg 1) 12  3  8)
                   (gfx:set-rgb4 viewport (+ color-reg 2) 13 13 13)
                   (gfx:set-rgb4 viewport (+ color-reg 3)  4  4 15)
                   ;; Position and size must match the image before
                   ;; ChangeSprite installs it.
                   (setf (gfx:simple-sprite-x sprite) 0
                         (gfx:simple-sprite-y sprite) 0
                         (gfx:simple-sprite-height sprite) +sprite-height+)
                   (setf image (make-sprite-image))
                   (gfx:change-sprite 0 sprite image)
                   (gfx:move-sprite 0 sprite 30 0)
                   ;; Back and forth, one move per video frame.
                   (loop named walk
                         for pass from 0 below passes
                         for delta = 1 then (- delta)
                         do (dotimes (step frames)
                              (when (and deadline (> (get-internal-real-time) deadline))
                                (return-from walk))
                              (gfx:move-sprite 0 sprite
                                               (+ (gfx:simple-sprite-x sprite) delta)
                                               (+ (gfx:simple-sprite-y sprite) delta))
                              (incf moved)
                              (gfx:wait-tof)
                              ;; Show the effect of turning sprite DMA off.
                              (when (= step 40) (sprite-dma nil))
                              (when (= step 60) (sprite-dma t))))
                   ;; If the sprite was switched off while being displayed it
                   ;; shows as a vertical bar -- switching it back on before
                   ;; freeing it is the "just to be sure" of the original.
                   (sprite-dma t)))
             (setf final-x (gfx:simple-sprite-x sprite)
                   final-y (gfx:simple-sprite-y sprite))))
      (when (/= sprite-num -1)
        (gfx:free-sprite sprite-num))
      (when screen (intui:close-screen screen))
      (when image (exec:free-vec image))
      (ffi:free-foreign sprite)
      (ffi:free-foreign title))
    (format t "sprite: ~D move~:P, sprite ~D now at (~D,~D)~%"
            moved sprite-num final-x final-y)
    (list :sprite sprite-num
          :color-reg color-reg
          :x final-x
          :y final-y
          :frames moved)))

(defun available-p ()
  (and gfx:*graphics-base* intui:*intuition-base* t))

(if (available-p)
    (run)
    (format t "sprite: not available - AmigaOS with graphics.library required~%"))
