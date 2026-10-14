;;; amiga/ahi.lisp — ahi.device (AHI) playback for AmiLisp
;;;
;;; Loaded via (require "amiga/ahi") -- and only then: AHI support is
;;; OPT-IN.  Nothing else in the runtime or the library opens or probes
;;; ahi.device, and AMIGA.AUDIO (audio.device: the Paula channels, chip
;;; RAM samples, the register pokes) stays the default sound module,
;;; independent of this one.  A program that wants AHI asks for it here;
;;; a program on a machine without AHI never notices this module exists.
;;; Both can be loaded in one program -- but AHI's Paula driver takes its
;;; channels through audio.device, so on a Paula machine the two contend
;;; for the four channels when used at the same time.
;;;
;;; AHI is the retargetable audio system of AmigaOS (Martin Blom's
;;; ahi.device, V4 1997 / V6 2005, from Aminet) and a part of MorphOS: it
;;; mixes 8/16-bit mono/stereo sounds at any sample rate from any memory
;;; onto whatever the user configured in its preferences -- Paula, a
;;; sound card, the PPC machines' audio.  Two tiers:
;;;
;;;   * The DEVICE INTERFACE -- OPEN-AHI on a unit, PLAY-SAMPLE,
;;;     QUEUE-SAMPLE, STOP-SAMPLE, PLAYING-P: CMD_WRITE requests on a
;;;     unit the user set up (mode, mixing rate, channels) in the AHI
;;;     preferences.  One sound at a time per handle, non-blocking like
;;;     AMIGA.AUDIO, double-buffered through ahir_Link for gapless
;;;     streaming.  The first thing to reach for.
;;;   * The LOW-LEVEL API -- ALLOC-AUDIO, LOAD-SOUND, PLAY, SET-VOLUME
;;;     ...: AHI's own mixer under the program's control -- several
;;;     sounds on several channels, each with its own rate, volume and
;;;     stereo position, loops -- on an audio mode the program picks
;;;     (BEST-AUDIO-MODE, AUDIO-MODES, AUDIO-MODE-INFO).  From a handle
;;;     opened with :UNIT :NONE, as the AHI autodoc asks: a unit opened
;;;     through the device interface holds the hardware (on Paula the
;;;     whole of it) and AHI_AllocAudioA on it then fails.  Deliberately
;;;     HOOK-FREE: AHI calls SoundFunc / PlayerFunc / RecordFunc hooks
;;;     from interrupt or server context, where a Lisp callback (which
;;;     allocates and may signal) has no business, so none is ever
;;;     installed and everything is driven from the program.
;;;     AHI_KillAudio is not bound at all.
;;;
;;; The device's functions are called library-style through the base
;;; OpenDevice leaves in the request's io_Device: OPEN-AHI stores it in
;;; AMIGA.RAW.AHI:*AHI-BASE*, the generated table's base variable, which
;;; arms every AMIGA.RAW.AHI function until the last handle closes.
;;; Struct fields are the generated accessors of AMIGA.RAW.EXEC (the
;;; IORequest part) and AMIGA.RAW.AHI (AHIRequest, AHISampleInfo) -- no
;;; hand-typed offsets here.
;;;
;;; See tests/amiga/test-ahi.lisp for usage end-to-end and
;;; examples/amiga/audio/ahi-play.lisp for a program.

;; compile-time too: COMPILE-FILE (the host builds the lib/amiga FASLs) must see
;; these packages at read time, not only LOAD.  The raw modules open nothing
;; at load (ahi.device has no OpenLibrary), so requiring them is free of
;; side effects on every platform.
(eval-when (:compile-toplevel :load-toplevel :execute)
  (require "amiga/ffi")
  (require "amiga/exec")
  (require "amiga/raw/exec")
  (require "amiga/raw/ahi"))

(defpackage "AMIGA.AHI"
  (:use "CL" "FFI" "AMIGA.FFI")
  (:local-nicknames ("EXEC" "AMIGA.RAW.EXEC")
                    ("RAW"  "AMIGA.RAW.AHI"))
  (:export
   ;; The device interface: a handle on a unit
   "OPEN-AHI" "CLOSE-AHI" "WITH-AHI" "AHI-P" "AHI-OPEN-P" "AHI-UNIT"
   "PLAY-SAMPLE" "QUEUE-SAMPLE" "STOP-SAMPLE" "PLAYING-P"
   ;; Sample data
   "SAMPLE-BYTES" "MAKE-SAMPLE-BUFFER" "SAMPLE-FRAME-SIZE" "SAMPLE-TYPE-CODE"
   ;; The audio mode database
   "AUDIO-MODES" "AUDIO-MODE-NAME" "AUDIO-MODE-INFO" "BEST-AUDIO-MODE"
   ;; The low-level API: AHI's mixer, driven from the program
   "ALLOC-AUDIO" "FREE-AUDIO" "WITH-AUDIO-CTRL"
   "LOAD-SOUND" "UNLOAD-SOUND" "START-PLAYBACK" "STOP-PLAYBACK"
   "PLAY" "SET-VOLUME" "SET-FREQUENCY" "SET-SOUND"
   ;; Fixed-point helpers and constants
   "TO-FIXED" "FROM-FIXED"
   "+AHI-DEFAULT-UNIT+" "+AHI-NO-UNIT+"))

(in-package "AMIGA.AHI")

;;; ================================================================
;;; devices/ahi.h values spelled here -- cross-checked against the
;;; generated AMIGA.RAW.AHI table by tests/test_amiga_curated_vs_raw.lisp
;;; ================================================================

(defconstant +ahi-default-unit+ 0
  "AHI_DEFAULT_UNIT: the unit OPEN-AHI opens by default -- unit 0 of the
user's AHI preferences.")

(defconstant +ahi-no-unit+ 255
  "AHI_NO_UNIT: what OPEN-AHI :UNIT :NONE opens -- the function
interface (the audio mode database and the low-level API) without a
playback unit.")

;;; ================================================================
;;; Fixed point and sample types
;;; ================================================================

(defun to-fixed (x)
  "AHI's Fixed (16.16 fixed point) for the real X: 1.0 -> #x10000."
  (round (* x #x10000)))

(defun from-fixed (n)
  "The real value of the Fixed N: #x8000 -> 1/2."
  (/ n #x10000))

;;; keyword -> (AHIST_* code . bytes per sample frame)
(defparameter *sample-types*
  `((:mono8    . (,raw:+ahist-m8-s+  . 1))
    (:mono16   . (,raw:+ahist-m16-s+ . 2))
    (:stereo8  . (,raw:+ahist-s8-s+  . 2))
    (:stereo16 . (,raw:+ahist-s16-s+ . 4))))

(defun %sample-type (type)
  (or (if (integerp type)
          (rassoc type *sample-types* :key #'car)
          (assoc type *sample-types*))
      (error "AMIGA.AHI: unknown sample type ~S -- want :MONO8, :MONO16, :STEREO8 or :STEREO16 (signed samples, stereo interleaved left/right, 16-bit big-endian)"
             type)))

(defun sample-type-code (type)
  "The AHIST_* code of the sample TYPE :MONO8, :MONO16, :STEREO8 or
:STEREO16 (signed samples, stereo interleaved left/right, 16-bit
big-endian) -- or of such a code itself."
  (cadr (%sample-type type)))

(defun sample-frame-size (type)
  "Bytes per sample frame of TYPE: 1, 2, 2, 4 for :MONO8, :MONO16,
:STEREO8, :STEREO16 (a frame is one sample of every channel)."
  (cddr (%sample-type type)))

;;; ================================================================
;;; Sample data -- any memory, no chip RAM needed (unlike audio.device)
;;; ================================================================

(defun sample-bytes (samples &key (bits 8))
  "The raw bytes AHI plays, from a vector of signed sample values: BITS
8 (-128..127, one byte each) or 16 (-32768..32767, big-endian pairs).
Stereo data is interleaved left, right by the caller.  Returns an
\(unsigned-byte 8) vector for MAKE-SAMPLE-BUFFER (or, for AMIGA.AUDIO,
AMIGA.EXEC:ALLOC-CHIP-BYTES)."
  (let ((n (length samples)))
    (ecase bits
      (8 (let ((out (make-array n :element-type '(unsigned-byte 8))))
           (dotimes (i n out)
             (setf (aref out i) (logand (aref samples i) #xFF)))))
      (16 (let ((out (make-array (* 2 n) :element-type '(unsigned-byte 8))))
            (dotimes (i n out)
              (let ((v (logand (aref samples i) #xFFFF)))
                (setf (aref out (* 2 i)) (ash v -8)
                      (aref out (1+ (* 2 i))) (logand v #xFF)))))))))

(defun make-sample-buffer (bytes)
  "Copy the (unsigned-byte 8) vector BYTES -- sample data as AHI plays
it, see SAMPLE-BYTES -- into a fresh foreign buffer and return it.  Free
it with FFI:FREE-FOREIGN once no request in flight and no loaded sound
refers to it any more."
  (let ((p (ffi:alloc-foreign (length bytes))))
    (ffi:poke-bytes p bytes)
    p))

;;; ================================================================
;;; The device base -- io_Device after OpenDevice arms the raw table
;;; ================================================================

(defvar *open-handles* 0
  "Handles OPEN-AHI returned and CLOSE-AHI has not seen yet; the last
close disarms AMIGA.RAW.AHI:*AHI-BASE* (the device may be expunged).")

(defvar *open-handles-lock* (mp:make-lock "AMIGA.AHI open-handles")
  "Guards *OPEN-HANDLES* and the RAW:*AHI-BASE*/RAW:*AHI-VERSION* arm/disarm
against two threads calling OPEN-AHI/CLOSE-AHI at once -- otherwise the
counter can lose an update, or the last close can disarm the base while
another thread's handle is still open.")

(defun %arm-base (io)
  (let ((base (amiga.exec:io-request-device io)))
    (mp:with-lock-held (*open-handles-lock*)
      (setf raw:*ahi-base* base
            raw:*ahi-version* (amiga.ffi:library-version base))
      (incf *open-handles*))))

(defun %need-base ()
  (unless raw:*ahi-base*
    (error "AMIGA.AHI: ahi.device is not open -- OPEN-AHI first (:UNIT :NONE opens the function interface alone)")))

;;; ================================================================
;;; The handle -- a unit, a reply port, two AHIRequests
;;; ================================================================

(defstruct (ahi (:constructor %make-ahi) (:copier nil) (:predicate ahi-p))
  port          ; the reply MsgPort
  unit          ; the unit number opened (+AHI-NO-UNIT+ for :NONE)
  io-a          ; the AHIRequest OpenDevice was made on
  io-b          ; its copy, the second buffer of a double-buffered stream
                ;   (NIL on a :NONE handle: no playback)
  (pending nil)) ; requests out with the device, oldest first

(defun ahi-open-p (ahi)
  "True while the handle AHI is open."
  (and (ahi-p ahi) (ahi-io-a ahi) t))

(defun %clone-request (port io)
  "The second AHIRequest of a handle: CreateIORequest'd on PORT, then a
byte copy of the opened IO -- io_Device, io_Unit, ahir_Version and the
device's private words, the way the AHI PlayTest example copies its
request -- same port, same length, so DeleteIORequest frees it too."
  (let ((copy (amiga.exec:create-io-request port raw:*ahi-request-size*)))
    (when copy
      (let ((bytes (make-array raw:*ahi-request-size*
                               :element-type '(unsigned-byte 8))))
        (ffi:peek-bytes io bytes)
        (ffi:poke-bytes copy bytes))
      copy)))

(defun open-ahi (&key (unit +ahi-default-unit+) (version 4))
  "Open ahi.device and return a handle, or NIL when AHI is not installed
or the unit will not open.  UNIT is 0..3 (the units of the user's AHI
preferences; 0, the default, is where a program without an opinion
plays) or :NONE for the function interface alone (the audio mode
database and the low-level API, no PLAY-SAMPLE).  VERSION is the AHI
version required, 4 by default -- the whole of this module's surface.
Never blocks."
  (let ((unit-no (cond ((eq unit :none) +ahi-no-unit+)
                       ((and (integerp unit) (<= 0 unit 255)) unit)
                       (t (error "AMIGA.AHI: OPEN-AHI unit must be 0..255 or :NONE: ~S" unit))))
        (port nil) (io-a nil) (io-b nil) (ok nil))
    ;; no exec devices off AmigaOS/MorphOS (the host loads this module
    ;; for compilation and the pure checks): "not available", not an error
    (unless (member :amigaos *features*)
      (return-from open-ahi nil))
    (unwind-protect
         (progn
           (setf port (amiga.exec:create-msg-port))
           (unless port (return-from open-ahi nil))
           (setf io-a (amiga.exec:create-io-request port raw:*ahi-request-size*))
           (unless io-a (return-from open-ahi nil))
           ;; ahir_Version "*must* be preset to the version you need"
           (setf (raw:ahi-request-version io-a) version)
           (unless (zerop (amiga.exec:open-device raw:+ahiname+ unit-no io-a 0))
             (return-from open-ahi nil))
           (unless (eql unit-no +ahi-no-unit+)
             (setf io-b (%clone-request port io-a))
             (unless io-b
               (amiga.exec:close-device io-a)
               (return-from open-ahi nil)))
           (%arm-base io-a)
           (setf ok t)
           (%make-ahi :port port :unit unit-no :io-a io-a :io-b io-b))
      (unless ok
        (when io-b (amiga.exec:delete-io-request io-b))
        (when io-a (amiga.exec:delete-io-request io-a))
        (when port (amiga.exec:delete-msg-port port))))))

(defun close-ahi (ahi)
  "Stop playback, close the unit, free the requests and the port.  The
last handle closed disarms the AMIGA.RAW.AHI functions.  Returns NIL."
  (unless (ahi-p ahi)
    (error "AMIGA.AHI: CLOSE-AHI: not an AHI handle: ~S" ahi))
  (when (ahi-io-a ahi)
    (when (ahi-io-b ahi) (stop-sample ahi))
    (amiga.exec:close-device (ahi-io-a ahi))
    (when (ahi-io-b ahi) (amiga.exec:delete-io-request (ahi-io-b ahi)))
    (amiga.exec:delete-io-request (ahi-io-a ahi))
    (amiga.exec:delete-msg-port (ahi-port ahi))
    (setf (ahi-io-a ahi) nil (ahi-io-b ahi) nil (ahi-port ahi) nil)
    (mp:with-lock-held (*open-handles-lock*)
      (when (zerop (decf *open-handles*))
        (setf raw:*ahi-base* nil raw:*ahi-version* nil))))
  nil)

(defmacro with-ahi ((var &rest open-args) &body body)
  "Open ahi.device (OPEN-ARGS as for OPEN-AHI), bind the handle to VAR,
close on exit.  Signals when it cannot be opened."
  `(let ((,var (open-ahi ,@open-args)))
     (unless ,var
       (error "Cannot open ahi.device -- is AHI installed, and the unit set up in its preferences?"))
     (unwind-protect (progn ,@body)
       (close-ahi ,var))))

;;; ================================================================
;;; The device interface -- CMD_WRITE, non-blocking, double-buffered
;;; ================================================================

(defun %check-length (length type)
  (let ((frame (sample-frame-size type)))
    (unless (and (integerp length) (plusp length) (zerop (mod length frame)))
      (error "AMIGA.AHI: the sample length must be a positive multiple of the ~D-byte ~S frame: ~S"
             frame type length))))

(defun %check-playback (ahi op)
  (unless (ahi-p ahi)
    (error "AMIGA.AHI: ~A: not an AHI handle: ~S" op ahi))
  (unless (ahi-io-a ahi)
    (error "AMIGA.AHI: ~A: the handle is closed" op))
  (unless (ahi-io-b ahi)
    (error "AMIGA.AHI: ~A: this handle was opened with :UNIT :NONE, it has no playback unit -- OPEN-AHI a unit, or use the low-level API (ALLOC-AUDIO ...)"
           op)))

(defun %reap (ahi)
  "Take back every completed request, oldest first (linked requests
complete in order)."
  (loop while (and (ahi-pending ahi)
                   (amiga.exec:check-io (first (ahi-pending ahi))))
        do (amiga.exec:wait-io (pop (ahi-pending ahi)))))

(defun %free-request (ahi)
  "The handle's request that is not out with the device, or NIL."
  (let ((pending (ahi-pending ahi)))
    (cond ((not (member (ahi-io-a ahi) pending)) (ahi-io-a ahi))
          ((not (member (ahi-io-b ahi) pending)) (ahi-io-b ahi))
          (t nil))))

(defun %send (ahi io data length type rate volume pan priority link)
  "Fill IO as a CMD_WRITE of LENGTH bytes at DATA and send it, linked
after LINK (a request in flight) when given.  T when the device took it."
  (setf (exec:node-pri io) priority)             ; the sound's priority on the unit
  (setf (exec:io-std-req-command io) exec:+cmd-write+)
  (setf (exec:io-std-req-data io) data)
  (setf (exec:io-std-req-length io) length)
  (setf (exec:io-std-req-offset io) 0)
  (setf (raw:ahi-request-type io) (sample-type-code type))
  (setf (raw:ahi-request-frequency io) rate)
  (setf (raw:ahi-request-volume io) (max 0 (min #x10000 (to-fixed volume))))
  (setf (raw:ahi-request-position io) (max 0 (min #x10000 (to-fixed pan))))
  (setf (raw:ahi-request-link io) (or link (ffi:make-foreign-pointer 0)))
  (amiga.exec:send-io io)
  (setf (ahi-pending ahi) (append (ahi-pending ahi) (list io)))
  ;; SendIO has no result; a request the device refuses comes straight
  ;; back completed with io_Error set.
  (if (and (amiga.exec:check-io io)
           (not (zerop (amiga.exec:io-request-error io))))
      (progn (amiga.exec:wait-io io)
             (setf (ahi-pending ahi) (remove io (ahi-pending ahi)))
             nil)
      t))

(defun play-sample (ahi data length &key (type :mono8) (rate 8000)
                                         (volume 1.0) (pan 0.5) (priority 0))
  "Start LENGTH bytes of sample data at DATA (a foreign pointer -- from
MAKE-SAMPLE-BUFFER, any memory) on the handle's unit, cutting off
whatever it was playing.  TYPE is :MONO8, :MONO16, :STEREO8 or
:STEREO16 (LENGTH a multiple of its frame size), RATE the sample rate
in Hz, VOLUME 0..1, PAN 0 (left) .. 1 (right), PRIORITY -128..127 the
sound's precedence against other programs on the unit.  Returns
immediately: T when the request went out, NIL when the device refused
it.  Poll with PLAYING-P; DATA must stay allocated until then."
  (%check-length length type)
  (%check-playback ahi 'play-sample)
  (stop-sample ahi)
  (%send ahi (ahi-io-a ahi) data length type rate volume pan priority nil))

(defun queue-sample (ahi data length &key (type :mono8) (rate 8000)
                                          (volume 1.0) (pan 0.5) (priority 0))
  "Like PLAY-SAMPLE, but after the sound in flight instead of in its
place: the request is linked to it (ahir_Link) and the device starts it
the moment that one ends, gapless -- streaming from two buffers.  At
most two requests are in flight; with both busy nothing is queued and
NIL is returned (poll PLAYING-P and queue again).  With nothing playing
it is PLAY-SAMPLE."
  (%check-length length type)
  (%check-playback ahi 'queue-sample)
  (%reap ahi)
  (let ((io (%free-request ahi)))
    (and io
         (%send ahi io data length type rate volume pan priority
                (car (last (ahi-pending ahi)))))))

(defun playing-p (ahi)
  "True while a PLAY-SAMPLE / QUEUE-SAMPLE is still sounding.  Reclaims
finished requests as a side effect."
  (%check-playback ahi 'playing-p)
  (%reap ahi)
  (not (null (ahi-pending ahi))))

(defun stop-sample (ahi)
  "Silence the unit: abort what is in flight (a queued request too) and
take the requests back.  Returns NIL.  WAIT-IO here only ever completes
an already finished or just aborted request, so nothing here stalls."
  (%check-playback ahi 'stop-sample)
  (dolist (io (ahi-pending ahi))
    (unless (amiga.exec:check-io io)
      (amiga.exec:abort-io io))
    (amiga.exec:wait-io io))
  (setf (ahi-pending ahi) nil))

;;; ================================================================
;;; The audio mode database (function interface: any open handle)
;;; ================================================================

(defconstant +invalid-id+ (logand raw:+ahi-invalid-id+ #xFFFFFFFF)
  "AHI_INVALID_ID as the unsigned 32-bit value the functions return.")

(defun %invalid-id-p (id)
  (= (logand id #xFFFFFFFF) +invalid-id+))

(defun audio-modes ()
  "Every audio mode ID in AHI's database -- the modes of the installed
drivers, in AHI's order.  AUDIO-MODE-NAME / AUDIO-MODE-INFO describe one."
  (%need-base)
  (loop with id = +invalid-id+
        do (setf id (raw:ahi-next-audio-id id))
        until (%invalid-id-p id)
        collect id))

(defun %mode-attr (id tag)
  "The ULONG AHI_GetAudioAttrsA stores for TAG about mode ID, or NIL."
  (let ((cell (ffi:alloc-foreign 4)))
    (unwind-protect
         (amiga.ffi:with-tag-list (tags tag cell)
           (when (raw:ahi-get-audio-attrs-a id 0 tags)
             (ffi:peek-u32 cell)))
      (ffi:free-foreign cell))))

(defun %mode-string (id tag)
  "The text AHI_GetAudioAttrsA copies for TAG about mode ID, or NIL."
  (let ((buf (ffi:alloc-foreign 64)))
    (unwind-protect
         (amiga.ffi:with-tag-list (tags raw:+ahidb-buffer-len+ 64 tag buf)
           (when (raw:ahi-get-audio-attrs-a id 0 tags)
             (ffi:foreign-to-string buf 64)))
      (ffi:free-foreign buf))))

(defun audio-mode-name (id)
  "The name of audio mode ID as the AHI preferences show it
\(\"Paula:8 bit stereo++\"), or NIL for an unknown ID."
  (%need-base)
  (%mode-string id raw:+ahidb-name+))

(defun audio-mode-info (id)
  "A plist describing audio mode ID: :ID, :NAME, :DRIVER, :BITS (output
bits), :MAX-CHANNELS, :MIN-MIX-FREQ / :MAX-MIX-FREQ (Hz) and the
booleans :STEREO, :PANNING, :HIFI, :VOLUME, :REALTIME, :RECORD."
  (%need-base)
  (flet ((num (tag) (%mode-attr id tag))
         (bool (tag) (let ((v (%mode-attr id tag))) (and v (not (zerop v))))))
    (list :id id
          :name (%mode-string id raw:+ahidb-name+)
          :driver (%mode-string id raw:+ahidb-driver+)
          :bits (num raw:+ahidb-bits+)
          :max-channels (num raw:+ahidb-max-channels+)
          :min-mix-freq (num raw:+ahidb-min-mix-freq+)
          :max-mix-freq (num raw:+ahidb-max-mix-freq+)
          :stereo (bool raw:+ahidb-stereo+)
          :panning (bool raw:+ahidb-panning+)
          :hifi (bool raw:+ahidb-hi-fi+)
          :volume (bool raw:+ahidb-volume+)
          :realtime (bool raw:+ahidb-realtime+)
          :record (bool raw:+ahidb-record+))))

(defun best-audio-mode (&key (stereo nil stereo-p) (panning nil panning-p)
                             (hifi nil hifi-p) (volume nil volume-p)
                             (record nil record-p) (realtime t)
                             bits max-channels)
  "The audio mode ID AHI_BestAudioIDA picks for the requirements given,
or NIL when no installed mode qualifies.  A boolean key given as T
demands the feature, as NIL forbids it, not given leaves it open; BITS
and MAX-CHANNELS are minimums.  REALTIME defaults to T (a mode that
plays as the program drives it -- the file-writing modes are not)."
  (%need-base)
  (let ((pairs '()))
    (flet ((bool (tag value) (push tag pairs) (push (if value 1 0) pairs)))
      ;; (1 for TRUE: AHI 4.158 and earlier compare against exactly 1)
      (when stereo-p (bool raw:+ahidb-stereo+ stereo))
      (when panning-p (bool raw:+ahidb-panning+ panning))
      (when hifi-p (bool raw:+ahidb-hi-fi+ hifi))
      (when volume-p (bool raw:+ahidb-volume+ volume))
      (when record-p (bool raw:+ahidb-record+ record))
      (bool raw:+ahidb-realtime+ realtime)
      (when bits (push raw:+ahidb-bits+ pairs) (push bits pairs))
      (when max-channels (push raw:+ahidb-max-channels+ pairs) (push max-channels pairs)))
    (let* ((tags (amiga.ffi:make-tag-list (nreverse pairs)))
           (id (unwind-protect (raw:ahi-best-audio-ida tags)
                 (ffi:free-foreign tags))))
      (if (%invalid-id-p id) nil id))))

;;; ================================================================
;;; The low-level API -- AHI's mixer, driven from the program, no hooks
;;; ================================================================

(defun %null-p (p)
  (or (null p) (and (ffi:foreign-pointer-p p) (ffi:null-pointer-p p))))

(defun alloc-audio (&key (audio-id raw:+ahi-default-id+) mix-freq
                         (channels 4) (sounds 8))
  "AHI_AllocAudioA: AHI's mixer on the audio mode AUDIO-ID (default the
user's preferred mode, AHI_DEFAULT_ID; see BEST-AUDIO-MODE) with
CHANNELS channels and room for SOUNDS sounds, mixing at MIX-FREQ Hz
when given (else the user's rate).  Returns the AHIAudioCtrl as a
foreign pointer, or NIL -- no such mode, too many channels, or the
hardware is taken: a unit opened through the device interface holds
it (on Paula the whole of it), so run the low-level API from a handle
opened with :UNIT :NONE, as the AHI autodoc asks.  Silent until
START-PLAYBACK; FREE-AUDIO ends it.  No hook is installed: the channels
are driven with PLAY, SET-VOLUME, SET-FREQUENCY and SET-SOUND from the
program."
  (%need-base)
  (let ((pairs (list raw:+ahia-audio-id+ audio-id
                     raw:+ahia-channels+ channels
                     raw:+ahia-sounds+ sounds)))
    (when mix-freq
      (setf pairs (append pairs (list raw:+ahia-mix-freq+ mix-freq))))
    (let* ((tags (amiga.ffi:make-tag-list pairs))
           (ctrl (unwind-protect (raw:ahi-alloc-audio-a tags)
                   (ffi:free-foreign tags))))
      (if (%null-p ctrl) nil ctrl))))

(defun free-audio (ctrl)
  "AHI_FreeAudio: stop and release the mixer CTRL (its sounds too).  Returns NIL."
  (%need-base)
  (raw:ahi-free-audio ctrl)
  nil)

(defmacro with-audio-ctrl ((var &rest alloc-args) &body body)
  "ALLOC-AUDIO (ALLOC-ARGS as for it), bind the AHIAudioCtrl to VAR,
FREE-AUDIO on exit.  Signals when the allocation fails."
  `(let ((,var (alloc-audio ,@alloc-args)))
     (unless ,var
       (error "AMIGA.AHI: AHI_AllocAudio failed -- no such audio mode, too many channels for it, or its hardware is taken (a unit open through the device interface holds it: use a handle opened with :UNIT :NONE for the low-level API)"))
     (unwind-protect (progn ,@body)
       (free-audio ,var))))

(defun load-sound (ctrl sound data frames &key (type :mono8))
  "AHI_LoadSound: make the FRAMES sample frames at DATA (a foreign
pointer, any memory -- untouched until UNLOAD-SOUND, a driver may have
uploaded it) sound number SOUND (0 below the :SOUNDS of ALLOC-AUDIO)
of CTRL, of the sample TYPE.  Returns T, or NIL and the AHIE_* code."
  (%need-base)
  (let ((info (ffi:alloc-foreign raw:*ahi-sample-info-size*)))
    (unwind-protect
         (progn
           (setf (raw:ahi-sample-info-type info) (sample-type-code type))
           (setf (raw:ahi-sample-info-address info) data)
           (setf (raw:ahi-sample-info-length info) frames)
           (let ((err (raw:ahi-load-sound sound raw:+ahist-sample+ info ctrl)))
             (if (zerop err) t (values nil err))))
      (ffi:free-foreign info))))

(defun unload-sound (ctrl sound)
  "AHI_UnloadSound: forget sound number SOUND of CTRL.  Returns NIL."
  (%need-base)
  (raw:ahi-unload-sound sound ctrl)
  nil)

(defun %control (ctrl tag value)
  (amiga.ffi:with-tag-list (tags tag value)
    (zerop (raw:ahi-control-audio-a ctrl tags))))

(defun start-playback (ctrl)
  "AHI_ControlAudio AHIC_Play TRUE: start the mixer CTRL.  T on success."
  (%need-base)
  (%control ctrl raw:+ahic-play+ 1))

(defun stop-playback (ctrl)
  "AHI_ControlAudio AHIC_Play FALSE: stop the mixer CTRL.  T on success."
  (%need-base)
  (%control ctrl raw:+ahic-play+ 0))

(defun play (ctrl channel &key (sound nil sound-p) (offset 0) length
                               frequency volume pan
                               loop-sound (loop-offset 0) loop-length
                               loop-frequency loop-volume loop-pan)
  "AHI_PlayA: set up CHANNEL of CTRL in one go.  SOUND (a LOAD-SOUND
number; NIL silences the channel) plays from sample frame OFFSET for
LENGTH frames (default: to its end) at FREQUENCY Hz, VOLUME 0..1, PAN
0..1 (a key not given keeps the channel's current value -- a SOUND
needs a FREQUENCY, and VOLUME and PAN go together: AHI takes a missing
one as 0).  LOOP-SOUND and its LOOP- keys say what plays after it,
each defaulting to the first's.  Returns NIL."
  (%need-base)
  (when (and sound-p sound (null frequency))
    (error "AMIGA.AHI: PLAY: a sound needs its :FREQUENCY (Hz)"))
  (when (and (plusp offset) (null length))
    (error "AMIGA.AHI: PLAY: an :OFFSET needs a :LENGTH (AHIP_Offset without AHIP_Length is undefined)"))
  (when (and (or volume pan) (not (and volume pan)))
    (error "AMIGA.AHI: PLAY: give :VOLUME and :PAN together (AHI takes a missing one as 0)"))
  (let ((pairs (list raw:+ahip-begin-channel+ channel)))
    (flet ((add (tag v) (setf pairs (append pairs (list tag v)))))
      (when frequency (add raw:+ahip-freq+ frequency))
      (when volume (add raw:+ahip-vol+ (to-fixed volume)))
      (when pan (add raw:+ahip-pan+ (to-fixed pan)))
      (when sound-p (add raw:+ahip-sound+ (or sound raw:+ahi-nosound+)))
      (when length
        (add raw:+ahip-offset+ offset)
        (add raw:+ahip-length+ length))
      (when loop-sound
        (when loop-frequency (add raw:+ahip-loop-freq+ loop-frequency))
        (when loop-volume (add raw:+ahip-loop-vol+ (to-fixed loop-volume)))
        (when loop-pan (add raw:+ahip-loop-pan+ (to-fixed loop-pan)))
        (add raw:+ahip-loop-sound+ loop-sound)
        (when loop-length
          (add raw:+ahip-loop-offset+ loop-offset)
          (add raw:+ahip-loop-length+ loop-length)))
      (add raw:+ahip-end-channel+ 0))
    (let ((tags (amiga.ffi:make-tag-list pairs)))
      (unwind-protect (raw:ahi-play-a ctrl tags)
        (ffi:free-foreign tags))))
  nil)

(defun %set-flags (immediate)
  (if immediate raw:+ahisf-imm+ raw:+ahisf-none+))

(defun set-volume (ctrl channel volume &key (pan 0.5) immediate)
  "AHI_SetVol: CHANNEL of CTRL to VOLUME 0..1 at PAN 0..1; IMMEDIATE
applies it now instead of at the next sample boundary.  Returns NIL."
  (%need-base)
  (raw:ahi-set-vol channel (to-fixed volume) (to-fixed pan) ctrl
                   (%set-flags immediate))
  nil)

(defun set-frequency (ctrl channel hz &key immediate)
  "AHI_SetFreq: CHANNEL of CTRL plays at HZ (0 pauses it).  Returns NIL."
  (%need-base)
  (raw:ahi-set-freq channel hz ctrl (%set-flags immediate))
  nil)

(defun set-sound (ctrl channel sound &key (offset 0) (length 0) immediate)
  "AHI_SetSound: CHANNEL of CTRL plays SOUND (NIL: silence) from sample
frame OFFSET for LENGTH frames (0: to the end).  Returns NIL."
  (%need-base)
  (raw:ahi-set-sound channel (or sound raw:+ahi-nosound+) offset length ctrl
                     (%set-flags immediate))
  nil)

;;; ================================================================
;;; Provide module
;;; ================================================================

(provide "amiga/ahi")
