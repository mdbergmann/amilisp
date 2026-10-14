;;; Host-side cross-check of the hand-written lib/amiga/*.lisp modules
;;; (AMIGA.EXEC, AMIGA.INTUITION, AMIGA.GFX, AMIGA.GADTOOLS, AMIGA.AUDIO, ...)
;;; against the GENERATED lib/amiga/raw/ bindings, whose constants come from
;;; the NDK 3.2 includes and whose LVOs are cross-checked by the generator
;;; against the NDK's own lvo/*.i.  Driven by tests/test_amiga_curated_vs_raw.sh.
;;;
;;; Why: the curated modules still carry hand-typed constants and LVO offsets
;;; (+WFLG-REPORTMOUSE+ was #x4 = DEPTHGADGET for a long time; the real value
;;; is #x200).  Until they are rebased onto the raw modules, this test makes
;;; every hand-typed value agree with the generated one.
;;;
;;; Four checks:
;;;   1. CONSTANTS  every +NAME+ constant owned by a curated package that also
;;;      exists under the same name in any AMIGA.RAW.* package has the same
;;;      value (compared on the loaded symbols, so macro/expression values
;;;      count too).  Curated constants with no raw namesake are reported as
;;;      "uncovered" -- informational, they are not OS constants or are named
;;;      differently.
;;;   2. LVO CONSTANTS  every +LVO-NAME+ in a curated module is the offset at
;;;      which the raw module of the same library defines a function of that
;;;      name (names compared without dashes: +LVO-SET-DRMD+ = SET-DR-MD).
;;;   3. DEFCFUN  every AMIGA.FFI:DEFCFUN in a curated module exists in the raw
;;;      module of its library base at the same offset with the same register
;;;      assignment.
;;;   4. STRUCT LAYOUTS  a curated module that reads an OS struct by hand
;;;      (AMIGA.MUI's custom-class support cannot require the raw muimaster
;;;      module, which opens the library at load) spells its offsets and sizes
;;;      as constants of a fixed form, and each must agree with the generated
;;;      struct rows: +<ACCESSOR>-OFFSET+ is the offset of the raw field
;;;      accessor <ACCESSOR> (+MUI-AREA-DATA-FLAGS-OFFSET+ = 36), +<STRUCT>-SIZE+
;;;      the raw *<STRUCT>-SIZE* (+MUI-NOTIFY-DATA-SIZE+ = 28) -- in whichever
;;;      raw module carries the struct.  A name without a raw namesake FAILS
;;;      (a typo would otherwise pin nothing).
;;; Checks 2 and 3 read the SOURCE forms (the offsets/registers are not
;;; introspectable on the compiled wrappers), honouring IN-PACKAGE and
;;; descending into the raw modules' WHEN/PROGN platform guards.
;;;
;;; Prints one "ok  "/"FAIL" line per module and check, the uncovered names,
;;; and a final CURATED-VS-RAW-RESULT ok=N fail=M uncovered=K line.

(defvar *ok* 0)
(defvar *fail* 0)
(defvar *uncovered* 0)

(defun pass (fmt &rest args)
  (incf *ok*)
  (format t "~&ok   ~?~%" fmt args))

(defun failed (fmt &rest args)
  (incf *fail*)
  (format t "~&FAIL ~?~%" fmt args))

;;; ------------------------------------------------------------------
;;; The curated modules: require name, package, the raw packages of the
;;; libraries whose LVOs they hand-type (first = the module's own library).
;;; ------------------------------------------------------------------

(defparameter *curated*
  '(("amiga/ffi"       "AMIGA.FFI"       ())
    ("amiga/exec"      "AMIGA.EXEC"      ("AMIGA.RAW.EXEC"))
    ("amiga/intuition" "AMIGA.INTUITION" ("AMIGA.RAW.INTUITION" "AMIGA.RAW.EXEC" "AMIGA.RAW.DOS"))
    ("amiga/graphics"  "AMIGA.GFX"       ("AMIGA.RAW.GRAPHICS"))
    ("amiga/gadtools"  "AMIGA.GADTOOLS"  ("AMIGA.RAW.GADTOOLS" "AMIGA.RAW.INTUITION"))
    ("amiga/audio"     "AMIGA.AUDIO"     ("AMIGA.RAW.EXEC"))
    ("amiga/ahi"       "AMIGA.AHI"       ("AMIGA.RAW.AHI" "AMIGA.RAW.EXEC"))
    ("amiga/arexx"     "AMIGA.AREXX"     ("AMIGA.RAW.REXXSYSLIB" "AMIGA.RAW.EXEC"))
    ("amiga/boopsi"    "AMIGA.BOOPSI"    ())
    ("amiga/reaction"  "AMIGA.REACTION"  ())
    ("amiga/mui"       "AMIGA.MUI"       ())))

;;; library base variable (as the curated DEFCFUNs name it) -> raw package
(defparameter *base-var->raw-package*
  '(("*EXEC-BASE*"      . "AMIGA.RAW.EXEC")
    ("*INTUITION-BASE*" . "AMIGA.RAW.INTUITION")
    ("*GFX-BASE*"       . "AMIGA.RAW.GRAPHICS")
    ("*GRAPHICS-BASE*"  . "AMIGA.RAW.GRAPHICS")
    ("*GADTOOLS-BASE*"  . "AMIGA.RAW.GADTOOLS")
    ("*DOS-BASE*"       . "AMIGA.RAW.DOS")
    ("*UTILITY-BASE*"   . "AMIGA.RAW.UTILITY")
    ("*REXXSYS-BASE*"   . "AMIGA.RAW.REXXSYSLIB")
    ("*AHI-BASE*"       . "AMIGA.RAW.AHI")
    ("*MUIMASTER-BASE*" . "AMIGA.RAW.MUIMASTER")))

;;; ------------------------------------------------------------------
;;; Load everything: all generated modules (two directory levels:
;;; lib/amiga/raw/<lib>.lisp and lib/amiga/raw/<class-dir>/<lib>.lisp),
;;; then the curated ones.  Every module loads on the host build (library
;;; bases stay NIL there).
;;; ------------------------------------------------------------------

(defun raw-module-name (path)
  "lib/amiga/raw/gadgets/button.lisp -> \"amiga/raw/gadgets/button\"."
  (let* ((dir (pathname-directory path))
         (tail (member "amiga" dir :test #'string=)))
    (format nil "~{~A/~}~A" tail (pathname-name path))))

(defparameter *raw-modules*
  (sort (mapcar #'raw-module-name
                (append (directory "lib/amiga/raw/*.lisp")
                        (directory "lib/amiga/raw/*/*.lisp")))
        #'string<))

(format t "~&; loading ~D raw modules~%" (length *raw-modules*))
(dolist (m *raw-modules*) (require m))
(dolist (c *curated*) (require (first c)))

(defun raw-packages ()
  (remove-if-not (lambda (p)
                   (let ((n (package-name p)))
                     (and (> (length n) 10) (string= "AMIGA.RAW." n :end2 10))))
                 (list-all-packages)))

;;; ------------------------------------------------------------------
;;; Source-form tables
;;; ------------------------------------------------------------------

(defun module-source (require-name)
  (format nil "lib/~A.lisp" require-name))

(defun raw-package-source (pkg-name)
  "AMIGA.RAW.GADGETS.BUTTON -> lib/amiga/raw/gadgets/button.lisp"
  (format nil "lib/amiga/raw/~A.lisp"
          (substitute #\/ #\. (string-downcase (subseq pkg-name 10)))))

(defun read-forms (path)
  "Top-level forms of PATH, read with *PACKAGE* following its IN-PACKAGE forms."
  (let ((*package* (find-package "CL-USER"))
        (forms '()))
    (with-open-file (in path)
      (loop for form = (read in nil :eof)
            until (eq form :eof)
            do (when (and (consp form) (symbolp (car form))
                          (string= (symbol-name (car form)) "IN-PACKAGE"))
                 (setf *package* (or (find-package (string (second form)))
                                     (error "~A: IN-PACKAGE ~S does not exist"
                                            path (second form)))))
               (push form forms)))
    (nreverse forms)))

(defun walk-defcfuns (form fn)
  "Call FN on every (DEFCFUN name base offset (regspec...) ...) form in FORM,
including those nested in the raw modules' WHEN/UNLESS/PROGN/EVAL-WHEN guards."
  (when (and (consp form) (symbolp (car form)))
    (let ((head (symbol-name (car form))))
      (cond ((string= head "DEFCFUN") (funcall fn form))
            ((member head '("WHEN" "UNLESS" "PROGN" "EVAL-WHEN" "LET" "IF")
                     :test #'string=)
             (dolist (sub (cdr form)) (walk-defcfuns sub fn)))))))

(defun defcfun-entry (form)
  "(name base-var-name offset regs) for a DEFCFUN form; OFFSET may be a
constant name in the curated modules, so it is evaluated."
  (destructuring-bind (name base offset regspec &rest keys) (cdr form)
    (declare (ignore keys))
    (list (symbol-name name)
          (symbol-name base)
          (eval offset)
          (loop for (reg nil) on regspec by #'cddr collect reg))))

(defun raw-table-from-package (pkg-name)
  "Hash: offset -> list of (name base regs) for every library function the
raw package PKG-NAME binds: the :fn rows of its binding table (a generated
module IS one AMIGA.FFI:DEFINE-BINDING-TABLE, see
specs/raw-bindings-footprint.md) plus the >7-register DEFCFUN forms that
follow the table in its source.  BASE is the table's :base variable name,
REGS the register keyword list — the same shape DEFCFUN-ENTRY yields."
  (let* ((table (make-hash-table))
         (info (amilisp::%binding-table-info pkg-name))
         (base (and info (getf info :base) (symbol-name (getf info :base)))))
    (unless info
      (error "~A carries no binding table (enumerated before the tables were read?)"
             pkg-name))
    (dolist (row (amilisp::%binding-table-entries pkg-name))
      (case (first row)
        (:fn
         (destructuring-bind (kind name lvo regs &rest more) row
           (declare (ignore kind more))
           (push (list name base regs) (gethash lvo table))))
        ;; the struct rows, for check 4: (:field "ACCESSOR" type offset) and
        ;; the (:var "*STRUCT-SIZE*" n) a (:struct ...) row expands into
        (:field
         (setf (gethash (second row) *raw-fields*) (cons pkg-name (fourth row))))
        (:var
         (let ((n (second row)))
           (when (and (> (length n) 7) (char= (char n 0) #\*) (suffix-p "-SIZE*" n))
             (setf (gethash (subseq n 1 (- (length n) 6)) *raw-sizes*)
                   (cons pkg-name (third row))))))))
    (dolist (form (read-forms (raw-package-source pkg-name)))
      (walk-defcfuns form
                     (lambda (f)
                       (destructuring-bind (name fbase offset regs) (defcfun-entry f)
                         (push (list name fbase regs) (gethash offset table))))))
    table))

(defvar *raw-tables* (make-hash-table :test #'equal)
  "raw package name -> its function table")
(defvar *raw-fields* (make-hash-table :test #'equal)
  "\"ACCESSOR\" -> (raw-package . offset), every struct field row of every raw module")
(defvar *raw-sizes* (make-hash-table :test #'equal)
  "\"STRUCT\" -> (raw-package . size), every *STRUCT-SIZE* of every raw module")

(defun suffix-p (suffix s)
  (and (>= (length s) (length suffix))
       (string= suffix s :start2 (- (length s) (length suffix)))))

(defun raw-table (pkg-name)
  (or (gethash pkg-name *raw-tables*)
      (setf (gethash pkg-name *raw-tables*) (raw-table-from-package pkg-name))))

;; Read every raw table now, before any check can enumerate a package
;; (DO-SYMBOLS on a lazy package materialises it and drops the table).
(dolist (rp (raw-packages)) (raw-table (package-name rp)))

(defun normalize (name)
  "Dash-free upper-case spelling, so SET-DRMD and SET-DR-MD compare equal."
  (remove #\- (string-upcase name)))

(defun raw-short-name (pkg-name)
  "AMIGA.RAW.DOS -> \"DOS\": the prefix a curated module may put on an LVO
constant of another library (+LVO-DOS-DELAY+ for dos.library's Delay)."
  (normalize (subseq pkg-name 10)))

(defun lvo-name-matches-p (fname raw-pkg entry)
  "FNAME (normalized, from +LVO-FNAME+) names ENTRY's function, with or
without the library prefix."
  (let ((rn (normalize (first entry))))
    (or (string= fname rn)
        (string= fname (concatenate 'string (raw-short-name raw-pkg) rn)))))

(defun same-os-value-p (a b)
  "Equal as 32-bit C values: ((STRPTR)~0) reads as -1 from the C header and as
#xFFFFFFFF when hand-typed -- the same pointer bits."
  (if (and (integerp a) (integerp b))
      (= (logand a #xFFFFFFFF) (logand b #xFFFFFFFF))
      (equal a b)))

(defun lvo-constant-function-name (sym)
  "+LVO-SET-DRMD+ -> \"SETDRMD\", or NIL when SYM is not an LVO constant."
  (let ((n (symbol-name sym)))
    (when (and (> (length n) 6) (string= "+LVO-" n :end2 5)
               (char= #\+ (char n (1- (length n)))))
      (normalize (subseq n 5 (1- (length n)))))))

(defun layout-constant-key (sym)
  "For a struct layout constant (check 4): (values \"ACCESSOR\" :offset) for
+ACCESSOR-OFFSET+, (values \"STRUCT\" :size) for +STRUCT-SIZE+; NIL otherwise."
  (let ((n (symbol-name sym)))
    (when (and (> (length n) 8) (char= #\+ (char n 0)) (char= #\+ (char n (1- (length n)))))
      (cond ((suffix-p "-OFFSET+" n) (values (subseq n 1 (- (length n) 8)) :offset))
            ((suffix-p "-SIZE+" n) (values (subseq n 1 (- (length n) 6)) :size))
            (t nil)))))

;;; ------------------------------------------------------------------
;;; Check 1: constants by name against every raw package
;;; ------------------------------------------------------------------

(defun owned-constants (pkg)
  "Constants +NAME+ whose home package is PKG, sorted by name."
  (let ((out '()))
    (do-symbols (s pkg)
      (let ((n (symbol-name s)))
        (when (and (eq (symbol-package s) pkg)
                   (> (length n) 2)
                   (char= #\+ (char n 0)) (char= #\+ (char n (1- (length n))))
                   (constantp s) (boundp s))
          (push s out))))
    (sort out #'string< :key #'symbol-name)))

(defun check-constants (pkg-name)
  (let* ((pkg (find-package pkg-name))
         (raws (raw-packages))
         (matched 0)
         (uncovered '()))
    (dolist (s (owned-constants pkg))
      (unless (or (lvo-constant-function-name s)   ; LVO constants: check 2
                  (layout-constant-key s))         ; struct layouts: check 4
        (let ((hits '()))
          (dolist (rp raws)
            (multiple-value-bind (rs status) (find-symbol (symbol-name s) rp)
              (when (and status (eq (symbol-package rs) rp) (constantp rs) (boundp rs))
                (push (cons (package-name rp) (symbol-value rs)) hits))))
          (cond ((null hits) (push (symbol-name s) uncovered))
                (t
                 (incf matched)
                 (dolist (h hits)
                   (unless (same-os-value-p (symbol-value s) (cdr h))
                     (failed "~A:~A = ~S but ~A:~A = ~S"
                             pkg-name (symbol-name s) (symbol-value s)
                             (car h) (symbol-name s) (cdr h)))))))))
    (incf *uncovered* (length uncovered))
    (pass "~A: ~D constant~:P agree with the raw bindings, ~D uncovered"
          pkg-name matched (length uncovered))
    (when uncovered
      (format t "~&     uncovered (no raw namesake): ~{~A~^ ~}~%" (nreverse uncovered)))))

;;; ------------------------------------------------------------------
;;; Check 2: +LVO-NAME+ constants against the raw modules' functions
;;; ------------------------------------------------------------------

(defun check-lvo-constants (pkg-name raw-pkgs)
  (let* ((pkg (find-package pkg-name))
         (n 0))
    (dolist (s (owned-constants pkg))
      (let ((fname (lvo-constant-function-name s)))
        (when fname
          (incf n)
          (let* ((offset (symbol-value s))
                 (found-at '())     ; (raw-pkg . names at that offset)
                 (named-ok nil))
            (dolist (rp raw-pkgs)
              (let ((entries (gethash offset (raw-table rp))))
                (when entries
                  (push (cons rp (mapcar #'first entries)) found-at)
                  (when (find-if (lambda (e) (lvo-name-matches-p fname rp e)) entries)
                    (setf named-ok rp)))))
            (cond (named-ok)
                  (found-at
                   (failed "~A:~A = ~D, but at that LVO the raw bindings have ~{~{~A: ~{~A~^, ~}~}~^; ~}"
                           pkg-name (symbol-name s) offset
                           (mapcar (lambda (fa) (list (car fa) (cdr fa))) found-at)))
                  (t
                   (failed "~A:~A = ~D: no function at that LVO in ~{~A~^, ~}"
                           pkg-name (symbol-name s) offset raw-pkgs)))))))
    (when (> n 0)
      (pass "~A: ~D LVO constant~:P name the function the raw bindings have at that offset"
            pkg-name n))))

;;; ------------------------------------------------------------------
;;; Check 3: DEFCFUN wrappers against the raw modules (offset + registers)
;;; ------------------------------------------------------------------

(defun check-defcfuns (require-name pkg-name)
  (let ((n 0))
    (dolist (form (read-forms (module-source require-name)))
      (walk-defcfuns
       form
       (lambda (f)
         (destructuring-bind (name base offset regs) (defcfun-entry f)
           (incf n)
           (let ((rp (cdr (assoc base *base-var->raw-package* :test #'string=))))
             (cond ((null rp)
                    (failed "~A: DEFCFUN ~A uses base ~A, which this test cannot map to a raw module (extend *BASE-VAR->RAW-PACKAGE*)"
                            pkg-name name base))
                   (t
                    (let ((entries (gethash offset (raw-table rp))))
                      (cond ((null entries)
                             (failed "~A: DEFCFUN ~A at LVO ~D: ~A has no function there"
                                     pkg-name name offset rp))
                            ((find regs entries :key #'third :test #'equal))
                            (t
                             (failed "~A: DEFCFUN ~A at LVO ~D takes ~S, but ~A's ~{~A ~S~^ / ~} there"
                                     pkg-name name offset regs rp
                                     (mapcan (lambda (e) (list (first e) (third e)))
                                             entries))))))))))))
    (when (> n 0)
      (pass "~A: ~D DEFCFUN~:P match the raw bindings' LVO and registers" pkg-name n))))

;;; ------------------------------------------------------------------
;;; Check 4: struct layout constants against the raw modules' struct rows
;;; ------------------------------------------------------------------

(defun check-layout-constants (pkg-name)
  (let ((n 0))
    (dolist (s (owned-constants (find-package pkg-name)))
      (multiple-value-bind (key kind) (layout-constant-key s)
        (when key
          (incf n)
          (let ((raw (if (eq kind :offset)
                         (gethash key *raw-fields*)
                         (gethash key *raw-sizes*))))
            (cond ((null raw)
                   (failed "~A:~A: no AMIGA.RAW.* module has ~:[a struct *~A-SIZE*~;a struct field accessor ~A~] -- the constant pins nothing (misspelt, or the struct is not generated?)"
                           pkg-name (symbol-name s) (eq kind :offset) key))
                  ((/= (cdr raw) (symbol-value s))
                   (failed "~A:~A = ~D but ~A's ~:[*~A-SIZE*~;~A~] is at ~D"
                           pkg-name (symbol-name s) (symbol-value s)
                           (car raw) (eq kind :offset) key (cdr raw))))))))
    (when (> n 0)
      (pass "~A: ~D struct layout constant~:P agree with the raw struct rows" pkg-name n))))

;;; ------------------------------------------------------------------

(dolist (c *curated*)
  (destructuring-bind (require-name pkg-name raw-pkgs) c
    (check-constants pkg-name)
    (when raw-pkgs
      (check-lvo-constants pkg-name raw-pkgs))
    (check-defcfuns require-name pkg-name)
    (check-layout-constants pkg-name)))

(format t "~&CURATED-VS-RAW-RESULT ok=~D fail=~D uncovered=~D~%" *ok* *fail* *uncovered*)
