;;; screenshot.lisp — save the front screen (or the Workbench) as an image.
;;;
;;; The photographer of the FS-UAE examples harness
;;; (verify/realamiga/screen-grab.lisp, which waits for windows to appear
;;; and shoots them unattended) as a program of its own: point it at a
;;; screen and it writes the pixels out as a binary PPM (P6), the plain
;;; RGB format every host tool reads (ffmpeg, ImageMagick, GIMP, a PPM
;;; datatype on the Amiga).
;;;
;;; Two kinds of screen, two ways to the pixels:
;;;
;;;   RTG screens (a CyberGraphX / Picasso96 / uaegfx / SAGA Workbench,
;;;   every MorphOS screen) keep their pixels in whatever chunky format
;;;   the graphics card likes; the portable way at them is the RTG
;;;   system's ReadPixelArray, which converts any RTG bitmap to 24-bit
;;;   RGB in one call.  cybergraphics.library's is the common API (both
;;;   CyberGraphX and Picasso96 provide it, MorphOS natively) with
;;;   p96ReadPixelArray of Picasso96API.library as the fallback.  Both
;;;   libraries are opened by the program itself and called by LVO
;;;   through AMIGA:CALL-LIBRARY: the generated amiga/raw/cybergraphics
;;;   module would open the library at load time and refuse to load on
;;;   an Amiga without it, and this program must run there too.
;;;
;;;   Native chipset screens (a custom LORES/HIRES screen, the Workbench
;;;   of an Amiga without a graphics card) are planar: one bit per pixel
;;;   per plane, in CHIP memory, and the ViewPort's ColorMap turns pen
;;;   numbers into colours.  The bitplanes are read straight out of the
;;;   BitMap with PEEK-BYTES -- a few hundred FFI calls for the whole
;;;   screen, not one ReadPixel per pixel -- and resolved through the
;;;   palette from GetRGB32.  Whether a screen is one or the other is
;;;   read off the BitMap's geometry (PLANAR-GEOMETRY-P).
;;;
;;; What it shows: LockPubScreen / UnlockPubScreen, the screen list
;;; under LockIBase (which screen is in front), struct Screen / BitMap /
;;; ColorMap through the generated raw accessors, PEEK-BYTES and
;;; POKE-BYTES between foreign memory and Lisp byte vectors, a DOS file
;;; written from a foreign buffer with no copy through the Lisp heap,
;;; and optional libraries opened by hand and called by LVO.
;;;
;;; Run on AmigaOS 3+ or MorphOS:
;;;   amilisp --load examples/amiga/gfx/screenshot.lisp
;;; It counts down three seconds so you can bring the screen you want
;;; to the front, then writes RAM:screenshot.ppm.  From the REPL,
;;; (screenshot:grab :screen :workbench :file "RAM:wb.ppm") shoots the
;;; default public screen without the countdown, and SCREEN-RGB hands
;;; you the pixels of any screen for your own purposes.  GRAB and RUN
;;; return a plist (:file :width :height :depth :method :bytes), :method
;;; being :CGX, :P96 or :BITPLANES.  amiga.intuition:*event-loop-timeout*
;;; (the unattended-run knob of the other examples) skips the countdown.
;;; Hardware sprites live in no bitmap, so the mouse pointer is never in
;;; the picture.

(require "amiga/raw/dos")
(require "amiga/raw/graphics")
(require "amiga/raw/intuition")
(require "amiga/intuition")      ; *EVENT-LOOP-TIMEOUT*

(defpackage "SCREENSHOT"
  (:use "CL")
  (:local-nicknames ("DOS"   "AMIGA.RAW.DOS")
                    ("GFX"   "AMIGA.RAW.GRAPHICS")
                    ("INTUI" "AMIGA.RAW.INTUITION"))
  (:export "AVAILABLE-P" "RTG-READER" "GRAB" "RUN" "SCREEN-RGB" "PIXEL"))

(in-package "SCREENSHOT")

(defparameter *default-file* "RAM:screenshot.ppm")

;;; The RTG libraries are optional: without either, only chipset screens
;;; can be photographed.  Opened once for the session; NIL where absent
;;; (AMIGA:OPEN-LIBRARY answers NIL for a library the OS cannot find, and
;;; exists on AmigaOS/MorphOS builds only).
(defvar *cgx-base*
  (and (member :amigaos *features*)
       (amiga:open-library "cybergraphics.library" 0)))
(defvar *p96-base*
  (and (member :amigaos *features*)
       (amiga:open-library "Picasso96API.library" 0)))

(defconstant +lvo-cgx-read-pixel-array+ -120)
(defconstant +rectfmt-rgb+ 0)           ; cybergraphics RECTFMT_RGB: 3 bytes/pixel
(defconstant +lvo-p96-read-pixel-array+ -108)
(defconstant +rgbfb-r8g8b8+ 2)          ; RenderInfo.RGBFormat: 24-bit RGB

(defun available-p ()
  (and gfx:*graphics-base* intui:*intuition-base* t))

(defun rtg-reader ()
  "How RTG screens will be read: :CGX, :P96, or NIL when neither library
is here and only chipset screens can be photographed."
  (cond (*cgx-base* :cgx)
        (*p96-base* :p96)
        (t nil)))

(defun foreign-ptr (value)
  "Struct accessors hand back NIL, an integer or a foreign pointer."
  (cond ((null value) nil)
        ((integerp value) (if (zerop value) nil (ffi:make-foreign-pointer value)))
        ((ffi:null-pointer-p value) nil)
        (t value)))

;;; ---------------------------------------------------------------- screens

(defun front-screen-address ()
  "IntuitionBase->FirstScreen, read under LockIBase: the screen in front
right now, as an address.  NIL when no screen is open."
  (let ((lock (intui:lock-i-base 0)))
    (unwind-protect
         (let ((scr (foreign-ptr (intui:intuition-base-first-screen
                                  intui:*intuition-base*))))
           (and scr (ffi:foreign-pointer-address scr)))
      (intui:unlock-i-base lock))))

(defun call-with-screen (which fn)
  "Call FN with the struct Screen WHICH names: :WORKBENCH (the default
public screen, locked while FN runs), :FRONT (whatever is in front --
locked when that is the public screen; a custom screen belongs to the
program that opened it and cannot be locked, so reading it is a best
effort while it stays open), or a Screen pointer or address of yours."
  (case which
    (:workbench
     (let ((scr (intui:lock-pub-screen nil)))
       (unless scr (error "screenshot: no default public screen to lock"))
       (unwind-protect (funcall fn scr)
         (intui:unlock-pub-screen nil scr))))
    (:front
     ;; Hold the public screen's lock either way: cheap, and it keeps
     ;; the Workbench from closing under a shot of it.
     (let ((pub (intui:lock-pub-screen nil)))
       (unwind-protect
            (let ((front (front-screen-address)))
              (unless front (error "screenshot: no screen is open"))
              (funcall fn (if (and pub (= front (ffi:foreign-pointer-address pub)))
                              pub
                              (ffi:make-foreign-pointer front))))
         (when pub (intui:unlock-pub-screen nil pub)))))
    (t
     (funcall fn (if (integerp which) (ffi:make-foreign-pointer which) which)))))

;;; ---------------------------------------------------------------- geometry

(defun bitmap-geometry (rp)
  "The BitMap RP renders into, as (VALUES bytes-per-row rows depth
planes-pointer), sanity-checked: a screen closing under us must send
the reader to an error, not off into the weeds."
  (let ((bm (foreign-ptr (gfx:rastport-bitmap rp))))
    (unless bm (error "screenshot: RastPort without a BitMap"))
    (let ((bpr (gfx:bitmap-bytes-per-row bm))
          (rows (gfx:bitmap-rows bm))
          (depth (gfx:bitmap-depth bm)))
      (unless (and (<= 2 bpr 16384) (<= 1 rows 4096) (<= 1 depth 32))
        (error "screenshot: implausible BitMap ~D bytes/row x ~D rows x ~D planes - screen gone?"
               bpr rows depth))
      (values bpr rows depth (gfx:bitmap-planes bm)))))

(defun planar-geometry-p (width height bytes-per-row rows depth)
  "Does a BitMap of this shape hold a WIDTH x HEIGHT screen as standard
bitplanes?  A planar row is WIDTH bits rounded up to words (the BitMap
may be a little wider than the screen); an RTG bitmap has BytesPerRow
of WIDTH times bytes per pixel and a Depth of bits per pixel, and its
'planes' are not planes.  Interleaved bitmaps (BytesPerRow = depth x
row) count as not planar here and go to Picasso96 like RTG ones."
  (and (<= depth 8)
       (<= width (* bytes-per-row 8) (+ width 64))
       (>= rows height)))

(defun planar-screen-p (scr rp)
  (multiple-value-bind (bpr rows depth) (bitmap-geometry rp)
    (planar-geometry-p (intui:screen-width scr) (intui:screen-height scr)
                       bpr rows depth)))

;;; ---------------------------------------------------------------- pixels

(defun read-rgb-cgx (rp width height buffer)
  "cybergraphics ReadPixelArray(buffer, 0, 0, w*3, rp, 0, 0, w, h,
RECTFMT_RGB): the screen as 24-bit RGB rows of w*3 bytes."
  (amiga:call-library *cgx-base* +lvo-cgx-read-pixel-array+
                      (list :a0 buffer :d0 0 :d1 0 :d2 (* width 3) :a1 rp
                            :d3 0 :d4 0 :d5 width :d6 height :d7 +rectfmt-rgb+)))

(defun read-rgb-p96 (rp width height buffer)
  "p96ReadPixelArray(ri, 0, 0, rp, 0, 0, w, h) into BUFFER as 24-bit RGB.
struct RenderInfo is Memory (APTR), BytesPerRow (WORD), pad (WORD),
RGBFormat (ULONG) -- 12 bytes, built by hand."
  (let ((ri (ffi:alloc-foreign 12)))
    (unwind-protect
         (progn
           (ffi:poke-u32 ri (ffi:foreign-pointer-address buffer) 0) ; Memory
           (ffi:poke-i16 ri (* width 3) 4)                           ; BytesPerRow
           (ffi:poke-u32 ri +rgbfb-r8g8b8+ 8)                        ; RGBFormat
           (amiga:call-library *p96-base* +lvo-p96-read-pixel-array+
                               (list :a0 ri :d0 0 :d1 0 :a1 rp
                                     :d2 0 :d3 0 :d4 width :d5 height)
                               1))                                   ; void
      (ffi:free-foreign ri))))

(defun screen-palette (scr)
  "The screen's colours as three 256-entry byte vectors (VALUES r g b),
from the ViewPort's ColorMap (as many entries as it has); black beyond.
GetRGB32 hands back 32-bit left-justified components: the top byte is
the 8-bit colour."
  (let* ((cm (foreign-ptr (gfx:view-port-color-map (intui:screen-view-port scr))))
         (count (if cm (min 256 (max 1 (gfx:color-map-count cm))) 0))
         (table (ffi:alloc-foreign (* 256 12)))
         (r (make-array 256 :element-type '(unsigned-byte 8) :initial-element 0))
         (g (make-array 256 :element-type '(unsigned-byte 8) :initial-element 0))
         (b (make-array 256 :element-type '(unsigned-byte 8) :initial-element 0)))
    (unwind-protect
         (when cm
           (gfx:get-rgb32 cm 0 count table)
           (dotimes (i count)
             (setf (aref r i) (ffi:peek-u8 table (* i 12))
                   (aref g i) (ffi:peek-u8 table (+ (* i 12) 4))
                   (aref b i) (ffi:peek-u8 table (+ (* i 12) 8)))))
      (ffi:free-foreign table))
    (values r g b)))

(defun nibble-table (depth r g b)
  "For DEPTH <= 3: indexed by the nibbles of the DEPTH planes at one
4-pixel column (plane 0 in bits 0-3, plane 1 in 4-7, plane 2 in 8-11),
the 12 RGB bytes of those four pixels, leftmost first.  Eight pixels
then cost two table lookups instead of eight pen assemblies."
  (let* ((n (ash 1 (* 4 depth)))
         (table (make-array (* n 12) :element-type '(unsigned-byte 8))))
    (dotimes (idx n)
      (dotimes (i 4)
        (let ((pen 0) (bit (- 3 i)))
          (dotimes (p depth)
            (when (logbitp (+ bit (* 4 p)) idx)
              (setf pen (logior pen (ash 1 p)))))
          (let ((off (+ (* idx 12) (* i 3))))
            (setf (aref table off) (aref r pen)
                  (aref table (+ off 1)) (aref g pen)
                  (aref table (+ off 2)) (aref b pen))))))
    table))

(defun read-planar-rgb (scr rp width height buffer)
  "Palette screens: the bitplanes of RP's BitMap, row by row out of CHIP
memory, resolved through SCR's palette into BUFFER as RGB triples.  Up
to three planes go through NIBBLE-TABLE, eight pixels per byte column;
deeper bitmaps (a 16-colour Workbench, AGA screens) take the per-pixel
path, which is slower but still a few seconds."
  (multiple-value-bind (bpr rows depth planes) (bitmap-geometry rp)
    (multiple-value-bind (r g b) (screen-palette scr)
      (let* ((row-bytes (* width 3))
             (row (make-array row-bytes :element-type '(unsigned-byte 8)
                                        :initial-element 0))
             (rowbufs (make-array depth))
             (plane-ptrs (make-array depth))
             (bytes (min bpr (ceiling width 8)))
             (table (and (<= depth 3) (nibble-table depth r g b))))
        (dotimes (p depth)
          (let ((address (ffi:peek-u32 planes (* p 4))))
            (when (zerop address) (error "screenshot: BitMap plane ~D is NULL" p))
            (setf (aref plane-ptrs p) (ffi:make-foreign-pointer address)
                  (aref rowbufs p) (make-array bpr :element-type '(unsigned-byte 8)))))
        (dotimes (y (min height rows))
          (dotimes (p depth)
            (ffi:peek-bytes (aref plane-ptrs p) (aref rowbufs p) (* y bpr) 0 bytes))
          (if table
              (let ((buf0 (aref rowbufs 0))
                    (buf1 (if (> depth 1) (aref rowbufs 1) nil))
                    (buf2 (if (> depth 2) (aref rowbufs 2) nil)))
                (dotimes (bx bytes)
                  (let* ((b0 (aref buf0 bx))
                         (b1 (if buf1 (aref buf1 bx) 0))
                         (b2 (if buf2 (aref buf2 bx) 0))
                         (hi (logior (ash b0 -4) (logand b1 #xF0)
                                     (ash (logand b2 #xF0) 4)))
                         (lo (logior (logand b0 #x0F) (ash (logand b1 #x0F) 4)
                                     (ash (logand b2 #x0F) 8)))
                         (off (* 24 bx)))
                    (replace row table :start1 off :end1 row-bytes
                                       :start2 (* hi 12) :end2 (+ (* hi 12) 12))
                    ;; The last byte column of a row whose WIDTH isn't a
                    ;; multiple of 8 covers pixels past WIDTH; when even
                    ;; the "hi" nibble's four pixels don't all fit, the
                    ;; "lo" nibble's start is at or past the row's end --
                    ;; skip it instead of handing REPLACE a start > end.
                    (when (< (+ off 12) row-bytes)
                      (replace row table :start1 (+ off 12) :end1 row-bytes
                                         :start2 (* lo 12) :end2 (+ (* lo 12) 12))))))
              (dotimes (x width)
                (let ((pen 0)
                      (byte (ash x -3))
                      (bit (- 7 (logand x 7))))
                  (dotimes (p depth)
                    (when (logbitp bit (aref (aref rowbufs p) byte))
                      (setf pen (logior pen (ash 1 p)))))
                  (let ((off (* 3 x)))
                    (setf (aref row off) (aref r pen)
                          (aref row (1+ off)) (aref g pen)
                          (aref row (+ off 2)) (aref b pen))))))
          (ffi:poke-bytes buffer row (* y row-bytes)))))))

(defun screen-rgb (screen)
  "Photograph SCREEN (a struct Screen pointer) into a fresh foreign
buffer of RGB triples, row-major, top-left first -- from the bitplanes
for a chipset screen, through cybergraphics or Picasso96 for an RTG
one.  Returns (VALUES BUFFER WIDTH HEIGHT DEPTH METHOD), METHOD one of
:BITPLANES, :CGX and :P96; the caller frees BUFFER with
FFI:FREE-FOREIGN.  Signals when the screen is RTG and neither RTG
library is here to read it with."
  (let* ((width (intui:screen-width screen))
         (height (intui:screen-height screen))
         (rp (foreign-ptr (intui:screen-rastport screen)))
         (depth (nth-value 2 (bitmap-geometry rp)))
         (method (if (planar-screen-p screen rp) :bitplanes (rtg-reader))))
    (unless method
      (error "screenshot: an RTG screen and neither cybergraphics.library nor Picasso96API.library to read it with"))
    (let ((buffer (ffi:alloc-foreign (* width height 3)))
          (done nil))
      (unwind-protect
           (progn
             (ecase method
               (:bitplanes (read-planar-rgb screen rp width height buffer))
               (:cgx (read-rgb-cgx rp width height buffer))
               (:p96 (read-rgb-p96 rp width height buffer)))
             (setf done t))
        (unless done (ffi:free-foreign buffer)))
      (values buffer width height depth method))))

(defun pixel (buffer width x y)
  "The (VALUES R G B) of pixel X,Y in a SCREEN-RGB buffer of WIDTH."
  (let ((off (* 3 (+ (* y width) x))))
    (values (ffi:peek-u8 buffer off)
            (ffi:peek-u8 buffer (+ off 1))
            (ffi:peek-u8 buffer (+ off 2)))))

;;; ---------------------------------------------------------------- file

(defun ppm-header (width height)
  "The header of a binary PPM: magic, dimensions, maximum sample value."
  (format nil "P6~%~D ~D~%255~%" width height))

(defun write-file-bytes (name header buffer length)
  "Create NAME and write HEADER (a string) followed by LENGTH bytes of the
foreign BUFFER -- dos.library Write straight from the buffer, no trip
through a Lisp vector for a megabyte of pixels."
  (ffi:with-foreign-string (n name)
    (ffi:with-foreign-string (h header)
      (let ((fh (dos:open n dos:+mode-newfile+)))
        (unless (and (integerp fh) (/= fh 0))
          (error "screenshot: cannot create ~A" name))
        (unwind-protect
             (progn (dos:write fh h (length header))
                    (dos:write fh buffer length))
          (dos:close fh))))))

;;; ---------------------------------------------------------------- program

(defun grab (&key (screen :front) (file *default-file*))
  "Save SCREEN -- :FRONT (default), :WORKBENCH, or a Screen pointer or
address -- as a binary PPM at FILE.  Returns (:file :width :height
:depth :method :bytes)."
  (unless (available-p)
    (error "screenshot: AmigaOS with intuition.library and graphics.library required"))
  (call-with-screen screen
    (lambda (scr)
      (multiple-value-bind (buffer width height depth method) (screen-rgb scr)
        (unwind-protect
             (let ((size (* width height 3))
                   (header (ppm-header width height)))
               (write-file-bytes file header buffer size)
               (list :file file :width width :height height :depth depth
                     :method method :bytes (+ (length header) size)))
          (ffi:free-foreign buffer))))))

(defun run (&key (screen :front) (file *default-file*)
                 (delay (if amiga.intuition:*event-loop-timeout* 0 3)))
  "Count DELAY seconds down (time to bring a screen to the front), then
GRAB SCREEN to FILE and say what was saved."
  (when (plusp delay)
    (format t "screenshot: bring the screen to photograph to the front - ")
    (loop for n from delay above 0
          do (format t "~D... " n)
             (finish-output)
             (sleep 1))
    (terpri))
  (let ((r (grab :screen screen :file file)))
    (format t "screenshot: ~A - ~Dx~D, ~D plane~:P, read ~A, ~D bytes~%"
            (getf r :file) (getf r :width) (getf r :height) (getf r :depth)
            (ecase (getf r :method)
              (:bitplanes "from the bitplanes")
              (:cgx "through cybergraphics.library")
              (:p96 "through Picasso96"))
            (getf r :bytes))
    r))

(if (available-p)
    (run)
    (format t "screenshot: not available - AmigaOS with graphics.library required~%"))
