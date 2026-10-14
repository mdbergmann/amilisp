;;; save-boot-image.lisp — write the bare-boot heap image a deployment ships
;;; beside its amilisp binary as `amilisp.img` (specs/image-save-load.md).
;;;
;;; Images are per-build, so this runs ON THE TARGET, with the binary that
;;; ships, from the directory that binary lives in, with nothing loaded but
;;; boot + CLOS:
;;;
;;;   cd bin/aos3
;;;   amilisp --no-userinit --no-image --non-interactive --load //scripts/save-boot-image.lisp
;;;
;;; (`//` is AmigaDOS for the grandparent directory: the native binary does
;;; not know `..`, so `../../scripts/...` fails there with "Cannot open
;;; file" -- that spelling is for a POSIX host only.)
;;;
;;; `make -f Makefile.cross image-amiga` runs it unattended in FS-UAE,
;;; `make -f Makefile.mos image` natively on MorphOS, `make image` on the
;;; host (into build/host/image/, for `make install`), and
;;; scripts/make-binary-release.sh runs it against the staged release layout
;;; (so the image is dumped from the very FASLs that ship).
;;; scripts/verify-boot-image.lisp checks the result.
;;;
;;; The image goes to the CURRENT directory: startup looks for an image
;;; beside the binary (PROGDIR: / the executable's directory), which is
;;; where a release keeps one per binary — a single image at the release
;;; root would be tried by every binary and refused by all but one.

(when ext:*image-restored-p*
  (format t "SAVE-BOOT-IMAGE-FAILED: this session was itself restored from an image (a stale amilisp.img beside the binary?) - rerun with --no-image~%")
  (finish-output)
  (quit 1))

;; Deferred: the dump runs at the top-level safe point after this load,
;; then the process exits (:quit t) — with --non-interactive, right away.
(ext:save-image "amilisp.img" :quit t)
