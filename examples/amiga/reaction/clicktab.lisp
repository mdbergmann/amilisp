;;; clicktab.lisp — clicktab.gadget tabs switching a page.gadget.
;;;
;;; Common Lisp port of the NDK 3.2 example Examples/ClickTab.c: a
;;; window.class window with a clicktab.gadget of four tabs, built from
;;; an exec list of clicktab nodes.  Where the C stops at showing the
;;; tabs, this adds what tabs are for: a page.gadget (part of
;;; layout.gadget) below them with one page per tab, switched with
;;; PAGE_Current + RethinkLayout when a tab is clicked — the way it is
;;; done on every ReAction version (3.2's CLICKTAB_PageGroup automates
;;; it, but only there).
;;;
;;; Run on AmigaOS 3.5+/3.2 or MorphOS (ReAction classes required):
;;;   amilisp --load examples/amiga/reaction/clicktab.lisp

(require "amiga/reaction")
(require "amiga/raw/exec")
(require "amiga/raw/intuition")
(require "amiga/raw/classes/window")
(require "amiga/raw/gadgets/layout")
(require "amiga/raw/gadgets/button")
(require "amiga/raw/gadgets/clicktab")
(require "amiga/raw/images/label")
(require "amiga/raw/images/bevel")

(defpackage "REACTION-CLICKTAB"
  (:use "CL")
  (:local-nicknames ("RA"       "AMIGA.REACTION")
                    ("EXEC"     "AMIGA.RAW.EXEC")
                    ("INTUI"    "AMIGA.RAW.INTUITION")
                    ("WIN"      "AMIGA.RAW.CLASSES.WINDOW")
                    ("LAYOUT"   "AMIGA.RAW.GADGETS.LAYOUT")
                    ("BUTTON"   "AMIGA.RAW.GADGETS.BUTTON")
                    ("CLICKTAB" "AMIGA.RAW.GADGETS.CLICKTAB")
                    ("LABEL"    "AMIGA.RAW.IMAGES.LABEL")
                    ("BEVEL"    "AMIGA.RAW.IMAGES.BEVEL")))

(in-package "REACTION-CLICKTAB")

(defconstant +id-clicktab+ 1)
(defconstant +id-quit+     2)

(defparameter *tab-names* '("Tab_1" "Tab_2" "Tab_3" "Tab_4"))

(defvar *main-group* nil)
(defvar *page* nil)

(defun make-tab-list (names)
  "The CLICKTAB_Labels list: one clicktab node per tab, numbered from 0."
  (let ((list (ra:new-list)))
    (loop for name in names
          for i from 0
          do (ra:with-tags (tags clicktab:+tna-text+ name
                                 clicktab:+tna-number+ i
                                 clicktab:+tna-enabled+ t
                                 clicktab:+tna-spacing+ 6)
               (let ((node (clicktab:alloc-click-tab-node-a tags)))
                 (unless node (error "clicktab: AllocClickTabNodeA failed"))
                 (exec:add-tail list node))))
    list))

(defun make-page (n)
  "One page: a bevelled group with a label and a button of its own."
  (ra:new-object (layout:layout-get-class)
                 layout:+layout-orientation+ layout:+layout-vertical+
                 layout:+layout-space-outer+ t
                 layout:+layout-bevel-style+ bevel:+bvs-group+
                 layout:+layout-horiz-alignment+ layout:+lalign-center+
                 layout:+layout-add-image+
                 (ra:new-object (label:label-get-class)
                                label:+label-text+ (format nil "This is page ~D" n))
                 layout:+layout-add-child+
                 (ra:new-object (button:button-get-class)
                                intui:+ga-text+ (format nil "A button on page ~D" n))
                 layout:+child-weighted-height+ 0))

(defun make-window-object (tab-list)
  (setf *page* (ra:new-object (layout:page-get-class)
                              layout:+page-add+ (make-page 1)
                              layout:+page-add+ (make-page 2)
                              layout:+page-add+ (make-page 3)
                              layout:+page-add+ (make-page 4))
        *main-group* (ra:new-object (layout:layout-get-class)
                                    layout:+layout-orientation+ layout:+layout-vertical+
                                    layout:+layout-space-outer+ t
                                    layout:+layout-defer-layout+ t
                                    layout:+layout-add-child+
                                    (ra:new-object (clicktab:clicktab-get-class)
                                                   intui:+ga-id+ +id-clicktab+
                                                   intui:+ga-rel-verify+ t
                                                   clicktab:+clicktab-labels+ tab-list
                                                   clicktab:+clicktab-current+ 0)
                                    layout:+child-weighted-height+ 0
                                    layout:+layout-add-child+ *page*
                                    layout:+layout-add-child+
                                    (ra:new-object (button:button-get-class)
                                                   intui:+ga-id+ +id-quit+
                                                   intui:+ga-rel-verify+ t
                                                   intui:+ga-text+ "_Quit")
                                    layout:+child-weighted-height+ 0))
  (ra:new-object (win:window-get-class)
                 intui:+wa-screen-title+ "ReAction"
                 intui:+wa-title+ "ReAction clicktab.gadget Example"
                 intui:+wa-size-gadget+ t
                 intui:+wa-left+ 40
                 intui:+wa-top+ 30
                 intui:+wa-depth-gadget+ t
                 intui:+wa-drag-bar+ t
                 intui:+wa-close-gadget+ t
                 intui:+wa-activate+ t
                 intui:+wa-smart-refresh+ t
                 win:+window-parent-group+ *main-group*))

(defun run ()
  (ra:with-foreign-pool ()
    (let* ((tab-list (make-tab-list *tab-names*))
           (win-obj (make-window-object tab-list)))
      (unwind-protect
           (let ((window (ra:open-window win-obj)))
             (unless window
               (error "clicktab: could not open the window"))
             (ra:do-window-events ((result code) win-obj)
               (let ((class (logand result win:+wmhi-classmask+)))
                 (cond ((= class win:+wmhi-closewindow+)
                        (return))
                       ((= class win:+wmhi-gadgetup+)
                        (let ((gid (logand result win:+wmhi-gadgetmask+)))
                          (cond ((= gid +id-quit+)
                                 (return))
                                ((= gid +id-clicktab+)
                                 ;; Code is the number of the tab clicked:
                                 ;; flip the page and let the layout redo
                                 ;; the window.
                                 (format t "~&; tab ~D selected~%" code)
                                 (ra:set-gadget-attrs *page* window layout:+page-current+ code)
                                 (layout:rethink-layout *main-group* window nil t)))))))))
        (ra:dispose-object win-obj)
        (ra:free-list-nodes tab-list #'clicktab:free-click-tab-node)
        (setf *main-group* nil *page* nil)))))

(if (ra:available-p)
    (run)
    (format t "~&; clicktab: the ReAction classes are not available on this system (AmigaOS 3.5+/3.2 or MorphOS needed) — nothing to show.~%"))
