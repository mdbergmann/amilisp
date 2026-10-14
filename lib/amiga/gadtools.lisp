;;; amiga/gadtools.lisp — GadTools library abstractions for AmiLisp
;;;
;;; Loaded via (require "amiga/gadtools").
;;; Provides gadget creation, menu construction, and GadTools message handling.

;; compile-time too: COMPILE-FILE (the host builds the lib/amiga FASLs) must see
;; these packages at read time, not only LOAD.
(eval-when (:compile-toplevel :load-toplevel :execute)
  (require "amiga/ffi")
  (require "amiga/intuition"))

(defpackage "AMIGA.GADTOOLS"
  (:use "CL" "FFI" "AMIGA.FFI")
  (:export
   ;; Library
   "*GADTOOLS-BASE*"
   ;; Visual info
   "GET-VISUAL-INFO" "FREE-VISUAL-INFO" "WITH-VISUAL-INFO"
   ;; Gadget creation
   "CREATE-CONTEXT" "CREATE-GADGET" "FREE-GADGETS"
   "WITH-GADGETS"
   ;; Gadget modification
   "SET-GADGET-ATTRS"
   ;; Menu creation
   "CREATE-MENUS" "LAYOUT-MENUS" "FREE-MENUS"
   "WITH-MENUS"
   ;; Message handling
   "GT-GET-MSG" "GT-REPLY-MSG"
   ;; Window refresh
   "GT-REFRESH-WINDOW" "GT-BEGIN-REFRESH" "GT-END-REFRESH"
   ;; Bevel box
   "DRAW-BEVEL-BOX"
   ;; Gadget kind constants
   "+BUTTON-KIND+" "+CHECKBOX-KIND+" "+INTEGER-KIND+"
   "+LISTVIEW-KIND+" "+MX-KIND+" "+NUMBER-KIND+"
   "+CYCLE-KIND+" "+PALETTE-KIND+" "+SCROLLER-KIND+"
   "+SLIDER-KIND+" "+STRING-KIND+" "+TEXT-KIND+"
   ;; NewGadget flags
   "+PLACETEXT-LEFT+" "+PLACETEXT-RIGHT+" "+PLACETEXT-ABOVE+"
   "+PLACETEXT-BELOW+" "+PLACETEXT-IN+" "+NG-HIGHLABEL+"
   ;; Tag constants
   "+GT-VISUAL-INFO+" "+GT-UNDERSCORE+"
   "+GTST-STRING+" "+GTST-MAX-CHARS+"
   "+GTIN-NUMBER+" "+GTIN-MAX-CHARS+"
   "+GTCB-CHECKED+"
   "+GTCY-LABELS+" "+GTCY-ACTIVE+"
   "+GTLV-LABELS+" "+GTLV-TOP+" "+GTLV-READ-ONLY+"
   "+GTLV-SELECTED+" "+GTLV-SHOW-SELECTED+"
   "+GTSL-MIN+" "+GTSL-MAX+" "+GTSL-LEVEL+"
   "+GTSC-TOP+" "+GTSC-TOTAL+" "+GTSC-VISIBLE+" "+GTSC-ARROWS+"
   "+GTMX-LABELS+" "+GTMX-ACTIVE+"
   "+GTTX-TEXT+" "+GTTX-COPY-TEXT+" "+GTTX-BORDER+"
   "+GTNM-NUMBER+" "+GTNM-BORDER+" "+GTNM-FORMAT+"
   "+GTBB-RECESSED+"
   "+GTMN-NEW-LOOK-MENUS+"
   ;; NewMenu constants
   "+NM-END+" "+NM-TITLE+" "+NM-ITEM+" "+NM-SUB+"
   "+NM-BARLABEL+"
   ;; NewMenu helpers
   "MAKE-NEW-MENU-ARRAY"
   ;; IDCMP helpers
   "+BUTTONIDCMP+" "+CHECKBOXIDCMP+" "+INTEGERIDCMP+"
   "+STRINGIDCMP+" "+CYCLEIDCMP+" "+MXIDCMP+"
   "+LISTVIEWIDCMP+" "+SCROLLERIDCMP+" "+SLIDERIDCMP+"))

(in-package "AMIGA.GADTOOLS")

;;; ================================================================
;;; GadTools library base.  NIL on the host build, where this module is
;;; loaded for COMPILE-FILE (the host builds the lib/amiga FASLs) -- same
;;; convention as the generated amiga/raw modules.
;;; ================================================================

(define-library-variable *gadtools-base*
  (when (member :amigaos *features*)
    (amiga:open-library "gadtools.library" 39)))
(when (and (member :amigaos *features*) (null *gadtools-base*))
  (error "Cannot open gadtools.library v39"))

;;; ================================================================
;;; LVO offsets
;;; ================================================================

(defconstant +lvo-create-gadget-a+       -30)
(defconstant +lvo-free-gadgets+          -36)
(defconstant +lvo-gt-set-gadget-attrs-a+ -42)
(defconstant +lvo-create-menus-a+        -48)
(defconstant +lvo-free-menus+            -54)
(defconstant +lvo-layout-menus-a+        -66)
(defconstant +lvo-gt-get-imsg+           -72)
(defconstant +lvo-gt-reply-imsg+         -78)
(defconstant +lvo-gt-refresh-window+     -84)
(defconstant +lvo-gt-begin-refresh+      -90)
(defconstant +lvo-gt-end-refresh+        -96)
(defconstant +lvo-create-context+       -114)
(defconstant +lvo-draw-bevel-box-a+    -120)
(defconstant +lvo-get-visual-info-a+   -126)
(defconstant +lvo-free-visual-info+    -132)
(defconstant +lvo-gt-get-gadget-attrs-a+ -174)

;;; ================================================================
;;; Gadget kind constants
;;; ================================================================

(defconstant +button-kind+     1)
(defconstant +checkbox-kind+   2)
(defconstant +integer-kind+    3)
(defconstant +listview-kind+   4)
(defconstant +mx-kind+         5)
(defconstant +number-kind+     6)
(defconstant +cycle-kind+      7)
(defconstant +palette-kind+    8)
(defconstant +scroller-kind+   9)
(defconstant +slider-kind+    11)
(defconstant +string-kind+   12)
(defconstant +text-kind+     13)

;;; ================================================================
;;; NewGadget placement flags
;;; ================================================================

(defconstant +placetext-left+  #x0001)
(defconstant +placetext-right+ #x0002)
(defconstant +placetext-above+ #x0004)
(defconstant +placetext-below+ #x0008)
(defconstant +placetext-in+    #x0010)
(defconstant +ng-highlabel+    #x0020)

;;; ================================================================
;;; Tag constants
;;; ================================================================

(defconstant +gt-tag-base+   (+ #x80000000 #x80000))

(defconstant +gt-visual-info+ (+ +gt-tag-base+ 52))
(defconstant +gt-underscore+  (+ +gt-tag-base+ 64))

;; Checkbox
(defconstant +gtcb-checked+   (+ +gt-tag-base+ 4))

;; Cycle
(defconstant +gtcy-labels+    (+ +gt-tag-base+ 14))
(defconstant +gtcy-active+    (+ +gt-tag-base+ 15))

;; Listview
(defconstant +gtlv-top+       (+ +gt-tag-base+ 5))
(defconstant +gtlv-labels+    (+ +gt-tag-base+ 6))
(defconstant +gtlv-read-only+ (+ +gt-tag-base+ 7))
(defconstant +gtlv-show-selected+ (+ +gt-tag-base+ 53))
(defconstant +gtlv-selected+  (+ +gt-tag-base+ 54))

;; MX (mutually exclusive)
(defconstant +gtmx-labels+    (+ +gt-tag-base+ 9))
(defconstant +gtmx-active+    (+ +gt-tag-base+ 10))

;; Text display
(defconstant +gttx-text+      (+ +gt-tag-base+ 11))
(defconstant +gttx-copy-text+ (+ +gt-tag-base+ 12))
(defconstant +gttx-border+    (+ +gt-tag-base+ 57))

;; Number display
(defconstant +gtnm-number+    (+ +gt-tag-base+ 13))
(defconstant +gtnm-border+    (+ +gt-tag-base+ 58))
(defconstant +gtnm-format+    (+ +gt-tag-base+ 75))

;; Scroller
(defconstant +gtsc-top+       (+ +gt-tag-base+ 21))
(defconstant +gtsc-total+     (+ +gt-tag-base+ 22))
(defconstant +gtsc-visible+   (+ +gt-tag-base+ 23))
(defconstant +gtsc-arrows+    (+ +gt-tag-base+ 59))

;; Slider
(defconstant +gtsl-min+       (+ +gt-tag-base+ 38))
(defconstant +gtsl-max+       (+ +gt-tag-base+ 39))
(defconstant +gtsl-level+     (+ +gt-tag-base+ 40))

;; String
(defconstant +gtst-string+    (+ +gt-tag-base+ 45))
(defconstant +gtst-max-chars+ (+ +gt-tag-base+ 46))

;; Integer
(defconstant +gtin-number+    (+ +gt-tag-base+ 47))
(defconstant +gtin-max-chars+ (+ +gt-tag-base+ 48))

;; Bevel box
(defconstant +gtbb-recessed+  (+ +gt-tag-base+ 51))

;; Menu
(defconstant +gtmn-new-look-menus+ (+ +gt-tag-base+ 67))

;;; ================================================================
;;; IDCMP flags for gadget types
;;; ================================================================

(defconstant +buttonidcmp+   amiga.intuition:+idcmp-gadgetup+)
(defconstant +checkboxidcmp+ amiga.intuition:+idcmp-gadgetup+)
(defconstant +integeridcmp+  amiga.intuition:+idcmp-gadgetup+)
(defconstant +stringidcmp+   amiga.intuition:+idcmp-gadgetup+)
(defconstant +cycleidcmp+    amiga.intuition:+idcmp-gadgetup+)
(defconstant +mxidcmp+       amiga.intuition:+idcmp-gadgetdown+)
;; LISTVIEWIDCMP = GADGETUP|GADGETDOWN|MOUSEMOVE|ARROWIDCMP, and ARROWIDCMP
;; adds INTUITICKS|MOUSEBUTTONS (gadtools.h) -- the ticks drive the arrow
;; auto-repeat.  (Was #x70 without those two; tests/test_amiga_curated_vs_raw.sh
;; checks every hand-typed value here against lib/amiga/raw.)
(defconstant +listviewidcmp+ (logior amiga.intuition:+idcmp-gadgetup+
                                      amiga.intuition:+idcmp-gadgetdown+
                                      amiga.intuition:+idcmp-mousemove+
                                      amiga.intuition:+idcmp-intuiticks+
                                      amiga.intuition:+idcmp-mousebuttons+))
(defconstant +scrolleridcmp+ (logior amiga.intuition:+idcmp-gadgetup+
                                      amiga.intuition:+idcmp-gadgetdown+
                                      amiga.intuition:+idcmp-mousemove+))
(defconstant +slideridcmp+   (logior amiga.intuition:+idcmp-gadgetup+
                                      amiga.intuition:+idcmp-gadgetdown+
                                      amiga.intuition:+idcmp-mousemove+))

;;; ================================================================
;;; NewGadget struct layout (30 bytes)
;;; ================================================================

(ffi:defcstruct new-gadget
  (left-edge   :u16  0)
  (top-edge    :u16  2)
  (width       :u16  4)
  (height      :u16  6)
  (gadget-text :pointer 8)
  (text-attr   :pointer 12)
  (gadget-id   :u16 16)
  (flags       :u32 18)
  (visual-info :pointer 22)
  (user-data   :pointer 26))

;;; ================================================================
;;; NewMenu constants
;;; ================================================================

(defconstant +nm-end+      0)
(defconstant +nm-title+    1)
(defconstant +nm-item+     2)
(defconstant +nm-sub+      3)
(defconstant +nm-barlabel+ #xFFFFFFFF)   ; (STRPTR)-1

;;; ================================================================
;;; Visual Info
;;; ================================================================

(defun get-visual-info (screen &rest tags)
  "Get VisualInfo for a screen. Returns a foreign pointer."
  (let* ((tag-list (if tags
                       (amiga.ffi:make-tag-list tags)
                       (amiga.ffi:make-tag-list nil)))
         (result (amiga:call-library *gadtools-base*
                                     +lvo-get-visual-info-a+
                                     (list :a0 screen :a1 tag-list))))
    (ffi:free-foreign tag-list)
    (if (zerop result)
        (error "GADTOOLS:GET-VISUAL-INFO failed")
        (ffi:make-foreign-pointer result))))

(defun free-visual-info (vi)
  "Free a VisualInfo obtained from GET-VISUAL-INFO."
  (amiga:call-library *gadtools-base* +lvo-free-visual-info+
                      (list :a0 vi)))

(defmacro with-visual-info ((var screen) &body body)
  "Get VisualInfo for SCREEN, bind to VAR, free on exit."
  `(let ((,var (get-visual-info ,screen)))
     (unwind-protect
       (progn ,@body)
       (free-visual-info ,var))))

;;; ================================================================
;;; Gadget creation
;;; ================================================================

(defun create-context (glist-ptr)
  "Create a gadget context (dummy head gadget for a gadget list).
GLIST-PTR is a foreign pointer to a Gadget* (4 bytes, initialized to 0).
Returns the context gadget."
  (let ((result (amiga:call-library *gadtools-base*
                                     +lvo-create-context+
                                     (list :a0 glist-ptr))))
    (if (zerop result)
        (error "GADTOOLS:CREATE-CONTEXT failed")
        (ffi:make-foreign-pointer result))))

(defun create-gadget (kind previous-gadget visual-info
                      &key (left 0) (top 0) (width 80) (height 14)
                           text (text-attr (ffi:make-foreign-pointer 0))
                           (gadget-id 0) (flags +placetext-in+)
                           (user-data 0) tags)
  "Create a GadTools gadget.
KIND is a gadget kind constant (e.g., +BUTTON-KIND+).
PREVIOUS-GADGET is the previous gadget in the list (or context).
VISUAL-INFO is from GET-VISUAL-INFO.
TAGS is an optional flat list of additional tag/value pairs."
  (ffi:with-foreign-alloc (ng *new-gadget-size*)
    (let ((text-fptr nil))
      (setf (new-gadget-left-edge ng) left)
      (setf (new-gadget-top-edge ng) top)
      (setf (new-gadget-width ng) width)
      (setf (new-gadget-height ng) height)
      ;; Handle text: if string, copy to foreign memory
      (if text
          (progn
            (setf text-fptr (ffi:foreign-string text))
            (setf (new-gadget-gadget-text ng)
                  (ffi:foreign-pointer-address text-fptr)))
          (setf (new-gadget-gadget-text ng) 0))
      (setf (new-gadget-text-attr ng)
            (if (ffi:foreign-pointer-p text-attr)
                (ffi:foreign-pointer-address text-attr) 0))
      (setf (new-gadget-gadget-id ng) gadget-id)
      (setf (new-gadget-flags ng) flags)
      (setf (new-gadget-visual-info ng)
            (ffi:foreign-pointer-address visual-info))
      (setf (new-gadget-user-data ng) user-data)
      ;; Build tag list
      (let* ((all-tags (or tags nil))
             (tag-list (amiga.ffi:make-tag-list all-tags))
             (result (amiga:call-library *gadtools-base*
                                         +lvo-create-gadget-a+
                                         (list :d0 kind
                                               :a0 previous-gadget
                                               :a1 ng
                                               :a2 tag-list))))
        (ffi:free-foreign tag-list)
        ;; Note: text-fptr must NOT be freed here — GadTools may reference it.
        ;; It will be freed when FreeGadgets is called (the gadget copies it).
        ;; Actually, GadTools copies the label, so we can free it.
        (when text-fptr (ffi:free-foreign text-fptr))
        (if (zerop result)
            nil  ; CreateGadget can legitimately fail
            (ffi:make-foreign-pointer result))))))

(defun free-gadgets (gadget-list)
  "Free all gadgets in a gadget list."
  (amiga:call-library *gadtools-base* +lvo-free-gadgets+
                      (list :a0 gadget-list)))

(defmacro with-gadgets ((glist-var context-var visual-info) &body body)
  "Create a gadget context, bind list pointer to GLIST-VAR and context to
CONTEXT-VAR, free all gadgets on exit."
  `(ffi:with-foreign-alloc (,glist-var 4)
     (ffi:poke-u32 ,glist-var 0)
     (let ((,context-var (create-context ,glist-var)))
       (unwind-protect
         (progn ,@body)
         (free-gadgets (ffi:make-foreign-pointer
                        (ffi:peek-u32 ,glist-var)))))))

;;; ================================================================
;;; Gadget modification
;;; ================================================================

(defun set-gadget-attrs (gadget window &rest tags)
  "Modify gadget attributes.  TAGS is a flat list of tag/value pairs."
  (let ((tag-list (amiga.ffi:make-tag-list tags)))
    (amiga:call-library *gadtools-base* +lvo-gt-set-gadget-attrs-a+
                        (list :a0 gadget :a1 window
                              :a2 (ffi:make-foreign-pointer 0)
                              :a3 tag-list))
    (ffi:free-foreign tag-list)))

;;; ================================================================
;;; Menu creation
;;; ================================================================

;;; NewMenu struct layout (20 bytes per entry)
(ffi:defcstruct new-menu-entry
  (nm-type     :u8   0)
  ;; 1 byte pad at offset 1
  (nm-label    :pointer 2)
  (nm-commkey  :pointer 6)
  (nm-flags    :u16 10)
  (nm-mutual-exclude :u32 12)
  (nm-user-data :pointer 16))

(defconstant +new-menu-size+ 20)

(defun make-new-menu-array (entries)
  "Build a foreign NewMenu array from a list of menu specs.
Each entry is (type label &key commkey flags userdata) or :bar for separator.
The array is terminated by NM_END.
Returns the foreign pointer (caller must free it and any label strings)."
  (let* ((n (length entries))
         (size (* (1+ n) +new-menu-size+))
         (ptr (ffi:alloc-foreign size))
         (strings nil))
    (loop for entry in entries
          for i from 0
          for offset = (* i +new-menu-size+)
          do (if (eq entry :bar)
                 ;; Separator bar
                 (progn
                   (ffi:poke-u8 ptr +nm-item+ offset)          ; type
                   (ffi:poke-u32 ptr +nm-barlabel+ (+ offset 2)) ; label = NM_BARLABEL
                   (ffi:poke-u32 ptr 0 (+ offset 6))           ; commkey = NULL
                   (ffi:poke-u16 ptr 0 (+ offset 10))          ; flags = 0
                   (ffi:poke-u32 ptr 0 (+ offset 12))          ; mutual exclude
                   (ffi:poke-u32 ptr 0 (+ offset 16)))         ; userdata
                 ;; Regular entry
                 (destructuring-bind (type label &key commkey (flags 0) (userdata 0))
                     entry
                   (ffi:poke-u8 ptr type offset)
                   ;; Label
                   (let ((lbl (ffi:foreign-string label)))
                     (push lbl strings)
                     (ffi:poke-u32 ptr (ffi:foreign-pointer-address lbl)
                                   (+ offset 2)))
                   ;; Commkey
                   (if commkey
                       (let ((ck (ffi:foreign-string commkey)))
                         (push ck strings)
                         (ffi:poke-u32 ptr (ffi:foreign-pointer-address ck)
                                       (+ offset 6)))
                       (ffi:poke-u32 ptr 0 (+ offset 6)))
                   (ffi:poke-u16 ptr flags (+ offset 10))
                   (ffi:poke-u32 ptr 0 (+ offset 12))
                   (ffi:poke-u32 ptr userdata (+ offset 16)))))
    ;; Terminate with NM_END
    (let ((end-offset (* n +new-menu-size+)))
      (ffi:poke-u8 ptr +nm-end+ end-offset))
    (values ptr strings)))

(defun create-menus (new-menu-array &rest tags)
  "Create a menu strip from a NewMenu array.  Returns foreign pointer to Menu."
  (let* ((tag-list (amiga.ffi:make-tag-list
                    (append tags (list +gtmn-new-look-menus+ 1))))
         (result (amiga:call-library *gadtools-base*
                                     +lvo-create-menus-a+
                                     (list :a0 new-menu-array
                                           :a1 tag-list))))
    (ffi:free-foreign tag-list)
    (if (zerop result)
        (error "GADTOOLS:CREATE-MENUS failed")
        (ffi:make-foreign-pointer result))))

(defun layout-menus (menu visual-info &rest tags)
  "Layout menus for display. Must be called after CREATE-MENUS.
Returns T when LayoutMenusA succeeded, NIL when it failed."
  (let* ((tag-list (amiga.ffi:make-tag-list tags))
         (result (amiga:call-library *gadtools-base*
                                     +lvo-layout-menus-a+
                                     (list :a0 menu :a1 visual-info
                                           :a2 tag-list))))
    (ffi:free-foreign tag-list)
    (not (zerop result))))

(defun free-menus (menu)
  "Free a menu strip."
  (amiga:call-library *gadtools-base* +lvo-free-menus+
                      (list :a0 menu)))

(defmacro with-menus ((var entries visual-info &optional window) &body body)
  "Create menus from ENTRIES, layout with VISUAL-INFO, optionally attach
to WINDOW, and clean up on exit."
  (let ((nm-array (gensym "NM"))
        (strings (gensym "STRINGS"))
        (win (gensym "WIN")))
    `(multiple-value-bind (,nm-array ,strings)
         (make-new-menu-array ,entries)
       (let ((,var (create-menus ,nm-array))
             (,win ,window))
         (layout-menus ,var ,visual-info)
         (when ,win
           (amiga.intuition:set-menu-strip ,win ,var))
         (unwind-protect
           (progn ,@body)
           (when ,win
             (amiga.intuition:clear-menu-strip ,win))
           (free-menus ,var)
           (ffi:free-foreign ,nm-array)
           (dolist (s ,strings)
             (ffi:free-foreign s)))))))

;;; ================================================================
;;; GadTools message handling
;;; ================================================================

(defun gt-get-msg (port)
  "Get next IDCMP message via GadTools (handles gadget-specific processing).
Returns a foreign pointer to IntuiMessage, or NIL."
  (let ((result (amiga:call-library *gadtools-base*
                                     +lvo-gt-get-imsg+
                                     (list :a0 port))))
    (if (zerop result) nil (ffi:make-foreign-pointer result))))

(defun gt-reply-msg (msg)
  "Reply to a GadTools-processed message."
  (amiga:call-library *gadtools-base* +lvo-gt-reply-imsg+
                      (list :a1 msg)))

;;; ================================================================
;;; Window refresh
;;; ================================================================

(defun gt-refresh-window (window)
  "Refresh GadTools gadgets after window resize/reveal."
  (amiga:call-library *gadtools-base* +lvo-gt-refresh-window+
                      (list :a0 window
                            :a1 (ffi:make-foreign-pointer 0))))

(defun gt-begin-refresh (window)
  "Begin optimized refresh (call between BeginRefresh/EndRefresh)."
  (amiga:call-library *gadtools-base* +lvo-gt-begin-refresh+
                      (list :a0 window)))

(defun gt-end-refresh (window &optional (complete t))
  "End optimized refresh."
  (amiga:call-library *gadtools-base* +lvo-gt-end-refresh+
                      (list :a0 window :d0 (if complete 1 0))))

;;; ================================================================
;;; Bevel box
;;; ================================================================

(defun draw-bevel-box (rastport left top width height
                       &key visual-info recessed)
  "Draw a bevel box on a RastPort."
  (let* ((tags (list +gt-visual-info+ visual-info))
         (_ (when recessed
              (setf tags (append tags (list +gtbb-recessed+ 1)))))
         (tag-list (amiga.ffi:make-tag-list tags)))
    (declare (ignore _))
    (amiga:call-library *gadtools-base* +lvo-draw-bevel-box-a+
                        (list :a0 rastport
                              :d0 left :d1 top
                              :d2 width :d3 height
                              :a1 tag-list))
    (ffi:free-foreign tag-list)))

;;; ================================================================
;;; Provide module
;;; ================================================================

(provide "amiga/gadtools")
