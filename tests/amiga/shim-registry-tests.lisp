; Bundled ASDF shim registration (lib/shims/) — loading ASDF must push
; lib/shims/{cl+ssl,swank}/ onto ASDF:*CENTRAL-REGISTRY*, resolved from
; *LOAD-TRUENAME* at load time.  On Amiga that exercises the device-path
; pathname leg (AmiLisp:lib/shims/...) that the host shell test
; (tests/test_shim_registry.sh) cannot reach.  The registry entries are
; what make (asdf:find-system "cl+ssl") resolve to the bundled facade
; ahead of any Quicklisp/ocicl-installed cl+ssl — the mechanism drakma
; and Hunchentoot HTTPS rely on in a binary install.
;
; Loaded from run-tests.lisp as a nested LOAD (same pattern as
; arexx-tests.lisp / tls-tests.lisp).  All ASDF access goes through
; FIND-SYMBOL: the ASDF package does not exist when this file is READ
; (the compile-file pre-pass reads every form before the REQUIRE below
; has run), so asdf: symbols cannot appear literally.
;
; The REQUIRE runs in a worker thread with an enlarged C stack: the
; from-source compile of lib/asdf.lisp recurses deeper than the suite's
; 128K baseline stack (the guard fires cleanly at "C stack nearly
; exhausted"), matching the documented `stack 800000` requirement for
; Quicklisp/ASDF work on Amiga.  Packages, functions, and the
; *CENTRAL-REGISTRY* global the epilogue pushes onto are process-global,
; so the main thread sees everything after the join.  The find-system
; checks below stay on the main thread — loading the small shim .asd
; files is fine on the baseline stack.

; No handler-case around the REQUIRE: an enclosing handler suppresses
; LOAD's per-form error recovery, turning the first failing form into a
; silent whole-load abort.  Bare, a failing form prints its error and
; backtrace to the log and the load continues — the registry checks
; below then report the outcome either way.
(check "shims: asdf loads in enlarged-stack worker" t
       (mp:join-thread
        (mp:make-thread
         (lambda () (require "asdf") t)
         :name "asdf-loader"
         :stack-size 800000
         :vm-frames 4096
         :vm-stack-size 16384)))

(let* ((asdf-pkg (find-package "ASDF"))
       (registry (symbol-value (find-symbol "*CENTRAL-REGISTRY*" asdf-pkg)))
       (find-system (find-symbol "FIND-SYSTEM" asdf-pkg))
       (source-dir (find-symbol "SYSTEM-SOURCE-DIRECTORY" asdf-pkg)))
  (flet ((registered-p (shim)
           (let ((frag (concatenate 'string "lib/shims/" shim)))
             (not (null (some (lambda (entry)
                                (search frag (namestring entry)
                                        :test #'char-equal))
                              registry)))))
         (resolves-to-shim-p (shim)
           (let ((frag (concatenate 'string "lib/shims/" shim))
                 (dir (funcall source-dir (funcall find-system shim))))
             (not (null (search frag (namestring dir) :test #'char-equal))))))
    (check "shims: cl+ssl on central registry" t (registered-p "cl+ssl"))
    (check "shims: swank on central registry" t (registered-p "swank"))
    (check "shims: cl+ssl resolves to bundled facade" t
           (resolves-to-shim-p "cl+ssl"))
    (check "shims: swank resolves to bundled stub" t
           (resolves-to-shim-p "swank"))))
