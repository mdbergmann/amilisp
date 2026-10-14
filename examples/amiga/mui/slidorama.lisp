;;; slidorama.lisp — the port of the MUI SDK's Slidorama.c: MUI's numeric
;;; classes, and four *custom classes* on top of them whose dispatchers
;;; are Lisp functions.
;;;
;;; Three dispatchers, each a few lines, exactly as the C has them:
;;;
;;;   Mousepower  a subclass of Levelmeter.mui that adds input handling to
;;;               its superclass -- OM_NEW reads its own MUIA_Mousepower_
;;;               Direction tag out of the opSet message, MUIM_Setup asks
;;;               for IDCMP_MOUSEMOVE / INTUITICKS / INACTIVEWINDOW with
;;;               MUI_RequestIDCMP, MUIM_HandleInput turns mouse movement
;;;               into MUIM_Numeric_Increase and lets the level decay on
;;;               every intuitick.  Wiggle the mouse over the window.
;;;   Rating      a subclass of Slider.mui that overrides
;;;               MUIM_Numeric_Stringify, so the slider shows ":-))"
;;;               ratings instead of a number.
;;;   Time        the same override again, printing mm:ss -- and used as
;;;               the dispatcher of *two* classes, one over Slider.mui and
;;;               one over Numericbutton.mui, which is the whole point the
;;;               C's comment makes about it.
;;;
;;; What it shows: AMIGA.MUI:CREATE-CUSTOM-CLASS over four different MUI
;;; superclasses, DO-SUPER-METHOD (including OM_NEW, whose result is the
;;; new object), INST-DATA as the C's struct MousepowerData / char buf[],
;;; a dispatcher returning a STRPTR, REQUEST-IDCMP / REJECT-IDCMP,
;;; MUIM_HandleInput with the struct IntuiMessage, GetTagData on an
;;; opSet's ops_AttrList, a private tag of one's own, MUIM_Numeric_
;;; Increase / _Decrease / _ValueToScale, and the knob table and numeric
;;; buttons of the C around them.
;;;
;;; Deviation from the C, on purpose: Slidorama.c creates the Timebutton
;;; and Timeslider classes but never makes an object of either (its own
;;; comment describes them as being in the program).  This port puts both
;;; in the window, in a "Time" group, so the third dispatcher is visible.
;;;
;;; A dispatcher runs inside MUI, on its stack: an error in a method is
;;; caught at the callback, the method returns 0, and the condition is
;;; re-signaled in Lisp when the MUI call that dispatched it returns.
;;;
;;; Run on AmigaOS 3.x with MUI 3.8+ installed, or on MorphOS:
;;;   amilisp --load examples/amiga/mui/slidorama.lisp
;;; Unattended (auto-closes after 5 s):
;;;   amilisp --eval '(require "amiga/mui")' \
;;;           --eval '(setf amiga.mui:*event-loop-timeout* 5)' \
;;;           --load examples/amiga/mui/slidorama.lisp

(require "amiga/mui")
(require "amiga/raw/intuition")    ; IDCMP_*, OM_NEW, struct opSet / IntuiMessage
(require "amiga/raw/utility")      ; GetTagData, TAG_USER

(when (amiga.mui:available-p)
  (require "amiga/raw/muimaster"))

(defpackage "MUI-SLIDORAMA"
  (:use "CL")
  (:local-nicknames ("MUI" "AMIGA.MUI")
                    ("INTUI" "AMIGA.RAW.INTUITION")
                    ("UTIL" "AMIGA.RAW.UTILITY")))

(in-package "MUI-SLIDORAMA")

(defun m (name)
  "The value of the amiga/raw/muimaster constant NAME -- looked up at run
time, so this file reads on a system where that package does not exist."
  (symbol-value (or (find-symbol name "AMIGA.RAW.MUIMASTER")
                    (error "slidorama: amiga.raw.muimaster:~A not found" name))))

(defun mf (name)
  "The function of the amiga/raw/muimaster binding NAME (a generated
struct accessor), looked up at run time like M.  A dispatcher resolves
what it needs once, outside the function MUI calls per method."
  (let ((symbol (find-symbol name "AMIGA.RAW.MUIMASTER")))
    (unless (and symbol (fboundp symbol))
      (error "slidorama: amiga.raw.muimaster:~A is not a function" name))
    (symbol-function symbol)))

(defun poke-c-string (buffer size text)
  "Copy TEXT and its terminating NUL into the SIZE-byte foreign BUFFER,
truncating what does not fit -- the C's sprintf(data->buf, ...) with the
bound it leaves to the programmer made explicit.  Returns BUFFER, which
is what a MUIM_Numeric_Stringify method hands back to MUI."
  (let ((n (min (length text) (1- size))))
    (ffi:poke-bytes buffer text 0 0 n)
    (ffi:poke-u8 buffer 0 n))
  buffer)

;;; =====================================================================
;;; Mousepower — a subclass of Levelmeter.mui that follows the mouse
;;; =====================================================================

;;; struct MousepowerData: four WORDs.
(defconstant +mousepower-data-size+ 8)
(defconstant +mousepower-decrease+ 0)
(defconstant +mousepower-mouse-x+ 2)
(defconstant +mousepower-mouse-y+ 4)
(defconstant +mousepower-direction+ 6)

;;; #define MUIA_Mousepower_Direction ((TAG_USER | (1<<16)) | 0x0001)
(defconstant +muia-mousepower-direction+
  (logior util:+tag-user+ (ash 1 16) 1))

(defun mousepower-idcmp ()
  (logior intui:+idcmp-mousemove+ intui:+idcmp-intuiticks+
          intui:+idcmp-inactivewindow+))

(defun make-mousepower-dispatcher ()
  (let ((new-id       intui:+om-new+)
        (setup-id     (m "+MUIM-SETUP+"))
        (cleanup-id   (m "+MUIM-CLEANUP+"))
        (input-id     (m "+MUIM-HANDLE-INPUT+"))
        (numeric-max  (m "+MUIA-NUMERIC-MAX+"))
        (numeric-value (m "+MUIA-NUMERIC-VALUE+"))
        (increase     (m "+MUIM-NUMERIC-INCREASE+"))
        (decrease     (m "+MUIM-NUMERIC-DECREASE+"))
        (imsg-of      (mf "MUIP-HANDLE-INPUT-IMSG"))
        (idcmp        (mousepower-idcmp)))
    (lambda (class object message)
      (let ((id (mui:method-id message)))
        (cond
          ;; OM_NEW: the superclass makes the object and returns it; only
          ;; then is there instance data to fill in.
          ((= id new-id)
           (let ((result (mui:do-super-method class object message)))
             (unless (zerop result)
               (let* ((new (ffi:make-foreign-pointer result))
                      (data (mui:inst-data class new)))
                 (ffi:poke-i16 data -1 +mousepower-mouse-x+)
                 (ffi:poke-i16 data (util:get-tag-data +muia-mousepower-direction+ 0
                                                       (intui:op-set-attr-list message))
                               +mousepower-direction+)
                 (mui:set-attrs new numeric-max 1000)))
             result))
          ;; MUIM_Setup: the object is about to appear in a window -- ask
          ;; that window for the input classes we want to see.
          ((= id setup-id)
           (if (zerop (mui:do-super-method class object message))
               nil
               (progn
                 (ffi:poke-i16 (mui:inst-data class object) -1 +mousepower-mouse-x+)
                 (mui:set-attrs object numeric-max 1000)
                 (mui:request-idcmp object idcmp)
                 t)))
          ;; MUIM_Cleanup: give them back, then let the superclass finish.
          ((= id cleanup-id)
           (mui:reject-idcmp object idcmp)
           (mui:do-super-method class object message))
          ;; MUIM_HandleInput: one IntuiMessage of a class we asked for.
          ((= id input-id)
           (let ((imsg (funcall imsg-of message)))
             (when imsg
               (let ((data (mui:inst-data class object))
                     (event (intui:intui-message-class imsg))
                     (mouse-x (intui:intui-message-mouse-x imsg))
                     (mouse-y (intui:intui-message-mouse-y imsg)))
                 (cond
                   ((= event intui:+idcmp-mousemove+)
                    (let ((last-x (ffi:peek-i16 data +mousepower-mouse-x+))
                          (last-y (ffi:peek-i16 data +mousepower-mouse-y+)))
                      (unless (= last-x -1)
                        (let ((delta (case (ffi:peek-i16 data +mousepower-direction+)
                                       (1 (* 2 (abs (- last-x mouse-x))))
                                       (2 (* 2 (abs (- last-y mouse-y))))
                                       (t (+ (abs (- last-x mouse-x))
                                             (abs (- last-y mouse-y))))))
                              (down (ffi:peek-i16 data +mousepower-decrease+)))
                          (when (> down 0)
                            (ffi:poke-i16 data (1- down) +mousepower-decrease+))
                          (mui:do-method object increase (floor delta 10)))))
                    (ffi:poke-i16 data mouse-x +mousepower-mouse-x+)
                    (ffi:poke-i16 data mouse-y +mousepower-mouse-y+))
                   ((= event intui:+idcmp-intuiticks+)
                    (let ((down (ffi:peek-i16 data +mousepower-decrease+)))
                      (mui:do-method object decrease (* down down))
                      (when (< down 50)
                        (ffi:poke-i16 data (1+ down) +mousepower-decrease+))))
                   ((= event intui:+idcmp-inactivewindow+)
                    (mui:set-attrs object numeric-value 0)))))
             0))
          (t (mui:do-super-method class object message)))))))

;;; =====================================================================
;;; Rating — a Slider.mui whose MUIM_Numeric_Stringify prints a rating
;;; =====================================================================

;;; struct RatingData: char buf[20].
(defconstant +rating-data-size+ 20)

(defparameter *ratings* '(":-((" ":-(" ":-|" ":-)" ":-))"))

(defun make-rating-dispatcher ()
  (let ((stringify-id   (m "+MUIM-NUMERIC-STRINGIFY+"))
        (value-to-scale (m "+MUIM-NUMERIC-VALUE-TO-SCALE+"))
        (value-of       (mf "MUIP-NUMERIC-STRINGIFY-VALUE"))
        (last           (1- (length *ratings*))))
    (lambda (class object message)
      (if (= (mui:method-id message) stringify-id)
          (let ((value (funcall value-of message)))
            (poke-c-string
             (mui:inst-data class object) +rating-data-size+
             (cond ((= value 0)   "You're kidding!")
                   ((= value 100) "It's magic!")
                   (t (let ((scale (mui:do-method object value-to-scale 0 last)))
                        (format nil "~3D points. ~A"
                                value (nth (min last scale) *ratings*)))))))
          (mui:do-super-method class object message)))))

;;; =====================================================================
;;; Time — mm:ss from MUIM_Numeric_Stringify, for two different classes
;;; =====================================================================

;;; struct TimeData: char buf[16].
(defconstant +time-data-size+ 16)

(defun make-time-dispatcher ()
  (let ((stringify-id (m "+MUIM-NUMERIC-STRINGIFY+"))
        (value-of     (mf "MUIP-NUMERIC-STRINGIFY-VALUE")))
    (lambda (class object message)
      (if (= (mui:method-id message) stringify-id)
          (let ((value (funcall value-of message)))
            (poke-c-string (mui:inst-data class object) +time-data-size+
                           (format nil "~2,'0D:~2,'0D"
                                   (floor value 60) (mod value 60))))
          (mui:do-super-method class object message)))))

;;; =====================================================================
;;; The mui.h macros this program uses, as functions
;;; =====================================================================

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

(defun mousepower (mcc direction title)
  "NewObject(MousepowerClass->mcc_Class, 0, MUIA_Mousepower_Direction,
DIRECTION, MUIA_Levelmeter_Label, TITLE, TAG_DONE)"
  (mui:new-object (mui:custom-class-class mcc)
    +muia-mousepower-direction+ direction
    (m "+MUIA-LEVELMETER-LABEL+") title))

;;; =====================================================================
;;; Main program
;;; =====================================================================

(defun run ()
  (mui:with-foreign-pool ()
    ;; SetupClasses(): four MUI_CreateCustomClass calls, three dispatchers.
    ;; The pool deletes every class when it exits -- after the application
    ;; (and so every object of them) has been disposed below.
    (let* ((time-dispatcher (make-time-dispatcher))
           (mousepower-class (mui:create-custom-class
                              :levelmeter (make-mousepower-dispatcher)
                              :data-size +mousepower-data-size+))
           (rating-class (mui:create-custom-class
                          :slider (make-rating-dispatcher)
                          :data-size +rating-data-size+))
           (timeslider-class (mui:create-custom-class
                              :slider time-dispatcher
                              :data-size +time-data-size+))
           (timebutton-class (mui:create-custom-class
                              :numericbutton time-dispatcher
                              :data-size +time-data-size+))
           (win (mui:new-object :window
                  (m "+MUIA-WINDOW-TITLE+") "Slidorama"
                  (m "+MUIA-WINDOW-ID+")    (mui:make-id "SLID")
                  (m "+MUIA-WINDOW-ROOT-OBJECT+")
                  (vgroup '()
                    (hgroup '()
                      ;; the knob table
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
                        ;; three Mousepower objects: the same class, three
                        ;; MUIA_Mousepower_Direction values
                        (vgroup (group-frame-t "Levelmeter Displays")
                          (v-space 0)
                          (hgroup '()
                            (h-space 0) (mousepower mousepower-class 1 "Horizontal")
                            (h-space 0) (mousepower mousepower-class 2 "Vertical")
                            (h-space 0) (mousepower mousepower-class 0 "Total")
                            (h-space 0))
                          (v-space 0))
                        ;; MUI's own numeric buttons, formatted by MUIA_Numeric_Format
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
                    ;; the Time classes: one dispatcher, a Slider subclass
                    ;; and a Numericbutton subclass (not in the C's window)
                    (col-group 2 (group-frame-t "Time (mm:ss)")
                      (label "Position:")
                      (mui:new-object (mui:custom-class-class timeslider-class)
                        (m "+MUIA-CYCLE-CHAIN+") 1
                        (m "+MUIA-NUMERIC-MIN+") 0
                        (m "+MUIA-NUMERIC-MAX+") 599
                        (m "+MUIA-NUMERIC-VALUE+") 125)
                      (label "Length:")
                      (mui:new-object (mui:custom-class-class timebutton-class)
                        (m "+MUIA-CYCLE-CHAIN+") 1
                        (m "+MUIA-NUMERIC-MIN+") 0
                        (m "+MUIA-NUMERIC-MAX+") 599
                        (m "+MUIA-NUMERIC-VALUE+") 245))
                    ;; the Rating slider: a Stringify override on Slider.mui
                    (col-group 2 '()
                      (label "Slidorama Rating:")
                      (mui:new-object (mui:custom-class-class rating-class)
                        (m "+MUIA-NUMERIC-VALUE+") 50
                        (m "+MUIA-CYCLE-CHAIN+") 1)))))
           (app (mui:new-object :application
                  (m "+MUIA-APPLICATION-TITLE+")       "Slidorama"
                  (m "+MUIA-APPLICATION-VERSION+")     "$VER: Slidorama 19.5 (12.02.97)"
                  (m "+MUIA-APPLICATION-COPYRIGHT+")   "(C) 1992-95, Stefan Stuntz"
                  (m "+MUIA-APPLICATION-AUTHOR+")      "Stefan Stuntz"
                  (m "+MUIA-APPLICATION-DESCRIPTION+") "Show different kinds of sliders"
                  (m "+MUIA-APPLICATION-BASE+")        "SLIDORAMA"
                  (m "+MUIA-APPLICATION-WINDOW+")      win)))
      (unwind-protect
           (progn
             (mui:notify win (m "+MUIA-WINDOW-CLOSE-REQUEST+") t
                         :application (m "+MUIM-APPLICATION-RETURN-ID+")
                         (m "+MUIV-APPLICATION-RETURN-ID-QUIT+"))
             (mui:set-attrs win (m "+MUIA-WINDOW-OPEN+") t)
             (when (zerop (or (mui:get-attr (m "+MUIA-WINDOW-OPEN+") win) 0))
               (error "slidorama: the window would not open"))
             ;; the ideal loop of an object oriented MUI program: nothing
             ;; but the quit check -- the classes do the rest
             (mui:do-application-events ((id) app)
               nil)                     ; no return IDs of our own
             (mui:set-attrs win (m "+MUIA-WINDOW-OPEN+") nil))
        (mui:dispose-object app)))))          ; then the pool deletes the classes

(if (mui:available-p)
    (run)
    (format t "~&; slidorama: MUI is not available on this system (muimaster.library 3.8+ on AmigaOS, or MorphOS, needed) — nothing to show.~%"))
