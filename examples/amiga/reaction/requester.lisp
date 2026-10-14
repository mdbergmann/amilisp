;;; requester.lisp — requester.class: info, multi-button, string and
;;; integer requesters.
;;;
;;; Common Lisp port of the NDK 3.2 example Examples/Requester.c: a
;;; window.class window with a single "_Press Me!" button; pressing it
;;; walks through a sequence of requester.class requesters — an
;;; information requester with styled body text, one with three buttons,
;;; a string requester editing a buffer, the same with a chooser of
;;; presets, and two integer requesters — printing each result.  The
;;; requester object is created once and re-used with different
;;; RM_OPENREQ attributes, as the C does with OpenRequesterTags().
;;;
;;; What it shows: AMIGA.REACTION:OPEN-REQUESTER (the RM_OPENREQ method
;;; with its orRequest message), buffers and string arrays that must
;;; outlive a call (POOL-ALLOC / POOL-STRING), reading results back with
;;; GET-ATTR.
;;;
;;; Run on AmigaOS 3.5+/3.2 or MorphOS (ReAction classes required):
;;;   amilisp --load examples/amiga/reaction/requester.lisp

(require "amiga/reaction")
(require "amiga/raw/intuition")
(require "amiga/raw/classes/window")
(require "amiga/raw/classes/requester")
(require "amiga/raw/gadgets/layout")
(require "amiga/raw/gadgets/button")

(defpackage "REACTION-REQUESTER"
  (:use "CL")
  (:local-nicknames ("RA"     "AMIGA.REACTION")
                    ("INTUI"  "AMIGA.RAW.INTUITION")
                    ("WIN"    "AMIGA.RAW.CLASSES.WINDOW")
                    ("REQ"    "AMIGA.RAW.CLASSES.REQUESTER")
                    ("LAYOUT" "AMIGA.RAW.GADGETS.LAYOUT")
                    ("BUTTON" "AMIGA.RAW.GADGETS.BUTTON")))

(in-package "REACTION-REQUESTER")

(defconstant +id-button+ 1)

(defparameter *chooser-labels* '("Label 1" "Label 2" "Label 3" "Label 4"))

(defun esc (string)
  "STRING with every ~ replaced by ESC — requester.class body text takes
the usual ESC sequences: ESC b bold, ESC c centre, ESC i italic, ESC n
normal, ESC f[font/size]."
  (substitute (code-char 27) #\~ string))

(defun make-window-object ()
  (ra:new-object (win:window-get-class)
    intui:+wa-screen-title+ "ReAction"
    intui:+wa-title+ "ReAction Requester Example"
    intui:+wa-size-gadget+ t
    intui:+wa-left+ 40
    intui:+wa-top+ 30
    intui:+wa-depth-gadget+ t
    intui:+wa-drag-bar+ t
    intui:+wa-close-gadget+ t
    intui:+wa-activate+ t
    intui:+wa-smart-refresh+ t
    win:+window-parent-group+
    (ra:new-object (layout:layout-get-class)
                   layout:+layout-orientation+ layout:+layout-vertical+
                   layout:+layout-space-outer+ t
                   layout:+layout-defer-layout+ t
                   layout:+layout-add-child+
                   (ra:new-object (button:button-get-class)
                                  intui:+ga-rel-verify+ t
                                  intui:+ga-id+ +id-button+
                                  intui:+ga-text+ "_Press Me!")
                   layout:+child-min-width+ 100
                   layout:+child-weighted-height+ 0)))

(defun string-array (strings)
  "A NULL-terminated array of STRPTRs, everything in the foreign pool."
  (let ((array (ra:pool-alloc (* 4 (1+ (length strings))))))
    (loop for s in strings
          for offset from 0 by 4
          do (ffi:poke-u32 array (ffi:foreign-pointer-address (ra:pool-string s)) offset))
    array))

(defun show-requesters (requester window)
  ;; 1. An information requester with styled body text.  The ESC
  ;;    sequences select bold, centring and a font — "\33" in the C.
  (format t "~&; info requester returned ~D~%"
          (ra:open-requester requester window
                             req:+req-type+ req:+reqtype-info+
                             req:+req-body-text+ (esc (format nil "~~b~~c~~f[cgtimes.font/50]Information~%for~%you."))
                             req:+req-gadget-text+ "_Ok"))
  ;; 2. Three buttons, separated by | in REQ_GadgetText.  The result is
  ;;    1 for the leftmost, ..., 0 for the rightmost.
  (format t "~&; three-button requester returned ~D~%"
          (ra:open-requester requester window
                             req:+req-type+ req:+reqtype-info+
                             req:+req-body-text+ (esc (format nil "~~c~~iSome fancy text here just to show off.~~n~%~%~~bReAction~~n rules!~%~%5~~b~~f[helvetica.font/15]ReAction~~n3  is magic!"))
                             req:+req-gadget-text+ "_Ok|_Roll the bones!|_Whee!, get me out of here"))
  ;; 3. A string requester edits a buffer we own.  The buffer must be
  ;;    writable foreign memory that outlives the call — pooled.
  (let ((buffer (ra:pool-alloc 128)))
    (ffi:poke-bytes buffer "Edit me!")
    (let ((result (ra:open-requester requester window
                                     req:+req-type+ req:+reqtype-string+
                                     req:+reqs-buffer+ buffer
                                     req:+reqs-show-default+ nil
                                     req:+reqs-max-chars+ 127
                                     req:+req-gadget-text+ "_Ok|_Cancel"
                                     req:+req-body-text+ "Enter a string:")))
      (format t "~&; string requester returned ~D, string: ~S~%"
              result (ffi:foreign-to-string buffer 127)))
    ;; 4. The same with a chooser of presets (a NULL-terminated STRPTR
    ;;    array); REQS_ChooserActive reads back which one was picked.
    (let ((result (ra:open-requester requester window
                                     req:+req-type+ req:+reqtype-string+
                                     req:+reqs-buffer+ buffer
                                     req:+reqs-show-default+ t
                                     req:+reqs-max-chars+ 127
                                     req:+reqs-chooser-array+ (string-array *chooser-labels*)
                                     req:+reqs-chooser-active+ 2
                                     req:+req-gadget-text+ "_Ok|_Patricia!|_Cancel"
                                     req:+req-body-text+ "Edit the string:")))
      (format t "~&; chooser requester returned ~D, string: ~S, active: ~D~%"
              result (ffi:foreign-to-string buffer 127)
              (ra:get-attr req:+reqs-chooser-active+ requester))))
  ;; 5. An integer requester with arrows; REQI_Number reads the value back.
  (let ((number 0))
    (format t "~&; integer requester returned ~D, number: ~D~%"
            (ra:open-requester requester window
                               req:+req-type+ req:+reqtype-integer+
                               req:+reqi-number+ number
                               req:+reqi-arrows+ t
                               req:+reqs-chooser-array+ nil      ; reset the labels
                               req:+req-gadget-text+ "_Ok|_Cancel"
                               req:+req-body-text+ "Enter a number:")
            (ra:get-attr req:+reqi-number+ requester)))
  ;; 6. ...and one with a range.
  (format t "~&; ranged integer requester returned ~D, number: ~D~%"
          (ra:open-requester requester window
                             req:+req-type+ req:+reqtype-integer+
                             req:+reqi-number+ (ra:get-attr req:+reqi-number+ requester)
                             req:+reqi-maximum+ 1000
                             req:+reqi-minimum+ -1000
                             req:+reqi-arrows+ nil
                             req:+req-gadget-text+ "_Ok|_Cancel"
                             req:+req-body-text+ "Enter a number:")
          ;; REQI_Number is a LONG: read it back signed
          (let ((n (ra:get-attr req:+reqi-number+ requester)))
            (if (logbitp 31 n) (- n #x100000000) n))))

(defun run ()
  (ra:with-foreign-pool ()
    (let ((requester (ra:new-object (req:requester-get-class)
                                    req:+req-title-text+ "Simple requester.class example"))
          (win-obj (make-window-object)))
      (unwind-protect
           (let ((window (ra:open-window win-obj)))
             (unless window
               (error "requester: could not open the window"))
             (ra:do-window-events ((result code) win-obj)
               (let ((class (logand result win:+wmhi-classmask+)))
                 (cond ((= class win:+wmhi-closewindow+)
                        (return))
                       ((and (= class win:+wmhi-gadgetup+)
                             (= (logand result win:+wmhi-gadgetmask+) +id-button+))
                        (show-requesters requester window))))))
        (ra:dispose-object win-obj)
        (ra:dispose-object requester)))))

(if (ra:available-p)
    (run)
    (format t "~&; requester: the ReAction classes are not available on this system (AmigaOS 3.5+/3.2 or MorphOS needed) — nothing to show.~%"))
