;;; amiga/reaction.lisp — ReAction helpers for AmiLisp
;;;
;;; Loaded via (require "amiga/reaction").
;;;
;;; ReAction (the OS 3.5+/3.2 and MorphOS GUI toolkit) is a set of
;;; BOOPSI class libraries — window.class, gadgets/layout.gadget,
;;; gadgets/button.gadget, ... — driven through intuition.library's
;;; NewObjectA / SetAttrsA / GetAttr / DisposeObject and through object
;;; METHODS.  The generated raw bindings (lib/amiga/raw/classes/,
;;; gadgets/, images/) already provide every class's tags, method IDs and
;;; the few real library functions they export; the toolkit-neutral BOOPSI
;;; half — DoMethod(), NewList(), tag lists from Lisp values, string
;;; literals that live as long as the objects that reference them — is
;;; AMIGA.BOOPSI (lib/amiga/boopsi.lisp), which this package :USEs and
;;; re-exports (DO-METHOD, OBJECT-CLASS, WITH-FOREIGN-POOL, POOL-ALLOC,
;;; POOL-STRING, NEW-LIST, FREE-LIST-NODES, WITH-TAGS, GET-ATTR,
;;; GET-ATTR-POINTER, SET-ATTRS), so amiga.reaction:with-foreign-pool and
;;; amiga.boopsi:with-foreign-pool are one symbol.  What this module adds
;;; is the ReAction-specific part — what a C program gets from
;;; reaction.lib and the RA_* macros:
;;;
;;;   NEW-OBJECT         NewObjectA with a Lisp tag plist — integers,
;;;                      foreign pointers, T/NIL and strings, the strings
;;;                      copied into a WITH-FOREIGN-POOL that outlives the
;;;                      objects
;;;   DISPOSE-OBJECT     intuition's DisposeObject
;;;   SET-GADGET-ATTRS   SetGadgetAttrsA
;;;   OPEN-WINDOW / CLOSE-WINDOW / ICONIFY / HANDLE-INPUT
;;;                      the RA_* macros as functions
;;;   DO-WINDOW-EVENTS   the canonical Wait()/RA_HandleInput event loop,
;;;                      with an optional timeout for unattended runs
;;;   OPEN-REQUESTER     requester.class RM_OPENREQ
;;;
;;; The module itself loads on any system (including the host and an
;;; AmigaOS 3.1 without ReAction): it only depends on exec/dos/utility/
;;; intuition.  AVAILABLE-P tells whether the classes can actually be
;;; opened; the per-class raw modules (amiga/raw/classes/window, ...)
;;; open their class at REQUIRE time and fail with a clear error where
;;; it is missing.
;;;
;;; See examples/amiga/reaction/ for ports of the NDK 3.2 ReAction
;;; examples, and tests/amiga/test-reaction.lisp for the executable
;;; specification of this module.

;; compile-time too: COMPILE-FILE (the host builds the lib/amiga FASLs) must see
;; these packages at read time, not only LOAD.
(eval-when (:compile-toplevel :load-toplevel :execute)
  (require "amiga/ffi")
  (require "amiga/boopsi")
  (require "amiga/raw/exec")
  (require "amiga/raw/dos")
  (require "amiga/raw/utility")
  (require "amiga/raw/intuition"))

;; The :EXPORT list names the AMIGA.BOOPSI symbols too: DEFPACKAGE processes
;; :USE before :EXPORT (CLHS DEFPACKAGE), so exporting an inherited name
;; re-exports that very symbol rather than creating a homonym.  The two
;; %-helpers this file uses itself are imported rather than duplicated.
(defpackage "AMIGA.REACTION"
  (:use "CL" "FFI" "AMIGA.FFI" "AMIGA.BOOPSI")
  (:import-from "AMIGA.BOOPSI" "%ULONG" "%WITH-TAGS")
  (:export
   ;; Availability
   "AVAILABLE-P"
   ;; Methods (AMIGA.BOOPSI, re-exported)
   "DO-METHOD" "OBJECT-CLASS"
   ;; Foreign memory whose lifetime is the GUI's lifetime (AMIGA.BOOPSI, re-exported)
   "WITH-FOREIGN-POOL" "POOL-ALLOC" "POOL-STRING" "POOL-HOOK" "POOL-FINALIZER"
   "NEW-LIST" "FREE-LIST-NODES"
   "WITH-TAGS"
   ;; Objects and attributes (GET-ATTR, GET-ATTR-POINTER, SET-ATTRS: AMIGA.BOOPSI, re-exported)
   "NEW-OBJECT" "DISPOSE-OBJECT" "GET-ATTR" "GET-ATTR-POINTER"
   "SET-ATTRS" "SET-GADGET-ATTRS"
   ;; window.class
   "OPEN-WINDOW" "CLOSE-WINDOW" "ICONIFY" "HANDLE-INPUT" "WINDOW-SIGNAL-MASK"
   "DO-WINDOW-EVENTS" "*EVENT-LOOP-TIMEOUT*"
   ;; requester.class
   "OPEN-REQUESTER"))

(in-package "AMIGA.REACTION")

;;; ================================================================
;;; Constants
;;; ================================================================

;;; window.class / requester.class values this module needs itself.
;;; They are duplicated from the generated amiga/raw/classes/window and
;;; amiga/raw/classes/requester modules on purpose: those open their
;;; class library at REQUIRE time, and this module must load (and
;;; AVAILABLE-P must answer) on systems where the classes are absent.
;;; tests/amiga/test-reaction.lisp cross-checks them against the
;;; generated constants.

(defconstant +wm-handleinput+  #x570001)     ; classes/window.h WM_HANDLEINPUT
(defconstant +wm-open+         #x570002)     ; WM_OPEN
(defconstant +wm-close+        #x570003)     ; WM_CLOSE
(defconstant +wm-iconify+      #x570005)     ; WM_ICONIFY
(defconstant +window-sig-mask+ #x85025002)   ; WINDOW_SigMask
(defconstant +wmhi-lastmsg+    0)            ; WMHI_LASTMSG
(defconstant +rm-openreq+      #x650001)     ; classes/requester.h RM_OPENREQ

;;; ================================================================
;;; Availability
;;; ================================================================

(defun available-p ()
  "True when the ReAction classes can be opened on this system: an
AmigaOS/MorphOS runtime whose window.class opens.  NIL on the host and on
an AmigaOS without ReAction (a bare 3.1), where the per-class raw
modules would fail to REQUIRE."
  (and (member :amigaos *features*)
       (let ((base (amiga:open-library "window.class" 0)))
         (when base
           (amiga:close-library base)
           t))))

;;; ================================================================
;;; Objects and attributes
;;; ================================================================

(defun new-object (class &rest tags)
  "NewObjectA(CLASS, NULL, TAGS): create a BOOPSI object.  CLASS is the
Class pointer a raw module's xxx-GET-CLASS returns (BUTTON-GET-CLASS,
LAYOUT-GET-CLASS, WINDOW-GET-CLASS...).  TAGS is a plist of tag constant
and value; integer, foreign-pointer, T/NIL and string values are
accepted, strings being copied into the enclosing WITH-FOREIGN-POOL
\(the object keeps the pointer).  Returns the object as a foreign
pointer; signals an error when the class returns NULL (out of memory, a
missing required attribute) -- a NULL child silently handed on to a
layout is worse than an error here."
  (unless (and class (ffi:foreign-pointer-p class) (not (ffi:null-pointer-p class)))
    (error "AMIGA.REACTION:NEW-OBJECT: ~S is not a class pointer -- pass what the class module's xxx-GET-CLASS returns (is that module loaded on an AmigaOS/MorphOS with ReAction?)"
           class))
  (or (%with-tags (array tags)
        (amiga.raw.intuition:new-object-a class nil array))
      (error "AMIGA.REACTION:NEW-OBJECT: NewObjectA returned NULL for class #x~X with ~D tags -- out of memory, or a required attribute is missing/invalid"
             (ffi:foreign-pointer-address class) (floor (length tags) 2))))

(defun dispose-object (object)
  "DisposeObject(OBJECT) -- disposes the object and, for a window or
layout object, everything attached to it.  NIL is ignored."
  (when object
    (amiga.raw.intuition:dispose-object object))
  nil)

(defun set-gadget-attrs (gadget window &rest tags)
  "SetGadgetAttrsA(GADGET, WINDOW, NULL, TAGS): change attributes of a
gadget that is displayed in WINDOW, refreshing it.  WINDOW may be NIL
for a gadget that is not (yet) in a window.  Same value rules as
NEW-OBJECT."
  (%with-tags (array tags)
    (amiga.raw.intuition:set-gadget-attrs-a gadget window nil array)))

;;; ================================================================
;;; window.class
;;; ================================================================

(defun open-window (window-object)
  "RA_OpenWindow: open (or re-open after ICONIFY) the window.class
WINDOW-OBJECT.  Returns the struct Window as a foreign pointer, or NIL."
  (let ((window (do-method window-object +wm-open+)))
    (if (zerop window)
        nil
        (ffi:make-foreign-pointer window))))

(defun close-window (window-object)
  "RA_CloseWindow: close the window of WINDOW-OBJECT without disposing
the object (DISPOSE-OBJECT closes it as well)."
  (do-method window-object +wm-close+)
  nil)

(defun iconify (window-object)
  "RA_Iconify: close the window and put its icon on the Workbench; needs
WINDOW_IconifyGadget / WINDOW_AppPort at creation.  True on success."
  (not (zerop (do-method window-object +wm-iconify+))))

(defun window-signal-mask (window-object)
  "WINDOW_SigMask: the signals to Wait() on for this window's events
\(0 while the window is closed or iconified)."
  (or (get-attr +window-sig-mask+ window-object) 0))

(defun handle-input (window-object)
  "RA_HandleInput: process the next pending input message of
WINDOW-OBJECT.  Returns two values: the WMHI_* result (class bits in
WMHI_CLASSMASK, the gadget ID in WMHI_GADGETMASK, WMHI_LASTMSG = 0 when
nothing is pending) and the message's Code word (a checkbox's new state,
a raw key, ...)."
  (let ((code (ffi:alloc-foreign 2)))
    (unwind-protect
         (values (do-method window-object +wm-handleinput+ code)
                 (ffi:peek-u16 code 0))
      (ffi:free-foreign code))))

(defvar *event-loop-timeout* nil
  "Default :TIMEOUT of DO-WINDOW-EVENTS, in seconds; NIL = interactive
\(block until the user closes the window).  Set it before loading a GUI
program to run it unattended -- the examples' harness and the test suite
do:  --eval '(require \"amiga/reaction\")'
     --eval '(setf amiga.reaction:*event-loop-timeout* 5)' --load ...")

(defmacro do-window-events (((result code) window-object
                             &key (timeout '*event-loop-timeout*) signals)
                            &body body)
  "The ReAction event loop.  Waits on WINDOW-OBJECT's WINDOW_SigMask
\(plus SIGBREAKF_CTRL_C and the extra SIGNALS mask, e.g. the signal of a
WINDOW_AppPort) and runs BODY once per input message with RESULT bound to
the WMHI_* result and CODE to the message Code -- see HANDLE-INPUT.  BODY
calls (RETURN) to leave the loop; Ctrl-C leaves it too.

With TIMEOUT (seconds; defaults to *EVENT-LOOP-TIMEOUT*) the loop polls
instead of blocking and returns when the time is up -- for unattended
runs (test suites, screenshots), where nobody will click the close
gadget."
  (let ((win (gensym "WIN")) (deadline (gensym "DEADLINE"))
        (extra (gensym "EXTRA")) (sigs (gensym "SIGS")))
    `(let* ((,win ,window-object)
            (,extra (or ,signals 0))
            (,deadline (let ((timeout ,timeout))
                         (and timeout
                              (+ (get-internal-real-time)
                                 (round (* timeout internal-time-units-per-second)))))))
       (block nil
         (tagbody
          :wait
            (cond (,deadline
                   (when (>= (get-internal-real-time) ,deadline)
                     (return))
                   (amiga.raw.dos:delay 5))
                  (t
                   ;; AMIGA:WAIT-SIGNALS: GC-cooperative, so a worker
                   ;; thread's collection does not wait for the next event.
                   (let ((,sigs (amiga:wait-signals
                                 (logior (window-signal-mask ,win) ,extra
                                         amiga.raw.dos:+sigbreakf-ctrl-c+))))
                     (when (logtest ,sigs amiga.raw.dos:+sigbreakf-ctrl-c+)
                       (return)))))
          :next
            (multiple-value-bind (,result ,code) (handle-input ,win)
              (declare (ignorable ,code))
              (when (= ,result +wmhi-lastmsg+)
                (go :wait))
              ,@body)
            (go :next))))))

;;; ================================================================
;;; requester.class
;;; ================================================================

(defun open-requester (requester window &rest tags)
  "RM_OPENREQ: open the requester.class object REQUESTER over WINDOW
\(a struct Window, or NIL for the default public screen) with the
REQ_* / REQS_* / REQI_* attributes in TAGS, and wait for the user.
Returns the number of the gadget chosen (1 = leftmost, 0 = the
rightmost / cancel), as the C OpenRequesterTags pattern does."
  (%with-tags (array tags)
    (let ((msg (ffi:alloc-foreign 16)))
      (unwind-protect
           (progn
             ;; struct orRequest { MethodID; or_Attrs; or_Window; or_Screen; }
             (ffi:poke-u32 msg +rm-openreq+ 0)
             (ffi:poke-u32 msg (ffi:foreign-pointer-address array) 4)
             (ffi:poke-u32 msg (%ulong window) 8)
             (ffi:poke-u32 msg 0 12)
             (amiga.raw.utility:call-hook-pkt (object-class requester) requester msg))
        (ffi:free-foreign msg)))))

(provide "amiga/reaction")
