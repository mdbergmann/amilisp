;;; ahi-play.lisp — sound through AHI (AMIGA.AHI), both tiers
;;;
;;;   amilisp --load examples/amiga/audio/ahi-play.lisp
;;;
;;; AHI is opt-in: this program asks for it with (require "amiga/ahi").
;;; It lists the audio modes of the installed drivers, plays a tone on
;;; the user's unit 0 through the device interface (one request, then
;;; two queued gapless at two pitches), and then a major chord on three
;;; channels of AHI's own mixer through the low-level API -- no hooks,
;;; the program drives every channel.  RUN returns a plist of what it
;;; did.  Without ahi.device it says so and returns NIL.
;;;
;;; The plain-Paula counterpart of the first part is AMIGA.AUDIO's
;;; PLAY-SAMPLE (see tests/amiga/test-audio.lisp): chip RAM, one Paula
;;; channel, no AHI needed.

(require "amiga/ahi")

(defpackage "AHI-PLAY"
  (:use "CL")
  (:export "RUN"))

(in-package "AHI-PLAY")

(defun delay (ticks)
  "dos.library Delay: TICKS fiftieths of a second."
  (amiga.ffi:with-library (dos "dos.library")
    (amiga:call-library dos -198 (list :d1 ticks))))

(defun sine (hz seconds rate)
  "SECONDS of a HZ sine at RATE Hz as (signed-byte 16) samples -- one
period computed, then tiled, so a 68020 is not asked for tens of
thousands of SINs."
  (let* ((period (max 2 (round rate hz)))
         (cycle (make-array period :element-type '(signed-byte 16)))
         (n (* seconds rate))
         (out (make-array n :element-type '(signed-byte 16))))
    (dotimes (i period)
      (setf (aref cycle i) (round (* 20000 (sin (/ (* 2 pi i) period))))))
    (dotimes (i n out)
      (setf (aref out i) (aref cycle (mod i period))))))

(defun wait-quiet (ahi)
  (loop while (amiga.ahi:playing-p ahi) do (delay 5)))

(defun run ()
  (let ((ahi (amiga.ahi:open-ahi)))
    (unless ahi
      (format t "~&ahi.device is not available here (AHI not installed, or unit 0 not set up in its preferences) -- nothing played.~%")
      (return-from run nil))
    (unwind-protect
         (let ((modes (amiga.ahi:audio-modes)))
           ;; 1. the audio mode database
           (format t "~&AHI V~D, ~D audio mode~:P:~%" amiga.raw.ahi:*ahi-version* (length modes))
           (dolist (id modes)
             (let ((i (amiga.ahi:audio-mode-info id)))
               (format t "  ~8,'0X  ~A (~D bit~:[~;, stereo~]~:[~;, HiFi~], ~D channel~:P)~%"
                       id (getf i :name) (getf i :bits) (getf i :stereo)
                       (getf i :hifi) (getf i :max-channels))))
           ;; 2. the device interface: unit 0, whatever the user put there
           (let* ((rate 22050)
                  (pcm (sine 440 2 rate))
                  (bytes (amiga.ahi:sample-bytes pcm :bits 16))
                  (buf (amiga.ahi:make-sample-buffer bytes)))
             (format t "~&Unit 0: a 440 Hz tone, 2 s, 16-bit mono at ~D Hz ...~%" rate)
             (amiga.ahi:play-sample ahi buf (length bytes) :type :mono16 :rate rate)
             (wait-quiet ahi)
             (format t "  ... then the same buffer twice, queued gapless, at 440 and 660 Hz~%")
             (amiga.ahi:queue-sample ahi buf (length bytes) :type :mono16 :rate rate)
             (amiga.ahi:queue-sample ahi buf (length bytes) :type :mono16
                                     :rate (round (* rate 3/2)) :volume 0.7)
             (wait-quiet ahi)
             (ffi:free-foreign buf))
           ;; 3. the low-level API: AHI's mixer, three channels, one sound.
           ;; From a :NONE handle with the unit closed: an open unit holds
           ;; the hardware (on Paula the whole of it), and the mixer could
           ;; not get it -- what the AHI autodoc's AHI_NO_UNIT is for.
           (amiga.ahi:close-ahi ahi)
           (setf ahi nil)
           (amiga.ahi:with-ahi (fn :unit :none)
             (let ((mode (or (amiga.ahi:best-audio-mode :stereo t)
                             (amiga.ahi:best-audio-mode))))
               (format t "~&Mixer on ~A: a major chord on three channels, 2 s~%"
                       (amiga.ahi:audio-mode-name mode))
               (amiga.ahi:with-audio-ctrl (ctrl :audio-id mode :channels 3 :sounds 1)
                 (let* ((rate 8000)
                        (pcm (sine 440 1 rate))
                        (bytes (amiga.ahi:sample-bytes pcm :bits 16))
                        (buf (amiga.ahi:make-sample-buffer bytes)))
                   (amiga.ahi:load-sound ctrl 0 buf (length pcm) :type :mono16)
                   (amiga.ahi:start-playback ctrl)
                   ;; the one sound at three rates: 440, 550, 660 Hz, looped
                   (amiga.ahi:play ctrl 0 :sound 0 :frequency rate
                                          :volume 0.5 :pan 0.3 :loop-sound 0)
                   (amiga.ahi:play ctrl 1 :sound 0 :frequency (round (* rate 5/4))
                                          :volume 0.5 :pan 0.5 :loop-sound 0)
                   (amiga.ahi:play ctrl 2 :sound 0 :frequency (round (* rate 3/2))
                                          :volume 0.5 :pan 0.7 :loop-sound 0)
                   (delay 100)
                   (amiga.ahi:stop-playback ctrl)
                   (amiga.ahi:unload-sound ctrl 0)
                   (ffi:free-foreign buf)))))
           (list :modes (length modes) :played t))
      (when ahi (amiga.ahi:close-ahi ahi)))))

(run)
