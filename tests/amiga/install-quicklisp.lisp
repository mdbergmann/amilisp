;; Install quicklisp to S:quicklisp/ on Amiga
;; Run once: amilisp --heap 48M --non-interactive --load tests/amiga/install-quicklisp.lisp
(load "lib/asdf.lisp")
(load "lib/quicklisp.lisp")
;; Set *home* directly to work around keyword default issues
(setf quicklisp-quickstart::*home* #P"S:quicklisp/")
(quicklisp-quickstart:install :path #P"S:quicklisp/")
(format t "~%Quicklisp installed to S:quicklisp/~%")
