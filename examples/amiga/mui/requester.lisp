;;; requester.lisp — MUI_Request, the easy requester: a window whose
;;; buttons open a plain information requester, a yes/no question, a
;;; three-way choice with a default gadget, and a requester whose text is
;;; formatted from Lisp values.  The answer lands in a text object and on
;;; standard output.
;;;
;;; What it shows: AMIGA.MUI:REQUEST (MUI_RequestA) with the gadget
;;; string syntax ('_' shortcut, '*' default, '|' separator), %ld / %s
;;; format parameters, its 1..n / 0 result, MUIA_Text_Contents set at run
;;; time (SET-ATTRS copies the string into the foreign pool),
;;; MUIA_Group_SameSize.
;;;
;;; Run on AmigaOS 3.x with MUI 3.8+ installed, or on MorphOS:
;;;   amilisp --load examples/amiga/mui/requester.lisp
;;; Unattended (auto-closes after 5 s; the requesters need a click, so
;;; only the window shows):
;;;   amilisp --eval '(require "amiga/mui")' \
;;;           --eval '(setf amiga.mui:*event-loop-timeout* 5)' \
;;;           --load examples/amiga/mui/requester.lisp

(require "amiga/mui")

(when (amiga.mui:available-p)
  (require "amiga/raw/muimaster"))

(defpackage "MUI-REQUESTER"
  (:use "CL")
  (:local-nicknames ("MUI" "AMIGA.MUI")))

(in-package "MUI-REQUESTER")

(defconstant +id-info+      1)
(defconstant +id-question+  2)
(defconstant +id-choice+    3)
(defconstant +id-formatted+ 4)

(defun m (name)
  "The value of the amiga/raw/muimaster constant NAME -- looked up at run
time, so this file reads on a system where that package does not exist."
  (symbol-value (or (find-symbol name "AMIGA.RAW.MUIMASTER")
                    (error "requester: amiga.raw.muimaster:~A not found" name))))

(defun children (objects)
  (loop for o in objects collect (m "+MUIA-GROUP-CHILD+") collect o))

(defun vgroup (tags &rest objects)
  (apply #'mui:new-object :group (append tags (children objects))))

(defun hgroup (tags &rest objects)
  (apply #'vgroup (list* (m "+MUIA-GROUP-HORIZ+") t tags) objects))

(defun centered (text)
  (concatenate 'string (m "+MUIX-C+") text))

(defun run ()
  (mui:with-foreign-pool ()
    (let* ((info      (mui:make-object :button "_Information"))
           (question  (mui:make-object :button "_Yes or no"))
           (choice    (mui:make-object :button "_Three choices"))
           (formatted (mui:make-object :button "_Formatted"))
           (quit      (mui:make-object :button "_Quit"))
           (answer (mui:new-object :text
                     (m "+MUIA-FRAME+") (m "+MUIV-FRAME-TEXT+")
                     (m "+MUIA-BACKGROUND+") (m "+MUII-TEXT-BACK+")
                     ;; the contents change at run time: do not let the
                     ;; first text fix the object's width
                     (m "+MUIA-TEXT-SET-MIN+") nil
                     (m "+MUIA-TEXT-CONTENTS+") (centered "No requester answered yet.")))
           (win (mui:new-object :window
                  (m "+MUIA-WINDOW-TITLE+") "Requesters"
                  (m "+MUIA-WINDOW-ID+")    (mui:make-id "REQU")
                  (m "+MUIA-WINDOW-ROOT-OBJECT+")
                  (vgroup '()
                    (mui:new-object :text
                      (m "+MUIA-FRAME+") (m "+MUIV-FRAME-TEXT+")
                      (m "+MUIA-BACKGROUND+") (m "+MUII-TEXT-BACK+")
                      (m "+MUIA-TEXT-CONTENTS+")
                      (centered (format nil "MUI_Request, the easy requester.~%Each button opens one; its answer is reported below.")))
                    answer
                    (hgroup (list (m "+MUIA-GROUP-SAME-SIZE+") t)
                      info question choice formatted)
                    (hgroup '()
                      (mui:new-object :rectangle) quit (mui:new-object :rectangle)))))
           (app (mui:new-object :application
                  (m "+MUIA-APPLICATION-TITLE+")       "Requesters"
                  (m "+MUIA-APPLICATION-VERSION+")     "$VER: Requesters 1.0"
                  (m "+MUIA-APPLICATION-DESCRIPTION+") "Show MUI_Request"
                  (m "+MUIA-APPLICATION-BASE+")        "REQUESTERS"
                  (m "+MUIA-APPLICATION-WINDOW+")      win))
           (answers 0)
           (last-answer "nothing"))
      (flet ((report (what result)
               (incf answers)
               (setf last-answer (format nil "~A: gadget ~D" what result))
               (format t "~&; ~A~%" last-answer)
               ;; set(answer, MUIA_Text_Contents, ...) -- the string is pooled
               (mui:set-attrs answer (m "+MUIA-TEXT-CONTENTS+") (centered last-answer))))
        (unwind-protect
             (progn
               (mui:notify win (m "+MUIA-WINDOW-CLOSE-REQUEST+") t
                           :application (m "+MUIM-APPLICATION-RETURN-ID+")
                           (m "+MUIV-APPLICATION-RETURN-ID-QUIT+"))
               (mui:notify quit (m "+MUIA-PRESSED+") nil
                           :application (m "+MUIM-APPLICATION-RETURN-ID+")
                           (m "+MUIV-APPLICATION-RETURN-ID-QUIT+"))
               (loop for button in (list info question choice formatted)
                     for id in (list +id-info+ +id-question+ +id-choice+ +id-formatted+)
                     do (mui:notify button (m "+MUIA-PRESSED+") nil
                                    :application (m "+MUIM-APPLICATION-RETURN-ID+") id))
               (mui:set-attrs win (m "+MUIA-WINDOW-OPEN+") t)
               (when (zerop (or (mui:get-attr (m "+MUIA-WINDOW-OPEN+") win) 0))
                 (error "requester: the window would not open"))
               (mui:do-application-events ((id) app)
                 (cond
                   ;; MUI_Request(app, win, 0, "Information", "_OK", "...")
                   ;; -- one gadget: the result is 1 either way
                   ((= id +id-info+)
                    (report "Information"
                            (mui:request app win "Information" "_OK"
                                         (format nil "A requester with a single gadget.~%Its result is 1 whether you click or press Return."))))
                   ;; two gadgets: 1 for the left one, 0 for the right one
                   ((= id +id-question+)
                    (report "Yes or no"
                            (mui:request app win "Question" "_Yes|_No"
                                         "Do you like Lisp on the Amiga?")))
                   ;; '*' marks the default gadget (Return), the rightmost is 0
                   ((= id +id-choice+)
                    (report "Three choices"
                            (mui:request app win "Unsaved changes" "_Save|_Use|*_Cancel"
                                         (format nil "Save the changes, use them for this session,~%or cancel?  Return picks the default, Cancel."))))
                   ;; %ld and %s take the parameters after the format string
                   ((= id +id-formatted+)
                    (report "Formatted"
                            (mui:request app win "Formatted" "_OK"
                                         (format nil "%ld requester~A answered so far.~%The last answer was: %s"
                                                 (if (= answers 1) "" "s"))
                                         answers last-answer))))))
          (mui:dispose-object app))))))

(if (mui:available-p)
    (run)
    (format t "~&; requester: MUI is not available on this system (muimaster.library 3.8+ on AmigaOS, or MorphOS, needed) — nothing to show.~%"))
