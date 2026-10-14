;;; screen-grab.lisp — FS-UAE harness helper: photograph new windows and
;;; screens.
;;;
;;; Run detached (Run >log amilisp --load verify/realamiga/screen-grab.lisp)
;;; next to an unattended GUI run.  It watches the default public screen
;;; and Intuition's screen list; whenever a window it has not seen before
;;; appears on the public screen, or a titled custom screen opens, it
;;; waits a moment for the thing to render and saves the whole screen as
;;; a binary PPM (P6) under build/amiga/shots/ — which the host converts
;;; to PNG with ffmpeg.  It leaves when T:examples-done exists or after
;;; *MAX-SECONDS*.
;;;
;;; The public screen's pixels come from Picasso96API.library's
;;; p96ReadPixelArray, which converts any RTG screen (the FS-UAE
;;; Workbench is a 16-bit uaegfx screen) to 24-bit RGB.  Without
;;; Picasso96, and for the custom screens the graphics examples open
;;; (native-chipset palette screens), the bitplanes are read straight
;;; out of the screen's BitMap with PEEK-BYTES and resolved through the
;;; ViewPort's ColorMap — a few hundred FFI calls rather than a ReadPixel
;;; per pixel, which matters because those screens live only for the
;;; example's few seconds.  Hardware sprites are not in any bitmap, so a
;;; shot of the sprite example's screen shows the screen without the
;;; sprite.
;;;
;;; The same technique as a program of its own, for a screen of your
;;; choice: examples/amiga/gfx/screenshot.lisp.

(require "amiga/ffi")
(require "amiga/raw/exec")
(require "amiga/raw/dos")
(require "amiga/raw/intuition")
(require "amiga/raw/graphics")

(defpackage "SCREEN-GRAB"
  (:use "CL")
  (:local-nicknames ("EXEC"  "AMIGA.RAW.EXEC")
                    ("DOS"   "AMIGA.RAW.DOS")
                    ("INTUI" "AMIGA.RAW.INTUITION")
                    ("GFX"   "AMIGA.RAW.GRAPHICS")))

(in-package "SCREEN-GRAB")

(defparameter *shot-dir* "build/amiga/shots/")
(defparameter *done-file* "T:examples-done")
(defparameter *max-seconds* 900)
(defparameter *settle-ticks* 100)       ; 2 s after a new window shows up
(defparameter *screen-settle-ticks* 50) ; 1 s after a new screen opens — the
                                        ; examples' screens live only seconds

(defvar *p96-base* (amiga:open-library "Picasso96API.library" 0))

;; Our own log file: a detached (Run) process's stdout is not reliably
;; where the harness looks.
(defvar *log* (open "build/amiga/screen-grab.log" :direction :output
                                                  :if-exists :supersede
                                                  :if-does-not-exist :create))

(defun say (fmt &rest args)
  (let ((line (apply #'format nil fmt args)))
    (format t "~A~%" line)
    (finish-output)
    (format *log* "~A~%" line)
    (finish-output *log*)))

(defun file-exists-p (name)
  (ffi:with-foreign-string (n name)
    (let ((lock (dos:lock n dos:+shared-lock+)))
      (when (and (integerp lock) (/= lock 0))
        (dos:un-lock lock)
        t))))

(defun foreign-ptr (value)
  "Struct accessors hand back NIL, an integer or a foreign pointer."
  (cond ((null value) nil)
        ((integerp value) (if (zerop value) nil (ffi:make-foreign-pointer value)))
        ((ffi:null-pointer-p value) nil)
        (t value)))

;;; ---------------------------------------------------------------- pixels

(defun read-rgb-p96 (rp width height buffer)
  "p96ReadPixelArray(ri, 0, 0, rp, 0, 0, w, h) into BUFFER as RGBFB_R8G8B8."
  (let ((ri (ffi:alloc-foreign 12)))
    (unwind-protect
         (progn
           (ffi:poke-u32 ri (ffi:foreign-pointer-address buffer) 0) ; Memory
           (ffi:poke-i16 ri (* width 3) 4)                           ; BytesPerRow
           (ffi:poke-u32 ri 2 8)                                     ; RGBFB_R8G8B8
           (amiga:call-library *p96-base* -108
                               (list :a0 ri :d0 0 :d1 0 :a1 rp
                                     :d2 0 :d3 0 :d4 width :d5 height)
                               1))                                   ; void
      (ffi:free-foreign ri))))

(defun screen-palette (scr)
  "The screen's colours as three 256-entry byte vectors (VALUES r g b),
from the ViewPort's ColorMap (as many entries as it has); black beyond."
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

(defun bitmap-geometry (rp)
  "The BitMap RP renders into, as (VALUES bytes-per-row rows depth
planes-pointer), sanity-checked: a screen closing under us must send
the reader to an error, not off into the weeds."
  (let ((bm (foreign-ptr (gfx:rastport-bitmap rp))))
    (unless bm (error "RastPort without a BitMap"))
    (let ((bpr (gfx:bitmap-bytes-per-row bm))
          (rows (gfx:bitmap-rows bm))
          (depth (gfx:bitmap-depth bm)))
      (unless (and (<= 2 bpr 16384) (<= 1 rows 4096) (<= 1 depth 32))
        (error "implausible BitMap ~D bytes/row x ~D rows x ~D planes - screen gone?"
               bpr rows depth))
      (values bpr rows depth (gfx:bitmap-planes bm)))))

(defun planar-screen-p (scr rp)
  "Is SCR's bitmap a standard planar one (a chipset screen) rather than
an RTG bitmap, where BytesPerRow is width x bytes-per-pixel and the
planes are not planes?  Interleaved bitmaps count as not planar here;
they go to p96 like RTG ones."
  (multiple-value-bind (bpr rows depth) (bitmap-geometry rp)
    (let ((width (intui:screen-width scr)))
      (and (<= depth 8)
           (<= width (* bpr 8) (+ width 64))
           (>= rows (intui:screen-height scr))))))

(defun nibble-table (depth r g b)
  "For DEPTH <= 3: indexed by the nibbles of the DEPTH planes at one
4-pixel column (plane 0 in bits 0-3, plane 1 in 4-7, plane 2 in 8-11),
the 12 RGB bytes of those four pixels, leftmost first."
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

(defun read-planar-rgb (scr rp width height)
  "Palette screens: the bitplanes of RP's BitMap, row by row out of CHIP
memory, resolved through SCR's palette into a Lisp byte vector of RGB
triples.  Up to three planes go through NIBBLE-TABLE, eight pixels per
byte column (the per-pixel Lisp loop took longer than the examples'
screens stay open); deeper bitmaps take the slow per-pixel path."
  (multiple-value-bind (bpr rows depth planes) (bitmap-geometry rp)
    (multiple-value-bind (r g b) (screen-palette scr)
      (let* ((rgb (make-array (* width height 3) :element-type '(unsigned-byte 8)
                                                 :initial-element 0))
             (rowbufs (make-array depth))
             (plane-ptrs (make-array depth))
             (bytes (min bpr (ceiling width 8)))
             (table (and (<= depth 3) (nibble-table depth r g b))))
        (dotimes (p depth)
          (let ((address (ffi:peek-u32 planes (* p 4))))
            (when (zerop address) (error "BitMap plane ~D is NULL" p))
            (setf (aref plane-ptrs p) (ffi:make-foreign-pointer address)
                  (aref rowbufs p) (make-array bpr :element-type '(unsigned-byte 8)))))
        (dotimes (y (min height rows))
          (dotimes (p depth)
            (ffi:peek-bytes (aref plane-ptrs p) (aref rowbufs p) (* y bpr) 0 bytes))
          (let ((base (* 3 y width))
                (end (* 3 (1+ y) width)))
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
                           (off (+ base (* 24 bx))))
                      (replace rgb table :start1 off :end1 end
                                         :start2 (* hi 12) :end2 (+ (* hi 12) 12))
                      ;; The last byte column of a row whose WIDTH isn't a
                      ;; multiple of 8 covers pixels past WIDTH; when even
                      ;; the "hi" nibble's four pixels don't all fit, the
                      ;; "lo" nibble's start is at or past the row's END —
                      ;; skip it instead of handing REPLACE a start > end.
                      (when (< (+ off 12) end)
                        (replace rgb table :start1 (+ off 12) :end1 end
                                           :start2 (* lo 12) :end2 (+ (* lo 12) 12))))))
                (dotimes (x width)
                  (let ((pen 0)
                        (byte (ash x -3))
                        (bit (- 7 (logand x 7))))
                    (dotimes (p depth)
                      (when (logbitp bit (aref (aref rowbufs p) byte))
                        (setf pen (logior pen (ash 1 p)))))
                    (let ((off (+ base (* 3 x))))
                      (setf (aref rgb off) (aref r pen)
                            (aref rgb (1+ off)) (aref g pen)
                            (aref rgb (+ off 2)) (aref b pen))))))))
        rgb))))

(defun write-file-bytes (name header buffer length)
  (ffi:with-foreign-string (n name)
    (ffi:with-foreign-string (h header)
      (let ((fh (dos:open n dos:+mode-newfile+)))
        (unless (and (integerp fh) (/= fh 0))
          (error "cannot create ~A" name))
        (unwind-protect
             (progn (dos:write fh h (length header))
                    (dos:write fh buffer length))
          (dos:close fh))))))

(defun write-ppm (path width height rgb)
  "A P6 PPM of the Lisp byte vector RGB."
  (with-open-file (s path :direction :output :if-exists :supersede
                          :element-type '(unsigned-byte 8))
    (loop for ch across (format nil "P6~%~D ~D~%255~%" width height)
          do (write-byte (char-code ch) s))
    (write-sequence rgb s)))

(defun save-screen (scr path p96)
  "Save the screen SCR as a P6 PPM at PATH, through p96 when P96, else
from its bitplanes."
  (let* ((width (intui:screen-width scr))
         (height (intui:screen-height scr))
         (rp (foreign-ptr (intui:screen-rastport scr))))
    (if p96
        (let* ((size (* width height 3))
               (buffer (ffi:alloc-foreign size)))
          (unwind-protect
               (progn
                 (read-rgb-p96 rp width height buffer)
                 (write-file-bytes path (format nil "P6~%~D ~D~%255~%" width height)
                                   buffer size))
            (ffi:free-foreign buffer)))
        (write-ppm path width height (read-planar-rgb scr rp width height)))
    (say "; shot ~A (~Dx~D, ~:[bitplanes~;p96~])" path width height p96)))

(defun grab-pub-screen (path)
  "Save the default public screen as a P6 PPM at PATH."
  (let ((scr (intui:lock-pub-screen nil)))
    (unless scr (error "no public screen"))
    (unwind-protect (save-screen scr path (and *p96-base* t))
      (intui:unlock-pub-screen nil scr))))

(defun grab-custom-screen (address path)
  "Save the custom screen at ADDRESS — one the examples opened.  A
native-chipset palette screen is read from its bitplanes, an RTG one
\(a ReAction requester's screen, say) through p96.  It cannot be locked
\(not public), so this is a best effort while it is open."
  (let* ((scr (ffi:make-foreign-pointer address))
         (rp (foreign-ptr (intui:screen-rastport scr)))
         (planar (planar-screen-p scr rp)))
    (unless (or planar *p96-base*)
      (error "RTG screen and no Picasso96 to read it"))
    (save-screen scr path (not planar))))

(defparameter *ready-file* "T:screen-grab-ready")

(defun announce-ready ()
  "Tell examples.lisp we are watching: it waits for this file so the
first example's window is not up before we look."
  (with-open-file (s *ready-file* :direction :output :if-exists :supersede
                                  :if-does-not-exist :create)
    (write-line "ready" s)))

;;; ---------------------------------------------------------------- windows

(defun window-titles ()
  "The (address . title) of every window on the default public screen."
  (let ((scr (intui:lock-pub-screen nil))
        (result '()))
    (when scr
      (unwind-protect
           (let ((lock (intui:lock-i-base 0)))
             (unwind-protect
                  (loop for win = (foreign-ptr (intui:screen-first-window scr))
                          then (foreign-ptr (intui:window-next-window win))
                        while win
                        do (let ((title (foreign-ptr (intui:window-title win))))
                             (push (cons (ffi:foreign-pointer-address win)
                                         (if title (ffi:foreign-to-string title 200) ""))
                                   result)))
               (intui:unlock-i-base lock)))
        (intui:unlock-pub-screen nil scr)))
    result))

(defun screen-titles ()
  "The (address . title) of every open screen, from IntuitionBase's list."
  (let ((result '())
        (lock (intui:lock-i-base 0)))
    (unwind-protect
         (loop for scr = (foreign-ptr (intui:intuition-base-first-screen
                                       intui:*intuition-base*))
                 then (foreign-ptr (intui:screen-next-screen scr))
               while scr
               do (let ((title (foreign-ptr (intui:screen-title scr))))
                    (push (cons (ffi:foreign-pointer-address scr)
                                (if title (ffi:foreign-to-string title 200) ""))
                          result)))
      (intui:unlock-i-base lock))
    result))

(defun sanitize (title)
  (let ((s (string-downcase (substitute-if #\- (lambda (c) (not (alphanumericp c))) title))))
    (subseq s 0 (min 40 (length s)))))

(defun watch ()
  (let ((seen (window-titles))
        (seen-screens (screen-titles))
        (shots 0)
        (deadline (+ (get-internal-real-time)
                     (* *max-seconds* internal-time-units-per-second))))
    (say "; screen-grab: watching, ~D windows and ~D screens at start, p96 ~A"
         (length seen) (length seen-screens) (if *p96-base* "yes" "no"))
    (announce-ready)
    (loop
      (when (or (file-exists-p *done-file*)
                (> (get-internal-real-time) deadline))
        (return))
      (dos:delay 10)
      ;; New custom screens first: they are short-lived.
      (let ((new (remove-if (lambda (s) (member s seen-screens :test #'equal))
                            (screen-titles))))
        (when new
          (dos:delay *screen-settle-ticks*)
          (dolist (s new)
            (push s seen-screens)
            (if (string= (cdr s) "")
                (say "; new screen without title at #x~X, not photographed" (car s))
                (progn
                  (incf shots)
                  (let ((path (format nil "~A~2,'0D-screen-~A.ppm"
                                      *shot-dir* shots (sanitize (cdr s)))))
                    (say "; new screen ~S" (cdr s))
                    ;; Still open?  It may have closed while we settled.
                    (if (member s (screen-titles) :test #'equal)
                        (handler-case (grab-custom-screen (car s) path)
                          (error (e) (say "; grab failed: ~A" e)))
                        (say "; screen ~S closed before it could be photographed"
                             (cdr s)))))))))
      (let ((new (remove-if (lambda (w) (member w seen :test #'equal)) (window-titles))))
        (when new
          (dos:delay *settle-ticks*)
          (dolist (w new)
            (incf shots)
            (let ((path (format nil "~A~2,'0D-~A.ppm" *shot-dir* shots (sanitize (cdr w)))))
              (say "; new window ~S" (cdr w))
              (handler-case (grab-pub-screen path)
                (error (e) (say "; grab failed: ~A" e))))
            (push w seen)))))
    (say "; screen-grab: done, ~D shots" shots)))

(watch)
