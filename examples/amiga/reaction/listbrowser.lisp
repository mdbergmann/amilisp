;;; listbrowser.lisp — listbrowser.gadget: a multi-column list.
;;;
;;; Common Lisp port of the NDK 3.2 example Examples/ListBrowser1.c: a
;;; three-column listbrowser.gadget with column titles and separators,
;;; its rows an exec list of listbrowser nodes.  Where the C walks
;;; through its modes on successive close-gadget clicks, this puts two
;;; of them on buttons — Multi-select and Auto-fit toggle the
;;; corresponding attributes through SET-GADGET-ATTRS — and adds a Quit
;;; button.  Selecting a row prints its first column, read back from the
;;; node with GetListBrowserNodeAttrsA; a double-click is told apart
;;; through LISTBROWSER_RelEvent.
;;;
;;; What it shows: AllocListBrowserNodeA with per-column tags (the node
;;; copies the text — LBNCA_CopyText), a hand-built struct ColumnInfo
;;; array in pooled memory, reading node attributes back.
;;;
;;; Run on AmigaOS 3.5+/3.2 or MorphOS (ReAction classes required):
;;;   amilisp --load examples/amiga/reaction/listbrowser.lisp

(require "amiga/reaction")
(require "amiga/raw/exec")
(require "amiga/raw/intuition")
(require "amiga/raw/classes/window")
(require "amiga/raw/gadgets/layout")
(require "amiga/raw/gadgets/button")
(require "amiga/raw/gadgets/listbrowser")

(defpackage "REACTION-LISTBROWSER"
  (:use "CL")
  (:local-nicknames ("RA"     "AMIGA.REACTION")
                    ("EXEC"   "AMIGA.RAW.EXEC")
                    ("INTUI"  "AMIGA.RAW.INTUITION")
                    ("WIN"    "AMIGA.RAW.CLASSES.WINDOW")
                    ("LAYOUT" "AMIGA.RAW.GADGETS.LAYOUT")
                    ("BUTTON" "AMIGA.RAW.GADGETS.BUTTON")
                    ("LB"     "AMIGA.RAW.GADGETS.LISTBROWSER")))

(in-package "REACTION-LISTBROWSER")

(defconstant +gid-list+    1)
(defconstant +gid-multi+   2)
(defconstant +gid-autofit+ 3)
(defconstant +gid-quit+    4)

(defparameter *rows*
  '("This is a" "test of the" "ListBrowser" "gadget class." "This is like"
    "a souped-up" "listview" "gadget.  It" "has many" "cool new" "features"
    "though like" "multiple" "columns," "horizontal" "scrolling," "images in"
    "nodes," "columns titles" "and much much" "more!"))

(defvar *list-gadget* nil)
(defvar *multi-select* nil)
(defvar *auto-fit* nil)

(defun make-column-info (columns)
  "A struct ColumnInfo array — one per column, terminated by { -1, ~0, ~0 }
— in pooled memory, laid out by the generated gadgets/listbrowser
bindings (*column-info-size*, column-info-width / -title / -flags).
COLUMNS is a list of (width title)."
  (let* ((n (length columns))
         (size lb:*column-info-size*)
         (ci (ra:pool-alloc (* size (1+ n)))))
    (loop for (width title) in columns
          for entry = ci then (ffi:pointer+ entry size)
          do (setf (lb:column-info-width entry) width
                   (lb:column-info-title entry) (ra:pool-string title)
                   (lb:column-info-flags entry) 0))
    (let ((end (ffi:pointer+ ci (* size n))))
      (setf (lb:column-info-width end) -1
            (lb:column-info-title end) #xFFFFFFFF
            (lb:column-info-flags end) #xFFFFFFFF))
    ci))

(defun make-row-list (rows)
  "The LISTBROWSER_Labels list: a three-column node per row.  With
LBNCA_CopyText the node copies the strings, so they need no pool."
  (let ((list (ra:new-list)))
    (loop for text in rows
          for i from 1
          do (ra:with-tags (tags lb:+lbna-column+ 0
                                 lb:+lbnca-copy-text+ t
                                 lb:+lbnca-text+ text
                                 lb:+lbna-column+ 1
                                 lb:+lbnca-copy-text+ t
                                 lb:+lbnca-text+ (format nil "~D" i)
                                 lb:+lbna-column+ 2
                                 lb:+lbnca-copy-text+ t
                                 lb:+lbnca-text+ (format nil "row ~D" i))
               (let ((node (lb:alloc-list-browser-node-a 3 tags)))
                 (unless node (error "listbrowser: AllocListBrowserNodeA failed"))
                 (exec:add-tail list node))))
    list))

(defun node-text (node column)
  "The text of COLUMN of the listbrowser NODE — GetListBrowserNodeAttrsA
with LBNCA_Text fills the longword we point it at."
  (let ((storage (ffi:alloc-foreign 4)))
    (unwind-protect
         (ra:with-tags (tags lb:+lbna-column+ column lb:+lbnca-text+ storage)
           (lb:get-list-browser-node-attrs-a node tags)
           (let ((text (ffi:peek-pointer storage 0)))
             (if (ffi:null-pointer-p text) "" (ffi:foreign-to-string text))))
      (ffi:free-foreign storage))))

(defun make-button (id text)
  (ra:new-object (button:button-get-class)
                 intui:+ga-id+ id
                 intui:+ga-rel-verify+ t
                 intui:+ga-text+ text))

(defun make-window-object (row-list column-info)
  (setf *list-gadget* (ra:new-object (lb:listbrowser-get-class)
                                     intui:+ga-id+ +gid-list+
                                     intui:+ga-rel-verify+ t
                                     lb:+listbrowser-labels+ row-list
                                     lb:+listbrowser-column-info+ column-info
                                     lb:+listbrowser-column-titles+ t
                                     lb:+listbrowser-separators+ t
                                     lb:+listbrowser-show-selected+ t
                                     lb:+listbrowser-multi-select+ nil))
  (ra:new-object (win:window-get-class)
    intui:+wa-screen-title+ "ReAction"
    intui:+wa-title+ "ReAction ListBrowser Example 1"
    intui:+wa-left+ 40
    intui:+wa-top+ 30
    intui:+wa-inner-width+ 320
    intui:+wa-inner-height+ 200
    intui:+wa-depth-gadget+ t
    intui:+wa-drag-bar+ t
    intui:+wa-close-gadget+ t
    intui:+wa-size-gadget+ t
    intui:+wa-activate+ t
    win:+window-parent-group+
    (ra:new-object (layout:layout-get-class)
                   layout:+layout-orientation+ layout:+layout-vertical+
                   layout:+layout-space-outer+ t
                   layout:+layout-defer-layout+ t
                   layout:+layout-add-child+ *list-gadget*
                   layout:+layout-add-child+
                   (ra:new-object (layout:layout-get-class)
                                  layout:+layout-orientation+ layout:+layout-horizontal+
                                  layout:+layout-even-size+ t
                                  layout:+layout-add-child+ (make-button +gid-multi+ "_Multi-select")
                                  layout:+layout-add-child+ (make-button +gid-autofit+ "_Auto-fit")
                                  layout:+layout-add-child+ (make-button +gid-quit+ "_Quit"))
                   layout:+child-weighted-height+ 0)))

(defun report-selection ()
  (let ((node (ra:get-attr-pointer lb:+listbrowser-selected-node+ *list-gadget*))
        (event (ra:get-attr lb:+listbrowser-rel-event+ *list-gadget*)))
    (if node
        (format t "~&; ~:[selected~;double-clicked~]: ~S (row ~D)~%"
                (eql event lb:+lbre-doubleclick+)
                (node-text node 0)
                (ra:get-attr lb:+listbrowser-selected+ *list-gadget*))
        (format t "~&; selection cleared~%"))))

(defun run ()
  (ra:with-foreign-pool ()
    (let* ((row-list (make-row-list *rows*))
           (column-info (make-column-info '((80 "Col 1") (60 "Col 2") (60 "Col 3"))))
           (win-obj (make-window-object row-list column-info)))
      (setf *multi-select* nil *auto-fit* nil)
      (unwind-protect
           (let ((window (ra:open-window win-obj)))
             (unless window
               (error "listbrowser: could not open the window"))
             (ra:do-window-events ((result code) win-obj)
               (let ((class (logand result win:+wmhi-classmask+)))
                 (cond ((= class win:+wmhi-closewindow+)
                        (return))
                       ((= class win:+wmhi-gadgetup+)
                        (let ((gid (logand result win:+wmhi-gadgetmask+)))
                          (cond ((= gid +gid-quit+)
                                 (return))
                                ((= gid +gid-list+)
                                 (report-selection))
                                ((= gid +gid-multi+)
                                 (setf *multi-select* (not *multi-select*))
                                 (format t "~&; multi-select ~:[off~;on~]~%" *multi-select*)
                                 (ra:set-gadget-attrs *list-gadget* window
                                                      lb:+listbrowser-multi-select+ *multi-select*))
                                ((= gid +gid-autofit+)
                                 (setf *auto-fit* (not *auto-fit*))
                                 (format t "~&; auto-fit ~:[off~;on~]~%" *auto-fit*)
                                 (ra:set-gadget-attrs *list-gadget* window
                                                      lb:+listbrowser-auto-fit+ *auto-fit*)))))))))
        ;; Dispose the window (and with it the gadget) before the nodes
        ;; it displays are freed.
        (ra:dispose-object win-obj)
        (ra:free-list-nodes row-list #'lb:free-list-browser-node)
        (setf *list-gadget* nil)))))

(if (ra:available-p)
    (run)
    (format t "~&; listbrowser: the ReAction classes are not available on this system (AmigaOS 3.5+/3.2 or MorphOS needed) — nothing to show.~%"))
