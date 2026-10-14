;;; hello.lisp — the minimal MUI application.
;;;
;;; The MUI counterpart of every toolkit's first program: an Application
;;; object owning one Window whose root object is a vertical Group with a
;;; centred Text and an "_OK" button.  Closing the window (through a
;;; MUIM_Notify on MUIA_Window_CloseRequest that returns
;;; MUIV_Application_ReturnID_Quit) or Ctrl-C ends the program; the button
;;; reports through a return ID of its own.  It is the MUIdev.guide
;;; "hello world" -- ApplicationObject / WindowObject / VGroup / TextObject /
;;; SimpleButton -- written with AMIGA.MUI over the raw mui.h constants.
;;;
;;; What it shows: AMIGA.MUI:NEW-OBJECT with class keywords and nested
;;; objects, MAKE-OBJECT (MUI_MakeObject's builtin button), NOTIFY (the
;;; MUIM_Notify idiom), SET-ATTRS / GET-ATTR on MUIA_Window_Open, and the
;;; MUIM_Application_NewInput loop as DO-APPLICATION-EVENTS.
;;;
;;; Run on AmigaOS 3.x with MUI 3.8+ installed, or on MorphOS:
;;;   amilisp --load examples/amiga/mui/hello.lisp
;;; Unattended (auto-closes after 5 s):
;;;   amilisp --eval '(require "amiga/mui")' \
;;;           --eval '(setf amiga.mui:*event-loop-timeout* 5)' \
;;;           --load examples/amiga/mui/hello.lisp

(require "amiga/mui")

;; The raw module opens muimaster.library at REQUIRE time and fails there
;; on a system without MUI (the host, an Amiga without it); AMIGA.MUI loads
;; anywhere and AVAILABLE-P says whether MUI is there, so the constants are
;; only pulled in where they can be.  The bow-out at the end reads the same
;; answer.
(when (amiga.mui:available-p)
  (require "amiga/raw/muimaster"))

(defpackage "MUI-HELLO"
  (:use "CL")
  (:local-nicknames ("MUI" "AMIGA.MUI")))

(in-package "MUI-HELLO")

(defun m (name)
  "The value of the amiga/raw/muimaster constant NAME -- looked up at run
time, so this file reads on a system where that package does not exist."
  (symbol-value (or (find-symbol name "AMIGA.RAW.MUIMASTER")
                    (error "hello: amiga.raw.muimaster:~A not found" name))))

(defun run ()
  ;; The pool keeps the title / text strings alive for as long as the
  ;; objects that reference them exist.
  (mui:with-foreign-pool ()
    (let* ((ok  (mui:make-object :button "_OK"))            ; SimpleButton("_OK")
           (win (mui:new-object :window                      ; WindowObject
                  (m "+MUIA-WINDOW-TITLE+") "Hello from Lisp"
                  (m "+MUIA-WINDOW-ID+")    (mui:make-id "HELO")
                  (m "+MUIA-WINDOW-ROOT-OBJECT+")
                  (mui:new-object :group                     ; VGroup
                    (m "+MUIA-GROUP-CHILD+")
                    (mui:new-object :text                    ; TextObject
                      (m "+MUIA-FRAME+") (m "+MUIV-FRAME-TEXT+")
                      (m "+MUIA-TEXT-CONTENTS+")
                      ;; MUIX_C: the text engine's "centre" escape
                      (concatenate 'string (m "+MUIX-C+") "Hello, MUI!"))
                    (m "+MUIA-GROUP-CHILD+") ok)))
           (app (mui:new-object :application                 ; ApplicationObject
                  (m "+MUIA-APPLICATION-TITLE+")       "HelloMUI"
                  (m "+MUIA-APPLICATION-VERSION+")     "$VER: HelloMUI 1.0"
                  (m "+MUIA-APPLICATION-DESCRIPTION+") "AmiLisp's first MUI program"
                  (m "+MUIA-APPLICATION-BASE+")        "HELLOMUI"
                  (m "+MUIA-APPLICATION-WINDOW+")      win)))
      (unwind-protect
           (progn
             ;; the close gadget -> ReturnID(Quit); the button -> ReturnID(1)
             (mui:notify win (m "+MUIA-WINDOW-CLOSE-REQUEST+") t
                         :application (m "+MUIM-APPLICATION-RETURN-ID+")
                         (m "+MUIV-APPLICATION-RETURN-ID-QUIT+"))
             (mui:notify ok (m "+MUIA-PRESSED+") nil
                         :application (m "+MUIM-APPLICATION-RETURN-ID+") 1)
             ;; set(win, MUIA_Window_Open, TRUE) -- and check it really opened
             (mui:set-attrs win (m "+MUIA-WINDOW-OPEN+") t)
             (when (zerop (or (mui:get-attr (m "+MUIA-WINDOW-OPEN+") win) 0))
               (error "hello: the window would not open"))
             ;; NewInput / Wait until the close request's Quit arrives
             (mui:do-application-events ((id) app)
               (case id
                 (1 (format t "~&; OK pressed~%")))))
        ;; Disposing the application disposes the window and all its
        ;; children.
        (mui:dispose-object app)))))

(if (mui:available-p)
    (run)
    (format t "~&; hello: MUI is not available on this system (muimaster.library 3.8+ on AmigaOS, or MorphOS, needed) — nothing to show.~%"))
