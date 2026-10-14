;;; amiga/audio.lisp — audio.device abstractions for AmiLisp
;;;
;;; Loaded via (require "amiga/audio").
;;; Allocates a Paula channel through audio.device and plays 8-bit
;;; signed samples from chip RAM.  Playback is strictly non-blocking:
;;; a sound is started with SendIO and reclaimed with CheckIO/AbortIO
;;; — WAIT-IO is only ever issued once the request is complete or
;;; aborted, so no call here stalls the caller (or the stop-the-world
;;; GC) for the duration of a sample.
;;;
;;; See tests/amiga/test-audio.lisp for usage end-to-end.

;; compile-time too: COMPILE-FILE (the host builds the lib/amiga FASLs) must see
;; these packages at read time, not only LOAD.
(eval-when (:compile-toplevel :load-toplevel :execute)
  (require "amiga/ffi")
  (require "amiga/exec"))

(defpackage "AMIGA.AUDIO"
  (:use "CL" "FFI" "AMIGA.FFI")
  (:export
   ;; Channel handle
   "OPEN-AUDIO" "CLOSE-AUDIO" "WITH-AUDIO"
   "AUDIO-CHANNEL-MASK"
   ;; Playback
   "PLAY-SAMPLE" "STOP-SAMPLE" "PLAYING-P"
   ;; Helpers
   "PERIOD-FOR-RATE"
   "+MAX-VOLUME+" "+MAX-SAMPLE-BYTES+" "+MAX-PRECEDENCE+"))

(in-package "AMIGA.AUDIO")

;;; ================================================================
;;; struct IOAudio (devices/audio.h) — 68 bytes.
;;; The embedded IORequest fields the module touches, then the
;;; audio-specific tail.  m68k struct alignment is 2 bytes, so the
;;; ioa_Data pointer really does sit at offset 34.
;;; ================================================================

(ffi:defcstruct ioaudio
  (ln-pri    :i8      9)   ; allocation precedence (message node priority)
  (unit      :u32    24)   ; allocated channel mask, filled by OpenDevice
  (command   :u16    28)
  (flags     :u8     30)
  (error     :i8     31)
  (alloc-key :u16    32)
  (data      :u32    34)   ; sample/channel-map address -- poke the integer address
  (length    :u32    38)
  (period    :u16    42)
  (volume    :u16    44)
  (cycles    :u16    46))

(defconstant +io-audio-size+ 68)

;;; Commands (exec/io.h, devices/audio.h)
(defconstant +cmd-write+ 3)

;;; io_Flags bits (devices/audio.h)
(defconstant +adiof-pervol+ (ash 1 4))  ; take ioa_Period/ioa_Volume from the request
(defconstant +adiof-nowait+ (ash 1 6))  ; fail allocation instead of queueing for a channel

(defconstant +max-volume+ 64)

(defconstant +max-precedence+ 127
  "ADALLOC_MAXPREC -- the highest allocation precedence; a channel held
at it cannot be stolen (nothing can steal a channel of equal or greater
precedence).  On m68k PLAY-SAMPLE writes the channel's Paula period and
volume registers by hand (this AmigaOS ignores ADIOF_PERVOL), and the
audio.device autodoc is explicit that a caller storing directly to the
hardware registers must lock the channel or hold it at max precedence so
it cannot be stolen out from under the poke.  OPEN-AUDIO takes the latter
road -- a field on the allocation, no extra (and, on this device,
hang-prone) synchronous command.")

;;; Paula's per-channel length register counts 16-bit words, so one
;;; request carries at most 65536 words of sample data (NDK audio.doc:
;;; CMD_WRITE's ioa_Length is valid "2 thru 131072, must be even number").
(defconstant +max-sample-bytes+ 131072)

;;; PAL colour clock — period = clock / sample rate.  (NTSC differs by
;;; 0.9%, far below what an 8-bit sample effect can carry.)
(defconstant +pal-clock+ 3546895)

(defun period-for-rate (rate)
  "Paula period for a sample RATE in Hz (PAL clock).  Clamped to the
hardware minimum of 124 (about 28.6 kHz)."
  (max 124 (round +pal-clock+ rate)))

;;; ================================================================
;;; The audio handle — one allocated channel, one reusable IOAudio.
;;; The exec device-I/O calls (OpenDevice, the request and port
;;; allocators, SendIO/CheckIO/WaitIO/AbortIO) are AMIGA.EXEC's.
;;; ================================================================

(defstruct (audio (:constructor %make-audio))
  port           ; MsgPort the device replies to
  io             ; the IOAudio request (foreign pointer)
  (pending nil)) ; T while a CMD_WRITE is out with the device

(defun audio-channel-mask (audio)
  "The Paula channel mask (1, 2, 4 or 8) OpenDevice allocated."
  (ioaudio-unit (audio-io audio)))

(defun open-audio (&key (precedence 0))
  "Allocate one Paula channel through audio.device and return an AUDIO
handle, or NIL if no channel (or no audio.device) is available.
PRECEDENCE (-128..127) is the allocation priority; equal-or-louder
users can steal the channel back.  Never blocks: allocation runs with
ADIOF_NOWAIT.

On m68k PRECEDENCE is a hint only: because PLAY-SAMPLE pokes the
channel's Paula registers directly there, the channel is always
allocated at +MAX-PRECEDENCE+ so it cannot be stolen (see that
constant).  Off m68k there is no poke and PRECEDENCE is honoured as
given."
  (let ((port nil) (io nil) (chanmap nil) (ok nil))
    (unwind-protect
        (progn
          (setf port (amiga.exec:create-msg-port))
          (unless port (return-from open-audio nil))
          (setf io (amiga.exec:create-io-request port +io-audio-size+))
          (unless io (return-from open-audio nil))
          ;; OpenDevice on audio.device doubles as ADCMD_ALLOCATE when
          ;; ioa_Length is non-zero: ioa_Data points at an array of
          ;; acceptable channel masks, one byte each — any single
          ;; channel suits us.
          (setf chanmap (ffi:alloc-foreign 4))
          (ffi:poke-bytes chanmap #(1 2 4 8))
          ;; m68k pokes this channel's Paula registers in PLAY-SAMPLE, so
          ;; it must be un-stealable — hold it at max precedence and let the
          ;; caller's softer hint go.  Off m68k, honour PRECEDENCE as asked.
          ;; Checked against *FEATURES* at runtime, not #+m68k: this file
          ;; ships as a host-precompiled FASL (see scripts/make-binary-
          ;; release.sh), and a reader conditional would bake in whichever
          ;; branch the host compiling it resolved, not the machine that
          ;; later loads it.
          (setf (ioaudio-ln-pri io)
                (logand (if (find :m68k *features*)
                            +max-precedence+
                            precedence)
                        #xFF))
          (setf (ioaudio-flags io) +adiof-nowait+)
          (setf (ioaudio-data io) (ffi:foreign-pointer-address chanmap))
          (setf (ioaudio-length io) 4)
          (unless (zerop (amiga.exec:open-device "audio.device" 0 io 0))
            (return-from open-audio nil))
          (setf ok t)
          (%make-audio :port port :io io))
      ;; The channel map is only read inside OpenDevice; the partial
      ;; plumbing only survives a success.
      (when chanmap (ffi:free-foreign chanmap))
      (unless ok
        (when io (amiga.exec:delete-io-request io))
        (when port (amiga.exec:delete-msg-port port))))))

(defun %reclaim (audio)
  "Take the in-flight request back from the device, aborting it if it
is still playing.  WAIT-IO here only ever completes an already done or
just-aborted request, so the wait is bounded and effectively instant."
  (when (audio-pending audio)
    (let ((io (audio-io audio)))
      (unless (amiga.exec:check-io io)
        (amiga.exec:abort-io io))
      (amiga.exec:wait-io io))
    (setf (audio-pending audio) nil)))

(defun playing-p (audio)
  "True while the last PLAY-SAMPLE is still sounding.  Reclaims the
request as a side effect once the device has finished with it."
  (when (audio-pending audio)
    (if (amiga.exec:check-io (audio-io audio))
        (progn (%reclaim audio) nil)
        t)))

(defun stop-sample (audio)
  "Silence the channel: abort any in-flight write."
  (%reclaim audio)
  nil)

;;; Period/volume without trusting ADIOF_PERVOL.
;;;
;;; The flag is meant to make audio.device take ioa_Period and ioa_Volume
;;; from the CMD_WRITE request.  On AmigaOS 3.2 it does not: the device
;;; ignores both and plays the channel at whatever its Paula period and
;;; volume registers already hold — and a freshly allocated channel holds
;;; volume 0, so the very first write on it is silent.  Verified by ear
;;; and by playback timing on FS-UAE and on real Vampire/AOS 3.2 hardware:
;;; a request carrying period 443 and volume 64 played at the wrong period
;;; and dead silent until AUDxVOL was set by hand, after which a request
;;; with NO flag played correctly.  So on m68k, PLAY-SAMPLE sets the
;;; channel's Paula registers itself just before the write; the flag stays
;;; set too, harmless on any device that does honour it.  The PPC ports
;;; have no Paula at all, so this is m68k-only and they keep the flag path.

(defun %channel-reg-base (mask)
  "Byte offset (add to #xDFF000) of the Paula register block for the
channel OpenDevice allocated, given ioa_Unit MASK (1, 2, 4 or 8): AUD0 at
#xA0, then +16 per channel.  NIL for a mask that names no single channel."
  (let ((ch (position mask '(1 2 4 8))))
    (when ch (+ #xA0 (* ch 16)))))

;;; Defined on every platform (it is only FFI pokes) but called just
;;; under a runtime m68k guard in PLAY-SAMPLE — never gated with #+m68k,
;;; so the one host-compiled lib/amiga FASL keeps it for the m68k target
;;; that loads it (same reason as OPEN-AUDIO's precedence).
(defun %poke-channel-pervol (mask period volume)
  "Write AUDxPER and AUDxVOL for channel MASK straight to the custom
registers (AUDxPER at block+6, AUDxVOL at block+8).  Meaningful only on
m68k (Paula); PLAY-SAMPLE calls it only there."
  (let ((base (%channel-reg-base mask)))
    (when base
      (let ((custom (ffi:make-foreign-pointer #xDFF000 #x200)))
        (ffi:poke-u16 custom period (+ base 6))
        (ffi:poke-u16 custom volume (+ base 8))))))

(defun play-sample (audio chip-data length &key (period 443)
                                                (volume +max-volume+)
                                                (cycles 1))
  "Start LENGTH bytes of signed 8-bit sample data at CHIP-DATA (a
chip-RAM pointer, e.g. from AMIGA.EXEC:ALLOC-CHIP-BYTES) on the
handle's channel.  Any sound still playing is cut off first.  PERIOD
is the Paula period (see PERIOD-FOR-RATE), VOLUME 0..64, CYCLES the
repeat count (0 loops until STOP-SAMPLE).  Returns immediately; T when
the write was queued, NIL if the device rejected it."
  (when (or (oddp length) (< length 2) (> length +max-sample-bytes+))
    (error "PLAY-SAMPLE: length must be even and in 2..~D: ~D"
           +max-sample-bytes+ length))
  (%reclaim audio)
  (let ((io (audio-io audio)))
    (setf (ioaudio-command io) +cmd-write+)
    (setf (ioaudio-flags io) +adiof-pervol+)
    (setf (ioaudio-data io) (ffi:foreign-pointer-address chip-data))
    (setf (ioaudio-length io) length)
    (setf (ioaudio-period io) period)
    (setf (ioaudio-volume io) (min +max-volume+ (max 0 volume)))
    (setf (ioaudio-cycles io) cycles)
    ;; this AmigaOS ignores ADIOF_PERVOL — set the channel's Paula
    ;; period/volume ourselves so the write is not silent (see above).
    ;; Runtime *FEATURES*, not #+m68k: this file ships as one host-compiled
    ;; FASL, so the m68k branch must survive to the m68k machine that loads
    ;; it (a #+m68k would resolve on the host that built the FASL).
    (when (find :m68k *features*)
      (%poke-channel-pervol (ioaudio-unit io) period
                            (min +max-volume+ (max 0 volume))))
    (amiga.exec:send-io io)
    (setf (audio-pending audio) t)
    ;; SendIO has no return value; a rejected request comes back
    ;; completed with io_Error set.
    (if (and (amiga.exec:check-io io)
             (not (zerop (ioaudio-error io))))
        (progn (%reclaim audio) nil)
        t)))

(defun close-audio (audio)
  "Stop playback, free the channel and all request plumbing."
  (%reclaim audio)
  (amiga.exec:close-device (audio-io audio))
  (amiga.exec:delete-io-request (audio-io audio))
  (amiga.exec:delete-msg-port (audio-port audio))
  (setf (audio-io audio) nil (audio-port audio) nil)
  nil)

(defmacro with-audio ((var &rest open-args) &body body)
  "Open an audio channel, bind the handle to VAR, close on exit.
Signals an error when no channel is available."
  `(let ((,var (open-audio ,@open-args)))
     (unless ,var (error "Cannot allocate an audio.device channel"))
     (unwind-protect (progn ,@body)
       (close-audio ,var))))

;;; ================================================================
;;; Provide module
;;; ================================================================

(provide "amiga/audio")
