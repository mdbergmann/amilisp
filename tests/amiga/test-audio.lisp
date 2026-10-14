;;; test-audio.lisp — audio.device tests (AMIGA.AUDIO)
;;;
;;; Loaded from run-tests.lisp via (load ...) inside #+amigaos.
;;; Must be a separate file because the reader needs the AMIGA.AUDIO
;;; package to exist before it can read the qualified symbols.
;;;
;;; NOTE: depends on the CHECK macro defined in run-tests.lisp.  The
;;; FASL system only invalidates this file's cache when *this* file's
;;; mtime changes, not when CHECK's definition changes — so if CHECK's
;;; expansion changes, touch this file too or you'll get "Undefined
;;; function: CHECK" at FASL load.

(require "amiga/audio")

(check "audio-package-exists" "AMIGA.AUDIO"
  (package-name (find-package "AMIGA.AUDIO")))

; --- Pure helpers ---
(check "audio-period-for-8kHz" 443 (amiga.audio:period-for-rate 8000))
(check "audio-period-clamps-to-hw-min" 124 (amiga.audio:period-for-rate 100000))
(check "audio-max-volume" 64 amiga.audio:+max-volume+)

; --- Delay via dos.library, so polling below is paced, not spinning ---
(defun audio-test-delay (ticks)
  (amiga.ffi:with-library (dos "dos.library")
    (amiga:call-library dos -198 (list :d1 ticks))))

; A second of square wave at 8 kHz: 50 samples up, 50 down (~80 Hz),
; as unsigned bytes carrying two's-complement signed values.
(defparameter *audio-test-wave*
  (let ((v (make-array 8000 :element-type '(unsigned-byte 8))))
    (dotimes (i 8000 v)
      (setf (aref v i) (if (evenp (floor i 50)) 100 156)))))

(defparameter *audio-test-chip* (amiga.exec:alloc-chip-bytes *audio-test-wave*))

; A buffer sized to the exact CMD_WRITE boundary (NDK audio.doc:
; ioa_Length valid "2 thru 131072, must be even number") so the
; length check can be probed right at the edge, not just past it.
(defparameter *audio-test-max-wave*
  (make-array amiga.audio:+max-sample-bytes+ :element-type '(unsigned-byte 8)
              :initial-element 128))

(defparameter *audio-test-max-chip*
  (amiga.exec:alloc-chip-bytes *audio-test-max-wave*))

; --- Paula register math for the ADIOF_PERVOL workaround ---
; This AmigaOS's audio.device ignores ioa_Period/ioa_Volume in a
; CMD_WRITE (verified on FS-UAE and real Vampire/AOS 3.2), so
; PLAY-SAMPLE sets the channel's Paula registers itself.
; %CHANNEL-REG-BASE is the offset of a channel's register block from
; #xDFF000: AUD0 at #xA0, then +16 per channel; NIL for a mask naming
; no single channel.  Pure arithmetic, so it is checked directly.
(check "audio-reg-base-ch0" #xA0 (amiga.audio::%channel-reg-base 1))
(check "audio-reg-base-ch1" #xB0 (amiga.audio::%channel-reg-base 2))
(check "audio-reg-base-ch2" #xC0 (amiga.audio::%channel-reg-base 4))
(check "audio-reg-base-ch3" #xD0 (amiga.audio::%channel-reg-base 8))
(check "audio-reg-base-multi-mask-nil" nil (amiga.audio::%channel-reg-base 3))
(check "audio-reg-base-zero-mask-nil" nil (amiga.audio::%channel-reg-base 0))

; MorphOS machines have no Paula and may lack audio.device entirely;
; OPEN-AUDIO then returns NIL cleanly.  Skip the device tests there
; instead of failing them — on m68k a NIL open is a real regression.
; (The device checks below run PLAY-SAMPLE on a freshly opened channel —
; the exact scenario the register workaround fixes — so they also
; exercise %POKE-CHANNEL-PERVOL on m68k.)
(defparameter *audio-available* nil)

(let ((audio (amiga.audio:open-audio)))
  (setq *audio-available* (not (null audio)))
  (cond (audio (check "audio-open" t t))
        (t #+m68k (check "audio-open" t nil)
           #-m68k (format t "SKIP: audio.device unavailable on this port~%")))
  (when audio
    (check "audio-channel-mask-is-one-channel" t
      (not (null (member (amiga.audio:audio-channel-mask audio) '(1 2 4 8)))))

    ; A channel whose Paula registers PLAY-SAMPLE pokes must be
    ; un-stealable (audio.device autodoc: direct register writes require a
    ; locked or max-precedence channel).  On m68k OPEN-AUDIO pins any
    ; allocation to +MAX-PRECEDENCE+ and lets the caller's softer hint go;
    ; off m68k there is no poke, so the hint is honoured as given.  ln_Pri
    ; is the message node's priority (IOAudio offset 9), the field
    ; OpenDevice/ADCMD_ALLOCATE reads for the allocation precedence.
    (let ((lowpri (amiga.audio:open-audio :precedence -50)))
      (when lowpri
        (check "audio-precedence-pinned"
               #+m68k amiga.audio:+max-precedence+ #-m68k -50
               (amiga.audio::ioaudio-ln-pri (amiga.audio::audio-io lowpri)))
        (amiga.audio:close-audio lowpri)))

    ; Full second queued: still audibly playing right after SendIO.
    (check "audio-play-sample-queues" t
      (amiga.audio:play-sample audio *audio-test-chip* 8000
                               :period (amiga.audio:period-for-rate 8000)))
    (check "audio-playing-after-start" t (amiga.audio:playing-p audio))
    (check "audio-stop-sample" nil (amiga.audio:stop-sample audio))
    (check "audio-not-playing-after-stop" nil (amiga.audio:playing-p audio))

    ; A short one-shot (~50 ms) completes on its own within 2 s of polling.
    #+m68k
    (check "audio-short-sample-completes" t
      (progn
        (amiga.audio:play-sample audio *audio-test-chip* 400)
        (let ((done nil))
          (dotimes (i 100)
            (unless (amiga.audio:playing-p audio)
              (setq done t)
              (return))
            (audio-test-delay 1))
          done)))
    ; MorphOS has no Paula; its audio.device emulation queues, plays and
    ; aborts CMD_WRITEs correctly (all verified by the other checks) but
    ; has been observed never to complete a finite one-shot on its own —
    ; CheckIO stays busy indefinitely.  Probe with a 10 s budget so the
    ; check self-reinstates if the emulation ever gains completion, and
    ; SKIP (not FAIL) when it doesn't: the timing is the emulation's
    ; property, not amilisp's.
    #-m68k
    (progn
      (amiga.audio:play-sample audio *audio-test-chip* 400)
      (let ((done nil))
        (dotimes (i 500)
          (unless (amiga.audio:playing-p audio)
            (setq done t)
            (return))
          (audio-test-delay 1))
        (if done
            (check "audio-short-sample-completes" t t)
            (progn
              (format t "SKIP: audio-short-sample-completes (one-shot CMD_WRITE still pending after 10 s -- audio.device emulation completion quirk)~%")
              (amiga.audio:stop-sample audio)))))

    ; CYCLES 0 loops until stopped: still playing long after 400 bytes
    ; (~50 ms) would have run out.
    (check "audio-cycles-zero-loops" t
      (progn
        (amiga.audio:play-sample audio *audio-test-chip* 400 :cycles 0)
        (audio-test-delay 25)
        (prog1 (amiga.audio:playing-p audio)
          (amiga.audio:stop-sample audio))))

    ; Odd or oversized lengths are rejected before touching the device.
    (check "audio-odd-length-errors" t
      (handler-case
          (progn (amiga.audio:play-sample audio *audio-test-chip* 401) nil)
        (error () t)))
    (check "audio-oversized-length-errors" t
      (handler-case
          (progn (amiga.audio:play-sample
                  audio *audio-test-max-chip*
                  (+ amiga.audio:+max-sample-bytes+ 2))
                 nil)
        (error () t)))

    ; The boundary itself (NDK audio.doc's documented max, 131072) is
    ; accepted, not rejected as out of range.
    (check "audio-max-sample-bytes-is-131072" 131072 amiga.audio:+max-sample-bytes+)
    (check "audio-max-length-accepted" t
      (prog1 (amiga.audio:play-sample audio *audio-test-max-chip*
                                       amiga.audio:+max-sample-bytes+)
        (amiga.audio:stop-sample audio)))

    ; A second handle gets a different channel; both close cleanly.
    ; Paula has four channels and audio.device steals only from a LOWER
    ; precedence, so a second precedence-0 allocation lands on a free one.
    ; MorphOS's audio.device emulation has been observed (3.20) to hand a
    ; precedence-0 request the very channel a precedence-0 holder still
    ; has, once another allocation came and went (the -50 probe above):
    ; OpenDevice returns 0 with io_Error 0 and the same ioa_AllocKey
    ; channel -- the emulation's bookkeeping, not amilisp's.  Off m68k a
    ; second handle on the same channel is therefore a SKIP, not a FAIL;
    ; a failed open or a multi-channel mask still fails everywhere, and
    ; the check reinstates itself where the emulation allocates like Paula.
    (let* ((audio2 (amiga.audio:open-audio))
           (mask2 (and audio2 (amiga.audio:audio-channel-mask audio2)))
           (single (and mask2 (member mask2 '(1 2 4 8)) t)))
      (cond ((and single (/= mask2 (amiga.audio:audio-channel-mask audio)))
             (check "audio-second-channel" t t))
            #-m68k
            (single
             (format t "SKIP: audio-second-channel (second handle got channel mask ~D, the first handle's -- audio.device emulation channel bookkeeping)~%"
                     mask2))
            (t (check "audio-second-channel" t nil)))
      (when audio2 (amiga.audio:close-audio audio2)))

    (check "audio-close" nil (amiga.audio:close-audio audio))))

; Channel freed on close: reopening still finds one, WITH-AUDIO cleans up.
(when *audio-available*
  (check "audio-with-audio-reopens" t
    (amiga.audio:with-audio (a)
      (not (null (member (amiga.audio:audio-channel-mask a) '(1 2 4 8)))))))

(amiga:free-chip *audio-test-chip*)
(amiga:free-chip *audio-test-max-chip*)
