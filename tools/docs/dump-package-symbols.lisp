;;;; dump-package-symbols.lisp
;;;;
;;;; Print the external symbols of AmiLisp's host-available extension
;;;; packages, one "PACKAGE|SYMBOL" line per export, sorted.  Consumed by
;;;; tools/docs/package-symbols.sh (the `make docs-check` / `make docs-update`
;;;; machinery) to keep docs/*.md symbol lists in sync with the real image.
;;;;
;;;; The AmigaOS-only packages (AMIGA, AMIGA.*) are NOT here — they don't exist
;;;; in the host build; package-symbols.sh parses those from source instead.

;; GRAY only exists once gray-streams is loaded; load it so the dump is
;; deterministic regardless of user init.
(ignore-errors (require "gray-streams"))

(dolist (p '("EXT" "MP" "FFI" "GRAY" "MOP" "CLAMIGA"))
  (let ((pkg (find-package p)))
    (when pkg
      (let ((names '()))
        (do-external-symbols (s pkg) (push (symbol-name s) names))
        (setf names (sort (remove-duplicates names :test #'string=) #'string<))
        (dolist (n names) (format t "~a|~a~%" p n))))))
(finish-output)
