;;; numeric.lisp — MUI's numeric objects: knobs, sliders, numeric
;;; buttons, a gauge with its scale and a levelmeter, wired together with
;;; notifications only.
;;;
;;; This is the notification-only side of slidorama.lisp, the port of the
;;; MUI SDK's Slidorama.c: there half the objects are instances of custom
;;; classes whose dispatchers are Lisp functions (a Levelmeter subclass
;;; that follows the mouse, sliders whose MUIM_Numeric_Stringify prints
;;; ratings and times), here the same subjects are reached with nothing
;;; but attributes and MUIM_Notify -- the knob table and the numeric
;;; buttons as in the C, MUIA_Numeric_Format where it overrode Stringify,
;;; and a slider that drives a gauge and a levelmeter through MUIM_Notify
;;; with MUIV_TriggerValue.
;;;
;;; What it shows: KnobObject, SliderObject, NumericbuttonObject
;;; (MUIO_NumericButton), GaugeObject with MUIA_Gauge_Current / _InfoText,
;;; ScaleObject, LevelmeterObject, MUIA_Numeric_Min / Max / Default / Value
;;; / Format, GroupFrameT, MUIO_Label_Centered labels, MUIA_Group_Spacing.
;;;
;;; Run on AmigaOS 3.x with MUI 3.8+ installed, or on MorphOS:
;;;   amilisp --load examples/amiga/mui/numeric.lisp
;;; Unattended (auto-closes after 5 s):
;;;   amilisp --eval '(require "amiga/mui")' \
;;;           --eval '(setf amiga.mui:*event-loop-timeout* 5)' \
;;;           --load examples/amiga/mui/numeric.lisp

(require "amiga/mui")

(when (amiga.mui:available-p)
  (require "amiga/raw/muimaster"))

(defpackage "MUI-NUMERIC"
  (:use "CL")
  (:local-nicknames ("MUI" "AMIGA.MUI")))

(in-package "MUI-NUMERIC")

(defun m (name)
  "The value of the amiga/raw/muimaster constant NAME -- looked up at run
time, so this file reads on a system where that package does not exist."
  (symbol-value (or (find-symbol name "AMIGA.RAW.MUIMASTER")
                    (error "numeric: amiga.raw.muimaster:~A not found" name))))

;;; The mui.h macros this program uses, as functions.

(defun children (objects)
  (loop for o in objects collect (m "+MUIA-GROUP-CHILD+") collect o))

(defun vgroup (tags &rest objects)
  (apply #'mui:new-object :group (append tags (children objects))))

(defun hgroup (tags &rest objects)
  (apply #'vgroup (list* (m "+MUIA-GROUP-HORIZ+") t tags) objects))

(defun col-group (columns tags &rest objects)
  (apply #'vgroup (list* (m "+MUIA-GROUP-COLUMNS+") columns tags) objects))

(defun group-frame-t (title)
  "GroupFrameT(title)"
  (list (m "+MUIA-FRAME+") (m "+MUIV-FRAME-GROUP+")
        (m "+MUIA-FRAME-TITLE+") title
        (m "+MUIA-BACKGROUND+") (m "+MUII-GROUP-BACK+")))

(defun group-spacing (n) (list (m "+MUIA-GROUP-SPACING+") n))

(defun h-space (n) (mui:make-object :h-space n))
(defun v-space (n) (mui:make-object :v-space n))

(defun label   (text) (mui:make-object :label text 0))
(defun label1  (text) (mui:make-object :label text (m "+MUIO-LABEL-SINGLE-FRAME+")))
(defun c-label (text) (mui:make-object :label text (m "+MUIO-LABEL-CENTERED+")))

(defun knob (&rest tags)
  "KnobObject, MUIA_CycleChain, 1, tags... End"
  (apply #'mui:new-object :knob (m "+MUIA-CYCLE-CHAIN+") 1 tags))

(defun volume-knob ()
  (knob (m "+MUIA-NUMERIC-MAX+") 64 (m "+MUIA-NUMERIC-DEFAULT+") 64))

(defun tone-knob ()
  (knob (m "+MUIA-NUMERIC-MIN+") -100 (m "+MUIA-NUMERIC-MAX+") 100))

(defun numeric-button (min max format)
  "MUI_MakeObject(MUIO_NumericButton, NULL, min, max, format)"
  (mui:make-object :numeric-button nil min max format))

(defun run ()
  (mui:with-foreign-pool ()
    (let* ((power (mui:new-object :slider
                    (m "+MUIA-CYCLE-CHAIN+") 1
                    (m "+MUIA-NUMERIC-MIN+") 0
                    (m "+MUIA-NUMERIC-MAX+") 100
                    (m "+MUIA-NUMERIC-VALUE+") 50
                    (m "+MUIA-NUMERIC-FORMAT+") "%ld %%"))
           (gauge (mui:new-object :gauge
                    (m "+MUIA-FRAME+") (m "+MUIV-FRAME-GAUGE+")
                    (m "+MUIA-GAUGE-HORIZ+") t
                    (m "+MUIA-GAUGE-MAX+") 100
                    (m "+MUIA-GAUGE-CURRENT+") 50
                    (m "+MUIA-GAUGE-INFO-TEXT+") "%ld %%"))
           (meter (mui:new-object :levelmeter
                    (m "+MUIA-NUMERIC-MAX+") 100
                    (m "+MUIA-NUMERIC-VALUE+") 50
                    (m "+MUIA-LEVELMETER-LABEL+") "Power"))
           (win (mui:new-object :window
                  (m "+MUIA-WINDOW-TITLE+") "Numeric Objects"
                  (m "+MUIA-WINDOW-ID+")    (mui:make-id "NUMR")
                  (m "+MUIA-WINDOW-ROOT-OBJECT+")
                  (vgroup '()
                    (hgroup '()
                      ;; the knob table of Slidorama.c
                      (vgroup (append (group-spacing 0) (group-frame-t "Knobs"))
                        (v-space 0)
                        (col-group 6 (group-spacing 0)
                          (v-space 0) (h-space 4)
                          (c-label "1") (c-label "2") (c-label "3") (c-label "4")
                          (v-space 2) (v-space 2) (v-space 2) (v-space 2) (v-space 2) (v-space 2)
                          (label "Volume:") (h-space 4)
                          (volume-knob) (volume-knob) (volume-knob) (volume-knob)
                          (label "Bass:") (h-space 4)
                          (tone-knob) (tone-knob) (tone-knob) (tone-knob)
                          (label "Treble:") (h-space 4)
                          (tone-knob) (tone-knob) (tone-knob) (tone-knob))
                        (v-space 0))
                      (vgroup '()
                        ;; where slidorama.lisp has its mouse-driven Levelmeter
                        ;; subclass: a slider feeding a levelmeter and a gauge
                        ;; by notification instead
                        (vgroup (group-frame-t "Slider -> Levelmeter and Gauge")
                          (hgroup '()
                            (h-space 0)
                            meter
                            (h-space 0)
                            (vgroup '()
                              (v-space 0)
                              gauge
                              (mui:new-object :scale (m "+MUIA-SCALE-HORIZ+") t)
                              (v-space 0)))
                          (hgroup '() (label "Power:") power))
                        ;; the numeric buttons of Slidorama.c
                        (vgroup (group-frame-t "Numeric Buttons")
                          (v-space 0)
                          (hgroup (group-spacing 0)
                            (h-space 0)
                            (col-group 4 (list (m "+MUIA-GROUP-VERT-SPACING+") 1)
                              (v-space 0)
                              (c-label "Left") (c-label "Right") (c-label "SPL")
                              (label1 "Low:")
                              (numeric-button 0 100 "%3ld %%")
                              (numeric-button 0 100 "%3ld %%")
                              (numeric-button 30 99 "%2ld dB")
                              (label1 "Mid:")
                              (numeric-button 0 100 "%3ld %%")
                              (numeric-button 0 100 "%3ld %%")
                              (numeric-button 30 99 "%2ld dB")
                              (label1 "High:")
                              (numeric-button 0 100 "%3ld %%")
                              (numeric-button 0 100 "%3ld %%")
                              (numeric-button 30 99 "%2ld dB"))
                            (h-space 0))
                          (v-space 0))))
                    (v-space 4)
                    ;; slidorama.lisp's Rating class prints ":-)" ratings from
                    ;; MUIM_Numeric_Stringify; MUIA_Numeric_Format is the
                    ;; notification-only part of that
                    (col-group 2 '()
                      (label "Rating:")
                      (mui:new-object :slider
                        (m "+MUIA-CYCLE-CHAIN+") 1
                        (m "+MUIA-NUMERIC-MIN+") 0
                        (m "+MUIA-NUMERIC-MAX+") 100
                        (m "+MUIA-NUMERIC-VALUE+") 50
                        (m "+MUIA-NUMERIC-FORMAT+") "%3ld points")))))
           (app (mui:new-object :application
                  (m "+MUIA-APPLICATION-TITLE+")       "Numeric"
                  (m "+MUIA-APPLICATION-VERSION+")     "$VER: Numeric 1.0"
                  (m "+MUIA-APPLICATION-DESCRIPTION+") "Show different kinds of sliders"
                  (m "+MUIA-APPLICATION-BASE+")        "NUMERIC"
                  (m "+MUIA-APPLICATION-WINDOW+")      win)))
      (unwind-protect
           (progn
             (mui:notify win (m "+MUIA-WINDOW-CLOSE-REQUEST+") t
                         :application (m "+MUIM-APPLICATION-RETURN-ID+")
                         (m "+MUIV-APPLICATION-RETURN-ID-QUIT+"))
             ;; the slider's value -> the levelmeter's value and the gauge's
             ;; current level, every time it changes
             (mui:notify power (m "+MUIA-NUMERIC-VALUE+") :every-time
                         meter (m "+MUIM-SET+") (m "+MUIA-NUMERIC-VALUE+") :trigger-value)
             (mui:notify power (m "+MUIA-NUMERIC-VALUE+") :every-time
                         gauge (m "+MUIM-SET+") (m "+MUIA-GAUGE-CURRENT+") :trigger-value)
             (mui:set-attrs win (m "+MUIA-WINDOW-OPEN+") t)
             (when (zerop (or (mui:get-attr (m "+MUIA-WINDOW-OPEN+") win) 0))
               (error "numeric: the window would not open"))
             (mui:do-application-events ((id) app)))
        (mui:dispose-object app)))))

(if (mui:available-p)
    (run)
    (format t "~&; numeric: MUI is not available on this system (muimaster.library 3.8+ on AmigaOS, or MorphOS, needed) — nothing to show.~%"))
