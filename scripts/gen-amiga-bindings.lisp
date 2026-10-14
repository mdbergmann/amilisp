;;; gen-amiga-bindings.lisp — generate lib/amiga/raw/*.lisp from SDK files
;;;
;;; Run on the HOST build of amilisp (not on the Amiga):
;;;
;;;   ./build/host/amilisp --non-interactive --load scripts/gen-amiga-bindings.lisp
;;;
;;; or, with the paths resolved for you, `make gen-amiga-bindings`
;;; (scripts/gen-amiga-bindings.sh).  Configuration is taken from the
;;; environment:
;;;
;;;   BINDGEN_NDK_SFD      dir with the OS 3.2 NDK *_lib.sfd files (primary)
;;;   BINDGEN_NDK_INCLUDE  dir with the NDK assembler includes (exec/*.i ...)
;;;   BINDGEN_NDK_INCLUDE_H dir with the NDK C headers (gadgets/*.h ...);
;;;                        default: the same directory as the .i files
;;;   BINDGEN_MOS_SFD      dir with MorphOS SDK *_lib.sfd files (optional;
;;;                        produced from the SDK's fd/ + clib/ by fd2sfd —
;;;                        the .sh wrapper does that)
;;;   BINDGEN_MOS_ONLY     comma list of MorphOS-only libraries to emit
;;;                        (default: muimaster,ahi,cybergraphics)
;;;   BINDGEN_MUI_SFD      dir with the MUI 3.8 developer kit's *_lib.sfd
;;;                        (optional; fd2sfd of FD/ + C/Include/clib/, the
;;;                        .sh wrapper does that).  Joins the PRIMARY
;;;                        (AmigaOS) tables: muimaster becomes an AmigaOS
;;;                        library with a MorphOS twin, like every NDK one.
;;;   BINDGEN_MUI_INCLUDE_H  the kit's C/Include — a second C-header root
;;;                        (libraries/mui.h), searched after the NDK's
;;;   BINDGEN_MCC_INCLUDE_H  a third C-header root holding mui/<Name>_mcc.h,
;;;                        the headers of MUI custom classes (the .sh
;;;                        wrapper collects them from the kit's ExtClasses/
;;;                        and MCC_HEADERS=<dir>) -> amiga/raw/mui/<name>
;;;   BINDGEN_MUI5_SFD     dir with the MUI 5 (AmigaOS) SDK's
;;;                        muimaster_lib.sfd (the SDK ships sfd/; the .sh
;;;                        wrapper copies it out alone).  ADDITIVE: the
;;;                        public functions beyond the 3.8 fd join
;;;                        muimaster under (%version>= 20) -- MUI 5 for
;;;                        AmigaOS and MorphOS's built-in MUI both report
;;;                        lib_Version 20.
;;;   BINDGEN_MUI5_INCLUDE_H  the MUI 5 SDK's C/include -- its
;;;                        libraries/mui.h is an additive constant source
;;;   BINDGEN_MOS_MUI_INCLUDE_H  a root holding the MorphOS SDK's
;;;                        libraries/mui.h (a copy of gg:os-include, i.e.
;;;                        MOS_SDK itself) -- an additive constant source
;;;                        too.  Additive sources need the MUI 3.8 kit
;;;                        (the baseline): their NEW names join the
;;;                        muimaster table unguarded, a name the baseline
;;;                        also defines must have the baseline's value --
;;;                        any other difference is fatal, except the known
;;;                        post-3.8 count/version evolutions of
;;;                        *MUI-VALUE-EVOLUTIONS* (the 3.8 value wins).
;;;                        Structs and functions are NOT taken from the
;;;                        additive headers (layouts stay 3.8's).
;;;   BINDGEN_AHI_SFD      dir with the AHI developer kit's ahi_lib.sfd
;;;                        (the kit ships sfd; the .sh wrapper copies it
;;;                        out alone, leaving ahi_sub -- the driver-side
;;;                        API -- behind).  Joins the PRIMARY tables:
;;;                        ahi.device becomes an AmigaOS module with a
;;;                        MorphOS twin.  Its V4 functions carry no version
;;;                        guard: V4 is the AHI baseline (see
;;;                        MERGE-LIBRARY-FUNCTIONS).
;;;   BINDGEN_AHI_INCLUDE_H  the kit's Include/C -- another C-header root
;;;                        (devices/ahi.h: the tags, sample types, error
;;;                        codes and the AHIRequest / AHISampleInfo ...
;;;                        structs), claimed by the ahi module
;;;   BINDGEN_AHI_LVO      the kit's Include/Asm/lvo -- its ahi_lib.i is
;;;                        the LVO cross-check for the AHI table, fatal on
;;;                        a mismatch like the NDK's lvo/*.i
;;;   BINDGEN_OUT          output directory (default lib/amiga/raw/)
;;;   BINDGEN_LIBS         comma list restricting the libraries generated
;;;                        (default: all)
;;;   BINDGEN_DOCSTRINGS   "0" to omit the C-prototype comments/docstrings
;;;
;;; What it produces — one module per library / NDK include subsystem,
;;; `(require "amiga/raw/<name>")`, package AMIGA.RAW.<NAME>.  Class
;;; libraries live under their kind directory, the way the OS stores them:
;;; gadgets/button.gadget -> amiga/raw/gadgets/button, images/bevel.image ->
;;; amiga/raw/images/bevel, window.class -> amiga/raw/classes/window.
;;;
;;; A module is ONE AMIGA.FFI:DEFINE-BINDING-TABLE form (plus the library
;;; base / version DEFVARs in front of it and the rare >7-register DEFCFUNs
;;; after it): every binding is a row of the table, packed at compile time
;;; and materialised on first reference — specs/raw-bindings-footprint.md.
;;;
;;;   * (:fn ...) rows — the DEFCFUN equivalent — for every public library
;;;     function: LVO from ==bias, registers and arity from the SFD,
;;;     :result kind from the C return type (pointer / signed / bool /
;;;     void ...), the C prototype as a trailing comment.
;;;   * (:const ...) rows for every EQU / ENUM / BITDEF in the matching .i
;;;     files (+NAME+ spelling), (:struct ...) rows (the DEFCSTRUCT
;;;     equivalent: *NAME-SIZE* + accessors) for every STRUCTURE.
;;;   * (:const ...) rows for the object-like #defines and enumerators of
;;;     the C headers that have NO assembler twin (the ReAction tags in
;;;     gadgets/*.h, images/*.h, classes/*.h, reaction/*.h, the MUI tags
;;;     and MUIC_* class-name strings of libraries/mui.h ...), and
;;;     (:struct ...) rows for their struct definitions (MUI_MinMax, the
;;;     MUIP_* messages, ColumnInfo ...), laid out by the m68k rules — see
;;;     "C header parsing" below.  The MUI custom-class headers
;;;     (mui/<Name>_mcc.h) are modules of their own, amiga/raw/mui/<name>.
;;;   * Platform tagging: functions the MorphOS SDK does not have at the
;;;     same LVO carry :not-morphos, MorphOS-only ones :morphos; functions
;;;     newer than OS 3.0 carry the minimum lib_Version.  All three are
;;;     checked when the name is looked up, against *FEATURES* and the
;;;     running library's version; a guarded-out name exists but is
;;;     unbound.
;;;
;;; The SFD grammar: see the NDK's sfd/README or sfdc.  The .i grammar is
;;; the exec/types.i macro set (STRUCTURE/APTR/WORD/.../LABEL/STRUCT,
;;; BITDEF, ENUM/EITEM) plus plain EQU with assembler expressions.

;; The generated modules :use AMIGA.FFI — load it here so name clashes with
;; its exports can be detected (see INHERITED-NAME-P).
(require "amiga/ffi")

(defpackage "CLAMIGA-BINDGEN"
  (:use "CL")
  (:export "RUN" "LISPIFY" "CONSTANT-NAME" "PARSE-SFD-FILE" "PARSE-I-FILES"
           "EVAL-ASM-EXPR"))

(in-package "CLAMIGA-BINDGEN")

;;; ================================================================
;;; Small string utilities
;;; ================================================================

(defun blank-string-p (s)
  (every (lambda (c) (member c '(#\Space #\Tab #\Return #\Newline))) s))

(defun trim (s)
  (string-trim '(#\Space #\Tab #\Return #\Newline) s))

(defun starts-with (prefix s)
  (and (>= (length s) (length prefix))
       (string= prefix s :end2 (length prefix))))

(defun ends-with (suffix s)
  (and (>= (length s) (length suffix))
       (string= suffix s :start2 (- (length s) (length suffix)))))

(defun split-string (s sep-chars)
  "Split S at any char in SEP-CHARS, dropping empty pieces."
  (let ((out nil) (start 0))
    (dotimes (i (length s))
      (when (member (char s i) sep-chars)
        (when (> i start) (push (subseq s start i) out))
        (setf start (1+ i))))
    (when (> (length s) start) (push (subseq s start) out))
    (nreverse out)))

(defun split-top-level-commas (s)
  "Split S at commas that are not inside parentheses."
  (let ((out nil) (start 0) (depth 0))
    (dotimes (i (length s))
      (let ((c (char s i)))
        (cond ((char= c #\() (incf depth))
              ((char= c #\)) (decf depth))
              ((and (char= c #\,) (zerop depth))
               (push (subseq s start i) out)
               (setf start (1+ i))))))
    (push (subseq s start) out)
    (mapcar #'trim (nreverse out))))

(defun collapse-whitespace (s)
  (let ((out (make-string-output-stream)) (prev-space nil))
    (loop for c across s
          do (if (member c '(#\Space #\Tab #\Newline #\Return))
                 (unless prev-space (write-char #\Space out) (setf prev-space t))
                 (progn (write-char c out) (setf prev-space nil))))
    (trim (get-output-stream-string out))))

(defun ident-char-p (c)
  (or (alphanumericp c) (char= c #\_)))

(defun comma-list (s)
  (and s (not (blank-string-p s)) (split-string s '(#\, #\Space))))

(defun getenv-or (name default)
  (let ((v (ext:getenv name)))
    (if (and v (not (blank-string-p v))) v default)))

(defun dir-path (s)
  "Ensure S ends with a slash."
  (if (ends-with "/" s) s (concatenate 'string s "/")))

;;; ================================================================
;;; Name mapping: C identifiers -> Lisp names
;;; ================================================================

;;; Whole-identifier spelling overrides, applied before the CamelCase
;;; split so that "RastPort", "RPort" and "BitMap" come out as one word
;;; the way the Amiga world writes them (rastport, window-rport, bitmap,
;;; init-bitmap, blt-bitmap ...), and mui.h's "RGBcolor" as rgb-color
;;; rather than rg-bcolor.  Everything else is mechanical.
(defparameter *spelling-overrides*
  '(("RastPort" . "Rastport")
    ("RPort" . "Rport")
    ("BitMap" . "Bitmap")
    ("RGBcolor" . "RgbColor")))

(defun apply-spelling-overrides (s)
  (dolist (pair *spelling-overrides* s)
    (let ((from (car pair)) (to (cdr pair)))
      (loop for pos = (search from s)
            while pos
            do (setf s (concatenate 'string (subseq s 0 pos) to
                                    (subseq s (+ pos (length from)))))))))

(defun split-camel (s)
  "Split a CamelCase identifier into words, the usual acronym-aware way:
a word boundary sits between a lowercase letter/digit and an uppercase
letter, and before an uppercase letter that follows another uppercase
letter and is itself followed by a lowercase letter (the last letter of
an acronym run starts the next word): OpenWindowTagList -> open window
tag list, ModifyIDCMP -> modify idcmp, IORequest -> io request,
SetAPen -> set a pen, AddGList -> add g list, SetRGB4 -> set rgb4."
  (let ((words nil) (cur (make-string-output-stream)) (n (length s)))
    (dotimes (i n)
      (let ((c (char s i))
            (prev (if (> i 0) (char s (1- i)) nil))
            (next (if (< (1+ i) n) (char s (1+ i)) nil)))
        (when (and prev (upper-case-p c)
                   (or (lower-case-p prev) (digit-char-p prev)
                       (and (upper-case-p prev) next (lower-case-p next))))
          (let ((w (get-output-stream-string cur)))
            (unless (string= w "") (push w words))))
        (write-char c cur)))
    (let ((w (get-output-stream-string cur)))
      (unless (string= w "") (push w words)))
    (nreverse words)))

(defun lispify (ident)
  "C identifier -> lowercase dashed Lisp name: OpenWindowTagList ->
open-window-tag-list, ModifyIDCMP -> modify-idcmp, WA_Left -> wa-left."
  (let* ((s (apply-spelling-overrides ident))
         (parts (split-string s '(#\_)))
         (words (mapcan #'split-camel parts)))
    (string-downcase (format nil "~{~A~^-~}" words))))

(defun constant-name (ident)
  (format nil "+~A+" (lispify ident)))

(defun var-name (ident)
  (format nil "*~A*" (lispify ident)))

;;; Lisp lambda-list parameter names: avoid the standardized constants
;;; (CLHS 11.1.2.1.2 forbids binding them) and duplicates.
(defun param-name (c-name used)
  (let ((n (lispify c-name)))
    (when (member n '("t" "nil" "pi") :test #'string=)
      (setf n (concatenate 'string n "-arg")))
    (loop while (member n used :test #'string=)
          do (setf n (concatenate 'string n "2")))
    n))

;;; ================================================================
;;; SFD parsing
;;; ================================================================

(defstruct sfd-fn
  name        ; C name
  lvo         ; negative offset
  ret-type    ; string, e.g. "struct Window *"
  params      ; list of (type-string . name-string)
  regs        ; list of strings "A0" "D0" ...
  version     ; integer or nil
  private     ; t for ==private entries
  source)     ; :ndk / :mos ...

(defstruct sfd-lib
  name        ; "intuition"  (from xxx_lib.sfd)
  libname     ; "intuition.library"
  base        ; "_IntuitionBase"
  id          ; ==id line or nil
  functions   ; list of sfd-fn (public and private, all LVOs)
  source)

(defun parse-c-param (s)
  "\"CONST struct TagItem *tagList\" -> (\"CONST struct TagItem *\" . \"tagList\").
Handles function-pointer params (VOID (*func)()) and arrays (name[])."
  (let ((s (collapse-whitespace s)))
    (cond
      ((string= s "...") (cons "..." "..."))
      ((search "(*" s)
       ;; function pointer: name is the identifier after "(*"
       (let* ((p (+ 2 (search "(*" s)))
              (e (position-if-not #'ident-char-p s :start p)))
         (cons "APTR" (subseq s p e))))
      (t
       (let* ((s2 (string-right-trim "[]" s))
              (end (length s2))
              (start (let ((i end))
                       (loop while (and (> i 0) (ident-char-p (char s2 (1- i))))
                             do (decf i))
                       i)))
         (if (= start end)
             (cons s nil)
             (cons (trim (subseq s2 0 start)) (subseq s2 start end))))))))

(defun register-token-p (tok)
  "A 68k register (d0-d7/a0-a7, a pair like d0-d1), or the MorphOS
calling-convention markers base/sysv/r12base."
  (let ((u (string-upcase tok)))
    (or (and (>= (length u) 2)
             (member (char u 0) '(#\A #\D))
             (digit-char-p (char u 1))
             (or (= (length u) 2)
                 ;; d0-d1 register pair
                 (and (= (length u) 5) (char= (char u 2) #\-)
                      (member (char u 3) '(#\A #\D)) (digit-char-p (char u 4)))))
        (member u '("BASE" "SYSV" "R12BASE") :test #'string=))))

(defun parse-sfd-entry (text)
  "TEXT is one complete SFD function entry:
   RET NAME(params) (regs) — the space before (regs) is optional (the NDK
   writes RemoveAppIcon(struct AppIcon *appIcon)(A0)).
Returns (values name ret params regs), or NIL when TEXT is not (yet) a
complete entry: the last parenthesised group must hold only register
tokens, the parameter group must close right before it, and what precedes
the name must be a plain C type (no parentheses or commas — that is what
tells the empty parens of a function-pointer parameter, VOID (*func)(),
apart from an empty register group)."
  (let* ((text (collapse-whitespace text))
         (rp (and (> (length text) 0) (char= (char text (1- (length text))) #\))
                  (1- (length text))))
         (lp (and rp (position #\( text :from-end t :end rp))))
    (when (and lp rp (> rp lp))
      (let* ((regs-str (subseq text (1+ lp) rp))
             (regs (mapcar #'string-upcase (split-string regs-str '(#\, #\/ #\Space))))
             (head (trim (subseq text 0 lp)))
             (hp (and (> (length head) 0) (char= (char head (1- (length head))) #\))
                      (1- (length head))))
             ;; find the matching "(" of the param list
             (open (and hp
                        (let ((depth 0))
                          (loop for i from hp downto 0
                                do (cond ((char= (char head i) #\)) (incf depth))
                                         ((char= (char head i) #\()
                                          (decf depth)
                                          (when (zerop depth) (return i))))))))
             (params-str (and open (subseq head (1+ open) hp)))
             (pre (and open (trim (subseq head 0 open))))
             (name-start (and pre
                              (let ((i (length pre)))
                                (loop while (and (> i 0) (ident-char-p (char pre (1- i))))
                                      do (decf i))
                                i))))
        (when (and (every #'register-token-p regs)
                   pre name-start (< name-start (length pre)))
          (let ((name (subseq pre name-start))
                (ret (trim (subseq pre 0 name-start))))
            (when (and (not (find #\( ret)) (not (find #\) ret)) (not (find #\, ret)))
              (let ((params (if (blank-string-p params-str)
                                nil
                                (mapcar #'parse-c-param (split-top-level-commas params-str)))))
                (when (string= ret "") (setf ret "ULONG"))
                (values name ret params regs)))))))))

(defun sfd-entry-complete-p (buf)
  "The accumulated text is a complete entry when its parentheses balance
and PARSE-SFD-ENTRY accepts it."
  (let ((s (trim buf)))
    (and (> (length s) 0)
         (char= (char s (1- (length s))) #\))
         (let ((depth 0))
           (loop for c across s
                 do (cond ((char= c #\() (incf depth))
                          ((char= c #\)) (decf depth))))
           (zerop depth))
         (parse-sfd-entry s)
         t)))

(defun parse-sfd-file (path &key (source :ndk))
  (let ((lib (make-sfd-lib :name (let ((n (pathname-name path)))
                                   (if (ends-with "_lib" n)
                                       (subseq n 0 (- (length n) 4))
                                       n))
                           :source source))
        (bias 30) (version nil) (private nil)
        (varargs nil) (alias nil)
        (buf "") (fns nil))
    (with-open-file (in path :direction :input :external-format :latin-1)
      (loop for line = (read-line in nil nil)
            while line
            do (let ((l (string-right-trim '(#\Return #\Space #\Tab) line)))
                 (cond
                   ((starts-with "==" l)
                    (let* ((ws (split-string (subseq l 2) '(#\Space #\Tab)))
                           (d (and ws (string-downcase (first ws))))
                           (arg (and (cdr ws) (second ws))))
                      (cond
                        ((null d))
                        ((string= d "bias") (setf bias (parse-integer arg)))
                        ((string= d "reserve") (incf bias (* 6 (parse-integer arg))))
                        ((string= d "version") (setf version (parse-integer arg)))
                        ((string= d "public") (setf private nil))
                        ((string= d "private") (setf private t))
                        ((string= d "varargs") (setf varargs t))
                        ((string= d "alias") (setf alias t))
                        ((string= d "libname") (setf (sfd-lib-libname lib) arg))
                        ((string= d "base") (setf (sfd-lib-base lib) arg))
                        ((string= d "id") (setf (sfd-lib-id lib) (trim (subseq l 4))))
                        ((string= d "end") (return)))))
                   ((or (blank-string-p l) (char= (char l 0) #\*))
                    nil)
                   (t
                    (setf buf (concatenate 'string buf " " l))
                    (when (sfd-entry-complete-p buf)
                      (multiple-value-bind (name ret params regs) (parse-sfd-entry buf)
                        (setf buf "")
                        (when name
                          (cond
                            ((or varargs alias)
                             ;; shares the previous entry's LVO; not bound
                             (setf varargs nil alias nil))
                            (t
                             (push (make-sfd-fn :name name :lvo (- bias)
                                                :ret-type ret :params params
                                                :regs regs :version version
                                                :private private :source source)
                                   fns)
                             (incf bias 6)))))))))))
    (setf (sfd-lib-functions lib) (nreverse fns))
    lib))

;;; ================================================================
;;; Assembler include (.i) parsing
;;; ================================================================

;;; Global symbol table: name -> (:value n) | (:expr "string") | (:struct-field ...)
(defvar *asm-symbols* (make-hash-table :test 'equal))
(defvar *asm-eval-stack* nil)
(defvar *asm-warnings* nil)

(defun asm-warn (fmt &rest args)
  (push (apply #'format nil fmt args) *asm-warnings*))

;;; --- expression tokenizer / evaluator ---

(defun tokenize-asm-expr (s)
  (let ((toks nil) (i 0) (n (length s)))
    (loop while (< i n)
          do (let ((c (char s i)))
               (cond
                 ((member c '(#\Space #\Tab)) (incf i))
                 ((char= c #\$)
                  (let ((j (1+ i)))
                    (loop while (and (< j n) (digit-char-p (char s j) 16)) do (incf j))
                    (push (list :num (parse-integer s :start (1+ i) :end j :radix 16)) toks)
                    (setf i j)))
                 ((char= c #\%)
                  (let ((j (1+ i)))
                    (loop while (and (< j n) (member (char s j) '(#\0 #\1))) do (incf j))
                    (push (list :num (parse-integer s :start (1+ i) :end j :radix 2)) toks)
                    (setf i j)))
                 ((char= c #\@)
                  (let ((j (1+ i)))
                    (loop while (and (< j n) (digit-char-p (char s j) 8)) do (incf j))
                    (push (list :num (parse-integer s :start (1+ i) :end j :radix 8)) toks)
                    (setf i j)))
                 ((and (char= c #\0) (< (1+ i) n) (char-equal (char s (1+ i)) #\x))
                  ;; C-style 0x hex (a few includes use it)
                  (let ((j (+ i 2)))
                    (loop while (and (< j n) (digit-char-p (char s j) 16)) do (incf j))
                    (push (list :num (parse-integer s :start (+ i 2) :end j :radix 16)) toks)
                    (setf i j)))
                 ((digit-char-p c)
                  (let ((j i))
                    (loop while (and (< j n) (digit-char-p (char s j))) do (incf j))
                    (push (list :num (parse-integer s :start i :end j)) toks)
                    (setf i j)))
                 ((or (char= c #\') (char= c #\"))
                  (let ((j (position c s :start (1+ i))))
                    (unless j (error "unterminated char literal in ~S" s))
                    ;; multi-char literal packs big-endian
                    (let ((v 0))
                      (loop for k from (1+ i) below j
                            do (setf v (+ (* v 256) (char-code (char s k)))))
                      (push (list :num v) toks))
                    (setf i (1+ j))))
                 ((or (alpha-char-p c) (char= c #\_) (char= c #\.))
                  (let ((j i))
                    (loop while (and (< j n) (or (ident-char-p (char s j)) (char= (char s j) #\.)))
                          do (incf j))
                    (push (list :sym (subseq s i j)) toks)
                    (setf i j)))
                 ((and (char= c #\<) (< (1+ i) n) (char= (char s (1+ i)) #\<))
                  (push (list :op "<<") toks) (incf i 2))
                 ((and (char= c #\>) (< (1+ i) n) (char= (char s (1+ i)) #\>))
                  (push (list :op ">>") toks) (incf i 2))
                 ((member c '(#\+ #\- #\* #\/ #\& #\! #\| #\^ #\~ #\( #\)))
                  (push (list :op (string c)) toks) (incf i))
                 (t (error "bad char ~S in asm expression ~S" c s)))))
    (nreverse toks)))

;;; Precedence climbing.  Binary precedence (high -> low):
;;;   * /      |  + -      |  << >>     |  &      |  ^      |  ! |
(defun asm-binop-prec (op)
  (cond ((member op '("*" "/") :test #'string=) 6)
        ((member op '("+" "-") :test #'string=) 5)
        ((member op '("<<" ">>") :test #'string=) 4)
        ((string= op "&") 3)
        ((string= op "^") 2)
        ((member op '("!" "|") :test #'string=) 1)
        (t nil)))

(defun asm-apply (op a b)
  (cond ((string= op "*") (* a b))
        ((string= op "/") (if (zerop b) (error "division by zero") (truncate a b)))
        ((string= op "+") (+ a b))
        ((string= op "-") (- a b))
        ((string= op "<<") (ash a b))
        ((string= op ">>") (ash a (- b)))
        ((string= op "&") (logand a b))
        ((string= op "^") (logxor a b))
        (t (logior a b))))

(defun eval-asm-tokens (toks)
  "Returns (values value remaining-tokens)."
  (labels ((peek () (car toks))
           (next () (pop toks))
           (primary ()
             (let ((tk (next)))
               (unless tk (error "unexpected end of expression"))
               (cond
                 ((eq (first tk) :num) (second tk))
                 ((eq (first tk) :sym) (resolve-asm-symbol (second tk)))
                 ((and (eq (first tk) :op) (string= (second tk) "("))
                  (let ((v (expr 0)))
                    (let ((close (next)))
                      (unless (and close (eq (first close) :op) (string= (second close) ")"))
                        (error "expected )")))
                    v))
                 ((and (eq (first tk) :op) (string= (second tk) "-")) (- (primary)))
                 ((and (eq (first tk) :op) (string= (second tk) "+")) (primary))
                 ((and (eq (first tk) :op) (string= (second tk) "~")) (lognot (primary)))
                 ((and (eq (first tk) :op) (string= (second tk) "!")) (lognot (primary)))
                 (t (error "unexpected token ~S" tk)))))
           (expr (min-prec)
             (let ((lhs (primary)))
               (loop
                 (let ((tk (peek)))
                   (unless (and tk (eq (first tk) :op) (asm-binop-prec (second tk))
                                (>= (asm-binop-prec (second tk)) min-prec))
                     (return lhs))
                   (next)
                   (let ((rhs (expr (1+ (asm-binop-prec (second tk))))))
                     (setf lhs (asm-apply (second tk) lhs rhs))))))))
    (let ((v (expr 0)))
      (values v toks))))

(defun eval-asm-expr (s)
  (multiple-value-bind (v rest) (eval-asm-tokens (tokenize-asm-expr s))
    (when rest (error "trailing tokens ~S in ~S" rest s))
    v))

(defun resolve-asm-symbol (name)
  (let ((entry (gethash name *asm-symbols*)))
    (cond
      ((null entry) (error "undefined symbol ~A" name))
      ((eq (first entry) :value) (second entry))
      ((eq (first entry) :expr)
       (when (member name *asm-eval-stack* :test #'string=)
         (error "circular definition of ~A" name))
       (let* ((*asm-eval-stack* (cons name *asm-eval-stack*))
              (v (eval-asm-expr (second entry))))
         (setf (gethash name *asm-symbols*) (list :value v))
         v))
      ;; a C #define / enumerator whose body could not be evaluated when it
      ;; was read (forward reference): try again now, with the C evaluator
      ((eq (first entry) :cexpr)
       (when (member name *asm-eval-stack* :test #'string=)
         (error "circular definition of ~A" name))
       (let ((*asm-eval-stack* (cons name *asm-eval-stack*)))
         (multiple-value-bind (v status) (c-eval-expr (second entry))
           (unless (eq status :ok)
             (error "C macro ~A is not an integer constant: ~A" name (second entry)))
           (setf (gethash name *asm-symbols*) (list :value v))
           v)))
      (t (error "symbol ~A is not a value" name)))))

(defun asm-symbol-value (name)
  "Value of NAME or NIL (with a warning) if it cannot be resolved."
  (handler-case (resolve-asm-symbol name)
    (error (e)
      (asm-warn "cannot resolve ~A: ~A" name e)
      nil)))

;;; --- .i line reader ---

(defun strip-asm-comment (line)
  "Remove ; comments and * comment lines.  Keeps char literals intact."
  (let ((s (string-right-trim '(#\Return) line)))
    (cond
      ((and (> (length s) 0) (char= (char s 0) #\*)) "")
      (t
       (let ((in-quote nil) (cut nil))
         (dotimes (i (length s))
           (let ((c (char s i)))
             (cond ((and (not in-quote) (or (char= c #\') (char= c #\"))) (setf in-quote c))
                   ((and in-quote (char= c in-quote)) (setf in-quote nil))
                   ((and (not in-quote) (char= c #\;)) (setf cut i) (return)))))
         (if cut (subseq s 0 cut) s))))))

(defparameter *asm-operator-chars* '(#\+ #\- #\/ #\! #\& #\| #\< #\> #\^ #\~ #\())

(defun operand-field (s start)
  "The operand field starts at START and ends at the first whitespace
outside quotes and parentheses that does not continue an expression —
anything after it is a comment (Motorola syntax: the `* new in V47'
trailing remarks).  Whitespace continues the field when an operator
precedes or follows it, so \"( 'D'<<8 ) ! ( 'O' )\" is one operand but
\"6 * new in V47\" stops at the 6.  <...> macro-argument brackets are
turned into parentheses."
  (let ((i start) (n (length s)) (quote-char nil) (depth 0)
        (last-sig nil)                ; last significant (non-space) char
        (out (make-string-output-stream)))
    (loop while (< i n)
          do (let* ((c (char s i))
                    (prev (if (> i start) (char s (1- i)) nil))
                    (next (if (< (1+ i) n) (char s (1+ i)) nil)))
               (cond
                 (quote-char
                  (write-char c out)
                  (when (char= c quote-char) (setf quote-char nil)))
                 ((or (char= c #\') (char= c #\"))
                  (setf quote-char c) (write-char c out) (setf last-sig c))
                 ((member c '(#\Space #\Tab))
                  (if (> depth 0)
                      nil
                      ;; continue only across an operator boundary
                      (let ((j i))
                        (loop while (and (< j n) (member (char s j) '(#\Space #\Tab))) do (incf j))
                        (let ((nxt (if (< j n) (char s j) nil)))
                          (unless (or (and last-sig (member last-sig *asm-operator-chars*))
                                      (and nxt (member nxt *asm-operator-chars*)))
                            (return))))))
                 ;; <...> macro-argument brackets (only at the field start /
                 ;; after a comma, and never the << shift operator)
                 ((and (char= c #\<) (or (null prev) (char= prev #\,))
                       (not (and next (char= next #\<))))
                  (write-char #\( out) (incf depth) (setf last-sig #\())
                 ((and (char= c #\>) (or (null next) (member next '(#\, #\Space #\Tab)))
                       (not (and prev (char= prev #\>))))
                  (write-char #\) out) (decf depth) (setf last-sig #\)))
                 ((char= c #\() (write-char c out) (incf depth) (setf last-sig c))
                 ((char= c #\)) (write-char c out) (decf depth) (setf last-sig c))
                 (t (write-char c out) (setf last-sig c))))
             (incf i))
    (let ((r (get-output-stream-string out)))
      (if (string= r "") nil r))))

(defun tokenize-asm-line (line)
  "Returns (values label mnemonic operand-string).  A label is a token
in column 1; the mnemonic follows; the operand field is the next
whitespace-delimited token (see OPERAND-FIELD)."
  (let* ((s (strip-asm-comment line)))
    (if (blank-string-p s)
        (values nil nil nil)
        (let* ((has-label (not (member (char s 0) '(#\Space #\Tab))))
               (i 0) (n (length s))
               (label nil) (mnem nil) (ops nil))
          (flet ((skip-ws ()
                   (loop while (and (< i n) (member (char s i) '(#\Space #\Tab))) do (incf i)))
                 (token ()
                   (let ((st i))
                     (loop while (and (< i n) (not (member (char s i) '(#\Space #\Tab)))) do (incf i))
                     (subseq s st i))))
            (when has-label
              (setf label (string-right-trim ":" (token))))
            (skip-ws)
            (when (< i n) (setf mnem (token)))
            (skip-ws)
            (when (< i n) (setf ops (operand-field s i))))
          (values label (and mnem (string-upcase mnem)) ops)))))

;;; Field macros: mnemonic -> (ffi-type . size)
(defparameter *field-macros*
  '(("BYTE" . (:i8 . 1)) ("UBYTE" . (:u8 . 1))
    ("WORD" . (:i16 . 2)) ("UWORD" . (:u16 . 2))
    ("SHORT" . (:i16 . 2)) ("USHORT" . (:u16 . 2))
    ("BOOL" . (:i16 . 2))
    ("LONG" . (:i32 . 4)) ("ULONG" . (:u32 . 4))
    ("FLOAT" . (:single . 4)) ("DOUBLE" . (:double . 8))
    ("APTR" . (:fptr . 4)) ("CPTR" . (:fptr . 4)) ("FPTR" . (:fptr . 4))
    ("BPTR" . (:u32 . 4)) ("BSTR" . (:u32 . 4))
    ("RPTR" . (:u16 . 2))))

(defstruct i-struct
  name          ; assembler STRUCTURE name, or the C struct's tag
  fields        ; list of (c-name type offset), most recent first; type = keyword, (:struct n) or (:array kw n)
  size-labels   ; list of (label-name . offset) for *_SIZE / *_SIZEOF labels
  size          ; a C struct's sizeof (NIL for a STRUCTURE: its size labels decide)
  file)

(defstruct i-const
  name value file  ; value: integer, string (a C header's "..." #define), or NIL = resolve later
  quiet)         ; t for C-header constants: unresolvable -> dropped silently

(defstruct i-file
  path           ; relative include path, e.g. "intuition/intuition.i"
  structs        ; list of i-struct
  constants      ; list of i-const (in definition order)
  (skipped 0)    ; .h only: object-like macros that are not integer constants
  skipped-structs) ; .h only: (name . reason) for struct definitions not laid out

(defvar *i-files* (make-hash-table :test 'equal)) ; rel path -> i-file
(defvar *i-include-root* nil)

(defun size-label-p (name)
  (let ((u (string-upcase name)))
    (or (ends-with "SIZE" u) (ends-with "SIZEOF" u))))

(defun define-asm-symbol (name entry)
  ;; first definition wins (include guards / conditional blocks re-define)
  (unless (gethash name *asm-symbols*)
    (setf (gethash name *asm-symbols*) entry)))

(defun parse-i-file (relpath)
  "Parse one .i file (and, recursively, its INCLUDEs) into *i-files*."
  (when (gethash relpath *i-files*)
    (return-from parse-i-file (gethash relpath *i-files*)))
  (let ((full (concatenate 'string *i-include-root* relpath)))
    (unless (probe-file full)
      (asm-warn "include not found: ~A" relpath)
      (return-from parse-i-file nil))
    (let ((ifile (make-i-file :path relpath))
          (structs nil) (consts nil)
          (cur nil)               ; current i-struct being filled
          (soffset 0)
          (eoffset 0)
          (devcmd-count 0)
          (libdef-count -30)
          (in-macro nil))
      ;; register first so recursive includes terminate
      (setf (gethash relpath *i-files*) ifile)
      (with-open-file (in full :direction :input :external-format :latin-1)
        (loop for line = (read-line in nil nil)
              while line
              do (multiple-value-bind (label mnem ops) (tokenize-asm-line line)
                   (cond
                     ((null mnem)
                      ;; a bare label line — ignore
                      nil)
                     (in-macro
                      (when (string= mnem "ENDM") (setf in-macro nil)))
                     ((string= mnem "MACRO") (setf in-macro t))
                     ((string= mnem "INCLUDE")
                      (let ((inc (string-trim "\"'<>" (or ops ""))))
                        (parse-i-file inc)))
                     ((member mnem '("IFND" "IFD" "IFC" "IFNC" "IFEQ" "IFNE" "IFGE" "IFGT"
                                     "IFLE" "IFLT" "ENDC" "ELSE" "ELSEIF" "ENDIF" "SECTION"
                                     "XDEF" "XREF" "CNOP" "DC.B" "DC.W" "DC.L" "DS.B" "DS.W"
                                     "DS.L" "EVEN" "OPT" "NOLIST" "LIST" "PAGE" "TTL"
                                     "RSRESET" "RSSET" "EXTERN_LIB" "END" "IDNT" "MC68020"
                                     "MACHINE" "FAR" "NEAR" "CODE" "DATA" "BSS" "CSECT"
                                     "MEXIT" "FAIL" "PRINTT" "PRINTV" "INCDIR" "OUTPUT")
                              :test #'string=)
                      nil)
                     ((string= mnem "STRUCTURE")
                      (let* ((args (split-string (or ops "") '(#\,)))
                             (name (trim (first args)))
                             (base-expr (if (cdr args) (trim (second args)) "0")))
                        (when cur (push cur structs))
                        (define-asm-symbol name (list :value 0))
                        (setf soffset (or (handler-case (eval-asm-expr base-expr)
                                            (error (e)
                                              (asm-warn "STRUCTURE ~A base ~A: ~A" name base-expr e)
                                              nil))
                                          0))
                        (setf cur (make-i-struct :name name :fields nil :size-labels nil
                                                 :file relpath))))
                     ((assoc mnem *field-macros* :test #'string=)
                      (let* ((spec (cdr (assoc mnem *field-macros* :test #'string=)))
                             (fname (trim (first (split-string (or ops "") '(#\,))))))
                        (define-asm-symbol fname (list :value soffset))
                        (when cur
                          (push (list fname (car spec) soffset) (i-struct-fields cur)))
                        (incf soffset (cdr spec))))
                     ((string= mnem "STRUCT")
                      (let* ((args (split-string (or ops "") '(#\,)))
                             (fname (trim (first args)))
                             (size (if (cdr args)
                                       (handler-case (eval-asm-expr (trim (second args)))
                                         (error (e)
                                           (asm-warn "STRUCT ~A size: ~A" fname e) 0))
                                       0)))
                        (define-asm-symbol fname (list :value soffset))
                        (when cur
                          (push (list fname (list :struct size) soffset) (i-struct-fields cur)))
                        (incf soffset size)))
                     ((string= mnem "LABEL")
                      (let ((fname (trim (first (split-string (or ops "") '(#\,))))))
                        (define-asm-symbol fname (list :value soffset))
                        (when cur
                          (if (size-label-p fname)
                              (push (cons fname soffset) (i-struct-size-labels cur))
                              (push (list fname (list :struct 0) soffset) (i-struct-fields cur))))))
                     ((string= mnem "ALIGNWORD")
                      (setf soffset (logand (+ soffset 1) (lognot 1))))
                     ((string= mnem "ALIGNLONG")
                      (setf soffset (logand (+ soffset 3) (lognot 3))))
                     ((string= mnem "ENUM")
                      (setf eoffset (if (and ops (not (blank-string-p ops)))
                                        (or (handler-case (eval-asm-expr ops)
                                              (error (e) (asm-warn "ENUM ~A: ~A" ops e) nil))
                                            0)
                                        0)))
                     ((string= mnem "EITEM")
                      ;; (some includes leave a stray trailing comma)
                      (let ((name (trim (first (split-string (or ops "") '(#\,))))))
                        (define-asm-symbol name (list :value eoffset))
                        (push (make-i-const :name name :value eoffset :file relpath) consts)
                        (incf eoffset)))
                     ;; exec/io.i's device-command macros: DEVINIT [base]
                     ;; resets the counter (to CMD_NONSTD when no base is
                     ;; given), DEVCMD name assigns the next value.
                     ((string= mnem "DEVINIT")
                      (setf devcmd-count
                            (if (and ops (not (blank-string-p ops)))
                                (or (handler-case (eval-asm-expr ops)
                                      (error (e) (asm-warn "DEVINIT ~A: ~A" ops e) nil))
                                    0)
                                (or (asm-symbol-value "CMD_NONSTD") 0))))
                     ((string= mnem "DEVCMD")
                      (let ((name (trim (or ops ""))))
                        (define-asm-symbol name (list :value devcmd-count))
                        (push (make-i-const :name name :value devcmd-count :file relpath) consts)
                        (incf devcmd-count)))
                     ;; exec/libraries.i's LVO macros: LIBINIT [base] resets
                     ;; the counter (LIB_BASE = -30), LIBDEF name assigns the
                     ;; next vector (step LIB_VECTSIZE = -6).
                     ((string= mnem "LIBINIT")
                      (setf libdef-count
                            (if (and ops (not (blank-string-p ops)))
                                (or (handler-case (eval-asm-expr ops)
                                      (error (e) (asm-warn "LIBINIT ~A: ~A" ops e) nil))
                                    -30)
                                (or (asm-symbol-value "LIB_BASE") -30))))
                     ((string= mnem "LIBDEF")
                      (let ((name (trim (or ops ""))))
                        (define-asm-symbol name (list :value libdef-count))
                        (push (make-i-const :name name :value libdef-count :file relpath) consts)
                        (decf libdef-count (or (asm-symbol-value "LIB_VECTSIZE") 6))))
                     ((string= mnem "BITDEF")
                      (let* ((args (mapcar #'trim (split-string (or ops "") '(#\,))))
                             (prefix (first args)) (nm (second args)) (bit-expr (third args))
                             (bit (handler-case (eval-asm-expr bit-expr)
                                    (error (e) (asm-warn "BITDEF ~A: ~A" ops e) nil))))
                        (when bit
                          (let ((bname (format nil "~AB_~A" prefix nm))
                                (fname (format nil "~AF_~A" prefix nm)))
                            (define-asm-symbol bname (list :value bit))
                            (define-asm-symbol fname (list :value (ash 1 bit)))
                            (push (make-i-const :name bname :value bit :file relpath) consts)
                            (push (make-i-const :name fname :value (ash 1 bit) :file relpath) consts)))))
                     ((string= mnem "BITDEF0")
                      (let* ((args (mapcar #'trim (split-string (or ops "") '(#\,))))
                             (name (format nil "~A~A~A" (first args) (third args) (second args)))
                             (val (handler-case (eval-asm-expr (fourth args))
                                    (error (e) (asm-warn "BITDEF0 ~A: ~A" ops e) nil))))
                        (when val
                          (define-asm-symbol name (list :value val))
                          (push (make-i-const :name name :value val :file relpath) consts))))
                     ((and label (string= mnem "EQU"))
                      (unless (gethash label *asm-symbols*)
                        (define-asm-symbol label (list :expr (or ops "0")))
                        (push (make-i-const :name label :value nil :file relpath) consts)))
                     ((and label (string= mnem "SET"))
                      ;; assembler variables (include guards, counters) — track, don't emit
                      (unless (gethash label *asm-symbols*)
                        (setf (gethash label *asm-symbols*) (list :expr (or ops "0")))))
                     (t nil)))))
      (when cur (push cur structs))
      (setf (i-file-structs ifile) (nreverse structs))
      (setf (i-file-constants ifile) (nreverse consts))
      ifile)))

;;; Symbols that the .i files reference but only the .h files define.
(defparameter *extra-symbols*
  '(("DTM_Dummy" . #x600)))          ; datatypes/datatypesclass.h

(defun parse-i-files (root relpaths)
  (let ((*i-include-root* (dir-path root)))
    (dolist (pair *extra-symbols*)
      (define-asm-symbol (car pair) (list :value (cdr pair))))
    (dolist (p relpaths) (parse-i-file p))))

;;; ================================================================
;;; C header (.h) parsing — object-like #defines and enumerators
;;; ================================================================
;;;
;;; The NDK ships an assembler include next to nearly every C header.
;;; Where it does not — the ReAction classes (gadgets/*.h, images/*.h,
;;; classes/*.h, reaction/*.h), devices/trackfile.h, libraries/keymap.h
;;; — the tags, method IDs and flags exist only as C #defines and enums.
;;; A .h WITHOUT a .i twin is therefore read too, and takes part in the
;;; module layout exactly like a .i file.  What is taken from it:
;;;
;;;   * every object-like #define whose body is an integer constant
;;;     expression — C operators, casts to the integer types, number
;;;     suffixes (0x45L), char literals, references to other macros and
;;;     to assembler constants (TAG_USER, GA_Image ...);
;;;   * every object-like #define whose body is ONE ASCII string literal
;;;     (mui.h's MUIC_Window "Window.mui", MUIMASTER_NAME, the MUIX_C
;;;     "\033c" text-style escapes) — a string constant; escapes are
;;;     decoded, control characters are written as a #. form so the
;;;     module stays printable; a literal with non-ASCII characters (the
;;;     FASLs must stay ASCII, see `make fasl`) or a concatenation of
;;;     literals is skipped;
;;;   * every enumerator of every top-level enum.
;;;
;;; seen through the preprocessor: comments, backslash continuations,
;;; #ifdef / #ifndef / #if 0 / #elif / #else / #endif (only macros defined
;;; by the headers themselves count as defined, so __cplusplus / __GNUC__
;;; blocks are skipped — and mui.h's own `#define MUI_OBSOLETE` makes its
;;; #ifdef MUI_OBSOLETE blocks live, as they are for a C program), #undef,
;;; and #include of another twin-less header (read first, so its macros
;;; are known).  Function-like macros, floats and anything that is not an
;;; integer expression or a string (the NewObject(...) convenience macros)
;;; are skipped — counted in the module header, not warned about.  C
;;; struct definitions are read too — see "C struct definitions" below.
;;;
;;; The C headers live under one or more roots searched in order — the
;;; NDK's Include_H first, then an SDK that ships headers of its own (the
;;; MUI developer kit's C/Include), then the directory of MUI custom-class
;;; headers (BINDGEN_MCC_INCLUDE_H); the first root that has a file wins.

(defvar *h-include-roots* nil)                      ; dirs of the .h files, in search order
(defvar *c-macros* (make-hash-table :test 'equal))  ; name -> t, for #ifdef

(defun h-file-path (rel)
  "Absolute path of the C header REL under the first root that has it;
under the first root when none does (so a probe fails and a warning names
a real location)."
  (or (loop for root in *h-include-roots*
            for p = (concatenate 'string root rel)
            when (probe-file p) return p)
      (concatenate 'string (or (first *h-include-roots*) "") rel)))

(defun c-string-literal (body)
  "If BODY is exactly one C string literal, its decoded contents (the
usual escapes: \\n \\t \\\\ \\\" octal, \\xHH ...), else NIL.  A second
value says whether every character is ASCII — what a string constant may
hold (the binding table and the FASLs are byte strings)."
  (let ((n (length body)))
    (when (and (>= n 2) (char= (char body 0) #\") (char= (char body (1- n)) #\"))
      (let ((out (make-string-output-stream)) (i 1) (ascii t) (closed nil))
        (loop while (< i n)
              do (let ((c (char body i)))
                   (cond
                     ((char= c #\")
                      (setf closed (= i (1- n)))
                      (return))
                     ((and (char= c #\\) (< (1+ i) n))
                      (let ((e (char body (1+ i))))
                        (incf i 2)
                        (cond
                          ((digit-char-p e 8)
                           ;; up to three octal digits
                           (let ((v (digit-char-p e 8)) (k 1))
                             (loop while (and (< k 3) (< i n) (digit-char-p (char body i) 8))
                                   do (setf v (+ (* v 8) (digit-char-p (char body i) 8)))
                                      (incf i) (incf k))
                             (write-char (code-char v) out)))
                          ((char-equal e #\x)
                           (let ((v 0) (any nil))
                             (loop while (and (< i n) (digit-char-p (char body i) 16))
                                   do (setf v (+ (* v 16) (digit-char-p (char body i) 16)) any t)
                                      (incf i))
                             (unless any (return-from c-string-literal nil))
                             (write-char (code-char (logand v 255)) out)))
                          (t (write-char (case e
                                           (#\n #\Newline) (#\t #\Tab) (#\r #\Return)
                                           (#\a (code-char 7)) (#\b (code-char 8))
                                           (#\f (code-char 12)) (#\v (code-char 11))
                                           (t e))
                                         out)))))
                     (t (write-char c out) (incf i)))))
        (when closed
          (let ((s (get-output-stream-string out)))
            (loop for ch across s
                  unless (< (char-code ch) 128) do (setf ascii nil))
            (values s ascii)))))))

(defun lisp-string-form (s)
  "S as Lisp source for a binding-table row: a string literal when every
character is printable, else a #. form that builds it from character
codes — mui.h's MUIX_C \"\\033c\" reads as (27 99) instead of a stray ESC
byte in the module."
  (if (every (lambda (c) (<= 32 (char-code c) 126)) s)
      (lisp-string s)
      (format nil "#.(map 'string #'code-char '(~{~D~^ ~}))" (map 'list #'char-code s))))

(define-condition c-unknown-symbol (error)
  ((name :initarg :name :reader c-unknown-symbol-name))
  (:report (lambda (c s) (format s "unknown symbol ~A" (c-unknown-symbol-name c)))))

;;; --- C constant-expression tokenizer / evaluator ---

(defun c-number-token (s i)
  "Parse the number starting at index I of S: decimal, 0x hex, 0 octal,
with any of the u/l suffixes.  Returns (values value next-index).  A
floating-point literal signals an error."
  (let* ((n (length s))
         (j i)
         (radix 10)
         (start i))
    (cond
      ((and (char= (char s i) #\0) (< (1+ i) n) (char-equal (char s (1+ i)) #\x))
       (setf radix 16 start (+ i 2) j (+ i 2))
       (loop while (and (< j n) (digit-char-p (char s j) 16)) do (incf j)))
      (t
       (loop while (and (< j n) (digit-char-p (char s j))) do (incf j))
       (when (and (< j n) (or (char= (char s j) #\.) (char-equal (char s j) #\e)))
         (error "floating-point literal in ~S" s))
       (when (and (> (- j i) 1) (char= (char s i) #\0))
         (setf radix 8 start (1+ i)))))
    (when (= j start) (error "bad number in ~S" s))
    (let ((v (parse-integer s :start start :end j :radix radix)))
      ;; suffixes: u U l L in any combination
      (loop while (and (< j n) (member (char s j) '(#\u #\U #\l #\L))) do (incf j))
      (values v j))))

(defun c-char-token (s i)
  "Parse the char literal starting at the quote at index I.  Multi-char
literals ('FORM') pack big-endian, like the assembler's.  Returns
(values value next-index)."
  (let ((n (length s)) (j (1+ i)) (v 0))
    (loop
      (when (>= j n) (error "unterminated char literal in ~S" s))
      (let ((c (char s j)))
        (cond
          ((char= c #\') (return (values v (1+ j))))
          ((char= c #\\)
           (when (>= (1+ j) n) (error "bad escape in ~S" s))
           (let ((e (char s (1+ j))))
             (setf v (+ (* v 256)
                        (case e
                          (#\n 10) (#\t 9) (#\r 13) (#\0 0) (#\a 7) (#\b 8)
                          (#\f 12) (#\v 11) (#\\ 92) (#\' 39) (#\" 34)
                          (t (char-code e)))))
             (incf j 2)))
          (t (setf v (+ (* v 256) (char-code c))) (incf j)))))))

(defun tokenize-c-expr (s)
  "Tokens: (:num v) (:sym name) (:op str).  Anything that cannot occur in
an integer constant expression (strings, floats, assignment, member
access, braces ...) signals an error."
  (let ((toks nil) (i 0) (n (length s)))
    (loop while (< i n)
          do (let ((c (char s i)))
               (cond
                 ((member c '(#\Space #\Tab #\Newline #\Return)) (incf i))
                 ((digit-char-p c)
                  (multiple-value-bind (v j) (c-number-token s i)
                    (push (list :num v) toks) (setf i j)))
                 ((char= c #\')
                  (multiple-value-bind (v j) (c-char-token s i)
                    (push (list :num v) toks) (setf i j)))
                 ((or (alpha-char-p c) (char= c #\_))
                  (let ((j i))
                    (loop while (and (< j n) (ident-char-p (char s j))) do (incf j))
                    (push (list :sym (subseq s i j)) toks)
                    (setf i j)))
                 ((and (< (1+ i) n)
                       (member (subseq s i (+ i 2)) '("<<" ">>" "<=" ">=" "==" "!=" "&&" "||")
                               :test #'string=))
                  (push (list :op (subseq s i (+ i 2))) toks) (incf i 2))
                 ((member c '(#\+ #\- #\* #\/ #\% #\& #\| #\^ #\~ #\! #\( #\) #\< #\> #\? #\:))
                  (push (list :op (string c)) toks) (incf i))
                 (t (error "not a constant expression: ~S" s)))))
    (nreverse toks)))

(defparameter *c-type-words*
  '("ULONG" "LONG" "UWORD" "WORD" "UBYTE" "BYTE" "BOOL" "APTR" "STRPTR"
    "CONST_STRPTR" "CONST_APTR" "IPTR" "SIPTR" "UQUAD" "QUAD" "Tag" "TEXT"
    "USHORT" "SHORT" "UCOUNT" "COUNT" "BPTR" "BSTR" "CPTR" "uint8" "uint16"
    "uint32" "int8" "int16" "int32" "int" "long" "short" "char" "unsigned"
    "signed" "void" "float" "double" "const" "volatile" "struct" "union" "enum"))

(defun c-cast-apply (words pointer v)
  "Narrow V the way a C cast to the type spelled by WORDS would.  POINTER
is true for a `T *' cast; the pointer typedefs (APTR, STRPTR ... — mui.h's
((STRPTR)~0)) are 32-bit addresses too, so they read unsigned."
  (flet ((has (w) (member w words :test #'string=))
         (u (bits) (logand v (1- (ash 1 bits))))
         (s (bits) (let ((m (logand v (1- (ash 1 bits)))))
                     (if (logbitp (1- bits) m) (- m (ash 1 bits)) m))))
    (cond
      ((or pointer (has "APTR") (has "CONST_APTR") (has "STRPTR") (has "CONST_STRPTR")
           (has "CPTR") (has "PLANEPTR"))
       (u 32))
      ((or (has "float") (has "double")) (error "floating-point cast"))
      ((or (has "UBYTE") (has "uint8") (and (has "unsigned") (has "char"))) (u 8))
      ((or (has "BYTE") (has "int8") (has "char")) (s 8))
      ((or (has "UWORD") (has "USHORT") (has "uint16") (and (has "unsigned") (has "short"))) (u 16))
      ((or (has "WORD") (has "SHORT") (has "int16") (has "short")) (s 16))
      ((or (has "ULONG") (has "uint32") (has "IPTR") (has "Tag") (has "UCOUNT")
           (has "BPTR") (has "BSTR") (has "unsigned"))
       (u 32))
      ((or (has "UQUAD")) (u 64))
      ((or (has "QUAD")) (s 64))
      ((or (has "LONG") (has "int32") (has "SIPTR") (has "BOOL") (has "COUNT")
           (has "long") (has "int") (has "signed"))
       (s 32))
      (t v))))

(defun c-binop-prec (op)
  (cond ((string= op "||") 1)
        ((string= op "&&") 2)
        ((string= op "|") 3)
        ((string= op "^") 4)
        ((string= op "&") 5)
        ((member op '("==" "!=") :test #'string=) 6)
        ((member op '("<" "<=" ">" ">=") :test #'string=) 7)
        ((member op '("<<" ">>") :test #'string=) 8)
        ((member op '("+" "-") :test #'string=) 9)
        ((member op '("*" "/" "%") :test #'string=) 10)
        (t nil)))

(defun c-apply (op a b)
  (flet ((bool (x) (if x 1 0)))
    (cond ((string= op "||") (bool (or (/= a 0) (/= b 0))))
          ((string= op "&&") (bool (and (/= a 0) (/= b 0))))
          ((string= op "|") (logior a b))
          ((string= op "^") (logxor a b))
          ((string= op "&") (logand a b))
          ((string= op "==") (bool (= a b)))
          ((string= op "!=") (bool (/= a b)))
          ((string= op "<") (bool (< a b)))
          ((string= op "<=") (bool (<= a b)))
          ((string= op ">") (bool (> a b)))
          ((string= op ">=") (bool (>= a b)))
          ((string= op "<<") (ash a b))
          ((string= op ">>") (ash a (- b)))
          ((string= op "+") (+ a b))
          ((string= op "-") (- a b))
          ((string= op "*") (* a b))
          ((string= op "/") (if (zerop b) (error "division by zero") (truncate a b)))
          (t (if (zerop b) (error "division by zero") (rem a b))))))

(defun c-resolve-symbol (name)
  (cond ((string= name "NULL") 0)
        ((string= name "TRUE") 1)
        ((string= name "FALSE") 0)
        ((gethash name *asm-symbols*) (resolve-asm-symbol name))
        (t (error 'c-unknown-symbol :name name))))

(defun eval-c-tokens (toks)
  "Precedence-climbing evaluator over TOKENIZE-C-EXPR's tokens, with
casts, unary - + ~ !, the binary operators and ?:.  Returns
(values value remaining-tokens)."
  (labels ((peek () (car toks))
           (next () (pop toks))
           (op-p (tk s) (and tk (eq (first tk) :op) (string= (second tk) s)))
           (type-word-p (tk) (and tk (eq (first tk) :sym)
                                  (member (second tk) *c-type-words* :test #'string=)))
           (cast-p ()
             ;; "(" type-words [*]* ")" — look ahead without consuming
             (and (op-p (peek) "(") (type-word-p (second toks))
                  (let ((rest (cddr toks)) (ok t))
                    (loop while (and rest (not (op-p (car rest) ")")))
                          do (unless (or (type-word-p (car rest)) (op-p (car rest) "*")
                                         ;; struct/enum/union NAME
                                         (eq (first (car rest)) :sym))
                               (setf ok nil) (return))
                             (pop rest))
                    (and ok rest))))
           (unary ()
             (let ((tk (peek)))
               (cond
                 ((null tk) (error "unexpected end of expression"))
                 ((cast-p)
                  (next)                ; (
                  (let ((words nil) (pointer nil))
                    (loop for tk2 = (next)
                          until (op-p tk2 ")")
                          do (if (op-p tk2 "*") (setf pointer t)
                                 (push (second tk2) words)))
                    (c-cast-apply words pointer (unary))))
                 ((op-p tk "-") (next) (- (unary)))
                 ((op-p tk "+") (next) (unary))
                 ((op-p tk "~") (next) (lognot (unary)))
                 ((op-p tk "!") (next) (if (zerop (unary)) 1 0))
                 (t (primary)))))
           (primary ()
             (let ((tk (next)))
               (cond
                 ((eq (first tk) :num) (second tk))
                 ((eq (first tk) :sym)
                  (when (string= (second tk) "sizeof") (error "sizeof"))
                  (when (op-p (peek) "(")   ; a function-like macro call
                    (error "call of ~A" (second tk)))
                  (c-resolve-symbol (second tk)))
                 ((op-p tk "(")
                  (let ((v (cond-expr)))
                    (unless (op-p (next) ")") (error "expected )"))
                    v))
                 (t (error "unexpected token ~S" tk)))))
           (expr (min-prec)
             (let ((lhs (unary)))
               (loop
                 (let ((tk (peek)))
                   (unless (and tk (eq (first tk) :op) (c-binop-prec (second tk))
                                (>= (c-binop-prec (second tk)) min-prec))
                     (return lhs))
                   (next)
                   (let ((rhs (expr (1+ (c-binop-prec (second tk))))))
                     (setf lhs (c-apply (second tk) lhs rhs)))))))
           (cond-expr ()
             (let ((c (expr 0)))
               (if (op-p (peek) "?")
                   (progn (next)
                          (let ((a (cond-expr)))
                            (unless (op-p (next) ":") (error "expected :"))
                            (let ((b (cond-expr)))
                              (if (/= c 0) a b))))
                   c))))
    (let ((v (cond-expr)))
      (values v toks))))

(defun c-eval-expr (s)
  "Evaluate the C constant expression S.  Returns (values value status):
status :ok, :unknown (a referenced macro is not defined — yet), or :bad
(not an integer constant expression at all)."
  (handler-case
      (multiple-value-bind (v rest) (eval-c-tokens (tokenize-c-expr s))
        (when rest (error "trailing tokens"))
        (unless (integerp v) (error "not an integer"))
        (values v :ok))
    (c-unknown-symbol () (values nil :unknown))
    (error () (values nil :bad))))

;;; --- preprocessor view of a header ---

(defun read-h-text (full)
  "The file's text with backslash-newline continuations joined and every
comment replaced by one space.  String and char literals are kept."
  (let* ((raw (with-open-file (in full :direction :input :external-format :latin-1)
                (with-output-to-string (out)
                  (loop for line = (read-line in nil nil)
                        while line
                        do (write-string (string-right-trim '(#\Return) line) out)
                           (write-char #\Newline out)))))
         (n (length raw))
         (out (make-string-output-stream))
         (i 0))
    (loop while (< i n)
          do (let ((c (char raw i))
                   (nx (if (< (1+ i) n) (char raw (1+ i)) nil)))
               (cond
                 ;; continuation
                 ((and (char= c #\\) nx (char= nx #\Newline)) (incf i 2))
                 ;; block comment
                 ((and (char= c #\/) nx (char= nx #\*))
                  (let ((e (search "*/" raw :start2 (+ i 2))))
                    (write-char #\Space out)
                    (setf i (if e (+ e 2) n))))
                 ;; line comment
                 ((and (char= c #\/) nx (char= nx #\/))
                  (let ((e (position #\Newline raw :start i)))
                    (write-char #\Space out)
                    (setf i (or e n))))
                 ;; string literal: copy through (escapes included)
                 ((char= c #\")
                  (write-char c out) (incf i)
                  (loop while (and (< i n) (char/= (char raw i) #\"))
                        do (when (and (char= (char raw i) #\\) (< (1+ i) n))
                             (write-char (char raw i) out) (incf i))
                           (write-char (char raw i) out) (incf i))
                  (when (< i n) (write-char #\" out) (incf i)))
                 ;; char literal: copy through
                 ((char= c #\')
                  (write-char c out) (incf i)
                  (loop while (and (< i n) (char/= (char raw i) #\') (char/= (char raw i) #\Newline))
                        do (when (and (char= (char raw i) #\\) (< (1+ i) n))
                             (write-char (char raw i) out) (incf i))
                           (write-char (char raw i) out) (incf i))
                  (when (and (< i n) (char= (char raw i) #\')) (write-char #\' out) (incf i)))
                 (t (write-char c out) (incf i)))))
    (get-output-stream-string out)))

(defun split-lines (text)
  (let ((out nil) (start 0))
    (loop for i from 0 below (length text)
          do (when (char= (char text i) #\Newline)
               (push (subseq text start i) out)
               (setf start (1+ i))))
    (when (< start (length text)) (push (subseq text start) out))
    (nreverse out)))

(defun c-directive (line)
  "If LINE is a preprocessor directive, (values name rest), else NIL."
  (let ((s (trim line)))
    (when (and (> (length s) 0) (char= (char s 0) #\#))
      (let* ((body (trim (subseq s 1)))
             (e (or (position-if-not #'ident-char-p body) (length body))))
        (values (subseq body 0 e) (trim (subseq body e)))))))

(defun c-if-value (expr)
  "#if EXPR as the header reader sees it: defined(X) / defined X over the
macros read so far, numbers and the usual operators; anything that
cannot be evaluated counts as false."
  (let ((s (collapse-whitespace expr)))
    ;; rewrite defined(X) and defined X into 1 / 0
    (loop for p = (search "defined" s)
          while p
          do (let* ((j (+ p 7))
                    (paren (and (< j (length s)) (char= (char s j) #\()))
                    (start (if paren (1+ j) j))
                    (start (or (position-if-not (lambda (c) (char= c #\Space)) s :start start) start))
                    (end (or (position-if-not #'ident-char-p s :start start) (length s)))
                    (name (subseq s start end))
                    (after (if (and paren (< end (length s)) (char= (char s end) #\))) (1+ end) end)))
               (setf s (concatenate 'string (subseq s 0 p)
                                    (if (gethash name *c-macros*) "1" "0")
                                    (subseq s after)))))
    (multiple-value-bind (v status) (c-eval-expr s)
      (and (eq status :ok) (/= v 0)))))

(defun h-twinless-p (rel)
  "True when the header REL (\"gadgets/button.h\") has no .i twin."
  (not (probe-file (concatenate 'string *i-include-root*
                                (subseq rel 0 (- (length rel) 2)) ".i"))))

(defun scan-c-enums (text)
  "Every top-level enum definition in TEXT (a header's active C text), as
a list of lists of (enumerator . value-expression-or-nil) in order.
Struct bodies are skipped by brace depth; string literals are ignored."
  (let ((out nil) (i 0) (n (length text)) (depth 0))
    (flet ((word-at (pos w)
             (and (string= w text :start2 pos :end2 (min n (+ pos (length w))))
                  (or (= pos 0) (not (ident-char-p (char text (1- pos)))))
                  (or (>= (+ pos (length w)) n) (not (ident-char-p (char text (+ pos (length w)))))))))
      (loop while (< i n)
            do (let ((c (char text i)))
                 (cond
                   ((char= c #\") ; skip string
                    (incf i)
                    (loop while (and (< i n) (char/= (char text i) #\"))
                          do (when (char= (char text i) #\\) (incf i)) (incf i))
                    (incf i))
                   ((char= c #\') ; skip char literal
                    (incf i)
                    (loop while (and (< i n) (char/= (char text i) #\'))
                          do (when (char= (char text i) #\\) (incf i)) (incf i))
                    (incf i))
                   ((char= c #\{) (incf depth) (incf i))
                   ((char= c #\}) (decf depth) (incf i))
                   ((and (zerop depth) (word-at i "enum"))
                    (let ((j (+ i 4)))
                      (loop while (and (< j n) (member (char text j) '(#\Space #\Tab #\Newline))) do (incf j))
                      ;; optional tag
                      (loop while (and (< j n) (ident-char-p (char text j))) do (incf j))
                      (loop while (and (< j n) (member (char text j) '(#\Space #\Tab #\Newline))) do (incf j))
                      (cond
                        ((and (< j n) (char= (char text j) #\{))
                         (let ((close (let ((d 0))
                                        (loop for k from j below n
                                              do (cond ((char= (char text k) #\{) (incf d))
                                                       ((char= (char text k) #\})
                                                        (decf d)
                                                        (when (zerop d) (return k))))))))
                           (unless close (return))
                           (let ((items (mapcar (lambda (item)
                                                  (let ((eq (position #\= item)))
                                                    (if eq
                                                        (cons (trim (subseq item 0 eq)) (trim (subseq item (1+ eq))))
                                                        (cons (trim item) nil))))
                                                (remove-if #'blank-string-p
                                                           (split-top-level-commas
                                                            (subseq text (1+ j) close))))))
                             (push items out))
                           (setf i (1+ close))))
                        (t (setf i j)))))
                   (t (incf i))))))
    (nreverse out)))

;;; --- C struct definitions ---
;;;
;;; A twin-less header's struct definitions become (:struct ...) rows
;;; like a .i file's STRUCTUREs: mui.h's MUI_MinMax / MUI_AreaData /
;;; MUI_RenderInfo and the MUIP_* method messages, listbrowser.h's
;;; ColumnInfo.  The layout rules are the m68k AmigaOS ABI's: a member of
;;; two or more bytes is aligned to 2 (a ULONG after a UBYTE sits at
;;; offset 2, not 4), a struct is padded to a multiple of its largest
;;; member alignment (1 or 2), an enum is an int.  The MorphOS SDK's
;;; headers `#pragma pack(2)' to exactly this layout (the OS structures
;;; are shared with 68k code), so one table serves both platforms.
;;;
;;; Members: the exec/types.h scalars and the C types (char, short, int,
;;; long, float, double, signed / unsigned), the pointer typedefs (APTR,
;;; STRPTR ...), `T *' and function pointers (:fptr), `struct X' by value
;;; ((:struct size) — X a STRUCTURE of the .i files, a C struct read
;;; before it, or a typedef of one), arrays ((:array elt n) for scalars
;;; and pointers; a char[n] buffer and an array of structs as (:struct
;;; bytes), a pointer to the field), `enum X' (:i32), and nested struct /
;;; union members, whose leaves are flattened into the enclosing struct
;;; next to the member itself (ihn_stuff.ihn_timer.ihn_millis becomes
;;; MUI-INPUT-HANDLER-NODE-MILLIS, which is what mui.h's ihn_Millis macro
;;; spells).  A struct with a bitfield or a member of unknown type is
;;; skipped with a comment naming the reason; a forward declaration, a
;;; reserved (_-prefixed) name and a definition inside an inactive #if
;;; block are not read.

(defparameter *c-scalar-types*
  ;; the words of a member's type (const / volatile dropped), in source
  ;; order -> keyword and size
  '(("BYTE" :i8 1) ("char" :i8 1) ("signed char" :i8 1) ("int8" :i8 1) ("TEXT" :i8 1)
    ("UBYTE" :u8 1) ("unsigned char" :u8 1) ("uint8" :u8 1)
    ("WORD" :i16 2) ("SHORT" :i16 2) ("short" :i16 2) ("short int" :i16 2)
    ("signed short" :i16 2) ("int16" :i16 2) ("BOOL" :i16 2) ("COUNT" :i16 2)
    ("UWORD" :u16 2) ("USHORT" :u16 2) ("unsigned short" :u16 2)
    ("unsigned short int" :u16 2) ("uint16" :u16 2) ("UCOUNT" :u16 2)
    ("LONG" :i32 4) ("int" :i32 4) ("long" :i32 4) ("long int" :i32 4) ("signed" :i32 4)
    ("signed int" :i32 4) ("signed long" :i32 4) ("int32" :i32 4) ("SIPTR" :i32 4)
    ("ULONG" :u32 4) ("unsigned" :u32 4) ("unsigned int" :u32 4) ("unsigned long" :u32 4)
    ("unsigned long int" :u32 4) ("uint32" :u32 4) ("IPTR" :u32 4) ("Tag" :u32 4)
    ("LONGBITS" :u32 4) ("BPTR" :u32 4) ("BSTR" :u32 4) ("Object" :u32 4)
    ;; devices/ahi.h: typedef LONG Fixed (16.16 fixed point), typedef
    ;; Fixed sposition (a stereo position) -- AHIRequest's volume and pan
    ("Fixed" :i32 4) ("sposition" :i32 4)
    ("FLOAT" :single 4) ("float" :single 4)
    ("DOUBLE" :double 8) ("double" :double 8)
    ("APTR" :fptr 4) ("CONST_APTR" :fptr 4) ("STRPTR" :fptr 4) ("CONST_STRPTR" :fptr 4)
    ("PLANEPTR" :fptr 4) ("CPTR" :fptr 4) ("FPTR" :fptr 4) ("ClassID" :fptr 4)
    ("Msg" :fptr 4) ("CxObj" :fptr 4) ("CxMsg" :fptr 4)
    ;; only ever behind a * or as a function pointer's return type
    ("void" :u32 4) ("VOID" :u32 4)))

(defvar *c-struct-layouts* (make-hash-table :test 'equal)) ; C struct / typedef name -> (size align)
(defvar *i-struct-layouts* (make-hash-table :test 'equal)) ; C name of a STRUCTURE -> (size align)
(defvar *i-struct-layouts-key* -1)                          ; (hash-table-count *i-files*) it was built for

(defun i-struct-layout (name)
  "(size align file) of the STRUCTURE the .i files parsed so far define
under the C name NAME, or NIL.  A STRUCTURE's alignment is 2 unless its
size is odd (the exec / intuition structures all start with a pointer or
a word)."
  (unless (= *i-struct-layouts-key* (hash-table-count *i-files*))
    (setf *i-struct-layouts-key* (hash-table-count *i-files*))
    (clrhash *i-struct-layouts*)
    (maphash (lambda (rel ifile)
               (dolist (st (i-file-structs ifile))
                 (unless (i-struct-size st)   ; a C struct: in *c-struct-layouts*
                   (dolist (v (struct-variants st))
                     (let ((cname (first v)) (size (second v)))
                       (unless (gethash cname *i-struct-layouts*)
                         (setf (gethash cname *i-struct-layouts*)
                               (list size (if (oddp size) 1 2) rel))))))))
             *i-files*))
  (gethash name *i-struct-layouts*))

(defun known-struct-layout (name)
  "(size align) of the struct NAME: a C struct read from a header (or a
typedef of one), else a STRUCTURE of the .i files under its C name, else
NIL."
  (or (gethash name *c-struct-layouts*)
      (let ((lay (i-struct-layout name)))
        (and lay (list (first lay) (second lay))))))

(defun align-up (n a)
  (* a (ceiling n a)))

(defun tokenize-c-decl (s)
  "Tokens of a struct member declaration or a struct statement: (:id
name), (:op \"*\") and the other punctuation, (:brace text) for a {...}
block, (:dim text) for a [...] subscript, (:num text) for a bitfield
width."
  (let ((toks nil) (i 0) (n (length s)))
    (flet ((matching (open close start)
             (let ((d 0))
               (loop for k from start below n
                     do (cond ((char= (char s k) open) (incf d))
                              ((char= (char s k) close)
                               (decf d)
                               (when (zerop d) (return k))))
                     finally (error "unbalanced ~C" open)))))
      (loop while (< i n)
            do (let ((c (char s i)))
                 (cond
                   ((member c '(#\Space #\Tab #\Newline #\Return)) (incf i))
                   ((or (alpha-char-p c) (char= c #\_))
                    (let ((j i))
                      (loop while (and (< j n) (ident-char-p (char s j))) do (incf j))
                      (push (list :id (subseq s i j)) toks)
                      (setf i j)))
                   ((digit-char-p c)
                    (let ((j i))
                      (loop while (and (< j n) (ident-char-p (char s j))) do (incf j))
                      (push (list :num (subseq s i j)) toks)
                      (setf i j)))
                   ((char= c #\{)
                    (let ((e (matching #\{ #\} i)))
                      (push (list :brace (subseq s (1+ i) e)) toks)
                      (setf i (1+ e))))
                   ((char= c #\[)
                    (let ((e (matching #\[ #\] i)))
                      (push (list :dim (subseq s (1+ i) e)) toks)
                      (setf i (1+ e))))
                   ((member c '(#\* #\( #\) #\, #\: #\;))
                    (push (list :op (string c)) toks)
                    (incf i))
                   (t (error "unexpected ~S" c)))))
      (nreverse toks))))

(defun split-c-declarations (body)
  "The member declarations of a struct body: BODY split at the semicolons
outside braces, brackets and parentheses; blank pieces dropped."
  (let ((out nil) (start 0) (depth 0))
    (dotimes (i (length body))
      (let ((c (char body i)))
        (cond ((member c '(#\{ #\[ #\()) (incf depth))
              ((member c '(#\} #\] #\))) (decf depth))
              ((and (char= c #\;) (zerop depth))
               (push (subseq body start i) out)
               (setf start (1+ i))))))
    (push (subseq body start) out)
    (remove-if #'blank-string-p (nreverse out))))

(defun c-member (base name pointer count dims char-p)
  "One laid-out member: (name type size align leaves).  BASE is the
declaration's type — (:scalar kw size align), (:aggregate size align
leaves) or (:tag name) for a `struct NAME' whose layout is looked up
only now: a pointer to it needs none (C allows a pointer to a struct
declared later, or never — mui.h's MUIP_AskMinMax points at a MUI_MinMax
defined 1400 lines below).  POINTER covers `*' declarators and function
pointers; COUNT is the product of the DIMS array bounds."
  (cond
    (pointer
     (if (> dims 0)
         (list name (list :array :fptr count) (* 4 count) 2 nil)
         (list name :fptr 4 2 nil)))
    ((eq (first base) :scalar)
     (destructuring-bind (kw size align) (cdr base)
       (cond ((zerop dims) (list name kw size align nil))
             ;; char[n]: a text buffer — a pointer to it, like an
             ;; embedded struct
             (char-p (list name (list :struct count) count 1 nil))
             (t (list name (list :array kw count) (* size count) align nil)))))
    (t
     (destructuring-bind (size align leaves)
         (if (eq (first base) :tag)
             (let ((lay (known-struct-layout (second base))))
               (unless lay (error "unknown struct ~A" (second base)))
               (list (first lay) (second lay) nil))
             (cdr base))
       (if (> dims 0)
           (list name (list :struct (* size count)) (* size count) align nil)
           (list name (list :struct size) size align leaves))))))

(defun parse-c-declaration (decl)
  "The members one declaration of a struct body declares — `WORD a, b;'
is two — each (name type size align leaves), NAME being NIL for an
anonymous nested struct / union (its LEAVES are what the enclosing struct
gets).  Signals on anything that cannot be laid out."
  (let ((toks (tokenize-c-decl decl))
        (base nil) (char-p nil))
    (labels ((peek () (car toks))
             (next () (pop toks))
             (id-p (tk &optional name)
               (and tk (eq (first tk) :id) (or (null name) (string= (second tk) name))))
             (op-p (tk s) (and tk (eq (first tk) :op) (string= (second tk) s)))
             (qualifier-p (tk)
               ;; const / volatile, and a compiler's calling-convention
               ;; word on a function pointer (LONG __STDARGS__ (*fn)())
               (and (id-p tk)
                    (or (member (second tk) '("const" "volatile") :test #'string=)
                        (starts-with "__" (second tk))))))
      (loop while (qualifier-p (peek)) do (next))
      ;; --- the type ---
      (cond
        ((and (id-p (peek)) (member (second (peek)) '("struct" "union") :test #'string=))
         (let ((kind (if (string= (second (next)) "union") :union :struct))
               (tag nil))
           (when (id-p (peek)) (setf tag (second (next))))
           (cond ((and (peek) (eq (first (peek)) :brace))
                  (multiple-value-bind (leaves size align)
                      (parse-c-aggregate-body (second (next)) kind)
                    (setf base (list :aggregate size align leaves))))
                 (tag (setf base (list :tag tag)))   ; resolved per declarator
                 (t (error "~(~A~) without a tag or a body" kind)))))
        ((id-p (peek) "enum")
         (next)
         (when (id-p (peek)) (next))
         (when (and (peek) (eq (first (peek)) :brace)) (next))
         (setf base (list :scalar :i32 4 2)))
        (t
         ;; type words: the leading identifiers — the last of them is the
         ;; first declarator's name unless a * or ( follows
         (let ((words nil))
           (loop while (id-p (peek)) do (push (second (next)) words))
           (setf words (nreverse words))
           (unless (or (op-p (peek) "*") (op-p (peek) "("))
             (unless (cdr words) (error "no type in ~S" (collapse-whitespace decl)))
             (push (list :id (car (last words))) toks)
             (setf words (butlast words)))
           (setf words (remove-if (lambda (w) (qualifier-p (list :id w))) words))
           (let* ((key (format nil "~{~A~^ ~}" words))
                  (hit (assoc key *c-scalar-types* :test #'string=)))
             (cond (hit
                    (setf base (list :scalar (second hit) (third hit) (min 2 (third hit)))
                          char-p (member key '("char" "TEXT") :test #'string=)))
                   ((and (null (cdr words)) (known-struct-layout (first words)))
                    (setf base (list :tag (first words))))   ; a typedef'd struct
                   (t (error "unknown type ~A" key)))))))
      ;; --- the declarators ---
      (let ((members nil))
        (loop
          (let ((pointer nil) (name nil) (count 1) (dims 0))
            (loop while (op-p (peek) "*") do (next) (setf pointer t))
            (cond
              ((op-p (peek) "(")
               ;; a function pointer: ( * name ) ( parameters )
               (next)
               (loop while (op-p (peek) "*") do (next))
               (unless (id-p (peek)) (error "function pointer without a name"))
               (setf name (second (next)) pointer t)
               (unless (op-p (next) ")") (error "function pointer ~A: expected )" name))
               (unless (op-p (next) "(") (error "function pointer ~A: expected (" name))
               (let ((d 1))
                 (loop while (> d 0)
                       do (let ((tk (next)))
                            (cond ((null tk) (error "function pointer ~A: unbalanced (" name))
                                  ((op-p tk "(") (incf d))
                                  ((op-p tk ")") (decf d)))))))
              (t
               (when (id-p (peek)) (setf name (second (next))))
               (loop while (and (peek) (eq (first (peek)) :dim))
                     do (let ((e (second (next))))
                          (multiple-value-bind (v status) (c-eval-expr e)
                            (unless (and (eq status :ok) (integerp v) (> v 0))
                              (error "array bound [~A] of ~A" (collapse-whitespace e) name))
                            (setf count (* count v))
                            (incf dims))))
               (when (op-p (peek) ":") (error "bitfield ~A" name))))
            (push (c-member base name pointer count dims char-p) members)
            (cond ((op-p (peek) ",") (next))
                  ((null (peek)) (return))
                  (t (error "unexpected ~A after ~A" (second (peek)) name)))))
        (nreverse members)))))

(defun parse-c-aggregate-body (text kind)
  "The members between the braces of a struct (KIND :struct) or union
(:union), laid out by the m68k rules.  Returns (values fields size
align): FIELDS are (c-name type offset) — every named member, each
followed by the leaves of its nested anonymous struct / union — with
offsets relative to the aggregate; SIZE is the padded sizeof."
  (let ((fields nil) (offset 0) (size 0) (align 1))
    (dolist (decl (split-c-declarations text))
      (dolist (m (parse-c-declaration decl))
        (destructuring-bind (name type msize malign leaves) m
          (let ((at (if (eq kind :union) 0 (align-up offset malign))))
            (when name (push (list name type at) fields))
            (dolist (leaf leaves)
              (push (list (first leaf) (second leaf) (+ at (third leaf))) fields))
            (setf align (max align malign))
            (if (eq kind :union)
                (setf size (max size msize))
                (setf offset (+ at msize) size offset))))))
    (values (nreverse fields) (align-up size align) align)))

(defun scan-c-structs (text)
  "Every top-level struct / union definition of TEXT (a header's active C
text), in order: (kind tag body typedef-names) — KIND :struct / :union,
TAG the tag or NIL, BODY the text between the braces, TYPEDEF-NAMES the
names a `typedef struct ... { } Name;' gives it; and (:alias tag
(names)) for a `typedef struct Tag Name;' without a body.  Prototypes,
extern declarations and forward declarations are passed over."
  (let ((out nil) (i 0) (n (length text)) (depth 0))
    (labels ((skip-literal (q)
               (incf i)
               (loop while (and (< i n) (char/= (char text i) q))
                     do (when (char= (char text i) #\\) (incf i)) (incf i))
               (incf i))
             (statement-end (start)
               ;; index of the ; that ends the statement at START, braces skipped
               (let ((d 0))
                 (loop for k from start below n
                       do (let ((c (char text k)))
                            (cond ((char= c #\{) (incf d))
                                  ((char= c #\}) (decf d))
                                  ((and (char= c #\;) (zerop d)) (return k))))
                       finally (return n))))
             (definition (toks)
               ;; the (kind tag body names) / (:alias ...) of one statement's tokens, or NIL
               (let ((typedef nil))
                 (when (and toks (eq (first (car toks)) :id) (string= (second (car toks)) "typedef"))
                   (setf typedef t) (pop toks))
                 (when (and toks (eq (first (car toks)) :id)
                            (member (second (car toks)) '("struct" "union") :test #'string=))
                   (let ((kind (if (string= (second (pop toks)) "union") :union :struct))
                         (tag nil))
                     (when (and toks (eq (first (car toks)) :id))
                       (setf tag (second (pop toks))))
                     (cond
                       ((and toks (eq (first (car toks)) :brace))
                        (let ((body (second (pop toks)))
                              (names nil))
                          (when typedef
                            ;; declarators: Name / , Name — a *Name is a
                            ;; pointer typedef, not a name of the struct
                            (let ((pointer nil))
                              (dolist (tk toks)
                                (cond ((and (eq (first tk) :op) (string= (second tk) "*")) (setf pointer t))
                                      ((and (eq (first tk) :op) (string= (second tk) ",")) (setf pointer nil))
                                      ((and (eq (first tk) :id) (not pointer)) (push (second tk) names))))))
                          (list kind tag body (nreverse names))))
                       ((and typedef tag toks (eq (first (car toks)) :id) (null (cdr toks)))
                        (list :alias tag (list (second (car toks)))))
                       (t nil)))))))
      (loop while (< i n)
            do (let ((c (char text i)))
                 (cond
                   ((char= c #\") (skip-literal #\"))
                   ((char= c #\') (skip-literal #\'))
                   ((char= c #\{) (incf depth) (incf i))
                   ((char= c #\}) (decf depth) (incf i))
                   ((or (alpha-char-p c) (char= c #\_))
                    (let ((j i))
                      (loop while (and (< j n) (ident-char-p (char text j))) do (incf j))
                      (cond
                        ((and (zerop depth)
                              (member (subseq text i j) '("typedef" "struct" "union") :test #'string=))
                         (let* ((e (statement-end i))
                                (def (handler-case (definition (tokenize-c-decl (subseq text i e)))
                                       (error () nil))))
                           (when def (push def out))
                           (setf i (1+ e))))
                        (t (setf i j)))))
                   (t (incf i))))))
    (nreverse out)))

(defun read-c-structs (hfile text)
  "Lay out every struct definition of TEXT and record it in HFILE: the
readable ones as i-structs (fields most recent first, like the .i
reader's), the rest as skipped with the reason.  Every struct read, and
every typedef name of it, becomes known to the structs after it."
  (let ((structs nil) (skipped nil))
    (dolist (def (scan-c-structs text))
      (if (eq (first def) :alias)
          (let ((lay (known-struct-layout (second def))))
            (when lay
              (dolist (nm (third def)) (setf (gethash nm *c-struct-layouts*) lay))))
          (destructuring-bind (kind tag body names) def
            (let ((name (or tag (first names)))
                  (twin nil))
              (when name
                (handler-case
                    (multiple-value-bind (fields size align) (parse-c-aggregate-body body kind)
                      (let ((lay (list size align)))
                        (cond
                          ;; a struct the .i files define too (libraries/keymap.h
                          ;; repeats devices/keymap.i's KeyMap): the STRUCTURE's
                          ;; row is the binding — the same layout, or a warning
                          ((setf twin (i-struct-layout name))
                           (push (cons name (format nil "the STRUCTURE of ~A is the binding (~D bytes)"
                                                    (third twin) (first twin)))
                                 skipped)
                           (unless (= (first twin) size)
                             (asm-warn "~A: struct ~A is ~D bytes here but the STRUCTURE of ~A is ~D — the .i wins"
                                       (i-file-path hfile) name size (third twin) (first twin))))
                          (t
                           (setf (gethash name *c-struct-layouts*) lay)
                           (dolist (nm names) (setf (gethash nm *c-struct-layouts*) lay))
                           (if (char= (char name 0) #\_)
                               (push (cons name "reserved identifier") skipped)
                               (push (make-i-struct :name name :size size :fields (reverse fields)
                                                    :file (i-file-path hfile))
                                     structs))))))
                  (error (e)
                    (push (cons name (princ-to-string e)) skipped))))))))
    (setf (i-file-structs hfile) (nreverse structs)
          (i-file-skipped-structs hfile) (nreverse skipped))
    hfile))

(defun parse-h-file (relpath)
  "Read one twin-less C header into *i-files*: its constants and its
struct definitions."
  (when (gethash relpath *i-files*)
    (return-from parse-h-file (gethash relpath *i-files*)))
  (let ((full (h-file-path relpath)))
    (unless (probe-file full)
      (asm-warn "include not found: ~A" relpath)
      (return-from parse-h-file nil))
    (let ((hfile (make-i-file :path relpath))
          (consts nil)
          (skipped 0)
          (undefined nil)           ; names #undef'd in this file
          (guard nil)               ; the include guard (first #ifndef)
          (first-directive t)
          (cond-stack nil)          ; frames (active taken parent-active)
          (c-text (make-string-output-stream)))
      (setf (gethash relpath *i-files*) hfile)
      (labels ((active-p () (or (null cond-stack) (first (car cond-stack))))
               (push-cond (c)
                 (let ((parent (active-p)))
                   (push (list (and parent c) c parent) cond-stack)))
               (register (name entry)
                 ;; C: the last definition wins only after #undef; a plain
                 ;; redefinition (include guards, a duplicate in a second
                 ;; header) keeps the first value, like the .i reader
                 (unless (gethash name *asm-symbols*)
                   (setf (gethash name *asm-symbols*) entry)))
               (emit-p (name)
                 ;; not the include guard, and not a redefinition after #undef
                 (and (not (equal name guard))
                      (not (member name undefined :test #'string=))))
               (define-constant (name body)
                 (multiple-value-bind (v status) (c-eval-expr body)
                   (case status
                     (:ok (register name (list :value v))
                      (when (emit-p name)
                        (push (make-i-const :name name :value v :file relpath :quiet t) consts)))
                     (:unknown (register name (list :cexpr body))
                      (when (emit-p name)
                        (push (make-i-const :name name :value nil :file relpath :quiet t) consts)))
                     (t (incf skipped)))))
               (define-string (name s ascii)
                 ;; a string macro: known to #ifdef and to the symbol table
                 ;; (an expression that references it is "not a value" ->
                 ;; skipped), emitted only when printable ASCII
                 (register name (list :string s))
                 (cond ((not ascii) (incf skipped))
                       ((emit-p name)
                        (push (make-i-const :name name :value s :file relpath :quiet t) consts))))
               (do-define (rest)
                 (let* ((e (or (position-if-not #'ident-char-p rest) (length rest)))
                        (name (subseq rest 0 e)))
                   (when (> (length name) 0)
                     (setf (gethash name *c-macros*) t)
                     (cond
                       ((and (< e (length rest)) (char= (char rest e) #\())
                        nil)            ; function-like macro
                       (t
                        (let ((body (trim (subseq rest e))))
                          (unless (blank-string-p body)
                            (multiple-value-bind (s ascii) (c-string-literal body)
                              (if s
                                  (define-string name s ascii)
                                  (define-constant name body))))))))))
               (do-include (rest)
                 (let* ((s (trim rest))
                        (open (and (> (length s) 0) (char s 0)))
                        (close (cond ((eql open #\<) #\>) ((eql open #\") #\") (t nil)))
                        (end (and close (position close s :start 1)))
                        (inc (and end (subseq s 1 end))))
                   (when (and inc (ends-with ".h" inc)
                              (probe-file (h-file-path inc))
                              (h-twinless-p inc)
                              (not (member inc *skip-includes* :test #'string=)))
                     (parse-h-file inc)))))
        (dolist (line (split-lines (read-h-text full)))
          (multiple-value-bind (dname rest) (c-directive line)
            (when (and dname first-directive)
              ;; the classic guard: the file's first directive is #ifndef X
              (setf first-directive nil)
              (when (string= dname "ifndef") (setf guard (trim rest))))
            (cond
              ((null dname)
               (when (active-p)
                 (write-string line c-text) (write-char #\Newline c-text)))
              ((string= dname "ifdef") (push-cond (and (gethash (trim rest) *c-macros*) t)))
              ((string= dname "ifndef") (push-cond (not (gethash (trim rest) *c-macros*))))
              ((string= dname "if") (push-cond (and (active-p) (c-if-value rest))))
              ((string= dname "elif")
               (when cond-stack
                 (destructuring-bind (a taken parent) (car cond-stack)
                   (declare (ignore a))
                   (let ((c (and parent (not taken) (c-if-value rest))))
                     (setf (car cond-stack) (list c (or taken c) parent))))))
              ((string= dname "else")
               (when cond-stack
                 (destructuring-bind (a taken parent) (car cond-stack)
                   (declare (ignore a))
                   (setf (car cond-stack) (list (and parent (not taken)) t parent)))))
              ((string= dname "endif") (pop cond-stack))
              ((not (active-p)) nil)
              ((string= dname "define") (do-define rest))
              ((string= dname "undef")
               (let ((name (trim rest)))
                 (remhash name *c-macros*)
                 (remhash name *asm-symbols*)
                 (push name undefined)))
              ((string= dname "include") (do-include rest))
              (t nil))))
        (let ((active (get-output-stream-string c-text)))
          ;; enumerators
          (dolist (items (scan-c-enums active))
          (let ((next 0) (prev nil))
            (dolist (item items)
              (destructuring-bind (name . expr) item
                (when (and (> (length name) 0) (every #'ident-char-p name))
                  (cond
                    (expr
                     (multiple-value-bind (v status) (c-eval-expr expr)
                       (case status
                         (:ok (register name (list :value v))
                          (push (make-i-const :name name :value v :file relpath :quiet t) consts)
                          (setf next (1+ v)))
                         (:unknown (register name (list :cexpr expr))
                          (push (make-i-const :name name :value nil :file relpath :quiet t) consts)
                          (setf next nil))
                         (t (incf skipped) (setf next nil)))))
                    (next
                     (register name (list :value next))
                     (push (make-i-const :name name :value next :file relpath :quiet t) consts)
                     (incf next))
                    (prev
                     (register name (list :cexpr (format nil "(~A + 1)" prev)))
                     (push (make-i-const :name name :value nil :file relpath :quiet t) consts)))
                  (setf prev name))))))
          ;; struct definitions, in order: a struct may embed one read before it
          (read-c-structs hfile active)))
      (setf (i-file-constants hfile) (nreverse consts)
            (i-file-skipped hfile) skipped)
      hfile)))

(defun parse-h-files (relpaths)
  (dolist (p relpaths) (parse-h-file p)))

;;; ================================================================
;;; Module layout: which SFD + which .i / .h files form a module
;;; ================================================================

;;; Assembler struct names for exec's abbreviated STRUCTUREs -> C names.
(defparameter *struct-aliases*
  '(("LN" . "Node") ("MLN" . "MinNode") ("LH" . "List") ("MLH" . "MinList")
    ("MP" . "MsgPort") ("MN" . "Message") ("IO" . "IORequest") ("IOSTD" . "IOStdReq")
    ("LIB" . "Library") ("TC" . "Task") ("TC_Struct" . "Task")
    ("SS" . "SignalSemaphore") ("SSR" . "SemaphoreRequest") ("SSM" . "SemaphoreMessage")
    ("SM" . "Semaphore") ("ML" . "MemList") ("ME" . "MemEntry") ("MH" . "MemHeader")
    ("MC" . "MemChunk") ("IS" . "Interrupt") ("IV" . "IntVector") ("SH" . "SoftIntList")
    ("DD" . "Device") ("UNIT" . "Unit") ("RT" . "Resident") ("SYSBASE" . "ExecBase")
    ("memh" . "MemHandlerData")
    ;; timer.i's TIMEVAL is C's struct timeval (texteditor.h embeds one);
    ;; the same Lisp name either way
    ("TIMEVAL" . "timeval")))

(defun struct-c-name (asm-name)
  (or (cdr (assoc asm-name *struct-aliases* :test #'string=)) asm-name))

;;; Libraries whose include files do not follow the <lib>/ or
;;; libraries/<lib>.i convention.  Everything not listed here is resolved
;;; by convention (DEFAULT-INCLUDES-FOR).  A .h entry is a C header
;;; without a .i twin (see "C header parsing").
(defparameter *module-includes*
  '(("layers"      "graphics/layers.i" "graphics/clip.i")
    ("keymap"      "devices/keymap.i" "libraries/keymap.h")
    ("muimaster"   "libraries/mui.h")          ; the MUI SDK's header root
    ("ahi"         "devices/ahi.h")            ; the AHI SDK's header root
    ("trackfile"   "devices/trackfile.h")
    ("timer"       "devices/timer.i")
    ("console"     "devices/console.i" "devices/conunit.i")
    ("input"       "devices/input.i")
    ("rexxsyslib"  "rexx/storage.i" "rexx/rxslib.i" "rexx/errors.i" "rexx/rexxio.i")
    ("wb"          "workbench/workbench.i" "workbench/startup.i")
    ("icon"        "workbench/icon.i")
    ("cardres"     "resources/card.i")
    ("battclock"   "resources/battclock.i")
    ("battmem"     "resources/battmem.i" "resources/battmembitsamiga.i"
                   "resources/battmembitsshared.i")
    ("cia"         "resources/cia.i")
    ("disk"        "resources/disk.i")
    ("misc"        "resources/misc.i")
    ("potgo"       "resources/potgo.i")
    ("expansion"   "libraries/expansion.i" "libraries/expansionbase.i"
                   "libraries/configvars.i" "libraries/configregs.i")
    ("mathffp"     "libraries/mathffp.h")
    ("mathieeedoubbas" "libraries/mathieeedp.h")
    ("mathieeedoubtrans" "libraries/mathieeedp.h")
    ("mathieeesingbas" "libraries/mathieeesp.h")
    ("mathieeesingtrans" "libraries/mathieeesp.h")
    ("bullet"      "diskfont/glyph.i" "diskfont/oterrors.i" "diskfont/diskfonttag.i")))

(defun include-path (rel)
  "Absolute path of the include REL: .h files live under the C header
roots (first hit), everything else under the assembler include root."
  (if (ends-with ".h" rel)
      (h-file-path rel)
      (concatenate 'string *i-include-root* rel)))

(defun twinless-h-files (dir)
  "Relative paths of the C headers in DIR that have no .i twin — the
ones that take part in the module layout.  Every header root is scanned
(a header the second root adds joins the layout like the NDK's); a
relative path counts once."
  (let ((rels nil))
    (dolist (root *h-include-roots*)
      (dolist (p (directory (concatenate 'string root dir "/*.h")))
        (let ((rel (concatenate 'string dir "/" (pathname-name p) ".h")))
          ;; (case-insensitively: on a case-insensitive file system the
          ;; mui/ and MUI/ scans find the same files)
          (unless (member rel rels :test #'string-equal) (push rel rels)))))
    (remove-if-not #'h-twinless-p (nreverse rels))))

(defun header-module-stem (rel)
  "The module stem (amiga/raw/<stem>) of a header-only module: the include
path without its extension, _ spelled - (reaction/reaction_macros.h ->
reaction/reaction-macros); an MUI custom-class header mui/<Name>_mcc.h ->
mui/<name>, the class's file name under MUI:Libs/mui/ in lower case."
  (let* ((slash (position #\/ rel))
         (dir (and slash (subseq rel 0 slash)))
         (base (if slash (subseq rel (1+ slash)) rel)))
    (if (and dir (string-equal dir "mui") (ends-with "_mcc.h" base))
        (format nil "mui/~(~A~)" (subseq base 0 (- (length base) 6)))
        (substitute #\- #\_ (subseq rel 0 (- (length rel) 2))))))

(defun default-includes-for (libname)
  "Convention: <lib>/*.i (plus the twin-less <lib>/*.h) if that NDK
directory exists; else the first of libraries/<lib>, gadgets/<lib>,
images/<lib>, classes/<lib> that exists — its .i, and its .h when that
has no .i twin."
  (let ((dir (concatenate 'string *i-include-root* libname "/")))
    (cond
      ((probe-file (concatenate 'string dir "."))
       (append (mapcar (lambda (p) (concatenate 'string libname "/" (pathname-name p) ".i"))
                       (directory (concatenate 'string dir "*.i")))
               (twinless-h-files libname)))
      (t
       (loop for sub in '("libraries/" "gadgets/" "images/" "classes/")
             for i-rel = (concatenate 'string sub libname ".i")
             for h-rel = (concatenate 'string sub libname ".h")
             for hits = (append (and (probe-file (include-path i-rel)) (list i-rel))
                                (and *h-include-roots*
                                     (probe-file (include-path h-rel))
                                     (h-twinless-p h-rel)
                                     (list h-rel)))
             when hits return hits)))))

(defun includes-for (libname)
  (let ((explicit (cdr (assoc libname *module-includes* :test #'string=))))
    (remove-if-not (lambda (rel)
                     (and (not (member rel *skip-includes* :test #'string=))
                          (probe-file (include-path rel))))
                   (or explicit (default-includes-for libname)))))

;;; Header-only modules: .i files (and twin-less .h files) that no library
;;; claims get a module named after their path, e.g. devices/audio.i ->
;;; amiga/raw/devices/audio, gadgets/tabs.h -> amiga/raw/gadgets/tabs.
;;; mui/ (either spelling) holds the MUI custom-class headers, <Name>_mcc.h
;;; -> amiga/raw/mui/<name>: the class opens by name through MUI_NewObjectA
;;; (there is no library to open), so the module is constants and structs.
(defparameter *header-only-dirs* '("devices" "hardware" "prefs" "resources"
                                   "libraries" "graphics" "exec" "dos" "intuition"
                                   "utility" "workbench" "datatypes" "diskfont"
                                   "rexx" "classes" "gadgets" "images" "reaction"
                                   "mui" "MUI"))

;;; Skipped includes: obsolete-name compatibility files, and C headers
;;; that hold no bindable constants (statement macros, stdio aliases) or
;;; duplicate another header (reaction_author.h is a subset of reaction.h).
(defparameter *skip-includes* '("intuition/iobsolete.i" "libraries/dos.i"
                                "libraries/dosextens.i" "exec/types.i"
                                "exec/macros.i" "exec/strings.i" "exec/ables.i"
                                "exec/initializers.i" "graphics/gfxmacros.i"
                                "hardware/cia.i"
                                "graphics/gfxmacros.h" "dos/ansiio.h"
                                "reaction/reaction_author.h"
                                ;; the AHI kit's driver-side API (ahi_sub):
                                ;; not generated, so its header stays out
                                "libraries/ahi_sub.h"))

(defun lib-module-stem (name libname)
  "Module path of a library: class libraries live under their kind
directory, the way the OS stores them — gadgets/button.gadget ->
gadgets/button, images/bevel.image -> images/bevel, window.class ->
classes/window.  Everything else is the bare library name."
  (cond ((null libname) name)
        ((ends-with ".gadget" libname) (concatenate 'string "gadgets/" name))
        ((ends-with ".image" libname) (concatenate 'string "images/" name))
        ((ends-with ".class" libname) (concatenate 'string "classes/" name))
        (t name)))

;;; ================================================================
;;; Type classification for function results
;;; ================================================================

(defun pointer-type-p (type)
  (let ((u (string-upcase (collapse-whitespace type))))
    (or (ends-with "*" u)
        (member u '("APTR" "CONST_APTR" "STRPTR" "CONST_STRPTR" "PLANEPTR" "MSG"
                    "CXMSG" "CXOBJ" "OBJECT" "CLASS" "CXOBJ *" "DATATYPE")
                :test #'string=))))

(defun result-kind-for (ret-type)
  "Map a C return type to a DEFCFUN :result keyword, or :unsupported."
  (let ((u (string-upcase (collapse-whitespace ret-type))))
    (cond
      ((member u '("VOID" "void") :test #'string=) :void)
      ((pointer-type-p u) :pointer)
      ((string= u "BOOL") :bool)
      ((member u '("LONG" "CONST LONG") :test #'string=) :signed)
      ((string= u "WORD") :i16)
      ((string= u "BYTE") :i8)
      ((string= u "UWORD") :u16)
      ((string= u "UBYTE") :u8)
      ((string= u "DOUBLE") :unsupported)
      ;; ULONG, BPTR, BSTR, Tag, FLOAT (raw IEEE/FFP bits), IPTR, enums...
      (t :unsigned))))

;;; ================================================================
;;; Merging one library across SDK inputs
;;; ================================================================

(defstruct emit-fn
  fn            ; the sfd-fn used for types/docs
  guard         ; nil | :not-morphos | :morphos
  min-version)  ; nil or integer > 39

(defun fn-signature-key (fn)
  (format nil "~A@~D:~{~A~^,~}" (sfd-fn-name fn) (sfd-fn-lvo fn) (sfd-fn-regs fn)))

(defun merge-library-functions (ndk-lib mos-lib)
  "Return a list of emit-fn for the public functions of a library that
exists in the NDK (ndk-lib) and maybe in the MorphOS SDK (mos-lib)."
  (let ((out nil)
        (mos-by-name (make-hash-table :test 'equal))
        (ndk-names (make-hash-table :test 'equal)))
    (when mos-lib
      (dolist (f (sfd-lib-functions mos-lib))
        (setf (gethash (sfd-fn-name f) mos-by-name) f)))
    (when ndk-lib
      (dolist (f (sfd-lib-functions ndk-lib))
        (setf (gethash (sfd-fn-name f) ndk-names) t)
        (unless (sfd-fn-private f)
          (let* ((m (gethash (sfd-fn-name f) mos-by-name))
                 (m-public (and m (not (sfd-fn-private m))))
                 (same (and m-public
                            (string= (fn-signature-key m) (fn-signature-key f))))
                 ;; a version below the floor is what the library always
                 ;; had: OS libraries open at 33..39, muimaster (the MUI
                 ;; SDK sources) at 19 (= MUI 3.8), ahi.device at 4 (AHI
                 ;; V4, 1997, is the baseline every installed AHI provides
                 ;; -- and a guard would be useless there anyway: the
                 ;; device base is only known at runtime, io_Device after
                 ;; OpenDevice, long after the guard was evaluated at
                 ;; intern time against a NIL version) -- only later
                 ;; versions need a runtime (%version>= N) guard
                 (vfloor (case (sfd-fn-source f)
                           ((:mui :mui5) 19)
                           (:ahi 4)
                           (t 39)))
                 (minv (and (sfd-fn-version f) (> (sfd-fn-version f) vfloor)
                            (sfd-fn-version f))))
            (push (make-emit-fn :fn f
                                :guard (cond ((null mos-lib) nil)
                                             (same nil)
                                             (t :not-morphos))
                                :min-version minv)
                  out)
            ;; same name, different LVO/registers on MorphOS: both variants,
            ;; each under its platform guard
            (when (and m-public (not same))
              (push (make-emit-fn :fn m :guard :morphos :min-version nil) out))))))
    (when mos-lib
      (dolist (f (sfd-lib-functions mos-lib))
        (unless (or (sfd-fn-private f) (gethash (sfd-fn-name f) ndk-names))
          (push (make-emit-fn :fn f :guard (if ndk-lib :morphos nil)
                              :min-version nil)
                out))))
    (nreverse out)))

;;; ================================================================
;;; Emission
;;; ================================================================

(defvar *docstrings* t)

(defun format-hex (v)
  (cond ((< v 0) (format nil "~D" v))
        ((< v 16) (format nil "~D" v))
        (t (format nil "#x~X" v))))

(defun c-prototype (fn)
  (format nil "~A ~A(~{~A~^, ~}) (~{~A~^,~}) LVO ~D"
          (sfd-fn-ret-type fn) (sfd-fn-name fn)
          (mapcar (lambda (p) (if (cdr p) (format nil "~A ~A" (car p) (cdr p)) (car p)))
                  (sfd-fn-params fn))
          (sfd-fn-regs fn) (sfd-fn-lvo fn)))

(defun lisp-string (s)
  "Quote S as a Lisp string literal."
  (with-output-to-string (o)
    (write-char #\" o)
    (loop for c across s
          do (when (or (char= c #\") (char= c #\\)) (write-char #\\ o))
             (write-char c o))
    (write-char #\" o)))

;;; Names that would collide with a symbol inherited from a used package
;;; (CL:OPEN / CL:FORMAT / AMIGA.FFI:LIBRARY-VERSION ...) are shadowed in
;;; the generated DEFPACKAGE, so dos.library's Open() becomes
;;; AMIGA.RAW.DOS:OPEN — a distinct symbol — instead of clobbering
;;; CL:OPEN.  The check is done against the live packages of the host
;;; amilisp running this generator.
(defparameter *used-packages* '("COMMON-LISP" "FFI" "AMIGA.FFI"))

(defun inherited-name-p (lisp-name)
  (let ((sym-name (string-upcase lisp-name)))
    (some (lambda (pkg)
            (let ((p (find-package pkg)))
              (and p (find-symbol sym-name p))))
          *used-packages*)))

(defun emit-function (body tail efn base-var name-rows)
  "Write one function binding.  The normal case is one :fn ROW of the
module's DEFINE-BINDING-TABLE into BODY; a function with more than seven
register arguments (no stub can carry it) gets a (:name ...) row so the
symbol is exported by the table, plus a DEFCFUN over CALL-LIBRARY written
after the table into TAIL — guarded like before.  NAME-ROWS (hash) keeps a
(:name) row from repeating for platform variants.  A skip is a comment.
Returns the Lisp name or NIL."
  (let* ((fn (emit-fn-fn efn))
         (lname (lispify (sfd-fn-name fn)))
         (kind (result-kind-for (sfd-fn-ret-type fn)))
         (regs (sfd-fn-regs fn))
         (params (sfd-fn-params fn))
         (skip nil))
    (cond
      ((eq kind :unsupported)
       (setf skip "DOUBLE result (d0/d1 register pair)"))
      ((some (lambda (r) (find #\- r)) regs)
       (setf skip "64-bit register-pair argument"))
      ((some (lambda (r) (not (and (= (length r) 2)
                                   (member (char r 0) '(#\A #\D))
                                   (digit-char-p (char r 1)))))
             regs)
       ;; MorphOS fd files mark PPC-native entry points with (sysv),
       ;; (base,sysv), (r12base,sysv): not reachable via a 68k register call
       (setf skip (format nil "not a 68k register call (~{~(~A~)~^,~})" regs)))
      ((member "A5" regs :test #'string=)
       (setf skip "argument in A5 (reserved by the call dispatcher)"))
      ((member "A6" regs :test #'string=)
       (setf skip "argument in A6 (library base register)"))
      ((/= (length regs) (length params))
       (setf skip (format nil "~D registers for ~D parameters" (length regs) (length params))))
      ((some (lambda (p) (string= (car p) "...")) params)
       (setf skip "varargs")))
    (cond
      (skip
       (format body "~&  ;; skipped ~A: ~A~%" (sfd-fn-name fn) skip)
       nil)
      ((> (length regs) 7)
       ;; the plist path: exported by the table, defined after it
       (let* ((used nil)
              (pnames (mapcar (lambda (p)
                                (let ((n (param-name (or (cdr p) "arg") used)))
                                  (push n used) n))
                              params))
              (regspec (format nil "~{:~(~A~) ~A~^ ~}"
                               (mapcan #'list regs pnames)))
              (guard-open "") (guard-close ""))
         (unless (gethash lname name-rows)
           (setf (gethash lname name-rows) t)
           (format body "~&  (:name ~S)   ; ~D registers: defined after the table via CALL-LIBRARY~%"
                   (string-upcase lname) (length regs)))
         (when (or (emit-fn-guard efn) (emit-fn-min-version efn))
           (let ((conds nil))
             (case (emit-fn-guard efn)
               (:not-morphos (push "(not (member :morphos *features*))" conds))
               (:morphos (push "(member :morphos *features*)" conds)))
             (when (emit-fn-min-version efn)
               (push (format nil "(%version>= ~D)" (emit-fn-min-version efn)) conds))
             (setf guard-open (format nil "(when ~A~%  "
                                      (if (cdr conds)
                                          (format nil "(and ~{~A~^ ~})" (nreverse conds))
                                          (first conds)))
                   guard-close ")")))
         (format tail "~&~A(amiga.ffi:defcfun ~A ~A ~D (~A)~%    :result ~(~S~)~@[~%    :doc ~A~])~A~%"
                 guard-open lname base-var (sfd-fn-lvo fn) regspec kind
                 (and *docstrings* (lisp-string (c-prototype fn)))
                 guard-close)
         lname))
      (t
       ;; (:fn "NAME" lvo (:reg ...) :result [:not-morphos|:morphos] [min-version])
       (format body "~&  (:fn ~S ~D (~{:~(~A~)~^ ~}) ~(~S~)~@[ ~(~S~)~]~@[ ~D~])~@[   ; ~A~]~%"
               (string-upcase lname) (sfd-fn-lvo fn) regs kind
               (emit-fn-guard efn) (emit-fn-min-version efn)
               (and *docstrings* (c-prototype fn)))
       lname))))

(defun strip-field-prefix (name)
  "wd_RPort -> RPort, LN_SUCC -> SUCC; names without a short prefix are
returned as-is."
  (let ((p (position #\_ name)))
    (if (and p (> p 0) (<= p 6) (< (1+ p) (length name)))
        (subseq name (1+ p))
        name)))

(defun emit-struct (out st cname size fields claim)
  "Write one :struct row of the binding table (the DEFCSTRUCT equivalent:
*NAME-SIZE* plus one accessor per field).  CLAIM is the module's name
registry (name kind -> boolean); a field whose accessor name is already
taken is dropped with a comment instead of clashing.  Returns the list of
names defined."
  (let* ((lname (lispify cname))
         (names (list (format nil "*~A-SIZE*" lname)))
         (seen nil))
    (format out "~&  (:struct ~S ~D   ; ~A (~A)~%"
            (string-upcase lname) size (i-struct-name st) (i-struct-file st))
    (dolist (f fields)
      (destructuring-bind (c-name type offset) f
        (let ((fname (lispify (strip-field-prefix c-name))))
          (loop while (member fname seen :test #'string=)
                do (setf fname (concatenate 'string fname "*")))
          (push fname seen)
          (let ((acc (format nil "~A-~A" lname fname)))
            (cond ((funcall claim acc :accessor)
                   (format out "    (~S ~(~S~) ~D)~%" (string-upcase fname) type offset)
                   (push acc names))
                  (t
                   (format out "    ;; dropped ~A: accessor name ~A already defined~%"
                           c-name acc)))))))
    (format out "    )~%")
    (nreverse names)))

(defun field-type-size (ty)
  (cond ((and (consp ty) (eq (first ty) :array))
         (* (third ty) (field-type-size (second ty))))
        ((consp ty) (second ty))
        ((member ty '(:i8 :u8)) 1)
        ((member ty '(:i16 :u16)) 2)
        ((member ty '(:i32 :u32 :single :fptr)) 4)
        ((eq ty :double) 8)
        (t 0)))

(defun struct-variants (st)
  "A STRUCTURE with several *_SIZE labels (exec's IO / IOSTD) yields one
C struct per size label, each covering the fields up to that label.  A C
struct (read from a header) carries its sizeof.  Returns a list of
(c-name size fields)."
  (let* ((labels (reverse (i-struct-size-labels st)))   ; definition order
         (fields (reverse (i-struct-fields st)))
         (max-end (reduce #'max
                          (mapcar (lambda (f) (+ (third f) (field-type-size (second f))))
                                  fields)
                          :initial-value 0))
         (out nil))
    (cond
      ((null labels)
       (list (list (struct-c-name (i-struct-name st))
                   (or (i-struct-size st) max-end)
                   fields)))
      (t
       (loop for (lbl . off) in labels
             do (let* ((prefix (let ((p (position #\_ lbl :from-end t)))
                                 (if p (subseq lbl 0 p)
                                     (string-right-trim "sizeofSIZEOF" lbl))))
                       ;; A size label names its struct through the alias
                       ;; table (IO_SIZE -> IORequest, IOSTD_SIZE ->
                       ;; IOStdReq); unknown prefixes (wd_SIZEOF) mean
                       ;; "the STRUCTURE itself".
                       (cname (cond ((assoc prefix *struct-aliases* :test #'string=)
                                     (struct-c-name prefix))
                                    ((string= prefix (i-struct-name st))
                                     (struct-c-name prefix))
                                    (t (struct-c-name (i-struct-name st))))))
                  ;; two size labels for one struct (wd_Size + wd_SIZEOF):
                  ;; keep the later one
                  (setf out (remove cname out :key #'first :test #'string=))
                  (push (list cname off
                              (remove-if-not (lambda (f) (< (third f) off)) fields))
                        out)))
       (nreverse out)))))

(defun lib-auto-open-p (libname)
  (and libname
       (some (lambda (suffix) (ends-with suffix libname))
             '(".library" ".gadget" ".image" ".class" ".datatype"))))

(defun lib-open-name (libname)
  "OpenLibrary name: classes live in SYS:Classes/<kind>/ which LIBS:
searches through the kind subdirectory."
  (cond ((ends-with ".gadget" libname) (concatenate 'string "gadgets/" libname))
        ((ends-with ".image" libname) (concatenate 'string "images/" libname))
        (t libname)))

(defun const-value (c)
  "Value of the constant C — an integer or a string — or NIL.  A value
read at definition time wins (a C macro #undef'd and redefined later
keeps its first value, like TEXTEDITOR_Dummy); otherwise the name is
resolved through the symbol table.  Assembler constants that cannot be resolved are a warning (the
.i grammar is fully understood, so it is a parser gap); C macros that
cannot be resolved are simply not integer constants (NewObject(...)
aliases and the like) and stay quiet."
  (cond ((i-const-value c))
        ((i-const-quiet c)
         (handler-case (resolve-asm-symbol (i-const-name c))
           (error () nil)))
        (t (asm-symbol-value (i-const-name c)))))

(defun emit-module (out-path module-name lib-short-name ndk-lib mos-lib includes
                    &key header-only)
  "Generate one module file.  LIB-SHORT-NAME is e.g. \"intuition\";
MODULE-NAME the require name (\"amiga/raw/intuition\")."
  (let* ((pkg (string-upcase (substitute #\. #\/ (subseq module-name (length "amiga/raw/")))))
         (pkg (concatenate 'string "AMIGA.RAW." pkg))
         (base-var (var-name (concatenate 'string lib-short-name "-base")))
         (version-var (var-name (concatenate 'string lib-short-name "-version")))
         (libname (or (and ndk-lib (sfd-lib-libname ndk-lib))
                      (and mos-lib (sfd-lib-libname mos-lib))))
         (body (make-string-output-stream))   ; rows of the binding table
         (tail (make-string-output-stream))   ; forms after the table (>7-register DEFCFUNs)
         (name-rows (make-hash-table :test 'equal))
         (exports nil)                        ; every name the table defines (for :shadow)
         (lisp-names (make-hash-table :test 'equal))
         (n-fns 0) (n-consts 0) (n-strings 0) (n-structs 0) (n-skipped 0)
         (n-macros-skipped 0))      ; C macros that are neither integer nor string constants
    (flet ((claim (name what &optional value)
             ;; WHAT is a kind keyword.  VALUE disambiguates: for constants
             ;; two C spellings of the same value (GA_Left / GA_LEFT)
             ;; collapse silently into one Lisp name; for functions it is
             ;; the C name, so the platform variants of ONE function (same
             ;; C name, different LVO under exclusive guards) all pass.
             (let ((prev (gethash name lisp-names)))
               (cond ((and prev (eq what :constant) (eq (car prev) :constant)
                           (equal (cdr prev) value))
                      nil)
                     ((and prev (eq what :function) (eq (car prev) :function)
                           (equal (cdr prev) value))
                      t)
                     (prev
                      (asm-warn "~A: name clash ~A (~A vs ~A) — second dropped"
                                module-name name (car prev) what)
                      nil)
                     (t (setf (gethash name lisp-names) (cons what value)) t)))))
      ;; --- constants + structs from the .i / .h files (NDK only) ---
      (dolist (rel includes)
        (let ((ifile (gethash rel *i-files*)))
          (when ifile
            (incf n-macros-skipped (i-file-skipped ifile))
            (let ((consts (remove-if (lambda (c)
                                       (when (null (const-value c))
                                         (when (i-const-quiet c) (incf n-macros-skipped))
                                         t))
                                     (i-file-constants ifile))))
              (when consts
                (format body "~&~%  ;; --- constants from ~A ---~%" rel)
                (dolist (c consts)
                  (let ((val (const-value c))
                        (cn (constant-name (i-const-name c))))
                    (when (and val (claim cn :constant val))
                      (incf n-consts)
                      (push cn exports)
                      (cond
                        ((stringp val)
                         ;; a string #define (MUIC_Window "Window.mui")
                         (incf n-strings)
                         (format body "  (:const ~S ~A)~%" (string-upcase cn) (lisp-string-form val)))
                        (t
                         (format body "  (:const ~S ~A)~@[   ; ~A~]~%"
                                 (string-upcase cn) (format-hex val)
                                 (and (or (> val #xFFFFFFFF) (< val (- #x80000000)))
                                      "value exceeds 32 bits")))))))))
            (when (or (i-file-structs ifile) (i-file-skipped-structs ifile))
              (format body "~&~%  ;; --- structures from ~A ---~%" rel)
              (dolist (st (i-file-structs ifile))
                (dolist (variant (struct-variants st))
                  (destructuring-bind (cname size fields) variant
                    (let ((lname (lispify cname)))
                      (when (claim (format nil "*~A-SIZE*" lname) :struct)
                        (incf n-structs)
                        (dolist (e (emit-struct body st cname size fields #'claim))
                          (push e exports)))))))
              ;; a C struct the header reader could not lay out
              (dolist (sk (i-file-skipped-structs ifile))
                (incf n-skipped)
                (format body "  ;; skipped struct ~A: ~A~%" (car sk) (cdr sk)))))))
      ;; --- functions ---
      (unless header-only
        (format body "~&~%  ;; --- functions (~A~@[ + ~A~]) ---~%"
                (cond ((null ndk-lib) "MorphOS SDK")
                      ((eq (sfd-lib-source ndk-lib) :mui) "MUI SDK")
                      ((eq (sfd-lib-source ndk-lib) :ahi) "AHI SDK")
                      (t (format nil "~A_lib.sfd" lib-short-name)))
                (and ndk-lib mos-lib "MorphOS SDK"))
        (dolist (efn (merge-library-functions ndk-lib mos-lib))
          (let* ((fn (emit-fn-fn efn))
                 (lname (lispify (sfd-fn-name fn))))
            (if (claim lname :function (sfd-fn-name fn))
                (let ((r (emit-function body tail efn base-var name-rows)))
                  (cond ((null r) (incf n-skipped))
                        ((member r exports :test #'string=) (incf n-fns))  ; 2nd variant
                        (t (incf n-fns) (push r exports))))
                (incf n-skipped)))))
      ;; --- assemble the file ---
      (ensure-directories-exist out-path)
      (with-open-file (out out-path :direction :output :if-exists :supersede
                                    :external-format :latin-1)
        (format out ";;; ~A — GENERATED by scripts/gen-amiga-bindings.lisp. DO NOT EDIT.~%" module-name)
        (format out ";;;~%;;; Sources:~%")
        (when ndk-lib
          (case (sfd-lib-source ndk-lib)
            (:mui
             (format out ";;;   MUI 3.8 SDK ~A_lib.fd + clib/~A_protos.h (via fd2sfd)~%"
                     lib-short-name lib-short-name))
            (:ahi
             (format out ";;;   AHI SDK ~A_lib.sfd~@[ (~A)~]~%" lib-short-name (sfd-lib-id ndk-lib)))
            (t
             (format out ";;;   ~A_lib.sfd~@[ (~A)~]~%" lib-short-name (sfd-lib-id ndk-lib)))))
        (when mos-lib
          (format out ";;;   MorphOS SDK ~A_lib.fd + clib/~A_protos.h (via fd2sfd)~%"
                  lib-short-name lib-short-name))
        (dolist (rel includes) (format out ";;;   ~A~%" rel))
        (format out ";;;~%;;; ~D functions, ~D constants~@[ (~D of them strings)~], ~D structs~@[, ~D skipped (see comments)~].~%"
                n-fns n-consts (and (> n-strings 0) n-strings) n-structs
                (and (> n-skipped 0) n-skipped))
        (when (> n-macros-skipped 0)
          (format out ";;; ~D C macro~:P skipped: not an integer or ASCII-string constant (call, float, non-ASCII).~%"
                  n-macros-skipped))
        (format out ";;; Regenerate with `make gen-amiga-bindings` — see README \"Raw OS bindings\".~%~%")
        (format out ";; compile-time too: COMPILE-FILE (the host builds the lib/amiga FASLs) must see~%;; AMIGA.FFI at read time, not only LOAD.~%(eval-when (:compile-toplevel :load-toplevel :execute)~%  (require \"amiga/ffi\"))~%~%")
        (setf exports (nreverse exports))
        (let ((shadows (remove-if-not #'inherited-name-p exports)))
          (format out "(defpackage ~S~%  (:use \"CL\" \"FFI\" \"AMIGA.FFI\")~%" pkg)
          (when shadows
            (format out "  ;; distinct symbols — these names also exist in CL / FFI / AMIGA.FFI~%")
            (format out "  (:shadow~{ ~S~})~%" (mapcar #'string-upcase shadows)))
          ;; Only the names defined by ordinary forms are exported here; every
          ;; binding in the table below is exported by the table itself.
          (cond (header-only
                 (format out "  (:export))~%"))
                (t
                 (format out "  (:export ~S ~S))~%"
                         (string-upcase base-var) (string-upcase version-var)))))
        (format out "~%(in-package ~S)~%" pkg)
        (unless header-only
          (format out "~%;;; Library base — opened at load time on AmigaOS/MorphOS; NIL on other~%")
          (format out ";;; hosts (the module still loads there for testing and compilation).~%")
          (format out ";;; Base and version are re-derived after a heap-image restore, before~%")
          (format out ";;; ~~/.amilisprc runs (AMIGA.FFI:DEFINE-LIBRARY-VARIABLE).~%")
          (cond
            ((lib-auto-open-p libname)
             (format out "(amiga.ffi:define-library-variable ~A~%  (when (member :amigaos *features*)~%    (amiga.ffi:open-library-or-die ~S 0)))~%"
                     base-var (lib-open-name libname)))
            (t
             (format out ";;; ~A is a device/resource: there is no OpenLibrary.  Set the base~%" libname)
             (format out ";;; yourself (IORequest io_Device after OpenDevice, or OpenResource);~%")
             (format out ";;; a restore sets it back to NIL.~%")
             (format out "(amiga.ffi:define-library-variable ~A nil)~%" base-var)))
          (format out "(amiga.ffi:define-library-variable ~A~%  (and ~A (amiga.ffi:library-version ~A)))~%" version-var base-var base-var)
          (format out "(defun %version>= (n)~%  (and ~A (>= ~A n)))~%" version-var version-var))
        ;; The binding table: every constant, struct accessor and library
        ;; function of the module in one form, materialised on first use.
        (let ((rows (get-output-stream-string body)))
          (format out "~%;;; Binding table — every name below is built the first time anything~%")
          (format out ";;; refers to it (specs/raw-bindings-footprint.md); until then the module~%")
          (format out ";;; costs the packed table only.  Row syntax: AMIGA.FFI:DEFINE-BINDING-TABLE.~%")
          (if header-only
              (format out "(amiga.ffi:define-binding-table ~S ()~%" pkg)
              (format out "(amiga.ffi:define-binding-table ~S~%    (:base ~A :version ~A)~%"
                      pkg base-var version-var))
          (write-string rows out)
          (format out "  )~%"))
        (let ((after (get-output-stream-string tail)))
          (when (plusp (length after))
            (format out "~%;;; Functions with more than seven register arguments: DEFUNs over~%")
            (format out ";;; AMIGA:CALL-LIBRARY (exported by the (:name ...) rows above).~%")
            (write-string after out)))
        (format out "~%(provide ~S)~%" module-name)))
    (list n-fns n-consts n-structs n-skipped)))

;;; ================================================================
;;; Driver
;;; ================================================================

;;; Cross-check: the NDK also ships lvo/<lib>_lib.i (sfdc's own rendering
;;; of the same SFD).  Every function we parsed must sit at exactly that
;;; LVO — any mismatch means the SFD parser miscounted an entry.
(defun verify-lvos-against-ndk (lib &optional lvo-dir)
  "LVO-DIR (with trailing slash) is where <lib>_lib.i lives; default the
NDK's lvo/ under the assembler include root.  The AHI kit ships its own."
  (let ((path (concatenate 'string
                           (or lvo-dir (concatenate 'string *i-include-root* "lvo/"))
                           (sfd-lib-name lib) "_lib.i"))
        (table (make-hash-table :test 'equal))
        (bad 0) (checked 0))
    (when (probe-file path)
      (with-open-file (in path :direction :input :external-format :latin-1)
        (loop for line = (read-line in nil nil)
              while line
              do (let ((toks (split-string (strip-asm-comment line) '(#\Space #\Tab))))
                   (when (and (>= (length toks) 3)
                              (starts-with "_LVO" (first toks))
                              (string-equal (second toks) "EQU"))
                     (setf (gethash (subseq (first toks) 4) table)
                           (handler-case (parse-integer (third toks)) (error () nil)))))))
      (dolist (f (sfd-lib-functions lib))
        (multiple-value-bind (lvo present) (gethash (sfd-fn-name f) table)
          (when present
            (incf checked)
            (unless (eql lvo (sfd-fn-lvo f))
              (incf bad)
              (asm-warn "LVO MISMATCH ~A ~A: parsed ~D, lvo/~A_lib.i says ~D"
                        (sfd-lib-name lib) (sfd-fn-name f) (sfd-fn-lvo f)
                        (sfd-lib-name lib) lvo))))))
    (values checked bad)))

(defun load-sfd-dir (dir source)
  (let ((libs (make-hash-table :test 'equal)))
    (when (and dir (probe-file (concatenate 'string (dir-path dir) ".")))
      (dolist (p (directory (concatenate 'string (dir-path dir) "*.sfd")))
        (handler-case
            (let ((lib (parse-sfd-file p :source source)))
              (setf (gethash (sfd-lib-name lib) libs) lib))
          (error (e)
            (format *error-output* "WARNING: cannot parse ~A: ~A~%" p e)))))
    libs))

;;; Cross-check of a library both the MUI SDK and the MorphOS SDK describe:
;;; the public functions common to both must sit at the same LVO with the
;;; same registers.  The two fd files count their ##private gaps
;;; independently, so a disagreement means one of them was miscounted —
;;; and a miscounted table would jump into the wrong vector on one
;;; platform.  Returns (values agreeing differing); each difference is a
;;; warning.
(defun verify-lvos-against-mos (lib other &optional (lib-label "MUI SDK")
                                            (other-label "MorphOS SDK"))
  (let ((by-name (make-hash-table :test 'equal)) (same 0) (bad 0))
    (dolist (f (sfd-lib-functions other))
      (unless (sfd-fn-private f) (setf (gethash (sfd-fn-name f) by-name) f)))
    (dolist (f (sfd-lib-functions lib))
      (let ((m (and (not (sfd-fn-private f)) (gethash (sfd-fn-name f) by-name))))
        (when m
          (cond ((and (= (sfd-fn-lvo m) (sfd-fn-lvo f))
                      (equal (sfd-fn-regs m) (sfd-fn-regs f)))
                 (incf same))
                (t (incf bad)
                   (asm-warn "~A: ~A is at LVO ~D (~{~A~^,~}) in the ~A but ~D (~{~A~^,~}) in the ~A — a ##private gap miscounted?"
                             (sfd-lib-name lib) (sfd-fn-name f)
                             (sfd-fn-lvo f) (sfd-fn-regs f) lib-label
                             (sfd-fn-lvo m) (sfd-fn-regs m) other-label))))))
    (values same bad)))

;;; ================================================================
;;; Additive MUI headers — the MUI 5 SDK's and the MorphOS SDK's mui.h
;;; ================================================================
;;;
;;; The muimaster module's baseline is MUI 3.8: its numbers are the common
;;; subset of every MUI a program can meet, and later MUIs keep them.  The
;;; post-3.8 headers (the MUI 5 SDK for AmigaOS, the MorphOS SDK's
;;; gg:os-include/libraries/mui.h) are ADDITIVE constant sources: each is
;;; parsed in a fresh environment (so its values are its own, not the
;;; baseline's first-definition-wins leftovers), then only the names the
;;; baseline does NOT define join the table — unguarded, because a tag
;;; unknown to an older muimaster is simply ignored by the class, and a
;;; missing class makes MUI_NewObjectA return NULL, which the curated
;;; layer reports by name.  A name both define must have the baseline's
;;; value: anything else is fatal (a wrong or miscounted header), except
;;; the known count/version markers below, which legitimately grew after
;;; 3.8 — for those the 3.8 value wins, silently (a marker only the
;;; additive sources carry keeps the earlier source's value: baseline,
;;; then MUI 5, then MorphOS).  Structs are NOT taken
;;; from additive headers (layouts stay 3.8's — a program must not poke
;;; fields the running MUI does not have); their new functions come from
;;; the MUI 5 SDK's sfd instead, under (%version>= 20) guards.

(defparameter *mui-value-evolutions*
  '("MUIMASTER_VMIN"       ; 11 -> 20 (each SDK's own OpenLibrary minimum)
    "MUIMASTER_VLATEST"    ; 19 -> 20
    "MPEN_COUNT"           ; 8 -> 9 (a pen was added after 3.8)
    "PSD_NUMMUIPENS"       ; defined as MPEN_COUNT
    "MUII_Count"           ; 42 -> 55 (built-in images added)
    "MUII_LASTPAT"         ; 145 -> 147
    "MUIV_Frame_Count"     ; 13 -> 24 (frame types added)
    "MUIV_Dirlist_SortType_Count"))  ; MUI 5 says 9, MorphOS 7

(defun copy-string-table (table)
  (let ((out (make-hash-table :test 'equal)))
    (maphash (lambda (k v) (setf (gethash k out) v)) table)
    out))

(defun baseline-known-value (name)
  "What NAME is worth in the fully parsed baseline environment: an
integer, a string, :OPAQUE (defined, but not as a value — a function-like
macro, an unresolvable expression), or :NONE."
  (let ((entry (gethash name *asm-symbols*)))
    (cond (entry (if (eq (first entry) :string)
                     (second entry)
                     (handler-case (resolve-asm-symbol name)
                       (error () :opaque))))
          ((gethash name *c-macros*) :opaque)
          (t :none))))

(defun parse-additive-mui-header (root snap-syms snap-macros)
  "Parse ROOT/libraries/mui.h (and the twin-less headers it includes
under ROOT) in a fresh environment seeded with SNAP-SYMS/SNAP-MACROS —
the symbol table as it stood after the .i files, before any C header —
so every value is the additive header's own.  Returns (values consts
skipped): the resolvable constants in definition order, values stored in
the i-consts, and the count of object-like macros that were skipped or
unresolvable."
  (let ((*asm-symbols* (copy-string-table snap-syms))
        (*c-macros* (copy-string-table snap-macros))
        (*i-files* (make-hash-table :test 'equal))
        (*c-struct-layouts* (make-hash-table :test 'equal))
        (*i-struct-layouts* (make-hash-table :test 'equal))
        (*i-struct-layouts-key* -1)
        (*h-include-roots* (list (dir-path root))))
    (parse-h-file "libraries/mui.h")
    (let ((files (sort (loop for k being the hash-keys of *i-files* collect k)
                       #'string<))
          (consts nil) (skipped 0))
      ;; the primary file's constants first, then its includes' (sorted)
      (dolist (rel (cons "libraries/mui.h"
                         (remove "libraries/mui.h" files :test #'string=)))
        (let ((hfile (gethash rel *i-files*)))
          (when hfile
            (incf skipped (i-file-skipped hfile))
            (dolist (c (i-file-constants hfile))
              (let ((v (const-value c)))
                (cond (v (setf (i-const-value c) v) (push c consts))
                      (t (incf skipped))))))))
      (values (nreverse consts) skipped))))

(defun merge-additive-mui-headers (sources snap-syms snap-macros baseline-p)
  "Parse each additive (LABEL ROOT) source and register one synthetic
i-file per source under a pseudo include path, holding only the constants
NEW relative to the baseline and to earlier sources.  Signals an error on
any value conflict not covered by *MUI-VALUE-EVOLUTIONS*.  Returns the
pseudo paths, in order, for the muimaster module's include list."
  (when (and sources (not baseline-p))
    (format t "MUI additive headers: ignored without the MUI 3.8 SDK (the baseline)~%")
    (return-from merge-additive-mui-headers nil))
  (let ((seen (make-hash-table :test 'equal))   ; new C name -> (value . label)
        (conflicts nil) (evolutions nil) (out nil))
    (dolist (src sources)
      (destructuring-bind (label root) src
        (multiple-value-bind (consts skipped)
            (parse-additive-mui-header root snap-syms snap-macros)
          (let ((new nil))
            (dolist (c consts)
              (let* ((name (i-const-name c)) (v (i-const-value c))
                     (base (baseline-known-value name))
                     (prev (gethash name seen)))
                (cond
                  ((not (eq base :none))
                   (cond ((equal base v))       ; agrees with the baseline
                         ((member name *mui-value-evolutions* :test #'string=)
                          (pushnew name evolutions :test #'string=))
                         ((eq base :opaque))    ; a baseline macro without a value
                         (t (push (list name base v label) conflicts))))
                  (prev
                   (unless (equal (car prev) v)
                     (if (member name *mui-value-evolutions* :test #'string=)
                         (pushnew name evolutions :test #'string=)
                         (push (list name (car prev) v
                                     (format nil "~A vs ~A" (cdr prev) label))
                               conflicts))))
                  (t (setf (gethash name seen) (cons v label))
                     (push c new)))))
            (let* ((pseudo (format nil "libraries/mui.h (~A, additive)" label))
                   (hf (make-i-file :path pseudo :constants (nreverse new))))
              (setf (gethash pseudo *i-files*) hf)
              (push pseudo out)
              (format t "~A libraries/mui.h: ~D new constants (additive)~@[, ~D skipped~]~%"
                      label (length (i-file-constants hf))
                      (and (plusp skipped) skipped)))))))
    (when evolutions
      (format t "MUI additive headers: ~D known value evolution~:P kept at the value of the earliest source (~{~A~^, ~})~%"
              (length evolutions) (sort evolutions #'string<)))
    (when conflicts
      (dolist (cfl (reverse conflicts))
        (format t "CONFLICT ~A: baseline ~S, ~S from the ~A~%"
                (first cfl) (second cfl) (third cfl) (fourth cfl)))
      (error "gen-amiga-bindings: ~D differing value~:P for existing MUI names — the 3.8 header is the baseline; a legitimate post-3.8 evolution belongs in *MUI-VALUE-EVOLUTIONS*, anything else is a wrong header"
             (length conflicts)))
    (nreverse out)))

(defun run ()
  (let* ((ndk-sfd (getenv-or "BINDGEN_NDK_SFD"
                             "tools/m68k-amigaos-gcc/prefix/m68k-amigaos/ndk/lib/sfd"))
         (ndk-inc (getenv-or "BINDGEN_NDK_INCLUDE"
                             "tools/m68k-amigaos-gcc/prefix/m68k-amigaos/ndk-include"))
         (ndk-inc-h (getenv-or "BINDGEN_NDK_INCLUDE_H" ndk-inc))
         (mos-sfd (getenv-or "BINDGEN_MOS_SFD" nil))
         (mos-only (comma-list (getenv-or "BINDGEN_MOS_ONLY" "muimaster,ahi,cybergraphics")))
         (mui-sfd (getenv-or "BINDGEN_MUI_SFD" nil))
         (mui-inc-h (getenv-or "BINDGEN_MUI_INCLUDE_H" nil))
         (mui5-sfd (getenv-or "BINDGEN_MUI5_SFD" nil))
         (mui5-inc-h (getenv-or "BINDGEN_MUI5_INCLUDE_H" nil))
         (mos-mui-inc-h (getenv-or "BINDGEN_MOS_MUI_INCLUDE_H" nil))
         (ahi-sfd (getenv-or "BINDGEN_AHI_SFD" nil))
         (ahi-inc-h (getenv-or "BINDGEN_AHI_INCLUDE_H" nil))
         (ahi-lvo (getenv-or "BINDGEN_AHI_LVO" nil))
         (mcc-inc-h (getenv-or "BINDGEN_MCC_INCLUDE_H" nil))
         (out-dir (dir-path (getenv-or "BINDGEN_OUT" "lib/amiga/raw")))
         (only (comma-list (getenv-or "BINDGEN_LIBS" nil)))
         (*docstrings* (not (string= (getenv-or "BINDGEN_DOCSTRINGS" "1") "0")))
         (*i-include-root* (dir-path ndk-inc))
         (*h-include-roots* (mapcar #'dir-path
                                    (remove nil (list ndk-inc-h mui-inc-h ahi-inc-h mcc-inc-h))))
         (*asm-symbols* (make-hash-table :test 'equal))
         (*c-macros* (make-hash-table :test 'equal))
         (*i-files* (make-hash-table :test 'equal))
         (*c-struct-layouts* (make-hash-table :test 'equal))
         (*i-struct-layouts* (make-hash-table :test 'equal))
         (*i-struct-layouts-key* -1)
         (*asm-warnings* nil)
         (ndk-libs (load-sfd-dir ndk-sfd :ndk))
         (mos-libs (load-sfd-dir mos-sfd :mos))
         (mui-libs (load-sfd-dir mui-sfd :mui))
         (mui5-libs (load-sfd-dir mui5-sfd :mui5))
         (ahi-libs (load-sfd-dir ahi-sfd :ahi))
         (extra-mui-includes nil)
         (claimed-includes (make-hash-table :test 'equal))
         (totals (list 0 0 0 0))
         (modules 0))
    (format t "NDK sfd: ~A (~D libraries)~%" ndk-sfd (hash-table-count ndk-libs))
    (format t "NDK include: ~A~%" ndk-inc)
    (format t "NDK C headers: ~A~%" ndk-inc-h)
    (format t "MorphOS sfd: ~A (~D libraries)~%" (or mos-sfd "none") (hash-table-count mos-libs))
    ;; The MUI SDK's libraries join the primary table: muimaster is then an
    ;; AmigaOS library with a MorphOS twin, merged like every NDK one
    ;; (MorphOS-only MUI_GetRGBColor gets :morphos), and its module claims
    ;; libraries/mui.h through *module-includes*.
    (cond
      (mui-sfd
       (format t "MUI SDK sfd: ~A (~D libraries), C headers: ~A~%"
               mui-sfd (hash-table-count mui-libs) (or mui-inc-h "none"))
       (maphash (lambda (k lib)
                  (when (gethash k ndk-libs)
                    (error "gen-amiga-bindings: ~A_lib.sfd is in both the NDK and the MUI SDK — which is primary?" k))
                  (setf (gethash k ndk-libs) lib))
                mui-libs))
      (t
       (format t "MUI SDK: none — muimaster is emitted from the MorphOS SDK's function table only (no libraries/mui.h constants); commit only output generated WITH the MUI SDK~%")))
    ;; The AHI developer kit's table joins the primary set the same way:
    ;; ahi.device is then an AmigaOS module with a MorphOS twin (no longer
    ;; a MorphOS-only allowlist entry) claiming devices/ahi.h through
    ;; *module-includes*.
    (cond
      (ahi-sfd
       (format t "AHI SDK sfd: ~A (~D libraries), C headers: ~A, lvo: ~A~%"
               ahi-sfd (hash-table-count ahi-libs) (or ahi-inc-h "none") (or ahi-lvo "none"))
       (maphash (lambda (k lib)
                  (when (gethash k ndk-libs)
                    (error "gen-amiga-bindings: ~A_lib.sfd is in both the NDK/MUI set and the AHI SDK — which is primary?" k))
                  (setf (gethash k ndk-libs) lib))
                ahi-libs))
      (t
       (format t "AHI SDK: none — ahi is emitted from the MorphOS SDK's function table only (no devices/ahi.h constants or structs); commit only output generated WITH the AHI SDK~%")))
    (when mcc-inc-h
      (format t "MCC headers: ~A (mui/*_mcc.h -> amiga/raw/mui/<name>)~%" mcc-inc-h))
    (format t "Output: ~A~%" out-dir)
    ;; LVO self-check against the NDK's lvo/*.i — a mismatch means the SFD
    ;; parser miscounted and every later LVO of that library is wrong, so
    ;; it is fatal: no output is written.
    (let ((checked 0) (bad 0))
      (maphash (lambda (k lib) (declare (ignore k))
                 (multiple-value-bind (c b) (verify-lvos-against-ndk lib)
                   (incf checked c) (incf bad b)))
               ndk-libs)
      (format t "LVO cross-check against lvo/*.i: ~D functions checked, ~D mismatches~%"
              checked bad)
      (when (> bad 0)
        (dolist (w (reverse *asm-warnings*)) (format t "  ~A~%" w))
        (error "gen-amiga-bindings: ~D LVO mismatches against the NDK's lvo/*.i — refusing to write bindings" bad)))
    ;; The MUI SDK has no lvo/*.i; its fd is checked against the MorphOS
    ;; SDK's rendering of the same library instead (a warning per
    ;; disagreement — the module then carries both variants under
    ;; exclusive guards, which the log makes visible).
    (when (plusp (hash-table-count mui-libs))
      (let ((same 0) (bad 0))
        (maphash (lambda (k lib)
                   (let ((mos (gethash k mos-libs)))
                     (when mos
                       (multiple-value-bind (s b) (verify-lvos-against-mos lib mos)
                         (incf same s) (incf bad b)))))
                 mui-libs)
        (format t "LVO cross-check MUI SDK vs MorphOS SDK: ~D functions agree, ~D differ~%" same bad)))
    ;; The AHI kit ships its own lvo/ahi_lib.i: the same fatal cross-check
    ;; as the NDK's, read from the kit's directory -- and the MorphOS SDK's
    ;; rendering of ahi.device is compared like the MUI SDK's (a warning
    ;; per disagreement).
    (when (plusp (hash-table-count ahi-libs))
      (when ahi-lvo
        (let ((checked 0) (bad 0))
          (maphash (lambda (k lib) (declare (ignore k))
                     (multiple-value-bind (c b)
                         (verify-lvos-against-ndk lib (dir-path ahi-lvo))
                       (incf checked c) (incf bad b)))
                   ahi-libs)
          (format t "LVO cross-check against the AHI SDK's lvo/*.i: ~D functions checked, ~D mismatches~%"
                  checked bad)
          (when (> bad 0)
            (dolist (w (reverse *asm-warnings*)) (format t "  ~A~%" w))
            (error "gen-amiga-bindings: ~D LVO mismatches against the AHI SDK's lvo/*.i — refusing to write bindings" bad))))
      (let ((same 0) (bad 0))
        (maphash (lambda (k lib)
                   (let ((mos (gethash k mos-libs)))
                     (when mos
                       (multiple-value-bind (s b)
                           (verify-lvos-against-mos lib mos "AHI SDK")
                         (incf same s) (incf bad b)))))
                 ahi-libs)
        (format t "LVO cross-check AHI SDK vs MorphOS SDK: ~D functions agree, ~D differ~%" same bad)))
    ;; The MUI 5 SDK's sfd (additive): the public functions beyond the 3.8
    ;; fd join the primary muimaster table under (%version>= 20) — MUI 5
    ;; for AmigaOS and MorphOS's built-in MUI both report lib_Version 20.
    ;; The overlap must agree exactly with the 3.8 rendering (a
    ;; disagreement means a ##private gap was miscounted somewhere).
    ;; Runs AFTER the cross-checks above, which compare the pristine
    ;; tables.
    (when (plusp (hash-table-count mui5-libs))
      (cond
        ((null mui-sfd)
         (format t "MUI 5 SDK sfd: ignored without the MUI 3.8 SDK (the baseline)~%"))
        (t
         (maphash
          (lambda (k lib5)
            (let ((base (gethash k ndk-libs)))
              (cond
                ((or (null base) (not (eq (sfd-lib-source base) :mui)))
                 (asm-warn "MUI 5 SDK: ~A_lib.sfd has no MUI 3.8 SDK twin — ignored" k))
                (t
                 (multiple-value-bind (same bad)
                     (verify-lvos-against-mos base lib5 "MUI SDK" "MUI 5 SDK")
                   (format t "LVO cross-check MUI SDK vs MUI 5 SDK: ~D functions agree, ~D differ~%"
                           same bad))
                 (let ((known (make-hash-table :test 'equal)) (added 0))
                   (dolist (f (sfd-lib-functions base))
                     (setf (gethash (sfd-fn-name f) known) t))
                   (dolist (f (sfd-lib-functions lib5))
                     (unless (or (sfd-fn-private f) (gethash (sfd-fn-name f) known))
                       (unless (and (sfd-fn-version f) (> (sfd-fn-version f) 19))
                         (setf (sfd-fn-version f) 20))
                       (setf (sfd-lib-functions base)
                             (append (sfd-lib-functions base) (list f)))
                       (incf added)))
                   (format t "MUI 5 SDK: ~A gains ~D post-3.8 function~:P ((%version>= 20))~%"
                           k added))))))
          mui5-libs))))
    ;; Parse every .i under the subsystem dirs first (the symbol table is
    ;; global: STRUCTURE bases and EQUs reference other files), then the
    ;; twin-less .h files (their macros reference the .i constants).
    (let ((all-inc nil))
      (dolist (d *header-only-dirs*)
        (dolist (p (directory (concatenate 'string *i-include-root* d "/*.i")))
          (let ((rel (concatenate 'string d "/" (pathname-name p) ".i")))
            (unless (member rel *skip-includes* :test #'string=)
              (push rel all-inc))))
        (dolist (rel (twinless-h-files d))
          (unless (or (member rel *skip-includes* :test #'string=)
                      (member rel all-inc :test #'string-equal))   ; mui/ vs MUI/
            (push rel all-inc))))
      (setf all-inc (sort all-inc #'string<))
      (parse-i-files ndk-inc (remove-if-not (lambda (r) (ends-with ".i" r)) all-inc))
      ;; snapshot for the additive MUI headers: the .i symbols only, no C
      ;; macro yet — their parse must see its own values, not the
      ;; baseline's first-definition-wins leftovers
      (let ((snap-syms (copy-string-table *asm-symbols*))
            (snap-macros (copy-string-table *c-macros*)))
        (parse-h-files (remove-if-not (lambda (r) (ends-with ".h" r)) all-inc))
        (setf extra-mui-includes
              (merge-additive-mui-headers
               (remove nil (list (and mui5-inc-h (list "MUI 5 SDK" mui5-inc-h))
                                 (and mos-mui-inc-h (list "MorphOS SDK" mos-mui-inc-h))))
               snap-syms snap-macros (and mui-sfd t))))
      ;; --- library modules ---
      (let ((names nil))
        (maphash (lambda (k v) (declare (ignore v)) (push k names)) ndk-libs)
        (dolist (m mos-only)
          (when (and (gethash m mos-libs) (not (gethash m ndk-libs)))
            (push m names)))
        (setf names (sort (remove-duplicates names :test #'string=) #'string<))
        (dolist (name names)
          (when (or (null only) (member name only :test #'string=))
            (let* ((ndk (gethash name ndk-libs))
                   (mos (gethash name mos-libs))
                   (includes (let ((inc (if ndk (includes-for name) nil)))
                               ;; the additive MUI headers' synthetic
                               ;; i-files (new constants only)
                               (if (string= name "muimaster")
                                   (append inc extra-mui-includes)
                                   inc)))
                   (stem (lib-module-stem name (or (and ndk (sfd-lib-libname ndk))
                                                   (and mos (sfd-lib-libname mos)))))
                   (module (concatenate 'string "amiga/raw/" stem))
                   (path (concatenate 'string out-dir stem ".lisp")))
              (dolist (i includes) (setf (gethash i claimed-includes) t))
              (let ((counts (emit-module path module name ndk mos includes)))
                (incf modules)
                (setf totals (mapcar #'+ totals counts))
                (format t "  ~A: ~D fns, ~D consts, ~D structs~@[, ~D skipped~]~%"
                        module (first counts) (second counts) (third counts)
                        (and (> (fourth counts) 0) (fourth counts))))))))
      ;; --- header-only modules for unclaimed .i / .h files ---
      (dolist (rel all-inc)
        (unless (gethash rel claimed-includes)
          (let* ((ifile (gethash rel *i-files*))
                 (stem (header-module-stem rel))
                 (module (concatenate 'string "amiga/raw/" stem))
                 (path (concatenate 'string out-dir stem ".lisp")))
            (when (and ifile
                       (or (i-file-structs ifile)
                           ;; a header whose macros are all non-constant
                           ;; (reaction_macros.h) yields no module
                           (some (lambda (c) (if (i-const-quiet c) (const-value c) t))
                                 (i-file-constants ifile)))
                       (or (null only) (member stem only :test #'string=)))
              (let ((counts (emit-module path module (substitute #\- #\/ stem) nil nil
                                         (list rel) :header-only t)))
                (incf modules)
                (setf totals (mapcar #'+ totals counts))
                (format t "  ~A: ~D consts, ~D structs~%" module
                        (second counts) (third counts))))))))
    (format t "~%~D modules: ~D functions, ~D constants, ~D structs, ~D skipped.~%"
            modules (first totals) (second totals) (third totals) (fourth totals))
    (when *asm-warnings*
      (format t "~D warnings:~%" (length *asm-warnings*))
      (dolist (w (reverse *asm-warnings*)) (format t "  ~A~%" w)))
    (length *asm-warnings*)))

;;; Run when loaded as a script (not when loaded by the unit test, which
;;; sets BINDGEN_NO_RUN=1 and calls the functions directly).
(unless (string= (getenv-or "BINDGEN_NO_RUN" "0") "1")
  (run))
