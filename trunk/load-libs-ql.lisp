;;; load-libs-ql.lisp — make the load-and-test dependencies
;;; resolvable through ASDF, with quicklisp kept entirely on this side.
;;;
;;; The sibling trunk/load-and-test-*.lisp scripts deliberately contain
;;; NO quicklisp references — they only ever call (asdf:load-system ...).
;;; That works because each of them loads THIS file first, and this file:
;;;
;;;   1. probes for quicklisp (~/quicklisp/setup.lisp on host,
;;;      S:quicklisp/setup.lisp on Amiga) and auto-installs it via
;;;      lib/quicklisp-install.lisp when missing;
;;;   2. loads setup.lisp + lib/quicklisp-compat.lisp once, which
;;;      registers quicklisp's dist + local-projects searchers with
;;;      ASDF — that is what lets (asdf:load-system :fiveam) find a
;;;      quicklisp-managed system at all;
;;;   3. defines ENSURE-QL-LIB, which a test script calls for the ONE
;;;      system it tests. It fetches that system from the quicklisp dist
;;;      into the local repo only if it is not there already, then leaves
;;;      the actual load to the script's own (asdf:load-system ...).
;;;
;;; We deliberately do NOT fetch every dependency up front. Each test
;;; script is meant to run from a cold boot (fresh image, and typically a
;;; cleared FASL cache), exercising the compile+load of just its own
;;; library — pre-fetching the others would touch the network for systems
;;; the run never uses. Fetching is idempotent at the local-repo level:
;;; once a release is on disk it is never re-downloaded, so cold-boot
;;; runs stay offline-fast.
;;;
;;; Run paths are relative to the repo root, matching how the scripts
;;; are invoked, e.g.:
;;;
;;;   ./build/host/amilisp --heap 24M --load trunk/load-and-test-5am.lisp
;;;
;;; The AmiLisp library forks (closer-mop, trivial-cltl2,
;;; introspect-environment, trivial-garbage) need no ENSURE-QL-LIB call —
;;; they are cloned into quicklisp's local-projects tree, so quicklisp's
;;; local-projects searcher resolves them ahead of the dist releases once
;;; setup runs.  The `swank` stub and `cl+ssl` facade (lib/shims/) need
;;; nothing either — lib/asdf.lisp auto-registers them on
;;; ASDF:*CENTRAL-REGISTRY*, searched ahead of the quicklisp searchers.

(require "asdf")

;; AmiLisp has no CFFI backend, so the cl+ssl/cffi chain can never load
;; here.  Tell HTTP libraries that pull it in transitively to fall back to
;; plain HTTP: drakma honours :drakma-no-ssl and then skips cl+ssl, which
;; is what lets drakma-based systems (e.g. chipi) compile on AmiLisp.
;; Harmless to other scripts — it only affects drakma's own dependencies.
(pushnew :drakma-no-ssl *features*)

;; Host: ~/quicklisp/setup.lisp, Amiga: S:quicklisp/setup.lisp
(defvar *ql-setup*
  #+amigaos #P"S:quicklisp/setup.lisp"
  #-amigaos (merge-pathnames "quicklisp/setup.lisp" (user-homedir-pathname)))

;; Install quicklisp if missing. Use FIND-SYMBOL to invoke the entry
;; point because the AMILISP-QL package does not exist at read time.
(unless (probe-file *ql-setup*)
  (load "lib/quicklisp-install.lisp")
  (funcall (find-symbol "INSTALL" "AMILISP-QL")))

;; Skip the SETUP load when AMILISP-QL:INSTALL has just initialised
;; quicklisp in this image (re-loading SETUP.LISP triggers a second
;; ASDF:OOS on the QL client, which redefines its CLOS classes and
;; corrupts generic-function dispatch). Same goes for QUICKLISP-COMPAT:
;; re-loading it re-wraps gray-streams and breaks dispatch.
(unless (member :quicklisp *features*)
  (load *ql-setup*))

(unless (member :quicklisp-compat *features*)
  (load "lib/quicklisp-compat.lisp"))

(defun ensure-ql-lib (name)
  "Make system NAME loadable from the local quicklisp repo, fetching it
from the dist only when it is not there yet.

If ASDF can already locate NAME (an already-downloaded dist release, or a
AmiLisp library fork / swank stub on quicklisp's local-projects path) we do nothing —
no network, no re-download. Otherwise we pull the matching dist release to
disk via QL-DIST:ENSURE-INSTALLED. Either way the system is NOT loaded
here; the caller does that with (asdf:load-system name).

Probing via ASDF loads the system's .asd, and some .asd files signal an
error themselves (static-vectors.asd rejects unsupported implementations
at load time).  Such a system counts as located — the .asd is on disk, a
dist fetch could not improve on it — and the error must not escape:
aborting here would kill a caller's whole dependency-fetch loop."
  (let ((sname (string-downcase (string name))))
    (unless (handler-case (asdf:find-system sname nil)
              (error (e)
                (format t "~&;; load-libs-ql: probing ~A signalled: ~A~%" sname e)
                ;; treat as locatable: the .asd exists (fetching from the
                ;; dist can't improve on it), it just refuses to load here
                t))
      (let ((sys (ignore-errors
                   (funcall (find-symbol "FIND-SYSTEM" "QL-DIST") sname))))
        (when sys
          (format t "~&;; load-libs-ql: fetching ~A from quicklisp dist~%" sname)
          (funcall (find-symbol "ENSURE-INSTALLED" "QL-DIST") sys))))
    sname))
