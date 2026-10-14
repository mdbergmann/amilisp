;;; test-ahi.lisp — ahi.device tests (AMIGA.AHI, the opt-in AHI module)
;;;
;;; Loaded from run-tests.lisp via (load ...) inside #+amigaos.
;;; Must be a separate file because the reader needs the AMIGA.AHI
;;; package to exist before it can read the qualified symbols.
;;;
;;; NOTE: depends on the CHECK macro defined in run-tests.lisp (same
;;; FASL-cache caveat as test-audio.lisp: touch this file when CHECK's
;;; expansion changes).
;;;
;;; AHI is opt-in and may well be absent: a bare AmigaOS 3.x install has
;;; no ahi.device (the FS-UAE Workbench of the harness carries AHI 4.18
;;; with the Paula driver and a prefs file putting Paula on unit 0;
;;; MorphOS ships AHI).  The pure checks always run; the device checks
;;; SKIP -- never FAIL -- when OPEN-AHI returns NIL, on every platform:
;;; a missing AHI is the machine's state, not amilisp's bug.

(require "amiga/ahi")

(check "ahi-package-exists" "AMIGA.AHI"
  (package-name (find-package "AMIGA.AHI")))

; --- Pure helpers ---
(check "ahi-to-fixed-one" #x10000 (amiga.ahi:to-fixed 1.0))
(check "ahi-to-fixed-half" #x8000 (amiga.ahi:to-fixed 1/2))
(check "ahi-to-fixed-zero" 0 (amiga.ahi:to-fixed 0))
(check "ahi-from-fixed-half" 1/2 (amiga.ahi:from-fixed #x8000))
(check "ahi-frame-size-mono8" 1 (amiga.ahi:sample-frame-size :mono8))
(check "ahi-frame-size-mono16" 2 (amiga.ahi:sample-frame-size :mono16))
(check "ahi-frame-size-stereo8" 2 (amiga.ahi:sample-frame-size :stereo8))
(check "ahi-frame-size-stereo16" 4 (amiga.ahi:sample-frame-size :stereo16))
(check "ahi-type-code-stereo16" amiga.raw.ahi:+ahist-s16-s+
  (amiga.ahi:sample-type-code :stereo16))
(check "ahi-type-code-passthrough" amiga.raw.ahi:+ahist-m16-s+
  (amiga.ahi:sample-type-code amiga.raw.ahi:+ahist-m16-s+))
(check "ahi-unknown-type-errors" t
  (handler-case (progn (amiga.ahi:sample-type-code :quad) nil)
    (error () t)))
(check "ahi-sample-bytes-8" '(0 127 128 255)
  (coerce (amiga.ahi:sample-bytes #(0 127 -128 -1) :bits 8) 'list))
(check "ahi-sample-bytes-16-big-endian" '(0 1 255 255 128 0)
  (coerce (amiga.ahi:sample-bytes #(1 -1 -32768) :bits 16) 'list))
(check "ahi-default-unit" 0 amiga.ahi:+ahi-default-unit+)
(check "ahi-no-unit" 255 amiga.ahi:+ahi-no-unit+)

; The argument checks run before any device is touched, so a blank
; handle probes them without AHI.
(check "ahi-blank-handle-not-open" nil (amiga.ahi:ahi-open-p (amiga.ahi::%make-ahi)))
(check "ahi-odd-length-for-16-bit-errors" t
  (handler-case
      (progn (amiga.ahi:play-sample (amiga.ahi::%make-ahi) (ffi:make-foreign-pointer 0)
                                    401 :type :mono16)
             nil)
    (error () t)))
(check "ahi-zero-length-errors" t
  (handler-case
      (progn (amiga.ahi:play-sample (amiga.ahi::%make-ahi) (ffi:make-foreign-pointer 0) 0)
             nil)
    (error () t)))
(check "ahi-closed-handle-errors" t
  (handler-case (progn (amiga.ahi:playing-p (amiga.ahi::%make-ahi)) nil)
    (error () t)))
; nothing has opened ahi.device yet: the function interface says so
(check "ahi-unarmed-function-interface-errors" t
  (handler-case (progn (amiga.ahi:audio-modes) nil)
    (error () t)))

; --- Delay via dos.library, so polling below is paced, not spinning ---
(defun ahi-test-delay (ticks)
  (amiga.ffi:with-library (dos "dos.library")
    (amiga:call-library dos -198 (list :d1 ticks))))

(defun ahi-test-wait-until-silent (ahi ticks)
  "Poll PLAYING-P once a tick for up to TICKS ticks; T once it went quiet."
  (dotimes (i ticks nil)
    (unless (amiga.ahi:playing-p ahi) (return t))
    (ahi-test-delay 1)))

; A second of square wave at 8 kHz: 50 samples up, 50 down (~80 Hz),
; as the raw bytes AHI plays (two's-complement signed values).
(defparameter *ahi-test-wave*
  (let ((v (make-array 8000 :element-type '(unsigned-byte 8))))
    (dotimes (i 8000 v)
      (setf (aref v i) (if (evenp (floor i 50)) 100 156)))))

(defparameter *ahi-available* nil)

(let ((ahi (amiga.ahi:open-ahi)))
  (setq *ahi-available* (not (null ahi)))
  (cond (ahi (check "ahi-open" t t))
        (t (format t "SKIP: ahi.device unavailable (AHI not installed, or unit 0 not set up in its preferences) -- the AHI device checks are skipped~%")))
  (when ahi
    (check "ahi-open-p" t (amiga.ahi:ahi-open-p ahi))
    (check "ahi-unit-is-default" amiga.ahi:+ahi-default-unit+ (amiga.ahi:ahi-unit ahi))
    (check "ahi-base-armed" t (ffi:foreign-pointer-p amiga.raw.ahi:*ahi-base*))
    (check "ahi-version-at-least-4" t
      (and (integerp amiga.raw.ahi:*ahi-version*) (>= amiga.raw.ahi:*ahi-version* 4)))
    (check "ahi-not-playing-initially" nil (amiga.ahi:playing-p ahi))

    (let ((buf (amiga.ahi:make-sample-buffer *ahi-test-wave*)))
      ; A full second queued: still sounding right after SendIO.
      (check "ahi-play-sample-queues" t
        (amiga.ahi:play-sample ahi buf 8000 :rate 8000))
      (check "ahi-playing-after-start" t (amiga.ahi:playing-p ahi))
      (check "ahi-stop-sample" nil (amiga.ahi:stop-sample ahi))
      (check "ahi-not-playing-after-stop" nil (amiga.ahi:playing-p ahi))

      ; VOLUME above the ahir_Volume range (0..#x10000) is clamped, like
      ; PAN already was -- a :VOLUME > 1.0 must not reach ahi.device as an
      ; out-of-spec Fixed value.
      (check "ahi-play-sample-clamps-over-range-volume" #x10000
        (progn (amiga.ahi:play-sample ahi buf 400 :rate 8000 :volume 2.0)
               (amiga.raw.ahi:ahi-request-volume (amiga.ahi::ahi-io-a ahi))))
      (amiga.ahi:stop-sample ahi)

      ; A 50 ms one-shot completes on its own within 2 s of polling.
      (check "ahi-short-sample-completes" t
        (progn (amiga.ahi:play-sample ahi buf 400 :rate 8000)
               (ahi-test-wait-until-silent ahi 100)))

      ; Double buffering: two requests in flight (the second linked to
      ; the first), a third refused while both are busy, both drain.
      (check "ahi-queue-first" t (amiga.ahi:queue-sample ahi buf 4000 :rate 8000))
      (check "ahi-queue-second-links" t (amiga.ahi:queue-sample ahi buf 4000 :rate 8000))
      (check "ahi-two-in-flight" 2 (length (amiga.ahi::ahi-pending ahi)))
      (check "ahi-queue-third-refused-while-both-busy" nil
        (amiga.ahi:queue-sample ahi buf 4000 :rate 8000))
      (check "ahi-queued-pair-completes" t (ahi-test-wait-until-silent ahi 200))

      ; PLAY-SAMPLE cuts a queued pair off and leaves one request out.
      (amiga.ahi:queue-sample ahi buf 8000 :rate 8000)
      (amiga.ahi:queue-sample ahi buf 8000 :rate 8000)
      (check "ahi-play-replaces-queue" t (amiga.ahi:play-sample ahi buf 400 :rate 8000))
      (check "ahi-one-in-flight-after-play" 1 (length (amiga.ahi::ahi-pending ahi)))
      (amiga.ahi:stop-sample ahi)

      ; 16-bit stereo from a (signed-byte 16) vector, half volume, hard left.
      (let* ((frames 2000)
             (pcm (make-array (* 2 frames) :element-type '(signed-byte 16))))
        (dotimes (i frames)
          (let ((s (if (evenp (floor i 50)) 12000 -12000)))
            (setf (aref pcm (* 2 i)) s
                  (aref pcm (1+ (* 2 i))) s)))
        (let* ((bytes (amiga.ahi:sample-bytes pcm :bits 16))
               (buf16 (amiga.ahi:make-sample-buffer bytes)))
          (check "ahi-stereo16-bytes" (* 4 frames) (length bytes))
          (check "ahi-play-stereo16" t
            (amiga.ahi:play-sample ahi buf16 (length bytes) :type :stereo16
                                   :rate 8000 :volume 0.5 :pan 0.0))
          (check "ahi-stereo16-completes" t (ahi-test-wait-until-silent ahi 100))
          (ffi:free-foreign buf16)))
      (ffi:free-foreign buf))

    ; --- The audio mode database, through the function interface ---
    (let ((modes (amiga.ahi:audio-modes)))
      (check "ahi-audio-modes-nonempty" t (not (null modes)))
      (check "ahi-audio-mode-name-string" t
        (stringp (amiga.ahi:audio-mode-name (first modes))))
      (check "ahi-audio-mode-unknown-name-nil" nil
        (amiga.ahi:audio-mode-name #x7FFFFFFF))
      (let ((info (amiga.ahi:audio-mode-info (first modes))))
        (check "ahi-audio-mode-info-id" (first modes) (getf info :id))
        (check "ahi-audio-mode-info-name" t (stringp (getf info :name)))
        (check "ahi-audio-mode-info-bits" t
          (and (integerp (getf info :bits)) (plusp (getf info :bits))))
        (check "ahi-audio-mode-info-channels" t
          (and (integerp (getf info :max-channels)) (plusp (getf info :max-channels))))))

    (let ((best (amiga.ahi:best-audio-mode)))
      (check "ahi-best-audio-mode" t (integerp best))
      (check "ahi-best-mode-is-in-database" t
        (not (null (member best (amiga.ahi:audio-modes)))))
      (check "ahi-best-audio-mode-impossible-nil" nil
        (amiga.ahi:best-audio-mode :bits 64)))

    (check "ahi-close" nil (amiga.ahi:close-ahi ahi))
    (check "ahi-closed-not-open-p" nil (amiga.ahi:ahi-open-p ahi))
    (check "ahi-base-disarmed-after-last-close" nil amiga.raw.ahi:*ahi-base*)))

; --- AMIGA.EXEC:DO-IO against the real device: AMIGA.AHI itself never
; blocks (it always takes the SendIO/CheckIO/WaitIO road), so this is
; DO-IO's only exerciser -- a synchronous CMD_WRITE built by hand from
; AMIGA.EXEC's primitives, the unit free again now the handle above closed.
(when *ahi-available*
  (let ((port (amiga.exec:create-msg-port)))
    (check "exec-do-io-msg-port" t (not (null port)))
    (when port
      (let ((io (amiga.exec:create-io-request port amiga.raw.ahi:*ahi-request-size*)))
        (check "exec-do-io-request" t (not (null io)))
        (when io
          (setf (amiga.raw.ahi:ahi-request-version io) 4)
          ; DoIO on an unopened request is undefined (io_Device is NULL) --
          ; only run it once OpenDevice actually succeeded, same as OPEN-AHI.
          (let ((opened (zerop (amiga.exec:open-device amiga.raw.ahi:+ahiname+
                                                        amiga.ahi:+ahi-default-unit+ io 0))))
            (check "exec-do-io-open-device" t opened)
            (when opened
              (let ((buf (amiga.ahi:make-sample-buffer *ahi-test-wave*)))
                (setf (amiga.raw.exec:node-pri io) 0
                      (amiga.raw.exec:io-std-req-command io) amiga.raw.exec:+cmd-write+
                      (amiga.raw.exec:io-std-req-data io) buf
                      (amiga.raw.exec:io-std-req-length io) 400
                      (amiga.raw.exec:io-std-req-offset io) 0
                      (amiga.raw.ahi:ahi-request-type io) amiga.raw.ahi:+ahist-m8-s+
                      (amiga.raw.ahi:ahi-request-frequency io) 8000
                      (amiga.raw.ahi:ahi-request-volume io) (amiga.ahi:to-fixed 1.0)
                      (amiga.raw.ahi:ahi-request-position io) (amiga.ahi:to-fixed 0.5)
                      (amiga.raw.ahi:ahi-request-link io) (ffi:make-foreign-pointer 0))
                ; DoIO blocks until the 50 ms sample finishes -- a real
                ; synchronous round trip through the device.
                (check "exec-do-io-synchronous" 0 (amiga.exec:do-io io))
                (check "exec-do-io-request-reaped" t (amiga.exec:check-io io))
                (ffi:free-foreign buf))
              (amiga.exec:close-device io)))
          (amiga.exec:delete-io-request io)))
      (amiga.exec:delete-msg-port port))))

; --- The low-level API: two channels, one sound, no hooks ---
; On a :NONE handle, with the unit-0 handle closed: an open unit holds the
; audio hardware (on Paula the whole of it), and AHI_AllocAudioA on the
; same hardware then fails -- the AHI autodoc says to open AHI_NO_UNIT for
; the low-level API, and this is why.
(when *ahi-available*
  (amiga.ahi:with-ahi (fn :unit :none)
    (let* ((best (amiga.ahi:best-audio-mode))
           (ctrl (amiga.ahi:alloc-audio :audio-id best :channels 2 :sounds 2)))
      (check "ahi-alloc-audio" t (not (null ctrl)))
      (when ctrl
        (let ((buf (amiga.ahi:make-sample-buffer *ahi-test-wave*)))
          (check "ahi-load-sound" t (amiga.ahi:load-sound ctrl 0 buf 8000 :type :mono8))
          (check "ahi-start-playback" t (amiga.ahi:start-playback ctrl))
          (check "ahi-play-channel" nil
            (amiga.ahi:play ctrl 0 :sound 0 :frequency 8000 :volume 1.0 :pan 0.5))
          (ahi-test-delay 10)
          (check "ahi-set-volume" nil
            (amiga.ahi:set-volume ctrl 0 0.5 :pan 0.25 :immediate t))
          (check "ahi-set-frequency" nil (amiga.ahi:set-frequency ctrl 0 11025))
          (check "ahi-play-with-loop" nil
            (amiga.ahi:play ctrl 1 :sound 0 :frequency 8000 :volume 0.5 :pan 0.5
                                   :length 4000 :loop-sound 0 :loop-length 400))
          (ahi-test-delay 10)
          (check "ahi-set-sound-silences" nil (amiga.ahi:set-sound ctrl 1 nil))
          (check "ahi-play-nosound" nil (amiga.ahi:play ctrl 0 :sound nil))
          (check "ahi-play-without-frequency-errors" t
            (handler-case (progn (amiga.ahi:play ctrl 0 :sound 0) nil)
              (error () t)))
          (check "ahi-play-offset-without-length-errors" t
            (handler-case (progn (amiga.ahi:play ctrl 0 :sound 0 :frequency 8000 :offset 10) nil)
              (error () t)))
          (check "ahi-stop-playback" t (amiga.ahi:stop-playback ctrl))
          (check "ahi-unload-sound" nil (amiga.ahi:unload-sound ctrl 0))
          (ffi:free-foreign buf))
        (check "ahi-free-audio" nil (amiga.ahi:free-audio ctrl)))))
  (check "ahi-base-disarmed-after-low-level" nil amiga.raw.ahi:*ahi-base*))

; A :NONE handle is the function interface alone -- no playback unit;
; WITH-AHI closes it.  Two handles share the base until the last closes.
(when *ahi-available*
  (check "ahi-with-ahi-none-unit" t
    (amiga.ahi:with-ahi (a :unit :none)
      (and (eql (amiga.ahi:ahi-unit a) amiga.ahi:+ahi-no-unit+)
           (integerp (amiga.ahi:best-audio-mode))
           (handler-case
               (progn (amiga.ahi:play-sample a (ffi:make-foreign-pointer 0) 2) nil)
             (error () t)))))
  (check "ahi-with-ahi-closes" nil amiga.raw.ahi:*ahi-base*)
  (check "ahi-two-handles-share-base" t
    (let ((a (amiga.ahi:open-ahi :unit :none))
          (b (amiga.ahi:open-ahi :unit :none)))
      (prog1 (and a b
                  (ffi:foreign-pointer-p amiga.raw.ahi:*ahi-base*)
                  (progn (amiga.ahi:close-ahi a)
                         (ffi:foreign-pointer-p amiga.raw.ahi:*ahi-base*)))
        (when b (amiga.ahi:close-ahi b)))))
  (check "ahi-base-disarmed-after-both" nil amiga.raw.ahi:*ahi-base*))
