;;; boot.lisp — Bootstrap functions for AmiLisp
;;; Loaded at startup to provide common CL conveniences.
;;;
;;; PACKAGE DISCIPLINE: this file is read in COMMON-LISP when source-loaded
;;; at boot, but in COMMON-LISP-USER when `make fasl` compiles it.  Any
;;; %-helper defined here that another file (clos.lisp, a compiled FASL of
;;; either) or C code refers to MUST be listed in clos_internal_names[]
;;; (src/core/package.c) so both read paths resolve it to the same exported
;;; AMILISP symbol.  A name missing from that list interns fresh in whichever
;;; package is current — two symbols with one name, and cross-file references
;;; silently land on the unbound one (real-Amiga finding 2026-08-09; see
;;; tests/test_boot_source_compile.sh).

;; Standard CL variables (set early, before any macro expansion)
(setq *macroexpand-hook* #'funcall)

;; CLHS REPL/condition variables that the C bootstrap interns without a
;; value: / // /// hold the value-lists of the three most recent forms,
;; *break-on-signals* gates whether a signalled condition enters the
;; debugger.  (+ ++ +++ - * ** *** are initialised by the C REPL bootstrap;
;; these four are not.)  Give them their standard initial value NIL and
;; make them special so they are BOUNDP and dynamically bindable
;; (ANSI cl-variable-symbols.1).
(proclaim '(special / // /// *break-on-signals*))
(setq / nil // nil /// nil *break-on-signals* nil)

;; Documentation storage -- ahead of everything else in this file on
;; purpose.  The compiler turns every docstring in a DEFUN / DEFMACRO /
;; DEFVAR / DEFPARAMETER / DEFCONSTANT / DEFTYPE into a load-time
;; (%SET-DOCUMENTATION 'name 'doc-type "...") call (compiler_extra.c,
;; emit_doc_call), so the writer has to exist before the first documented
;; definition below runs, whether boot is loaded from source or from the
;; FASL.  DOCUMENTATION itself, and the DEFSETF, are further down with the
;; other CL functions; CLOS later turns both into generic functions.
;; EXT:*CAPTURE-DOCUMENTATION* NIL at compile time drops the strings.
(defvar *documentation-table* (make-hash-table :test 'equal))

(defun %set-documentation (obj doc-type string)
  (setf (gethash (cons obj doc-type) *documentation-table*) string))

;; Composite CAR/CDR accessors (all 28 from CL spec)
(defun cadr (x) (car (cdr x)))
(defun caar (x) (car (car x)))
(defun cdar (x) (cdr (car x)))
(defun cddr (x) (cdr (cdr x)))
(defun caaar (x) (car (car (car x))))
(defun cdaar (x) (cdr (car (car x))))
(defun cddar (x) (cdr (cdr (car x))))
(defun caddr (x) (car (cdr (cdr x))))
(defun cadar (x) (car (cdr (car x))))
(defun caadr (x) (car (car (cdr x))))
(defun cdadr (x) (cdr (car (cdr x))))
(defun cdddr (x) (cdr (cdr (cdr x))))
(defun caaaar (x) (car (car (car (car x)))))
(defun cadaar (x) (car (cdr (car (car x)))))
(defun caddar (x) (car (cdr (cdr (car x)))))
(defun cadddr (x) (car (cdr (cdr (cdr x)))))
(defun cdaaar (x) (cdr (car (car (car x)))))
(defun cddaar (x) (cdr (cdr (car (car x)))))
(defun cdddar (x) (cdr (cdr (cdr (car x)))))
(defun cddddr (x) (cdr (cdr (cdr (cdr x)))))
(defun caadar (x) (car (car (cdr (car x)))))
(defun caaddr (x) (car (car (cdr (cdr x)))))
(defun cdadar (x) (cdr (car (cdr (car x)))))
(defun cdaddr (x) (cdr (car (cdr (cdr x)))))
(defun caaadr (x) (car (car (car (cdr x)))))
(defun cadadr (x) (car (cdr (car (cdr x)))))
(defun cdaadr (x) (cdr (car (car (cdr x)))))
(defun cddadr (x) (cdr (cdr (car (cdr x)))))

;; Utility functions
(defun identity (x) x)
(defun endp (x)
  (cond ((null x) t)
        ((consp x) nil)
        (t (error 'type-error :datum x :expected-type 'list))))

;; prog1 / prog2
(defmacro prog1 (first &rest body) (let ((g (gensym))) `(let ((,g ,first)) ,@body ,g)))
(defmacro prog2 (first second &rest body) (let ((g (gensym))) `(progn ,first (let ((,g ,second)) ,@body ,g))))

;; ----------------------------------------------------------------------
;; Real macro expansions for the control-flow operators that the compiler
;; ALSO inlines as special forms (compile_cond / compile_and / compile_or /
;; compile_case / compile_typecase).  The C bootstrap registers an *identity*
;; macro stub for each so that MACRO-FUNCTION and FBOUNDP return non-NIL; but
;; that stub makes (macroexpand-1 '(cond ...)) return the form UNCHANGED.
;;
;; Code walkers (e.g. iterate's clause walker) treat "macro-function is
;; non-NIL but macroexpand makes no progress" as "a macro that won't expand"
;; and refuse to descend into it — so iterate clauses such as COLLECT hidden
;; inside a COND/CASE were never seen, and reached run time as undefined
;; function calls.  Per the HyperSpec these operators ARE macros, so we give
;; them genuine expansions here.  The compiler dispatches them by symbol
;; BEFORE consulting the macro table (see compile_expr), so these definitions
;; never change how the forms compile — they only make MACROEXPAND conformant.
;; These defmacros shadow the C identity stubs (macro table is prepend-wins).
(defmacro cond (&rest clauses)
  (if (null clauses)
      nil
      (let* ((clause (car clauses))
             (test (car clause))
             (forms (cdr clause)))
        (cond ((eq test t) `(progn ,@forms))
              ((null forms)
               (let ((g (gensym)))
                 `(let ((,g ,test)) (if ,g ,g (cond ,@(cdr clauses))))))
              (t `(if ,test (progn ,@forms) (cond ,@(cdr clauses))))))))

(defmacro and (&rest forms)
  (cond ((null forms) t)
        ((null (cdr forms)) (car forms))
        (t `(if ,(car forms) (and ,@(cdr forms))))))

(defmacro or (&rest forms)
  (cond ((null forms) nil)
        ((null (cdr forms)) (car forms))
        (t (let ((g (gensym)))
             `(let ((,g ,(car forms))) (if ,g ,g (or ,@(cdr forms))))))))

;; The clause bodies below are wrapped in (progn ...) so a matched clause
;; with an EMPTY body yields NIL — (cond (test)) would otherwise return the
;; test value, which is the wrong CASE/TYPECASE semantics (CLHS 5.3).
(defmacro case (keyform &rest clauses)
  (let ((g (gensym)))
    `(let ((,g ,keyform))
       (cond ,@(mapcar
                (lambda (clause)
                  (let ((keys (car clause)) (body (cdr clause)))
                    (cond ((or (eq keys t) (eq keys 'otherwise)) `(t (progn ,@body)))
                          ((listp keys) `((member ,g ',keys) (progn ,@body)))
                          (t `((eql ,g ',keys) (progn ,@body))))))
                clauses)))))

(defmacro ecase (keyform &rest clauses)
  ;; In ECASE the symbols T and OTHERWISE are NOT special — they are literal
  ;; keys (CLHS 5.3) — so every clause is matched, then a fall-through error.
  (let ((g (gensym)))
    `(let ((,g ,keyform))
       (cond ,@(mapcar
                (lambda (clause)
                  (let ((keys (car clause)) (body (cdr clause)))
                    (if (listp keys) `((member ,g ',keys) (progn ,@body))
                        `((eql ,g ',keys) (progn ,@body)))))
                clauses)
             (t (error 'type-error :datum ,g :expected-type 'symbol))))))

(defmacro typecase (keyform &rest clauses)
  (let ((g (gensym)))
    `(let ((,g ,keyform))
       (cond ,@(mapcar
                (lambda (clause)
                  (let ((type (car clause)) (body (cdr clause)))
                    (if (or (eq type t) (eq type 'otherwise)) `(t (progn ,@body))
                        `((typep ,g ',type) (progn ,@body)))))
                clauses)))))

(defmacro etypecase (keyform &rest clauses)
  (let ((g (gensym)))
    `(let ((,g ,keyform))
       (cond ,@(mapcar
                (lambda (clause)
                  `((typep ,g ',(car clause)) (progn ,@(cdr clause))))
                clauses)
             (t (error 'type-error :datum ,g :expected-type t))))))

;; multiple-value-setq — assign multiple values to variables
(defmacro multiple-value-setq (vars form)
  (let ((tmps (mapcar (lambda (v) (declare (ignore v)) (gensym)) vars)))
    `(multiple-value-bind ,tmps ,form
       ,@(mapcar (lambda (var tmp) `(setq ,var ,tmp)) vars tmps)
       ,(car tmps))))

;; prog — block nil + let + tagbody
(defmacro prog (bindings &body body)
  `(block nil (let ,bindings (tagbody ,@body))))
(defmacro prog* (bindings &body body)
  `(block nil (let* ,bindings (tagbody ,@body))))

;; constantp — returns T for self-evaluating forms and (quote ...) forms
(defun constantp (form &optional environment)
  (declare (ignore environment))
  (cond ((null form) t)              ; NIL
        ((eq form t) t)              ; T
        ((keywordp form) t)          ; keywords
        ((symbolp form)              ; check defconstant flag
         (%symbol-constant-p form))
        ((consp form)                ; (quote ...) is constant
         (eq (car form) 'quote))
        (t t)))                      ; numbers, chars, strings, etc.

;; setf modify macros — evaluate all subforms left-to-right exactly once
;; per CLHS 5.1.1.1.1.  For non-symbol places we bind each subform of the
;; place to a temp, then reuse those temps in both the SETF target and
;; the value form so that the place's subforms are seen only once.
;; Operators where (setf (op subforms) val) mutates one of the subforms in
;; place rather than rewriting outward to (setf <subform> ...).  For these,
;; we can safely bind the subforms to gensyms once and reuse them in both
;; the SETF target and the value form.  Operators NOT in this set (getf,
;; ldb, mask-field, etc.) propagate the assignment to their first argument
;; via SETF expansion, so subform-binding would lose the update — we must
;; let SETF see the original PLACE.
(defparameter %place-direct-mutators
  '(car cdr first second third fourth fifth sixth seventh eighth ninth tenth
    rest nth caar cadr cdar cddr caaar caadr cadar caddr cdaar cdadr cddar
    cdddr caaaar caaadr caadar caaddr cadaar cadadr caddar cadddr cdaaar
    cdaadr cdadar cdaddr cddaar cddadr cdddar cddddr aref svref elt char
    schar bit sbit gethash get slot-value row-major-aref symbol-value
    symbol-function fdefinition))

(defun %place-direct-mutator-p (op)
  (and (symbolp op) (member op %place-direct-mutators :test #'eq)))

(defmacro push (item place)
  (cond
    ((symbolp place)
     ;; A variable has no subforms, and (cons item place) reads it after
     ;; the item — the order CLHS 5.1.1.1 / PUSH prescribe — so no
     ;; temporary is needed.  (The compiler turns this shape on an
     ;; unboxed lexical variable into one OP_PUSH_LOCAL before it ever
     ;; reaches the macro; specials, globals and closed-over variables
     ;; take this expansion.)
     `(setq ,place (cons ,item ,place)))
    ((and (consp place) (%place-direct-mutator-p (car place)))
     (let* ((op (car place))
            (subs (cdr place))
            (item-t (gensym "ITEM"))
            (sub-ts (mapcar (lambda (x) (declare (ignore x)) (gensym "T"))
                            subs)))
       `(let* ((,item-t ,item)
               ,@(mapcar #'list sub-ts subs))
          (setf (,op ,@sub-ts) (cons ,item-t (,op ,@sub-ts))))))
    (t `(setf ,place (cons ,item ,place)))))

(defmacro pop (place)
  (cond
    ((symbolp place)
     (let ((g (gensym)))
       `(let ((,g (car ,place)))
          (setq ,place (cdr ,place))
          ,g)))
    ((and (consp place) (%place-direct-mutator-p (car place)))
     (let* ((op (car place))
            (subs (cdr place))
            (sub-ts (mapcar (lambda (x) (declare (ignore x)) (gensym "T"))
                            subs))
            (g (gensym "OLD")))
       `(let* (,@(mapcar #'list sub-ts subs))
          (let ((,g (car (,op ,@sub-ts))))
            (setf (,op ,@sub-ts) (cdr (,op ,@sub-ts)))
            ,g))))
    (t (let ((g (gensym)))
         `(let ((,g (car ,place)))
            (setf ,place (cdr ,place))
            ,g)))))

;; CLtS 5.1.3 demands DELTA be evaluated before PLACE is read, so that
;; e.g. (incf x (setf x 1)) returns 2 and leaves x = 2 (not 1) — the
;; setf-of-x is the delta, then the modified x is what we add to.
;; INCF/DECF: a variable place (no setf temporaries, the access form is
;; the variable itself — so not a symbol macro) with a literal number for
;; DELTA has nothing to sequence, and `(setq var (+ var delta))` is the
;; whole thing: three opcodes instead of the two LET temporaries.  Every
;; other place keeps the general get-setf-expansion shape.
(defmacro incf (place &optional (delta 1) &environment env)
  (multiple-value-bind (temps vals stores set-form access-form)
      (get-setf-expansion place env)
    (if (and (symbolp place) (null temps) (eq access-form place) (numberp delta))
        `(setq ,place (+ ,place ,delta))
        (let ((d (gensym "DELTA")) (store (car stores)))
          `(let* (,@(mapcar #'list temps vals)
                  (,d ,delta)
                  (,store (+ ,access-form ,d)))
             ,set-form
             ,store)))))

(defmacro decf (place &optional (delta 1) &environment env)
  (multiple-value-bind (temps vals stores set-form access-form)
      (get-setf-expansion place env)
    (if (and (symbolp place) (null temps) (eq access-form place) (numberp delta))
        `(setq ,place (- ,place ,delta))
        (let ((d (gensym "DELTA")) (store (car stores)))
          `(let* (,@(mapcar #'list temps vals)
                  (,d ,delta)
                  (,store (- ,access-form ,d)))
             ,set-form
             ,store)))))

(defmacro remf (place indicator)
  ;; CLHS 5.1.2: remf modifies the place.  %remf returns the new list as
  ;; primary value and T/NIL as second; we setf the place and return the
  ;; second value as a generalized boolean.  Per CLHS 5.1.1.1.1, place
  ;; subforms must be evaluated once before INDICATOR.
  (cond
    ((symbolp place)
     (let ((new (gensym)) (found (gensym)) (ind (gensym)))
       `(let ((,ind ,indicator))
          (multiple-value-bind (,new ,found) (amilisp::%remf ,place ,ind)
            (setq ,place ,new)
            ,found))))
    ((and (consp place) (%place-direct-mutator-p (car place)))
     (let* ((op (car place))
            (subs (cdr place))
            (sub-ts (mapcar (lambda (x) (declare (ignore x)) (gensym "T"))
                            subs))
            (ind (gensym "IND")) (new (gensym)) (found (gensym)))
       `(let* (,@(mapcar #'list sub-ts subs)
               (,ind ,indicator))
          (multiple-value-bind (,new ,found)
              (amilisp::%remf (,op ,@sub-ts) ,ind)
            (setf (,op ,@sub-ts) ,new)
            ,found))))
    (t (let ((new (gensym)) (found (gensym)))
         `(multiple-value-bind (,new ,found) (amilisp::%remf ,place ,indicator)
            (setf ,place ,new)
            ,found)))))
(defsetf elt %setf-elt)
(defsetf readtable-case %setf-readtable-case)

;; rotatef — rotate values among places
;; rotatef — rotate values among places (CLHS 5.3).  Each place's subforms
;; must be evaluated exactly once, left to right, so we go through
;; GET-SETF-EXPANSION rather than re-reading the place form on the store
;; (the old version re-evaluated subforms — e.g. a (random ...) index in
;; (aref v (+ i (random n))) — once for the read and again for the write,
;; corrupting Fisher-Yates shuffles).
(defmacro rotatef (&rest places)
  (if (< (length places) 2)
      nil
      (let ((let*-binds nil) (svars nil) (accesses nil) (store-forms nil))
        (dolist (p places)
          (multiple-value-bind (vars vals stores store access)
              (get-setf-expansion p)
            (setq let*-binds (append let*-binds (mapcar #'list vars vals)))
            (setq svars (append svars (list (car stores))))
            (setq accesses (append accesses (list access)))
            (setq store-forms (append store-forms (list store)))))
        `(let* ,let*-binds
           ;; Read every place (into its store var) BEFORE any write, so all
           ;; reads see the old values; store var i gets place i+1, last gets 1.
           (let ,(mapcar #'list svars (append (cdr accesses) (list (car accesses))))
             ,@store-forms
             nil)))))

;; shiftf — shift values among places, return the first place's old value
;; (CLHS 5.3).  Subforms evaluated once via GET-SETF-EXPANSION, same as rotatef.
(defmacro shiftf (&rest places-and-newval)
  (let* ((places (butlast places-and-newval))
         (newval (car (last places-and-newval)))
         (let*-binds nil) (svars nil) (accesses nil) (store-forms nil)
         (nv-temp (gensym "NV")) (result (gensym "RES")))
    (dolist (p places)
      (multiple-value-bind (vars vals stores store access)
          (get-setf-expansion p)
        (setq let*-binds (append let*-binds (mapcar #'list vars vals)))
        (setq svars (append svars (list (car stores))))
        (setq accesses (append accesses (list access)))
        (setq store-forms (append store-forms (list store)))))
    `(let* (,@let*-binds (,nv-temp ,newval))
       (let ((,result ,(car accesses))
             ,@(mapcar #'list svars (append (cdr accesses) (list nv-temp))))
         ,@store-forms
         ,result))))

;; define-modify-macro — define a macro that modifies a place (CLHS 5.1.3).
;; The generated macro must read/write the place evaluating its subforms only
;; once, so it expands through GET-SETF-EXPANSION (the old version expanded to
;; (setf place (fn place args)), evaluating the place — and its subforms — twice).
(defmacro define-modify-macro (name lambda-list function &optional documentation)
  (let* ((place-var (gensym "PLACE"))
         (env-var (gensym "ENV"))
         (rest-pos (position '&rest lambda-list))
         (rest-var (when rest-pos (nth (1+ rest-pos) lambda-list)))
         ;; the individually-passed argument vars (required + optional),
         ;; i.e. everything before &rest with the &optional marker dropped.
         ;; An optional spec may be (var default) or (var default supplied-p)
         ;; — take just the var name, otherwise the spliced (var default)
         ;; would be emitted as a call form, e.g. (&optional (delta 1))
         ;; splicing literal (delta 1) → "Undefined function: DELTA".
         (individual (mapcar (lambda (spec)
                               (if (consp spec) (car spec) spec))
                             (remove '&optional
                                     (if rest-pos (subseq lambda-list 0 rest-pos)
                                         lambda-list)))))
    `(defmacro ,name (,place-var ,@lambda-list &environment ,env-var)
       ,@(when documentation (list documentation))
       (multiple-value-bind (vars vals stores store access)
           (get-setf-expansion ,place-var ,env-var)
         (let ((new-val ,(if rest-var
                             `(list* ',function access ,@individual ,rest-var)
                             `(list ',function access ,@individual))))
           (append
            (list 'let*
                  (append (mapcar #'list vars vals)
                          (list (list (car stores) new-val))))
            (list store)))))))

;; destructuring-bind (CLHS macro).  amilisp compiles destructuring-bind via a
;; native special-form path (compile_destructuring_bind), dispatched ahead of
;; the macro-expansion check, so normal compilation never uses this macro.
;; But MACROEXPAND and code-walkers (iterate, serapeum's internal-definitions
;; walker) require destructuring-bind to be a *real* macro that actually
;; expands — a walker doing (expand (macroexpand form)) loops forever if
;; macroexpand returns the form unchanged.  This provides that genuine
;; expansion, equivalent to the native path: a let*-cursor walk of the
;; destructuring lambda list.
(defun %dbind-parse-opt (item)
  "Parse a &optional spec: var | (var [default [supplied-p]])."
  (if (consp item)
      (values (car item)
              (if (cdr item) (cadr item) nil)
              (if (cddr item) (caddr item) nil))
      (values item nil nil)))

(defun %dbind-parse-key (item)
  "Parse a &key spec, returning (values keyword var default supplied-p).
Spec is var | (var ...) | ((keyword var) ...)."
  (if (consp item)
      (let ((head (car item)))
        (if (consp head)
            (values (car head) (cadr head)
                    (if (cdr item) (cadr item) nil)
                    (if (cddr item) (caddr item) nil))
            (values (intern (symbol-name head) (find-package "KEYWORD")) head
                    (if (cdr item) (cadr item) nil)
                    (if (cddr item) (caddr item) nil))))
      (values (intern (symbol-name item) (find-package "KEYWORD")) item nil nil)))

(defun %dbind-expand (lambda-list source body)
  (let ((bindings nil)              ; reversed list of (var init-form)
        (missing (gensym "DBMISS")))
    (labels
        ((b (var init) (setq bindings (cons (list var init) bindings)))
         (walk (ll src)
           ;; Bind a cursor to SRC, then advance it through the segments.
           ;; Re-binding the same cursor symbol in let* sequentially advances
           ;; it (later binding shadows the earlier one).
           (let ((cur (gensym "DB")))
             (b cur src)
             (scan ll 0 cur)))
         (scan (ll mode cur)        ; mode: 0=required 1=optional 2=key 3=aux
           ;; CLHS 3.4.5: a destructuring lambda list may be dotted —
           ;; (a . b) is equivalent to (a &rest b).  An atomic tail binds
           ;; the rest of the source and ends the walk.  The dotted
           ;; abbreviation stands in the &rest position, which must precede
           ;; &key/&aux (CLHS 3.4.5) — mirror compile_destructure_pattern's
           ;; rejection of a dotted tail in mode 2/3 instead of silently
           ;; accepting it.
           (when (and ll (atom ll))
             (when (>= mode 2)
               (error "destructuring-bind: dotted lambda-list tail ~S after ~A ~
                       (the dotted abbreviation stands in the &rest position, ~
                       which must precede ~A)"
                      ll (if (= mode 2) "&key" "&aux")
                      (if (= mode 2) "&key" "&aux")))
             (b ll cur)
             (return-from scan nil))
           (when ll
             (let ((item (car ll)))
               (cond
                 ((eq item '&whole)
                  (b (cadr ll) cur) (scan (cddr ll) mode cur))
                 ((eq item '&optional) (scan (cdr ll) 1 cur))
                 ((or (eq item '&rest) (eq item '&body))
                  (b (cadr ll) cur) (scan (cddr ll) mode cur))
                 ((eq item '&key)
                  ;; Unknown-keyword check (CLHS 3.4.4 gives destructuring
                  ;; &key the 3.4.1.4 rules): a keyword the section does not
                  ;; name is an error unless the lambda list says
                  ;; &allow-other-keys, or the list itself carries
                  ;; :allow-other-keys with a true value (3.4.1.4.1) — the
                  ;; runtime helper handles the latter.  Mirrors the call
                  ;; compile_destructure_pattern emits on the native path.
                  (let ((kws nil) (allow nil))
                    (do ((tail (cdr ll) (cdr tail)))
                        ((or (atom tail) (eq (car tail) '&aux)))
                      (if (eq (car tail) '&allow-other-keys)
                          (setq allow t)
                          (push (%dbind-parse-key (car tail)) kws)))
                    (when (not allow)
                      (b (gensym "DBK")
                         (list 'amilisp::%dbind-check-keys cur
                               (list 'quote (nreverse kws))))))
                  (scan (cdr ll) 2 cur))
                 ((eq item '&allow-other-keys) (scan (cdr ll) mode cur))
                 ((eq item '&aux) (scan (cdr ll) 3 cur))
                 ((= mode 0)
                  (b (gensym)
                     (list 'unless (list 'consp cur)
                           (list 'error "destructuring-bind: too few values for ~S"
                                 (list 'quote lambda-list))))
                  (if (consp item)
                      (walk item (list 'car cur))
                      (b item (list 'car cur)))
                  (b cur (list 'cdr cur))
                  (scan (cdr ll) 0 cur))
                 ((= mode 1)
                  (multiple-value-bind (var default supp) (%dbind-parse-opt item)
                    (when supp (b supp (list 'consp cur)))
                    (let ((val (list 'if (list 'consp cur) (list 'car cur) default)))
                      (if (consp var) (walk var val) (b var val)))
                    (b cur (list 'cdr cur)))
                  (scan (cdr ll) 1 cur))
                 ((= mode 2)
                  (multiple-value-bind (kw var default supp) (%dbind-parse-key item)
                    (let ((f (gensym "DBF")))
                      (b f (list 'getf cur kw (list 'quote missing)))
                      (when supp (b supp (list 'not (list 'eq f (list 'quote missing)))))
                      (b var (list 'if (list 'eq f (list 'quote missing)) default f))))
                  (scan (cdr ll) 2 cur))
                 ((= mode 3)
                  (if (consp item)
                      (b (car item) (cadr item))
                      (b item nil))
                  (scan (cdr ll) 3 cur))))))
         )
      (walk lambda-list source)
      (cons 'let* (cons (nreverse bindings) body)))))

(defmacro destructuring-bind (lambda-list expr &body body)
  (%dbind-expand lambda-list expr body))

;; Compiler macros (CLHS 3.2.2.1).  The expander runs at compile time
;; on the call form; returning the original form (eq) means "decline"
;; and the compiler proceeds with the regular call path.  See
;; cl_get_compiler_macro / compile_call in compiler.c.
;;
;; Lambda list supports &whole (must be first) for the decline pattern and
;; &environment (any position); &environment's var is bound to the macro
;; environment the compiler passes as the expander's 2nd argument.
(defun %dcm-split-whole (ll)
  "Return (cons WHOLE-VAR CLEANED-LL).  WHOLE-VAR is NIL when no &whole."
  (if (and (consp ll) (eq (car ll) '&whole))
      (cons (cadr ll) (cddr ll))
      (cons nil ll)))

;; Remove a single `&environment <var>` pair from anywhere in a
;; compiler-macro lambda list (CLHS 3.4.4: &environment may appear at any
;; position).  Returns (cons ENV-VAR CLEANED-LL); ENV-VAR is NIL when absent.
;; Compared by symbol-name so a wrong-package &ENVIRONMENT is still caught.
;; Without this strip, &environment + its var flow into the inner
;; DESTRUCTURING-BIND as bogus required params → spurious "too few elements"
;; (e.g. alexandria's `(define-compiler-macro of-type (&whole form type
;; &environment env) ...)`).
(defun %dcm-split-env (ll)
  (cond ((not (consp ll)) (cons nil ll))
        ((and (symbolp (car ll))
              (string= (symbol-name (car ll)) "&ENVIRONMENT"))
         (cons (cadr ll) (cddr ll)))
        (t (let ((sub (%dcm-split-env (cdr ll))))
             (cons (car sub) (cons (car ll) (cdr sub)))))))

;; CLHS 3.4.11: the body is enclosed in an implicit block named NAME,
;; same rule as defun (CLHS 3.4.10).  Libraries (e.g. serapeum's fnil)
;; rely on (return-from <name> <form>) to short-circuit expansion.
;;
;; (setf <name>) function-names are accepted but registration is skipped
;; — compile_call only consults compiler-macros by symbol, and compile_setf
;; never does, so a compiler macro on (setf foo) could never fire.  This
;; lets libraries that try to install one (e.g. serapeum's (setf href))
;; load without erroring; the regular (setf <name>) function still works.
;; Block name for setf function-names is the symbol per CLHS 5.3.2.
(defmacro define-compiler-macro (name lambda-list &body body)
  (let* ((setf-p   (and (consp name) (eq (car name) 'setf)))
         (block-name (if setf-p (cadr name) name))
         (form-var (gensym "FORM"))
         (env-var  (gensym "ENV"))
         (env-split (%dcm-split-env lambda-list))
         (user-env (car env-split))
         (split    (%dcm-split-whole (cdr env-split)))
         (whole    (car split))
         (clean    (cdr split))
         (inner    `(destructuring-bind ,clean (cdr ,form-var)
                      (block ,block-name ,@body)))
         (with-whole (if whole `(let ((,whole ,form-var)) ,inner) inner)))
    (if setf-p
        `',name
        `(progn
           (amilisp::%setf-compiler-macro-function
            (lambda (,form-var ,env-var)
              ,@(if user-env
                    ;; Expose the macro environment to the user's
                    ;; &environment var (the compiler passes the env as the
                    ;; expander's 2nd arg).
                    `((let ((,user-env ,env-var))
                        (declare (ignorable ,user-env))
                        ,with-whole))
                    `((declare (ignore ,env-var))
                      ,with-whole)))
            ',name)
           ',name))))

;; define-symbol-macro — stores expansion on the symbol's plist under
;; %SYMBOL-MACRO-EXPANSION; the compiler consults this when compiling a
;; variable reference (see cl_lookup_global_symbol_macro in compiler.c).
(defmacro define-symbol-macro (symbol expansion)
  `(eval-when (:compile-toplevel :load-toplevel :execute)
     (setf (get ',symbol '%symbol-macro-expansion) ',expansion)
     ',symbol))

;; Setf expanders — define-setf-expander registers a 5-values expander
;; per CLHS 5.1.4.  The expander takes the place's argument forms and
;; returns (temps vals stores store-form access-form).  Two parallel
;; tables: %setf-expansion-fns is the 5-values registry consulted by
;; get-setf-expansion; the C-side setf_expander_table holds a single-form
;; wrapper consulted by the compiler.  define-setf-expander populates both
;; so place-subform-once semantics propagate through nested setf forms.
(defparameter amilisp::*setf-expansion-fns* nil)

(defun amilisp::%register-setf-expansion (name fn)
  (let ((existing (assoc name amilisp::*setf-expansion-fns*)))
    (if existing
        (rplacd existing fn)
        (setq amilisp::*setf-expansion-fns*
              (cons (cons name fn) amilisp::*setf-expansion-fns*))))
  name)

(defun amilisp::%get-setf-expansion-fn (name)
  (cdr (assoc name amilisp::*setf-expansion-fns*)))

;; CLHS 5.1.2 GET-SETF-EXPANSION — returns 5 values bracketing a place
;; so that subforms are evaluated exactly once.  CLHS 5.1.2.7: when the
;; place is a macro call, the place is macroexpanded first.  Order of
;; preference after expansion: (1) explicit define-setf-expander
;; registry, (2) defsetf setter, (3) compiler-handled fallback.
(defun get-setf-expansion (place &optional env)
  ;; CLHS 5.1.2.9: an operator's own setf expander takes precedence over
  ;; macroexpansion, even when the operator is ALSO a macro (e.g. FSet's
  ;; @, which is both a macro and a define-setf-expander).  Consult the
  ;; expander registry BEFORE MACROEXPAND — otherwise the place is expanded
  ;; as an ordinary read form and the expander is lost, yielding a bogus
  ;; generic expansion ((setf (progn #:t) ...)).  This mirrors the
  ;; precedence compile_setf_place already applies on the compiler side.
  (let ((head-expander
          (and (consp place) (symbolp (car place))
               (amilisp::%get-setf-expansion-fn (car place)))))
    (when head-expander
      (return-from get-setf-expansion (apply head-expander (cdr place)))))
  (let ((place (macroexpand place env)))
    (cond
      ((symbolp place)
       (let ((store (gensym "NEW")))
         (values nil nil (list store) (list 'setq place store) place)))
      ;; CLHS 5.1.2.4: (the type place) as a place — the place is the
      ;; inner form; the type spec wraps the access form.  Without this
      ;; the default branch would treat TYPE as a value subform and try
      ;; to evaluate it (e.g. unbound-variable on FIXNUM).
      ((and (consp place) (eq (car place) 'the))
       (multiple-value-bind (temps vals stores set-form access-form)
           (get-setf-expansion (third place) env)
         (values temps vals stores set-form
                 (list 'the (second place) access-form))))
      ((and (consp place) (symbolp (car place)))
       (let ((expander (amilisp::%get-setf-expansion-fn (car place))))
         (cond
           (expander
            (apply expander (cdr place)))
           (t
            (let ((temps (mapcar (lambda (x) (declare (ignore x)) (gensym "T"))
                                 (cdr place)))
                  (store (gensym "NEW"))
                  (setter (%get-defsetf-setter (car place))))
              (cond
                (setter
                 ;; defsetf: setter receives place args... new-value
                 (values temps (cdr place) (list store)
                         (append (list* setter temps) (list store))
                         (cons (car place) temps)))
                (t
                 ;; Default: emit (setf (head temps...) store) and let the
                 ;; compiler dispatch — covers AREF, GETHASH, CAR, etc.
                 (values temps (cdr place) (list store)
                         (list 'setf (cons (car place) temps) store)
                         (cons (car place) temps)))))))))
      (t (error "GET-SETF-EXPANSION: cannot expand ~S" place)))))

(defmacro define-setf-expander (access-fn lambda-list &body body)
  ;; Strip &environment from lambda-list, bind it to nil.  The body
  ;; must return 5 values (CLHS 5.1.4); we register it for both
  ;; get-setf-expansion (5-values) and the compiler (single form wrapper).
  (let ((env-pos (position-if (lambda (x) (and (symbolp x)
                                                (string= (symbol-name x) "&ENVIRONMENT")))
                              lambda-list))
        (clean-ll lambda-list)
        (env-var nil))
    (when env-pos
      (setq env-var (nth (1+ env-pos) lambda-list))
      (setq clean-ll (append (subseq lambda-list 0 env-pos)
                             (subseq lambda-list (+ env-pos 2)))))
    `(progn
       (amilisp::%register-setf-expansion
        ',access-fn
        (lambda ,clean-ll
          ,@(if env-var
                `((let ((,env-var nil))
                    (declare (ignorable ,env-var))
                    ,@body))
                body)))
       (amilisp::%register-setf-expander
        ',access-fn
        (lambda (place-form value-form)
          (multiple-value-bind (temps vals stores store-form access-form)
              (apply (amilisp::%get-setf-expansion-fn ',access-fn)
                     (cdr place-form))
            (declare (ignore access-form))
            (let ((bindings (append (mapcar #'list temps vals)
                                    (list (list (car stores) value-form)))))
              (list 'let* bindings store-form)))))
       ',access-fn)))

;; Long-form DEFSETF (CLHS 5.5.5):
;;   (defsetf access-fn defsetf-lambda-list (store-variable*) [decl|doc]* form*)
;; The C compiler (compile_defsetf) rewrites the long form to a call to this
;; helper macro, which expands it into a DEFINE-SETF-EXPANDER.  Semantics: the
;; access-fn's argument subforms become temp gensyms (evaluated once, in
;; order); the defsetf-lambda-list variables are bound to those temps; the
;; store-variables are bound to fresh store gensyms; BODY is then evaluated as
;; a macro body to produce the storing form.  Using a real lambda + APPLY to
;; bind the lambda-list against the temps lets &optional/&key defaults work.
(defmacro amilisp::%defsetf-long (access-fn lambda-list store-vars &body body)
  ;; Strip a trailing &environment var from the access lambda list (CLHS
  ;; permits it); bind it to NIL since the compiler-side expander has no
  ;; environment to thread here.
  (let ((env-pos (position-if (lambda (x)
                                (and (symbolp x)
                                     (string= (symbol-name x) "&ENVIRONMENT")))
                              lambda-list))
        (clean-ll lambda-list)
        (env-var nil)
        (args (gensym "ARGS")))
    (when env-pos
      (setq env-var (nth (1+ env-pos) lambda-list))
      (setq clean-ll (append (subseq lambda-list 0 env-pos)
                             (subseq lambda-list (+ env-pos 2)))))
    `(define-setf-expander ,access-fn (&rest ,args)
       (let* ((temps (mapcar (lambda (a)
                               (declare (ignore a))
                               (gensym "TMP"))
                             ,args))
              ,@(mapcar (lambda (sv) `(,sv (gensym "STORE"))) store-vars)
              (stores (list ,@store-vars)))
         (values
          temps
          ,args
          stores
          (apply (lambda ,clean-ll
                   ,@(if env-var
                         `((let ((,env-var nil))
                             (declare (ignorable ,env-var))
                             ,@body))
                         body))
                 temps)
          (cons ',access-fn temps))))))

;; (setf (ldb bytespec int-place) new-byte) — CLHS 5.1.4 expander.
;; Composes with int-place's own setf expansion so subforms (e.g. of
;; (aref ...) under the LDB) are evaluated exactly once.
(define-setf-expander ldb (bytespec int-place &environment env)
  (multiple-value-bind (temps vals stores set-form access-form)
      (get-setf-expansion int-place env)
    (let ((bs (gensym "BS")) (val (gensym "VAL")) (store (car stores)))
      (values
       (cons bs temps)
       (cons bytespec vals)
       (list val)
       `(let ((,store (dpb ,val ,bs ,access-form)))
          ,set-form
          ,val)
       `(ldb ,bs ,access-form)))))

(define-setf-expander mask-field (bytespec int-place &environment env)
  (multiple-value-bind (temps vals stores set-form access-form)
      (get-setf-expansion int-place env)
    (let ((bs (gensym "BS")) (val (gensym "VAL")) (store (car stores)))
      (values
       (cons bs temps)
       (cons bytespec vals)
       (list val)
       `(let ((,store (deposit-field ,val ,bs ,access-form)))
          ,set-form
          ,val)
       `(mask-field ,bs ,access-form)))))

;; (setf (getf place indicator [default]) new) — CLHS 5.1.2.4 expander.
;; Composes with PLACE's own setf-expansion so adding a property (which
;; conses a fresh (indicator value) pair onto the front, changing the
;; list head) reassigns PLACE itself rather than a temp.  Without this,
;; GET-SETF-EXPANSION fell through to the generic branch that binds PLACE
;; to a temp, so INCF/DECF/PUSH on an absent key silently dropped the new
;; pair (ANSI INCF-GETF.2).  DEFAULT, when supplied, is evaluated once and
;; read by the access form; its value is ignored on store, per the spec.
;; %SETF-GETF mutates in place when the key is present and returns the
;; (possibly new) list head otherwise.
(define-setf-expander getf (place indicator &optional default &environment env)
  (multiple-value-bind (temps vals stores set-form access-form)
      (get-setf-expansion place env)
    (let ((itemp (gensym "IND"))
          (dtemp (gensym "DEFAULT"))
          (store (gensym "VAL"))
          (ptemp (car stores)))
      (values
       (append temps (list itemp dtemp))
       (append vals (list indicator default))
       (list store)
       `(let ((,ptemp (amilisp::%setf-getf ,access-form ,itemp ,store)))
          ,set-form
          ,store)
       `(getf ,access-form ,itemp ,dtemp)))))

;; List searching — CLHS MEMBER accepts :test, :test-not, and :key.
;; :test defaults to EQL; :key, if supplied, is applied to each list
;; element before testing (item itself is NOT keyed, per spec).
(defun member (item list &key (test nil test-p)
                              (test-not nil test-not-p)
                              (key nil))
  (when (and test-p test-not-p)
    (error ":TEST and :TEST-NOT are mutually exclusive"))
  (do ((l list (cdr l)))
      ((null l) nil)
    (let* ((elem (if key (funcall key (car l)) (car l)))
           (match (cond (test-not-p (not (funcall test-not item elem)))
                        (test-p (funcall test item elem))
                        (t (eql item elem)))))
      (when match
        (return-from member l)))))

;; CLHS 14.2 MEMBER-IF / MEMBER-IF-NOT — return the tail whose car
;; satisfies (or fails) PREDICATE.  :KEY is applied to the element, not
;; to the predicate's result.  :KEY = NIL means use #'identity.
(defun member-if (predicate list &key (key nil))
  (do ((l list (cdr l)))
      ((null l) nil)
    (let ((elem (if key (funcall key (car l)) (car l))))
      (when (funcall predicate elem)
        (return-from member-if l)))))

(defun member-if-not (predicate list &key (key nil))
  (do ((l list (cdr l)))
      ((null l) nil)
    (let ((elem (if key (funcall key (car l)) (car l))))
      (unless (funcall predicate elem)
        (return-from member-if-not l)))))

;; ADJOIN — override C version with full :test/:test-not/:key support.
(defun adjoin (item list &key (test nil test-p) (test-not nil test-not-p)
                              (key nil))
  (when (and test-p test-not-p)
    (error "ADJOIN: cannot supply both :TEST and :TEST-NOT"))
  (let ((pred (cond (test-not-p (complement test-not))
                    (test-p test)
                    (t #'eql)))
        (item-key (if key (funcall key item) item)))
    (dolist (x list)
      (when (funcall pred item-key (if key (funcall key x) x))
        (return-from adjoin list)))
    (cons item list)))

;; pushnew — push via ADJOIN so :test/:test-not/:key are honored.
;; Per CLHS 5.1.1.1.1, evaluate item, then place subforms, then keys
;; (each subform exactly once).
(defmacro pushnew (item place &rest keys)
  (cond
    ((symbolp place)
     (let ((g (gensym "ITEM")))
       `(let ((,g ,item)) (setq ,place (adjoin ,g ,place ,@keys)))))
    ((and (consp place) (%place-direct-mutator-p (car place)))
     (let* ((op (car place))
            (subs (cdr place))
            (item-t (gensym "ITEM"))
            (sub-ts (mapcar (lambda (x) (declare (ignore x)) (gensym "T"))
                            subs)))
       `(let* ((,item-t ,item)
               ,@(mapcar #'list sub-ts subs))
          (setf (,op ,@sub-ts)
                (adjoin ,item-t (,op ,@sub-ts) ,@keys)))))
    (t (let ((g (gensym)))
         `(let ((,g ,item))
            (setf ,place (adjoin ,g ,place ,@keys)))))))

;; Set operations — support :test, :test-not, :key per CL spec.
;; Per CLHS 17.2.1, the :test function is invoked with a list1 element
;; first and a list2 element second.  Caller specifies which side
;; ITEM came from via REVERSE-ARGS: NIL means ITEM is from list1 (the
;; default — used by intersection / set-difference's outer loop, and
;; the first set-exclusive-or loop) and the test is called as
;; (test item list-elem); T means ITEM is from list2 (used by union's
;; loop and the second set-exclusive-or loop) and the test is invoked
;; as (test list-elem item) so list1 is still the first argument.
(defun %set-member (keyed-item list test key reverse-args)
  (dolist (y list nil)
    (let ((y-key (if key (funcall key y) y)))
      (when (if reverse-args
                (funcall test y-key keyed-item)
                (funcall test keyed-item y-key))
        (return t)))))

(defun intersection (list1 list2 &key (test nil test-p) (test-not nil test-not-p) (key nil))
  (when (and test-p test-not-p)
    (error "Cannot supply both :TEST and :TEST-NOT"))
  (let ((result nil)
        (actual-test (cond (test-not-p (complement test-not))
                           (test-p test)
                           (t #'eql))))
    (dolist (x list1 (nreverse result))
      (when (%set-member (if key (funcall key x) x) list2 actual-test key nil)
        (push x result)))))

(defun union (list1 list2 &key (test nil test-p) (test-not nil test-not-p) (key nil))
  (when (and test-p test-not-p)
    (error "Cannot supply both :TEST and :TEST-NOT"))
  (let ((result (copy-list list1))
        (actual-test (cond (test-not-p (complement test-not))
                           (test-p test)
                           (t #'eql))))
    (dolist (x list2 (nreverse result))
      (unless (%set-member (if key (funcall key x) x) list1 actual-test key t)
        (push x result)))))

(defun set-difference (list1 list2 &key (test nil test-p) (test-not nil test-not-p) (key nil))
  (when (and test-p test-not-p)
    (error "Cannot supply both :TEST and :TEST-NOT"))
  (let ((result nil)
        (actual-test (cond (test-not-p (complement test-not))
                           (test-p test)
                           (t #'eql))))
    (dolist (x list1 (nreverse result))
      (unless (%set-member (if key (funcall key x) x) list2 actual-test key nil)
        (push x result)))))

;; ASSOC / RASSOC — override the C builtins to add :test-not and :key.
(defun assoc (item alist &key (test nil test-p) (test-not nil test-not-p)
                              (key nil))
  (when (and test-p test-not-p)
    (error "ASSOC: cannot supply both :TEST and :TEST-NOT"))
  (let ((pred (cond (test-not-p (complement test-not))
                    (test-p test)
                    (t #'eql))))
    (dolist (pair alist nil)
      (cond ((null pair))
            ((consp pair)
             (let ((k (car pair)))
               (when (funcall pred item (if key (funcall key k) k))
                 (return-from assoc pair))))
            (t (error 'type-error :datum pair :expected-type 'list))))))

(defun rassoc (item alist &key (test nil test-p) (test-not nil test-not-p)
                              (key nil))
  (when (and test-p test-not-p)
    (error "RASSOC: cannot supply both :TEST and :TEST-NOT"))
  (let ((pred (cond (test-not-p (complement test-not))
                    (test-p test)
                    (t #'eql))))
    (dolist (pair alist nil)
      (cond ((null pair))
            ((consp pair)
             (let ((v (cdr pair)))
               (when (funcall pred item (if key (funcall key v) v))
                 (return-from rassoc pair))))
            (t (error 'type-error :datum pair :expected-type 'list))))))

;; SUBLIS / SUBST / NSUBST — override C builtins for full :test/:test-not/:key.
(defun sublis (alist tree &key (test nil test-p) (test-not nil test-not-p)
                               (key nil))
  (when (and test-p test-not-p)
    (error "SUBLIS: cannot supply both :TEST and :TEST-NOT"))
  (let ((pred (cond (test-not-p (complement test-not))
                    (test-p test)
                    (t #'eql))))
    (labels ((walk (tree)
               (let ((tk (if key (funcall key tree) tree)))
                 (dolist (pair alist)
                   (when (consp pair)
                     (when (funcall pred tk (car pair))
                       (return-from walk (cdr pair))))))
               (if (consp tree)
                   (let ((car-r (walk (car tree)))
                         (cdr-r (walk (cdr tree))))
                     (if (and (eq car-r (car tree)) (eq cdr-r (cdr tree)))
                         tree
                         (cons car-r cdr-r)))
                   tree)))
      (walk tree))))

(defun subst (new old tree &key (test nil test-p) (test-not nil test-not-p)
                                (key nil))
  (when (and test-p test-not-p)
    (error "SUBST: cannot supply both :TEST and :TEST-NOT"))
  (let ((pred (cond (test-not-p (complement test-not))
                    (test-p test)
                    (t #'eql))))
    (labels ((walk (tree)
               (cond
                 ((funcall pred old (if key (funcall key tree) tree)) new)
                 ((consp tree)
                  (let ((car-r (walk (car tree)))
                        (cdr-r (walk (cdr tree))))
                    (if (and (eq car-r (car tree)) (eq cdr-r (cdr tree)))
                        tree
                        (cons car-r cdr-r))))
                 (t tree))))
      (walk tree))))

(defun nsubst (new old tree &rest keys)
  (apply #'subst new old tree keys))

;; COPY-ALIST / MAKE-LIST :initial-element
(defun copy-alist (alist)
  "Return a copy of ALIST in which each pair is itself copied."
  (let ((result nil))
    (dolist (pair alist (nreverse result))
      (push (if (consp pair) (cons (car pair) (cdr pair)) pair) result))))

(defun subst-if (new test tree &key (key nil))
  "Substitute NEW for every subtree of TREE for which TEST returns true."
  (cond
    ((funcall test (if key (funcall key tree) tree)) new)
    ((consp tree)
     (let ((car-r (subst-if new test (car tree) :key key))
           (cdr-r (subst-if new test (cdr tree) :key key)))
       (if (and (eq car-r (car tree)) (eq cdr-r (cdr tree)))
           tree
           (cons car-r cdr-r))))
    (t tree)))

(defun subst-if-not (new test tree &key (key nil))
  "Substitute NEW for every subtree of TREE for which TEST returns false."
  (subst-if new (complement test) tree :key key))

(defun nsubst-if (new test tree &key (key nil))
  (subst-if new test tree :key key))

(defun nsubst-if-not (new test tree &key (key nil))
  (subst-if-not new test tree :key key))

(defun nsublis (alist tree &rest keys)
  (apply #'sublis alist tree keys))

(defun set-exclusive-or (list1 list2 &key (test nil test-p) (test-not nil test-not-p) (key nil))
  "Return a list of elements appearing in exactly one of LIST1, LIST2."
  (when (and test-p test-not-p)
    (error "Cannot supply both :TEST and :TEST-NOT"))
  (let ((actual-test (cond (test-not-p (complement test-not))
                           (test-p test)
                           (t #'eql)))
        (result nil))
    (dolist (x list1)
      (unless (%set-member (if key (funcall key x) x) list2 actual-test key nil)
        (push x result)))
    (dolist (y list2)
      (unless (%set-member (if key (funcall key y) y) list1 actual-test key t)
        (push y result)))
    (nreverse result)))

;; CLHS: the N-prefixed variants may destructively modify their list
;; arguments but are not required to. We delegate to the non-destructive
;; versions — correctness wins, optimization can come later.
(defun nunion (list1 list2 &rest keys)
  (apply #'union list1 list2 keys))

(defun nintersection (list1 list2 &rest keys)
  (apply #'intersection list1 list2 keys))

(defun nset-difference (list1 list2 &rest keys)
  (apply #'set-difference list1 list2 keys))

(defun nset-exclusive-or (list1 list2 &rest keys)
  (apply #'set-exclusive-or list1 list2 keys))

(defun subsetp (list1 list2 &key (test nil test-p) (test-not nil test-not-p) (key nil))
  (when (and test-p test-not-p)
    (error "Cannot supply both :TEST and :TEST-NOT"))
  (let ((actual-test (cond (test-not-p (complement test-not))
                           (test-p test)
                           (t #'eql))))
    (dolist (x list1 t)
      (unless (%set-member (if key (funcall key x) x) list2 actual-test key nil)
        (return-from subsetp nil)))))

;; documentation — CL standard documentation function.  The table and
;; %SET-DOCUMENTATION are at the top of this file (see there).
(defun documentation (obj doc-type)
  ;; One value (CLHS): the string or NIL -- not GETHASH's found-p too.
  (values (gethash (cons obj doc-type) *documentation-table*)))

(defsetf documentation %set-documentation)

;; with-open-file — defined early so compile-file can use it.
;; Dependencies: open, close (C builtins), unwind-protect/locally/when (special forms).
(defmacro with-open-file (spec &body body)
  (let ((var (car spec))
        (open-args (cdr spec)))
    `(let ((,var (open ,@open-args)))
       (unwind-protect
         (locally ,@body)
         (when ,var (close ,var))))))

(defmacro with-open-stream ((var stream) &body body)
  `(let ((,var ,stream))
     (unwind-protect
       (locally ,@body)
       (when ,var (close ,var)))))

;; compile-file and compile-file-pathname are C builtins (builtins_io.c).
;; compile-file produces real FASL binary files that load without reparsing.

(defmacro with-compilation-unit ((&rest options) &body body)
  (declare (ignore options))
  `(progn ,@body))

;; with-hash-table-iterator — CLHS 18.2.
;; Snapshots the table as a list of (key . value) pairs and exposes a
;; local iterator function that returns three values per call:
;;   (values t key value)  — for each remaining pair
;;   (values nil nil nil)  — once exhausted
(defmacro with-hash-table-iterator ((iter-name hash-table) &body body)
  (let ((pairs (gensym "PAIRS"))
        (pair (gensym "PAIR")))
    `(let ((,pairs (%hash-table-pairs ,hash-table)))
       (flet ((,iter-name ()
                (if ,pairs
                    (let ((,pair (pop ,pairs)))
                      (values t (car ,pair) (cdr ,pair)))
                    (values nil nil nil))))
         ,@body))))

;; (canonical definition below, near the printer helpers)

(defmacro print-unreadable-object ((object stream &key type identity) &body body)
  (let ((obj-var (gensym)))
    `(let ((,obj-var ,object))
       (write-string "#<" ,stream)
       ,@(when type `((write-string (symbol-name (type-of ,obj-var)) ,stream)
                       ,(when body `(write-char #\Space ,stream))))
       ,@body
       (write-string ">" ,stream)
       nil)))

;; handler-case — run clause bodies after unwinding
;; Uses catch/throw + cons box because:
;;   - handlers run in separate VM context (cl_vm_apply), so return-from can't
;;     reach the establishing block
;;   - closures use value capture, so setq on outer variables doesn't propagate;
;;     rplaca on a shared cons cell does propagate
(defmacro handler-case (form &rest clauses)
  ;; The clauses compile to the AMILISP::%HANDLER-CASE special form: one NLX
  ;; frame plus one handler binding per clause, no cons and no closure per
  ;; entry, and the clause body entered directly by the condition-signalling
  ;; code once the form's dynamic extent has been unwound (CLHS 9.1.3).  ALL
  ;; values of FORM are returned on the normal path.  A :no-error clause is
  ;; rewritten first, exactly as CLHS 9.2.23 (HANDLER-CASE) specifies:
  ;;   (block #1=#:error-return
  ;;     (multiple-value-call #'(lambda (nvars) nbody)
  ;;       (block #2=#:normal-return
  ;;         (return-from #1# (handler-case (return-from #2# form) clauses)))))
  (let ((no-error (assoc :no-error clauses)))
    (if no-error
        (let ((error-return (gensym "HC-ERROR-RETURN"))
              (normal-return (gensym "HC-NORMAL-RETURN")))
          `(block ,error-return
             (multiple-value-call (lambda ,(cadr no-error) ,@(cddr no-error))
               (block ,normal-return
                 (return-from ,error-return
                   (handler-case (return-from ,normal-return ,form)
                     ,@(remove no-error clauses)))))))
        `(amilisp::%handler-case ,form ,@clauses))))

;; ignore-errors — catch errors, return (values nil condition)
(defmacro ignore-errors (&rest body)
  `(handler-case (progn ,@body)
     (error (c) (values nil c))))

;; with-simple-restart — establish a named restart that returns (values nil t).
;; Per CLHS the spec is (name format-control format-argument*); format-control
;; is a format control evaluated at print time, so the restart's :report must
;; be a function (a bare symbol/variable there would be read as a function
;; name) that formats the control with its arguments.
(defmacro with-simple-restart (restart-spec &rest body)
  (let ((name (car restart-spec))
        (format-control (cadr restart-spec))
        (format-args (cddr restart-spec))
        (s (gensym "STREAM")))
    `(restart-case (progn ,@body)
       (,name ()
         :report (lambda (,s) (format ,s ,format-control ,@format-args))
         (values nil t)))))

;; restart-bind — like restart-case but the body is *not* wrapped in a
;; non-local exit; if a restart handler is invoked it is called as a regular
;; function and execution continues after restart-bind returns its value.
;; AmiLisp has no interactive debugger driver so we cannot really invoke
;; user-provided restart handlers; the pragmatic stub just runs the body.
(defmacro restart-bind (bindings &rest body)
  (declare (ignore bindings))
  `(progn ,@body))

;; cerror — continuable error: establish CONTINUE restart around error.
;; The CONTINUE restart's report formats format-control with args at print
;; time, so it must be a report function (a bare variable would be read as a
;; function name by restart-case).
(defun cerror (format-control datum &rest args)
  (restart-case (apply #'error datum args)
    (continue () :report (lambda (s) (apply #'format s format-control args)) nil)))

;; %set-condition-default-initargs is a AMILISP builtin (builtins_condition.c):
;; it records a condition type's :default-initargs so make-condition / error /
;; signal can fill in initargs the caller omitted.

;; define-condition — define a user condition type with slots and readers
(defmacro define-condition (name parent-types slot-specs &rest options)
  ;; A slot-specifier may be a bare symbol (a slot name with no options) or a
  ;; list (slot-name . options) — see CLHS DEFINE-CONDITION / DEFCLASS.
  ;; Normalize the bare-symbol form to (slot-name) so the CAR/CDR slot walks
  ;; below stay valid (otherwise (car 'name) signals a TYPE-ERROR).
  (let* ((slot-specs (mapcar (lambda (spec) (if (consp spec) spec (list spec)))
                             slot-specs))
        (parents-list (cond ((consp parent-types) parent-types)
                            ((null parent-types) '(condition))
                            (t (list parent-types))))
        (slot-pairs (mapcar (lambda (spec) (cons (car spec) (getf (cdr spec) :initarg))) slot-specs))
        ;; For each slot that declares an :initform, build a registration spec
        ;; (slot-name initarg . thunk) so make-condition can evaluate the form
        ;; per instance for any slot the caller does not supply.  Slots whose
        ;; initform is the literal NIL are skipped — an absent condition slot
        ;; already reads as NIL, so there is nothing to install.
        (initform-spec-forms
         (let ((acc nil))
           (dolist (spec slot-specs)
             (let* ((opts (cdr spec))
                    (tail (member :initform opts)))
               (when (and tail (cadr tail))
                 (push `(list ',(car spec)
                              ',(getf opts :initarg)
                              (lambda () ,(cadr tail)))
                       acc))))
           (nreverse acc)))
        (report nil)
        (default-initargs nil)
        (documentation nil)
        (clos-available (fboundp '%add-method-to-gf)))
    ;; Parse options
    (dolist (opt options)
      (case (car opt)
        (:report (setq report (cadr opt)))
        (:default-initargs (setq default-initargs (cdr opt)))
        (:documentation (setq documentation (cadr opt)))))
    `(progn
       ;; Register the FULL parent list so condition type membership
       ;; (TYPEP / SUBTYPEP / handler-case) sees every superclass, including
       ;; ones where ERROR is not the first parent (e.g. usocket's
       ;; (define-condition socket-error (socket-condition error))).
       (%register-condition-type ',name ',parents-list ',slot-pairs)
       ;; (:documentation "...") -> the TYPE doc-type, as for DEFCLASS.
       ,@(when (stringp documentation)
           `((%set-documentation ',name 'type ,documentation)))
       (when (fboundp '%register-condition-class)
         (%register-condition-class ',name ',parents-list))
       ,@(mapcan (lambda (slot-spec)
                   (let* ((slot-name (car slot-spec))
                          (opts (cdr slot-spec))
                          (reader (or (getf opts :reader)
                                      (getf opts :accessor)))
                          (accessor (getf opts :accessor)))
                     (append
                       (when reader
                         (if clos-available
                             (list `(defmethod ,reader ((c ,name)) (condition-slot-value c ',slot-name)))
                             (list `(defun ,reader (c) (condition-slot-value c ',slot-name)))))
                       (when accessor
                         (let ((setter-name (amilisp::%setf-store-symbol accessor)))
                           (list `(defun ,setter-name (val c)
                                    (%set-condition-slot-value c ',slot-name val))))))))
                 slot-specs)
       ,@(when report
           (cond
             ((stringp report)
              `((defmethod print-object ((c ,name) stream)
                  (write-string ,report stream))))
             ;; A SYMBOL :report arg names a function of (condition stream)
             ;; (CLHS define-condition).  Splicing it bare into funcall's first
             ;; (evaluated) slot would read it as a VARIABLE — "Unbound variable:
             ;; FOO" — so quote it as a function designator.  A lambda
             ;; expression already evaluates to a function, so pass it through.
             ((symbolp report)
              `((defmethod print-object ((c ,name) stream)
                  (funcall ',report c stream))))
             (t
              `((defmethod print-object ((c ,name) stream)
                  (funcall ,report c stream))))))
       ,@(when default-initargs
           `((%set-condition-default-initargs ',name ',default-initargs)))
       ,@(when (and initform-spec-forms (fboundp '%register-condition-slot-initforms))
           `((%register-condition-slot-initforms ',name (list ,@initform-spec-forms))))
       ',name)))

;; check-type — signal type-error if place is not of type
(defmacro check-type (place type &optional type-string)
  (let ((val (gensym)))
    `(let ((,val ,place))
       (unless (typep ,val ',type)
         (error 'type-error :datum ,val :expected-type ',type)))))

;; KEYPLACE is settable (a place we can SETF) when it is a non-constant symbol
;; or a non-QUOTE compound form.  Self-evaluating literals (numbers, strings,
;; T/NIL/keywords) and (quote ...) forms are NOT places — CCASE/CTYPECASE are
;; sometimes called with such a bare keyFORM (e.g. lparallel), and emitting a
;; (setf <literal> ...) for them would fail to COMPILE even though that branch
;; is never reached at runtime.  For those we still offer the STORE-VALUE retry,
;; but it only updates the loop's local copy, not the (non-existent) place.
(defun %ccase-place-p (keyplace)
  (cond ((symbolp keyplace)
         (not (or (null keyplace) (eq keyplace t) (keywordp keyplace))))
        ((consp keyplace) (not (eq (car keyplace) 'quote)))
        (t nil)))

;; ccase — like ecase, but the type-error it signals when no clause matches is
;; *continuable* (CLHS 5.3): it establishes a STORE-VALUE restart that stores a
;; new value into KEYPLACE and retries from the top.  The current value lives in
;; a local the loop re-tests; the STORE-VALUE handler updates that local (and,
;; when KEYPLACE is a real place, the place too).  The handler body is compiled
;; as a closure, so the boxing analysis must — and now does — share the local
;; with the re-test, otherwise the retry would loop forever.
(defmacro ccase (keyplace &rest clauses)
  (let ((blk (gensym "CCASE"))
        (g (gensym "VAL"))
        (nv (gensym "NEWVAL"))
        (place-p (%ccase-place-p keyplace))
        (keys '()))
    (dolist (clause clauses)
      (let ((k (car clause)))
        (if (listp k) (setq keys (append keys k))
            (setq keys (append keys (list k))))))
    `(let ((,g ,keyplace))
       (block ,blk
         (loop
           (cond
             ,@(mapcar
                (lambda (clause)
                  (let ((k (car clause)) (body (cdr clause)))
                    (if (listp k)
                        `((member ,g ',k) (return-from ,blk (progn ,@body)))
                        `((eql ,g ',k) (return-from ,blk (progn ,@body))))))
                clauses)
             (t (restart-case
                  (error 'type-error :datum ,g :expected-type '(member ,@keys))
                  (store-value (,nv)
                    :report "Supply a new value for the place and retry."
                    :interactive (lambda () (list (eval (read))))
                    ,@(when place-p `((setf ,keyplace ,nv)))
                    (setq ,g ,nv))))))))))

;; ctypecase — same relationship to etypecase: a continuable type-error with a
;; STORE-VALUE restart that stores into KEYPLACE and retries.
(defmacro ctypecase (keyplace &rest clauses)
  (let ((blk (gensym "CTYPECASE"))
        (g (gensym "VAL"))
        (nv (gensym "NEWVAL"))
        (place-p (%ccase-place-p keyplace))
        (types '()))
    (dolist (clause clauses)
      (setq types (append types (list (car clause)))))
    `(let ((,g ,keyplace))
       (block ,blk
         (loop
           (cond
             ,@(mapcar
                (lambda (clause)
                  `((typep ,g ',(car clause))
                    (return-from ,blk (progn ,@(cdr clause)))))
                clauses)
             (t (restart-case
                  (error 'type-error :datum ,g
                         :expected-type '(or ,@types))
                  (store-value (,nv)
                    :report "Supply a new value for the place and retry."
                    :interactive (lambda () (list (eval (read))))
                    ,@(when place-p `((setf ,keyplace ,nv)))
                    (setq ,g ,nv))))))))))

;; assert — signal error if test-form is false
(defmacro assert (test-form &optional places string &rest args)
  (declare (ignore places))
  (if (and string args)
      `(unless ,test-form
         (error 'simple-error
                :format-control ,string
                :format-arguments (list ,@args)))
      `(unless ,test-form
         (error 'simple-error
                :format-control ,(or string "Assertion failed: ~S")
                :format-arguments (list ',test-form)))))

;; defpackage — define a package with :use, :export, :nicknames, :local-nicknames,
;;   :import-from, :shadow, :shadowing-import-from, :intern options
(defmacro defpackage (name &rest options)
  (let ((pkg-name (if (symbolp name) (symbol-name name) name))
        (uses nil)
        (exports nil)
        (nicknames nil)
        (local-nicks nil)
        (import-froms nil)
        (shadows nil)
        (shadowing-import-froms nil)
        (interns nil))
    ;; Parse options
    (dolist (opt options)
      (case (car opt)
        (:use (setq uses (append uses (cdr opt))))
        (:export (setq exports (append exports (cdr opt))))
        (:nicknames (setq nicknames (append nicknames (cdr opt))))
        (:local-nicknames (setq local-nicks (cdr opt)))
        (:import-from (setq import-froms (cons (cdr opt) import-froms)))
        (:shadow (setq shadows (append shadows (cdr opt))))
        (:shadowing-import-from (setq shadowing-import-froms (cons (cdr opt) shadowing-import-froms)))
        (:intern (setq interns (append interns (cdr opt))))))
    `(eval-when (:compile-toplevel :load-toplevel :execute)
       (let ((pkg (or (find-package ,pkg-name)
                      (make-package ,pkg-name :nicknames ',nicknames))))
         ,@(when shadows
             `((dolist (s ',shadows)
                 (shadow (if (symbolp s) (symbol-name s) s) pkg))))
         ,@(when uses
             `((dolist (u ',uses)
                 (use-package (or (find-package (if (symbolp u) (symbol-name u) u))
                                  (error "Package ~A not found" u))
                              pkg))))
         ,@(mapcar (lambda (import-spec)
                     (let ((from-pkg (car import-spec))
                           (syms (cdr import-spec)))
                       `(let ((src-pkg (or (find-package ,(if (symbolp from-pkg)
                                                             (symbol-name from-pkg)
                                                             from-pkg))
                                           (error "Package ~A not found" ',from-pkg))))
                          (dolist (s ',syms)
                            (let ((sym (find-symbol (if (symbolp s) (symbol-name s) s) src-pkg)))
                              (when sym (import sym pkg)))))))
                   import-froms)
         ,@(mapcar (lambda (import-spec)
                     (let ((from-pkg (car import-spec))
                           (syms (cdr import-spec)))
                       `(let ((src-pkg (or (find-package ,(if (symbolp from-pkg)
                                                             (symbol-name from-pkg)
                                                             from-pkg))
                                           (error "Package ~A not found" ',from-pkg))))
                          (dolist (s ',syms)
                            (let ((sym (find-symbol (if (symbolp s) (symbol-name s) s) src-pkg)))
                              (when sym (shadowing-import sym pkg)))))))
                   shadowing-import-froms)
         ,@(when interns
             `((dolist (s ',interns)
                 (intern (if (symbolp s) (symbol-name s) s) pkg))))
         ,@(when exports
             `((dolist (e ',exports)
                 (export (intern (if (symbolp e) (symbol-name e) e) pkg) pkg))))
         ,@(when local-nicks
             `((dolist (ln ',local-nicks)
                 (add-package-local-nickname
                  (car ln)
                  (or (find-package (if (symbolp (cadr ln)) (symbol-name (cadr ln)) (cadr ln)))
                      (error "Package ~A not found for local nickname" (cadr ln)))
                  pkg))))
         pkg))))

;; do-symbols — iterate over the symbols ACCESSIBLE in a package (CLHS
;; 11.1.1.2.1): the ones present in it plus the exported symbols of every
;; package it uses.  %PACKAGE-SYMBOLS alone is the present set; the
;; inherited ones are what completion and APROPOS in a user package are
;; mostly about (MAPCAR from CL-USER).  A symbol reachable both ways may
;; be seen twice, which the standard allows.
(defun %accessible-symbols (pkg)
  (let ((inherited nil))
    (dolist (used (package-use-list pkg))
      (setq inherited (append (%package-external-symbols used) inherited)))
    (append (%package-symbols pkg) inherited)))

(defmacro do-symbols (spec &body body)
  (let ((var (car spec))
        (package (cadr spec))
        (result (caddr spec))
        (pkg (gensym)) (syms (gensym)) (s (gensym)))
    `(let* ((,pkg ,(if package `(find-package ,package) '*package*))
            (,syms (%accessible-symbols ,pkg)))
       (dolist (,s ,syms ,result)
         (let ((,var ,s))
           ,@body)))))

;; do-external-symbols — iterate over exported symbols in a package
(defmacro do-external-symbols (spec &body body)
  (let ((var (car spec))
        (package (cadr spec))
        (result (caddr spec))
        (pkg (gensym)) (syms (gensym)) (s (gensym)))
    `(let* ((,pkg ,(if package `(find-package ,package) '*package*))
            (,syms (%package-external-symbols ,pkg)))
       (dolist (,s ,syms ,result)
         (let ((,var ,s))
           ,@body)))))

;; do-all-symbols — iterate over the symbols of all registered packages.
;; CLHS: spec is (var [result-form]) — no package argument.  A symbol that is
;; present in more than one package is processed only once (we track visited
;; symbols by identity); result-form is evaluated with var bound to NIL.
(defmacro do-all-symbols (spec &body body)
  ;; Use tagbody/go instead of dolist so the inner loops don't create their
  ;; own implicit (block nil ...) that would intercept (return ...) from body.
  (let ((var (car spec))
        (result (cadr spec))
        (pkgs (gensym)) (pkg (gensym))
        (syms (gensym)) (s (gensym))
        (seen (gensym))
        (pkg-loop (gensym)) (sym-loop (gensym)) (done (gensym)))
    `(block nil
       (let ((,pkgs (list-all-packages))
             (,pkg nil)
             (,syms nil)
             (,s nil)
             (,seen (make-hash-table :test 'eq)))
         (tagbody
          ,pkg-loop
          (when (null ,pkgs) (go ,done))
          (setq ,pkg (car ,pkgs))
          (setq ,pkgs (cdr ,pkgs))
          (setq ,syms (%package-symbols ,pkg))
          ,sym-loop
          (when (null ,syms) (go ,pkg-loop))
          (setq ,s (car ,syms))
          (setq ,syms (cdr ,syms))
          (unless (gethash ,s ,seen)
            (setf (gethash ,s ,seen) t)
            (let ((,var ,s))
              ,@body))
          (go ,sym-loop)
          ,done))
       (let ((,var nil)) ,result))))

;; apropos / apropos-list (CLHS 25.1.3).  A case-insensitive substring
;; match on the symbol name over the symbols accessible in PACKAGE, or over
;; every package when none is given.  APROPOS-LIST returns a fresh list,
;; sorted by name and free of duplicates, so an editor can show it as it
;; is; APROPOS prints one symbol per line with what the symbol names, and
;; returns no values.  See tests/test_documentation.c.
(defun %apropos-package (designator)
  (or (find-package designator)
      (error "APROPOS: no such package: ~a" designator)))

(defun apropos-list (string &optional package)
  (let ((string (string string))
        (result nil))
    (if package
        (do-symbols (s (%apropos-package package))
          (when (search string (symbol-name s) :test #'char-equal)
            (push s result)))
        (do-all-symbols (s)
          (when (search string (symbol-name s) :test #'char-equal)
            (push s result))))
    (sort (remove-duplicates result :test #'eq) #'string< :key #'symbol-name)))

(defun apropos (string &optional package)
  (dolist (s (apropos-list string package))
    (fresh-line)
    (prin1 s)
    (cond ((special-operator-p s) (write-string "  [special operator]"))
          ((macro-function s) (write-string "  [macro]"))
          ((fboundp s) (write-string "  [function]")))
    (when (boundp s) (write-string "  [bound]")))
  (fresh-line)
  (values))

;; defstruct — define a named structure type
;; Supports options: :conc-name, :constructor, :predicate, :copier, :include
(defun %defstruct-parse-slot (spec)
  "Parse a slot spec into (name default). Accepts NAME or (NAME DEFAULT)."
  (if (consp spec)
      (list (car spec) (cadr spec))
      (list spec nil)))

(defun %boa-patch-defaults (boa-lambda-list all-slots)
  "Patch &optional and &key params in a BOA lambda list to use slot defaults
when the param has no explicit default.  CL spec 3.4.6 requires this."
  (let ((state :required)
        (result nil))
    (dolist (p boa-lambda-list)
      (cond
        ((member p '(&optional &key &rest &aux &allow-other-keys))
         (setq state p)
         (push p result))
        ((or (eq state :required) (eq state '&rest))
         (push p result))
        ((or (eq state '&optional) (eq state '&key))
         (if (and (symbolp p)
                  ;; bare symbol &optional/&key param — inject slot default
                  (not (member p '(&optional &key &rest &aux &allow-other-keys))))
             (let ((slot (assoc p all-slots)))
               (if (and slot (cadr slot))
                   ;; Slot has a default form, wrap param with it
                   (push (list p (cadr slot)) result)
                   (push p result)))
             ;; Already a list (has explicit default) — keep as-is
             (push p result)))
        (t (push p result))))
    (reverse result)))

;; Compiler-macro expander shared by every DEFSTRUCT keyword constructor
;; (specs/performance.md 4.2 item 6).  A call whose arguments are literal
;; keywords naming slots — (make-item :message m :time-out 5) — becomes a
;; positional (%make-struct 'name v0 v1 ...): no &key parse in OP_CALL, no
;; wrapper frame.  Evaluation order follows the keyword call exactly: the
;; supplied argument forms left to right (bound to temporaries unless they
;; are constants), then the init-forms of the unsupplied slots in slot order
;; (CLHS 3.4.1.4).  The expander DECLINES — returns FORM unchanged, so the
;; ordinary keyword call is compiled — whenever the shape is not that simple:
;; a non-keyword or unknown key, a duplicate key (leftmost wins at run time),
;; :allow-other-keys, an odd argument count, or an unsupplied slot whose
;; init-form is not a constant (a non-constant init-form belongs in the
;; constructor's own lexical environment, not the caller's).
(defun %struct-keyword-ctor-expand (form struct-name slot-specs)
  (let* ((args (cdr form))
         (n (length args))
         (slot-keys (mapcar (lambda (s) (intern (symbol-name (car s)) :keyword))
                            slot-specs))
         (supplied nil)              ; alist (slot-key . value-form), call order
         (ok t))
    (if (oddp n)
        form
        (progn
          ;; Validate every key; collect (key . value-form) in call order.
          (do ((a args (cddr a)))
              ((or (null a) (not ok)))
            (let ((k (car a)))
              (cond ((not (keywordp k)) (setq ok nil))
                    ((eq k :allow-other-keys) (setq ok nil))
                    ((not (member k slot-keys)) (setq ok nil))
                    ((assoc k supplied) (setq ok nil))
                    (t (setq supplied (cons (cons k (cadr a)) supplied))))))
          (setq supplied (nreverse supplied))
          ;; Every unsupplied slot needs a constant init-form.
          (when ok
            (do ((keys slot-keys (cdr keys))
                 (specs slot-specs (cdr specs)))
                ((or (null keys) (not ok)))
              (unless (or (assoc (car keys) supplied)
                          (constantp (cadr (car specs))))
                (setq ok nil))))
          (if (not ok)
              form
              ;; Bind non-constant supplied forms to temporaries in call
              ;; order, then build the positional %make-struct call.
              (let ((bindings nil)
                    (values-by-key nil))
                (dolist (pair supplied)
                  (let ((vf (cdr pair)))
                    (if (constantp vf)
                        (setq values-by-key (cons (cons (car pair) vf) values-by-key))
                        (let ((tmp (gensym "CTOR")))
                          (setq bindings (cons (list tmp vf) bindings))
                          (setq values-by-key
                                (cons (cons (car pair) tmp) values-by-key))))))
                ;; The positional call bakes in the slot order and the
                ;; defaults: the code being compiled now depends on this
                ;; layout (the FASL's DEPS trailer records it).
                (amilisp::%note-struct-use struct-name)
                (let ((positional
                        (mapcar (lambda (key spec)
                                  (let ((hit (assoc key values-by-key)))
                                    (if hit (cdr hit) (cadr spec))))
                                slot-keys slot-specs))
                      (call nil))
                  (setq call (cons 'amilisp::%make-struct
                                   (cons (list 'quote struct-name) positional)))
                  (if bindings
                      (list 'let* (nreverse bindings) call)
                      call))))))))

;; The hidden function behind (SETF (acc obj) val) for a structure accessor:
;; AMILISP::%STRUCT-SET-<home package>::<accessor>, the scheme (SETF name)
;; functions use.  It was %SET-<accessor> interned in the current package,
;; i.e. in the USER's namespace: (defstruct view title) silently took over a
;; user function named %SET-VIEW-TITLE - DEFUN order decided whose body won,
;; and the setter's compiler macro rewrote every direct call to the user's
;; function into a raw slot write on its first argument.
(defun amilisp::%struct-setter-name (acc-name)
  (let ((pkg (symbol-package acc-name)))
    (intern (concatenate 'string "%STRUCT-SET-"
                         (if pkg (package-name pkg) "")
                         "::" (symbol-name acc-name))
            "AMILISP")))

(defmacro defstruct (name-and-options &rest slot-specs)
  (let* ((name (if (consp name-and-options) (car name-and-options) name-and-options))
         (options (if (consp name-and-options) (cdr name-and-options) nil))
         (name-str (symbol-name name))
         ;; Parse options
         (conc-name-opt nil) (conc-name-set nil)
         ;; CLHS 3.4.6.6: a struct may declare MANY (:constructor ...) options.
         ;; CONSTRUCTORS collects (NAME HAS-BOA-P BOA-LAMBDA-LIST) per option;
         ;; (:constructor nil) suppresses (adds nothing).
         (constructors nil) (constructor-set nil)
         (predicate-opt nil) (predicate-set nil)
         (copier-opt nil) (copier-set nil)
         (include-name nil) (include-slots nil)
         (type-opt nil)
         (print-function-opt nil) (print-object-opt nil)
         (struct-doc nil))
    ;; Process options
    (dolist (opt options)
      (cond
        ((and (consp opt) (eq (car opt) :conc-name))
         (setq conc-name-set t)
         (setq conc-name-opt (cadr opt)))
        ((eq opt :conc-name)
         (setq conc-name-set t)
         (setq conc-name-opt nil))
        ((and (consp opt) (eq (car opt) :constructor))
         (setq constructor-set t)
         ;; (:constructor name)          => keyword constructor
         ;; (:constructor name arglist)  => BOA constructor
         ;; (:constructor nil)           => suppress (record nothing)
         (when (cadr opt)
           (setq constructors
                 (cons (list (cadr opt) (consp (cddr opt)) (caddr opt))
                       constructors))))
        ((eq opt :constructor)
         (setq constructor-set t))
        ((and (consp opt) (eq (car opt) :predicate))
         (setq predicate-set t)
         (setq predicate-opt (cadr opt)))
        ((eq opt :predicate)
         (setq predicate-set t)
         (setq predicate-opt nil))
        ((and (consp opt) (eq (car opt) :copier))
         (setq copier-set t)
         (setq copier-opt (cadr opt)))
        ((eq opt :copier)
         (setq copier-set t)
         (setq copier-opt nil))
        ((and (consp opt) (eq (car opt) :include))
         (setq include-name (cadr opt)))
        ((and (consp opt) (eq (car opt) :type))
         (setq type-opt (cadr opt)))
        ((and (consp opt) (eq (car opt) :print-function))
         (setq print-function-opt (cadr opt)))
        ((and (consp opt) (eq (car opt) :print-object))
         (setq print-object-opt (cadr opt)))))
    ;; Compute inherited slots from :include (with defaults)
    (when include-name
      ;; The parent's slot specs are copied into this expansion, so the
      ;; file being compiled depends on the parent's layout.
      (amilisp::%note-struct-use include-name)
      (let ((parent-specs (%struct-slot-specs include-name)))
        (dolist (spec parent-specs)
          (push spec include-slots))
        (setq include-slots (reverse include-slots))))
    ;; A leading string is the docstring (CLHS DEFSTRUCT), recorded under
    ;; the STRUCTURE doc-type by the first form of the expansion.
    (when (and slot-specs (stringp (car slot-specs)))
      (setq struct-doc (pop slot-specs)))
    ;; Build full slot list: inherited + own
    (let* ((own-parsed (mapcar #'%defstruct-parse-slot slot-specs))
           (all-slots (append include-slots own-parsed))
           (n-slots (length all-slots))
           (slot-names (mapcar #'car all-slots))
           ;; Compute conc-name prefix (user supplies the full prefix incl. separator)
           (prefix (cond
                     ((not conc-name-set) (concatenate 'string name-str "-"))
                     ((null conc-name-opt) "")
                     (t (symbol-name conc-name-opt))))
           ;; Constructor specs (NAME HAS-BOA-P BOA-LL).  With no (:constructor
           ;; ...) option, default to a single keyword constructor MAKE-<name>.
           (ctor-specs (reverse
                        (if constructor-set
                            constructors
                            (list (list (intern (concatenate 'string "MAKE-" name-str))
                                        nil nil)))))
           ;; Predicate name
           (pred-name (cond
                        ((not predicate-set) (intern (concatenate 'string name-str "-P")))
                        ((null predicate-opt) nil)
                        (t predicate-opt)))
           ;; Copier name
           (copy-name (cond
                        ((not copier-set) (intern (concatenate 'string "COPY-" name-str)))
                        ((null copier-opt) nil)
                        (t copier-opt)))
           ;; Pushed onto and reversed below, so the doc form comes first.
           (forms (when struct-doc
                    (list `(%set-documentation ',name 'structure ,struct-doc)))))
      ;; A typed-sequence struct: (:type vector), (:type (vector ELT [SIZE]))
      ;; — e.g. ironclad's (:type (vector (unsigned-byte 32))) — or (:type
      ;; list) — e.g. rfc2388's (defstruct (header (:type list) ...)).  The
      ;; declared element type/size are advisory and ignored; the value is a
      ;; real CL:VECTOR / CL:LIST so REPLACE/AREF/SVREF/NTH work on it, and
      ;; snooze's (second (parse-header ...)) sees an actual list.
      (if (or (eq type-opt 'vector)
              (eq type-opt 'list)
              (and (consp type-opt) (eq (car type-opt) 'vector)))
          ;; --- (:type vector) / (:type list) mode: represent as a typed
          ;; sequence (CL:VECTOR or CL:LIST), not a real struct ---
          (let* ((seq-kind (cond
                             ((or (eq type-opt 'vector)
                                  (and (consp type-opt)
                                       (eq (car type-opt) 'vector)))
                              'vector)
                             (t 'list)))
                 (builder (if (eq seq-kind 'vector) 'vector 'list)))
            ;; Constructors (one per (:constructor ...) option)
            (dolist (cspec ctor-specs)
              (let ((ctor-name (car cspec))
                    (has-boa (cadr cspec))
                    (boa-lambda-list (caddr cspec)))
                (if has-boa
                    (let ((slot-inits (mapcar (lambda (s)
                                                (let* ((sname (car s))
                                                       (sdefault (cadr s)))
                                                  (if (member sname boa-lambda-list
                                                              :test (lambda (name p)
                                                                      (cond ((symbolp p)
                                                                             (and (not (member p '(&optional &key &rest &aux)))
                                                                                  (eq name p)))
                                                                            ((consp p) (eq name (car p)))
                                                                            (t nil))))
                                                      sname
                                                      sdefault)))
                                              all-slots)))
                      (push `(defun ,ctor-name ,(%boa-patch-defaults boa-lambda-list all-slots)
                               (,builder ,@slot-inits))
                            forms))
                    (let ((key-params (mapcar (lambda (s) (list (car s) (cadr s)))
                                              all-slots)))
                      (push `(defun ,ctor-name (&key ,@key-params)
                               (,builder ,@(mapcar #'car all-slots)))
                            forms)))))
            ;; No predicate or copier for typed-sequence structs — no type
            ;; discrimination.  Accessors index by position (svref / nth).
            (let ((idx 0))
              (dolist (sname slot-names)
                (let* ((acc-name (intern (concatenate 'string prefix (symbol-name sname))))
                       (setter-name (amilisp::%struct-setter-name acc-name))
                       (getter-form (if (eq seq-kind 'vector)
                                        `(svref obj ,idx)
                                        `(nth ,idx obj)))
                       (set-place   (if (eq seq-kind 'vector)
                                        `(svref obj ,idx)
                                        `(nth ,idx obj))))
                  (push `(defun ,acc-name (obj) ,getter-form) forms)
                  (push `(defun ,setter-name (obj val) (setf ,set-place val)) forms)
                  (push `(defsetf ,acc-name ,setter-name) forms))
                (setq idx (+ idx 1))))
            ;; EVAL-WHEN (not PROGN): a top-level DEFSTRUCT must register its
            ;; slots/accessors at COMPILE time so a later same-file DEFSTRUCT
            ;; with (:INCLUDE this) can resolve inherited slots when COMPILE-FILE
            ;; macroexpands it (CLHS 3.2.3.1).  With PROGN the registration only
            ;; ran at load time, so compiled (:include ...) constructors dropped
            ;; their inherited-slot keyword args.
            `(eval-when (:compile-toplevel :load-toplevel :execute)
               ,@(reverse forms) ',name))
        ;; --- Normal struct mode ---
        (progn
          ;; Register the struct type (store slot specs with defaults for :include)
          (push `(%register-struct-type ',name ,n-slots
                   ,(if include-name `',include-name nil)
                   ',all-slots)
                forms)
          ;; Constructors (one per (:constructor ...) option; CLHS 3.4.6.6)
          (dolist (cspec ctor-specs)
            (let ((ctor-name (car cspec))
                  (has-boa (cadr cspec))
                  (boa-lambda-list (caddr cspec)))
              (if has-boa
                  ;; BOA constructor: positional args mapped to slots by name
                  (let ((slot-inits (mapcar (lambda (s)
                                              ;; For each slot, use the BOA param if named,
                                              ;; otherwise use the slot default
                                              (let* ((sname (car s))
                                                     (sdefault (cadr s)))
                                                ;; Check if slot name appears in BOA params
                                                (if (member sname boa-lambda-list
                                                            :test (lambda (name p)
                                                                    (cond ((symbolp p)
                                                                           (and (not (member p '(&optional &key &rest &aux)))
                                                                                (eq name p)))
                                                                          ((consp p) (eq name (car p)))
                                                                          (t nil))))
                                                    sname
                                                    sdefault)))
                                            all-slots)))
                    (push `(defun ,ctor-name ,(%boa-patch-defaults boa-lambda-list all-slots)
                             (%make-struct ',name ,@slot-inits))
                          forms))
                  ;; Standard keyword constructor — plus a compiler macro
                  ;; that turns a literal-keyword call into a positional
                  ;; %make-struct (see %struct-keyword-ctor-expand).
                  (let ((key-params (mapcar (lambda (s)
                                              (list (car s) (cadr s)))
                                            all-slots)))
                    (push `(defun ,ctor-name (&key ,@key-params)
                             (%make-struct ',name ,@(mapcar #'car all-slots)))
                          forms)
                    (push `(define-compiler-macro ,ctor-name (&whole form &rest args)
                             (declare (ignore args))
                             (%struct-keyword-ctor-expand form ',name ',all-slots))
                          forms)))))
          ;; Predicate
          (when pred-name
            (push `(defun ,pred-name (obj) (typep obj ',name))
                  forms))
          ;; Copier
          (when copy-name
            (push `(defun ,copy-name (obj) (%copy-struct obj))
                  forms))
          ;; Accessors and setf writers.  We emit both a defun (so #'line-sx
          ;; works for funcall/mapcar) AND a compiler macro that inlines
          ;; the call into (amilisp::%struct-ref obj <idx>) — which the
          ;; compiler then lowers to OP_STRUCT_REF, skipping the wrapper
          ;; frame entirely on direct calls.  Same for the setter.
          ;; %STRUCT-ACC-FORM builds that form and records that the code
          ;; being compiled depends on this structure's layout.
          (let ((idx 0))
            (dolist (sname slot-names)
              (let* ((acc-name (intern (concatenate 'string prefix (symbol-name sname))))
                     (setter-name (amilisp::%struct-setter-name acc-name)))
                (push `(defun ,acc-name (obj) (amilisp::%struct-ref obj ,idx)) forms)
                (push `(define-compiler-macro ,acc-name (obj)
                         (amilisp::%struct-acc-form ',name ,idx obj))
                      forms)
                (push `(defun ,setter-name (obj val) (amilisp::%struct-set obj ,idx val)) forms)
                (push `(define-compiler-macro ,setter-name (obj val)
                         (amilisp::%struct-acc-form ',name ,idx obj val))
                      forms)
                (push `(defsetf ,acc-name ,setter-name) forms))
              (setq idx (+ idx 1))))
          ;; Register struct as CLOS class if CLOS is loaded.  Prefer
          ;; %register-struct-class (populates slot metaobjects so MOP
          ;; introspection — class-slots/compute-slots — reflects the real
          ;; layout); fall back to the bare class for older bootstraps.
          (push `(cond
                   ((fboundp '%register-struct-class)
                    (%register-struct-class ',name
                      (list (find-class ',(or include-name 'structure-object)))
                      ',slot-names))
                   ((fboundp '%make-bootstrap-class)
                    (%make-bootstrap-class ',name
                      (list (find-class ',(or include-name 'structure-object))))))
                forms)
          ;; :print-function — defmethod print-object calling (fn obj stream depth)
          (when print-function-opt
            (push `(when (fboundp 'print-object)
                     (defmethod print-object ((object ,name) stream)
                       (,print-function-opt object stream 0)))
                  forms))
          ;; :print-object — defmethod print-object calling (fn obj stream)
          (when print-object-opt
            (push `(when (fboundp 'print-object)
                     (defmethod print-object ((object ,name) stream)
                       (,print-object-opt object stream)))
                  forms))
          ;; EVAL-WHEN (not PROGN) — see the typed-struct branch above: the
          ;; struct's %register-struct-type / accessors must take effect at
          ;; COMPILE time so a later same-file (:include this) resolves its
          ;; inherited slots during COMPILE-FILE macroexpansion (CLHS 3.2.3.1).
          `(eval-when (:compile-toplevel :load-toplevel :execute)
             ,@(reverse forms) ',name))))))

;; #S(type slot value ...) structure-literal reader support (CLHS 2.4.8.13).
;; The C reader reads the (type slot value ...) list and calls this helper to
;; build the structure at read time, locating the default keyword constructor
;; MAKE-<type> in the type symbol's home package and applying it.  Slot names
;; are coerced to keywords so both #S(POINT :X 1) and #S(POINT X 1) work.
;;
;; Defined explicitly in the AMILISP package (not the current package, which
;; varies with the load path) so the C reader's cl_intern_in(...,
;; cl_package_amilisp) resolves the SAME symbol regardless of whether boot is
;; loaded from source or FASL.
(defun amilisp::%read-eval-struct (spec)
  (unless (consp spec)
    (error "#S: malformed structure literal ~S" spec))
  (let ((type (car spec))
        (plist (cdr spec)))
    (unless (symbolp type)
      (error "#S: ~S is not a symbol naming a structure type" type))
    (let ((ctor (find-symbol (concatenate 'string "MAKE-" (symbol-name type))
                             (symbol-package type)))
          (args nil))
      (unless (and ctor (fboundp ctor))
        (error "#S: structure type ~S has no default constructor (~A)"
               type (concatenate 'string "MAKE-" (symbol-name type))))
      (do ((p plist (cddr p)))
          ((null p))
        (let ((slot (car p)))
          (unless (and (cdr p) (symbolp slot))
            (error "#S: malformed slot/value plist in ~S" spec))
          (push (intern (symbol-name slot) :keyword) args)
          (push (cadr p) args)))
      (apply ctor (nreverse args)))))

;; CL bitwise functions not in C builtins
(defun logandc1 (integer-1 integer-2) (logand (lognot integer-1) integer-2))
(defun logandc2 (integer-1 integer-2) (logand integer-1 (lognot integer-2)))
(defun logorc1  (integer-1 integer-2) (logior (lognot integer-1) integer-2))
(defun logorc2  (integer-1 integer-2) (logior integer-1 (lognot integer-2)))
(defun lognand  (integer-1 integer-2) (lognot (logand integer-1 integer-2)))
(defun lognor   (integer-1 integer-2) (lognot (logior integer-1 integer-2)))
(defun logeqv   (&rest integers)
  (dolist (x integers)
    (unless (integerp x) (error 'type-error :datum x :expected-type 'integer)))
  (if (null integers) -1
    (reduce (lambda (a b) (lognot (logxor a b))) integers)))

;; psetq — parallel setq: evaluate all values, then assign
(defmacro psetq (&rest pairs)
  (let ((temps nil) (sets nil) (p pairs))
    (do () ((null p))
      (let ((var (car p))
            (val (cadr p))
            (tmp (gensym)))
        (push (list tmp val) temps)
        (push `(setq ,var ,tmp) sets)
        (setq p (cddr p))))
    `(let ,(reverse temps) ,@(reverse sets) nil)))

;; psetf — parallel setf: evaluate all values AND place subforms, then assign.
;; For compound places like (cdr x), captures x before any assignments so
;; that cross-referencing pairs work correctly.
(defmacro psetf (&rest pairs)
  (let ((bindings nil) (sets nil) (p pairs))
    (do () ((null p))
      (let ((place (car p))
            (val (cadr p))
            (val-tmp (gensym "PVAL")))
        (push (list val-tmp val) bindings)
        (if (symbolp place)
            ;; Simple variable — just assign the temp
            (push `(setf ,place ,val-tmp) sets)
            ;; Compound place — capture subforms in temps
            (let ((head (car place))
                  (args (cdr place))
                  (arg-temps nil))
              (dolist (arg args)
                (let ((at (gensym "PARG")))
                  (push (list at arg) bindings)
                  (push at arg-temps)))
              (push `(setf (,head ,@(reverse arg-temps)) ,val-tmp) sets)))
        (setq p (cddr p))))
    `(let ,(reverse bindings) ,@(reverse sets) nil)))

;; read-from-string (CLHS): reads from the substring bounded by START/END and
;; returns two values — the object read and the index of the first character
;; not read (uiop/asdf rely on both the keyword arguments and the position).
;; EOF-ERROR-P defaults to T per the spec.  Per CLHS 23.1.2, plain READ (the
;; PRESERVE-WHITESPACE-NIL default) consumes the single whitespace character
;; that terminated the token's lookahead, while READ-PRESERVING-WHITESPACE
;; leaves it unread — so (read-from-string "1 3 5") reads "1" and reports
;; position 2 (past the trailing space), not 1.
(defun read-from-string (string &optional (eof-error-p t) eof-value
                                &key (start 0) end preserve-whitespace)
  (let ((s (make-string-input-stream string start end)))
    (let ((obj (read s eof-error-p eof-value)))
      (unless preserve-whitespace
        (let ((ch (peek-char nil s nil nil)))
          (when (and ch (member ch '(#\Space #\Tab #\Newline #\Return #\Page)))
            (read-char s nil nil))))
      (values obj (file-position s)))))

;; read-preserving-whitespace — same as read for our implementation
(defun read-preserving-whitespace (&optional stream (eof-error-p t) eof-value recursive-p)
  (declare (ignore recursive-p))
  (read stream eof-error-p eof-value))

;; break — enter debugger with CONTINUE restart
(defun break (&optional (format-control "Break"))
  (restart-case
    (invoke-debugger (make-condition 'simple-condition
                                     :format-control format-control))
    (continue () nil)))

;; --- String stream macros ---

;; with-output-to-string (var &optional string-form &key element-type) decl* form*
;; (CLHS).  With no STRING-FORM, returns the accumulated string.  When
;; STRING-FORM is supplied it must be a string with a fill pointer: output is
;; appended to it (via vector-push-extend) and the form returns the value of
;; the body, not the string.
(defmacro with-output-to-string (spec &body body)
  (let* ((var (car spec))
         (rest (cdr spec))
         ;; STRING-FORM is the first optional, unless it's a keyword (then only
         ;; keyword options were supplied and there is no target string).
         (string-form (and rest (not (keywordp (car rest))) (car rest))))
    (if string-form
        (let ((tmp (gensym "TMP")) (i (gensym "I"))
              (target (gensym "TARGET")))
          `(let* ((,target ,string-form)
                  (,var (make-string-output-stream)))
             (unwind-protect
               (multiple-value-prog1
                 (locally ,@body)
                 (let ((,tmp (get-output-stream-string ,var)))
                   (dotimes (,i (length ,tmp))
                     (vector-push-extend (char ,tmp ,i) ,target))))
               (close ,var))))
        `(let ((,var (make-string-output-stream)))
           (unwind-protect
             (locally ,@body (get-output-stream-string ,var))
             (close ,var))))))

;; formatter (CLHS 22.3.6.4): (formatter control-string) => function.
;; The function writes using control-string and returns the unconsumed arg
;; tail, so ~@? can advance the parent arg pointer by the right count.
(defmacro formatter (control-string)
  (let ((stream (gensym "STREAM"))
        (args (gensym "ARGS")))
    `(lambda (,stream &rest ,args)
       (amilisp::%formatter-inner ,stream ,control-string ,args))))

;; with-input-from-string (var string &key index start end) decl* form*
;; (CLHS).  START/END bound the substring made available through the stream;
;; INDEX, if supplied, is a place set on exit (normal or non-local) to the
;; index in STRING at which the stream stopped reading — the same value
;; PARSE-INTEGER/PARSE-FLOAT return as their second value.
(defmacro with-input-from-string (spec &body body)
  (let* ((var (car spec))
         (string (cadr spec))
         (opts (cddr spec))
         (index (getf opts :index))
         (start (getf opts :start 0))
         (end (getf opts :end)))
    `(let ((,var (make-string-input-stream ,string ,start ,end)))
       (unwind-protect
         (locally ,@body)
         ,@(when index `((setf ,index (file-position ,var))))
         (close ,var)))))

;; with-open-file — open a file stream, execute body, ensure close
;; with-open-file defined earlier in boot.lisp (before compile-file)

;; --- Stream output control ---
;; No-ops: console/file output is unbuffered, socket send() is immediate.

;; finish-output, force-output, and clear-output are C builtins — do not redefine here

;; --- Byte and sequence I/O ---
;; read-byte and write-byte are C builtins (builtins_stream.c)
;; They use raw byte I/O without UTF-8 encoding/decoding.

;; read-sequence and write-sequence are C builtins (builtins_sequence.c):
;; per-element VM loops here made bulk file I/O cost a VM round-trip per
;; byte.  The builtins fast-path (unsigned-byte 8) vectors on file streams
;; (chunked platform reads/writes) and fall back to per-element C loops for
;; every other sequence/stream combination.

;; with-standard-io-syntax — bind the reader/printer control variables to
;; their standard values (CLHS "with-standard-io-syntax").  Binding the full
;; set matters: e.g. alexandria's FORMAT-SYMBOL wraps its FORMAT in this macro
;; precisely so an outer *PRINT-CASE* of :DOWNCASE/:CAPITALIZE cannot change
;; the symbol name it builds.  Only the control variables implemented in this
;; runtime are bound.
(defmacro with-standard-io-syntax (&body body)
  `(let ((*package* (find-package "CL-USER"))
         (*print-base* 10)
         (*print-radix* nil)
         (*print-case* :upcase)
         (*print-circle* nil)
         (*print-escape* t)
         (*print-gensym* t)
         (*print-level* nil)
         (*print-length* nil)
         (*print-readably* t)
         (*read-base* 10)
         (*read-default-float-format* 'single-float)
         (*read-eval* t))
     ,@body))

;; --- Printer-to-string functions ---

(defun prin1-to-string (object)
  (with-output-to-string (s) (prin1 object s)))

(defun princ-to-string (object)
  (with-output-to-string (s) (princ object s)))


;; --- Time functions (Step 10) ---

(defun decode-universal-time (universal-time &optional time-zone)
  "Decode universal time into 9 values: sec min hour date month year dow dst tz"
  (let* ((tz (or time-zone 0))
         (adjusted (- universal-time (* tz 3600)))
         ;; Seconds within the day
         (day-seconds (mod adjusted 86400))
         (total-days (truncate adjusted 86400))
         ;; Time components
         (second (mod day-seconds 60))
         (minute (mod (truncate day-seconds 60) 60))
         (hour (truncate day-seconds 3600))
         ;; Date computation — days since 1900-01-01 (Monday)
         ;; Day of week: 0=Monday, 6=Sunday
         (dow (mod total-days 7))
         ;; Year calculation with leap year handling
         (remaining-days total-days)
         (year 1900)
         (leap nil)
         (month 1)
         (date 1))
    ;; Find year using tagbody/go
    (block find-year
      (tagbody
       year-loop
        (setq leap (and (zerop (mod year 4))
                        (or (not (zerop (mod year 100)))
                            (zerop (mod year 400)))))
        (let ((days-in-year (if leap 366 365)))
          (when (< remaining-days days-in-year)
            (return-from find-year))
          (setq remaining-days (- remaining-days days-in-year))
          (setq year (+ year 1)))
        (go year-loop)))
    ;; Find month (remaining-days is 0-based day within year)
    (let ((month-days (if leap
                          '(31 29 31 30 31 30 31 31 30 31 30 31)
                          '(31 28 31 30 31 30 31 31 30 31 30 31))))
      (dolist (md month-days)
        (when (< remaining-days md)
          (setq date (+ remaining-days 1))
          (return))
        (setq remaining-days (- remaining-days md))
        (setq month (+ month 1))))
    (values second minute hour date month year dow nil tz)))

(defun encode-universal-time (second minute hour date month year &optional time-zone)
  "Encode time components into universal time."
  (let* ((tz (or time-zone 0))
         ;; Count days from 1900-01-01 to year-01-01
         (y (- year 1900))
         (total-days (+ (* y 365)
                        (truncate (+ y 3) 4)        ; leap years (every 4)
                        (- (truncate (+ y 99) 100)) ; minus century years
                        (truncate (+ y 299) 400)))   ; plus 400-year cycles
         ;; Add days for months within the year
         (leap (and (zerop (mod year 4))
                    (or (not (zerop (mod year 100)))
                        (zerop (mod year 400)))))
         (month-days (if leap
                         '(0 31 60 91 121 152 182 213 244 274 305 335)
                         '(0 31 59 90 120 151 181 212 243 273 304 334))))
    (setq total-days (+ total-days (nth (- month 1) month-days) (- date 1)))
    (+ (* total-days 86400) (* hour 3600) (* minute 60) second (* tz 3600))))

(defun get-decoded-time ()
  "Return the current time decoded into 9 values."
  (decode-universal-time (get-universal-time)))

;; --- Phase 8 Step 4: Higher-order functions ---
(defun complement (fn)
  "Return a function that returns the opposite of FN."
  (lambda (&rest args) (not (apply fn args))))

(defun constantly (value)
  "Return a function that always returns VALUE."
  (lambda (&rest args) value))

;; --- Phase 8 Step 1: Missing list operations ---

(defun tree-equal (a b &key (test #'eql test-supplied-p)
                              (test-not nil test-not-supplied-p))
  "Compare two trees recursively using TEST (or TEST-NOT).
Per CLHS, x and y are tree-equal iff: both are conses with tree-equal
car and cdr, OR both are atoms and the test is satisfied."
  (when (and test-supplied-p test-not-supplied-p)
    (error "TREE-EQUAL: cannot supply both :TEST and :TEST-NOT"))
  (let ((pred (if test-not-supplied-p
                  (lambda (x y) (not (funcall test-not x y)))
                  test)))
    (labels ((walk (a b)
               (cond
                 ((and (consp a) (consp b))
                  (and (walk (car a) (car b))
                       (walk (cdr a) (cdr b))))
                 ((or (consp a) (consp b)) nil)
                 (t (and (funcall pred a b) t)))))
      (walk a b))))

(defun list-length (list)
  "Return the length of LIST, or NIL if circular (tortoise-and-hare)."
  (do ((n 0 (+ n 2))
       (slow list (cdr slow))
       (fast list))
      (nil)
    (when (null fast) (return n))
    (setq fast (cdr fast))
    (when (null fast) (return (+ n 1)))
    (setq fast (cdr fast))
    (when (eq slow fast) (return nil))))

(defun tailp (object list)
  "Return true if OBJECT is EQL to LIST or any CDR of LIST."
  (do ((l list (cdr l)))
      ((atom l) (eql object l))
    (when (eql object l) (return t))))

(defun ldiff (list object)
  "Return a copy of LIST up to but not including the tail OBJECT.
If OBJECT is not EQL to any tail of LIST, a copy of LIST is returned --
including any dotted-list terminator (so the result is also dotted)."
  (unless (listp list)
    (error 'type-error :datum list :expected-type 'list))
  (do ((l list (cdr l))
       (result nil))
      (nil)
    (cond ((atom l)
           (return (if (eql l object)
                       (nreverse result)
                       (nreconc result l))))
          ((eql l object)
           (return (nreverse result)))
          (t (push (car l) result)))))

(defun revappend (list tail)
  "Non-destructively reverse LIST and append TAIL."
  (do ((l list (cdr l))
       (result tail (cons (car l) result)))
      ((null l) result)))

(defun nreconc (list tail)
  "Destructively reverse LIST and append TAIL."
  (do ((l list)
       (next nil))
      ((null l) tail)
    (setq next (cdr l))
    (rplacd l tail)
    (setq tail l)
    (setq l next)))

(defun assoc-if (predicate alist &key key)
  "Return first pair in ALIST where PREDICATE is true of the key."
  (dolist (pair alist nil)
    (cond ((null pair))
          ((consp pair)
           (when (funcall predicate
                          (if key (funcall key (car pair)) (car pair)))
             (return pair)))
          (t (error 'type-error :datum pair :expected-type 'list)))))

(defun assoc-if-not (predicate alist &key key)
  "Return first pair in ALIST where PREDICATE is false of the key."
  (dolist (pair alist nil)
    (cond ((null pair))
          ((consp pair)
           (unless (funcall predicate
                            (if key (funcall key (car pair)) (car pair)))
             (return pair)))
          (t (error 'type-error :datum pair :expected-type 'list)))))

(defun rassoc-if (predicate alist &key key)
  "Return first pair in ALIST where PREDICATE is true of the value."
  (dolist (pair alist nil)
    (cond ((null pair))
          ((consp pair)
           (when (funcall predicate
                          (if key (funcall key (cdr pair)) (cdr pair)))
             (return pair)))
          (t (error 'type-error :datum pair :expected-type 'list)))))

(defun rassoc-if-not (predicate alist &key key)
  "Return first pair in ALIST where PREDICATE is false of the value."
  (dolist (pair alist nil)
    (cond ((null pair))
          ((consp pair)
           (unless (funcall predicate
                            (if key (funcall key (cdr pair)) (cdr pair)))
             (return pair)))
          (t (error 'type-error :datum pair :expected-type 'list)))))

;; --- make-sequence ---
(defun %seq-elt-type-is-char (et)
  (and (symbolp et)
       (let ((n (symbol-name et)))
         (or (string= n "CHARACTER") (string= n "BASE-CHAR")
             (string= n "STANDARD-CHAR") (string= n "EXTENDED-CHAR")))))

(defun %seq-elt-type-is-bit (et)
  (and (symbolp et) (string= (symbol-name et) "BIT")))

(defun %seq-len-arg (x)
  "Interpret a length spec element: an integer is a constraint; * or NIL is none."
  (if (integerp x) x nil))

(defun %seq-ctor-info (type)
  "Classify a sequence constructor TYPE.  Returns (values KIND LENGTH ELT-TYPE)
   where KIND is one of :list :cons :null :string :bit-vector :vector, or NIL
   when the type does not denote a constructible sequence.  LENGTH is the
   declared length constraint (an integer) or NIL when unspecified or `*`.
   ELT-TYPE is the declared element type for a specialized :vector (e.g.
   SINGLE-FLOAT from (VECTOR SINGLE-FLOAT)) or NIL for a general vector."
  ;; Resolve a class metaobject down to its proper name.
  (when (and (not (symbolp type)) (not (consp type))
             (typep type 'standard-class))
    (setq type (class-name type)))
  (cond
    ((symbolp type)
     (let ((n (symbol-name type)))
       (cond
         ((string= n "LIST") (values :list nil))
         ((string= n "CONS") (values :cons nil))
         ((string= n "NULL") (values :null nil))
         ((or (string= n "STRING") (string= n "SIMPLE-STRING")
              (string= n "BASE-STRING") (string= n "SIMPLE-BASE-STRING"))
          (values :string nil))
         ((or (string= n "BIT-VECTOR") (string= n "SIMPLE-BIT-VECTOR"))
          (values :bit-vector nil))
         ((or (string= n "VECTOR") (string= n "SIMPLE-VECTOR")
              (string= n "ARRAY") (string= n "SIMPLE-ARRAY"))
          (values :vector nil))
         (t (values nil nil)))))
    ((consp type)
     (let* ((head (car type))
            (hn (and (symbolp head) (symbol-name head)))
            (rest (cdr type)))
       (cond
         ((null hn) (values nil nil))
         ((or (string= hn "STRING") (string= hn "SIMPLE-STRING")
              (string= hn "BASE-STRING") (string= hn "SIMPLE-BASE-STRING"))
          (values :string (%seq-len-arg (and rest (car rest)))))
         ((or (string= hn "BIT-VECTOR") (string= hn "SIMPLE-BIT-VECTOR"))
          (values :bit-vector (%seq-len-arg (and rest (car rest)))))
         ((or (string= hn "VECTOR") (string= hn "SIMPLE-VECTOR"))
          (let ((elt (and rest (car rest)))
                (len (%seq-len-arg (and rest (cdr rest) (cadr rest)))))
            (cond ((%seq-elt-type-is-char elt) (values :string len))
                  ((%seq-elt-type-is-bit elt)  (values :bit-vector len))
                  (t (values :vector len (and elt (not (eq elt '*)) elt))))))
         ((or (string= hn "ARRAY") (string= hn "SIMPLE-ARRAY"))
          (let ((elt (and rest (car rest))))
            (cond ((%seq-elt-type-is-char elt) (values :string nil))
                  ((%seq-elt-type-is-bit elt)  (values :bit-vector nil))
                  (t (values :vector nil (and elt (not (eq elt '*)) elt))))))
         (t (values nil nil)))))
    (t (values nil nil))))

(defun make-sequence (type size &key (initial-element nil ie-p))
  "Create a sequence of the given TYPE and SIZE."
  (multiple-value-bind (kind len elt) (%seq-ctor-info type)
    (when (and len (/= len size))
      (error 'type-error :datum size :expected-type type))
    (case kind
      (:list
       (if ie-p (make-list size :initial-element initial-element)
           (make-list size)))
      (:cons
       (when (zerop size)
         (error 'type-error :datum size :expected-type type))
       (if ie-p (make-list size :initial-element initial-element)
           (make-list size)))
      (:null
       (unless (zerop size)
         (error 'type-error :datum size :expected-type type))
       nil)
      (:string
       (if ie-p (make-string size :initial-element initial-element)
           (make-string size)))
      (:bit-vector
       (make-array size :element-type 'bit
                        :initial-element (if ie-p initial-element 0)))
      (:vector
       ;; Preserve a specialized element type (e.g. (VECTOR SINGLE-FLOAT)) so
       ;; the result reports the right ARRAY-ELEMENT-TYPE (ansi MAKE-SEQUENCE.29).
       (if elt
           (if ie-p (make-array size :element-type elt :initial-element initial-element)
               (make-array size :element-type elt))
           (if ie-p (make-array size :initial-element initial-element)
               (make-array size))))
      (t (error 'type-error :datum type :expected-type 'sequence)))))

;; --- Pathname functions (Step 10) ---
;; Core pathname functions (pathname, pathnamep, parse-namestring, namestring,
;; make-pathname, merge-pathnames, pathname-host/device/directory/name/type/version,
;; file-namestring, directory-namestring, enough-namestring) are C builtins.
;; truename is now a C builtin (resolves symlinks via platform_realpath).

(defun translate-logical-pathname (pathspec &rest keys)
  (declare (ignore keys))
  (pathname pathspec))

(defun logical-pathname-translations (host)
  (declare (ignore host))
  nil)

(defun %ensure-dirs-helper (dir pos len created)
  "Helper for ensure-directories-exist. Walks dir string creating directories."
  (if (>= pos len)
      created
      (let ((next-sep nil))
        (do ((i pos (+ i 1)))
            ((>= i len))
          (when (or (char= (char dir i) #\/)
                    (char= (char dir i) #\:))
            (setq next-sep (+ i 1))
            (return)))
        (if next-sep
            (progn
              (when (char= (char dir (- next-sep 1)) #\/)
                (let ((sub (subseq dir 0 next-sep)))
                  (when (%mkdir sub) (setq created t))))
              (%ensure-dirs-helper dir next-sep len created))
            created))))

(defun ensure-directories-exist (pathname)
  "Create all directories in the path of PATHNAME. Returns pathname and created-p."
  (let* ((dir (directory-namestring pathname))
         (created (%ensure-dirs-helper dir 0 (length dir) nil)))
    (values pathname created)))

;; --- LOOP macro (Phase 8) ---

(defun %loop-keyword-p (sym name)
  "Check if SYM is a symbol whose name matches NAME."
  (and (symbolp sym)
       (string= (symbol-name sym) name)))

(defun %loop-keyword-sym-p (sym)
  "Check if SYM is any recognized loop keyword."
  (and (symbolp sym)
       (let ((n (symbol-name sym)))
         (or (string= n "FOR") (string= n "AS")
             (string= n "WITH") (string= n "DO") (string= n "DOING")
             (string= n "COLLECT") (string= n "COLLECTING")
             (string= n "APPEND") (string= n "APPENDING")
             (string= n "NCONC") (string= n "NCONCING")
             (string= n "SUM") (string= n "SUMMING")
             (string= n "COUNT") (string= n "COUNTING")
             (string= n "MAXIMIZE") (string= n "MAXIMIZING")
             (string= n "MINIMIZE") (string= n "MINIMIZING")
             (string= n "WHILE") (string= n "UNTIL")
             (string= n "REPEAT")
             (string= n "ALWAYS") (string= n "NEVER") (string= n "THEREIS")
             (string= n "WHEN") (string= n "IF") (string= n "UNLESS")
             (string= n "RETURN")
             (string= n "NAMED") (string= n "INITIALLY") (string= n "FINALLY")
             (string= n "IN") (string= n "ON") (string= n "BY")
             (string= n "FROM") (string= n "DOWNFROM") (string= n "UPFROM")
             (string= n "TO") (string= n "BELOW") (string= n "ABOVE")
             (string= n "DOWNTO") (string= n "UPTO")
             (string= n "ACROSS") (string= n "THEN")
             (string= n "INTO") (string= n "BEING") (string= n "EACH")
             (string= n "THE") (string= n "OF") (string= n "USING")
             (string= n "HASH-KEY") (string= n "HASH-KEYS")
             (string= n "HASH-VALUE") (string= n "HASH-VALUES")
             (string= n "SYMBOL") (string= n "SYMBOLS")
             (string= n "PRESENT-SYMBOL") (string= n "PRESENT-SYMBOLS")
             (string= n "EXTERNAL-SYMBOL") (string= n "EXTERNAL-SYMBOLS")
             (string= n "AND") (string= n "ELSE") (string= n "END")))))

(defun %loop-simple-type-spec-p (x)
  "CLHS 6.1.1.7: a bare (no OF-TYPE) LOOP type-spec must be one of the
simple type specifiers FIXNUM, FLOAT, T or NIL."
  (and (symbolp x)
       (let ((n (symbol-name x)))
         (or (string= n "FIXNUM") (string= n "FLOAT")
             (string= n "T") (string= n "NIL")))))

(defun %loop-skip-type-spec (rest)
  "Consume an optional LOOP type-spec at the head of REST and return the
remaining list.  A type-spec is either OF-TYPE followed by a single
d-type-spec form, or a bare simple-type-spec (FIXNUM/FLOAT/T/NIL).  The
type is purely advisory here, so it is discarded once parsed."
  (cond ((null rest) rest)
        ((%loop-keyword-p (car rest) "OF-TYPE") (cddr rest))
        ((%loop-simple-type-spec-p (car rest)) (cdr rest))
        (t rest)))

(defun %loop-list-accum-p (kn)
  "True if accumulation kind KN builds a list (COLLECT/NCONC/APPEND)."
  (or (string= kn "COLLECT") (string= kn "COLLECTING")
      (string= kn "NCONC") (string= kn "NCONCING")
      (string= kn "APPEND") (string= kn "APPENDING")))

(defun %loop-accum-body (kn expr accum-var &optional tail-var)
  "Generate the body form for accumulation kind KN.

Two strategies for the list accumulators (COLLECT/NCONC/APPEND), both
letting the three be freely mixed into one accumulation (CLHS 6.1.3),
e.g. `(loop ... if x nconc l else collect e)`:

* Anonymous accumulator (TAIL-VAR is NIL): build REVERSED (newest element
  first) and NREVERSE once at loop end -- the cheapest way on a 68020, and
  unobservable because nothing can see the variable before the loop
  returns.  COLLECT pushes one element; NCONC reverse-splices
  destructively (NRECONC -- the argument may be destroyed, like NCONC);
  APPEND reverse-copies (REVAPPEND -- no destruction, like APPEND).  The
  clause parser emits the matching NREVERSE in the result form.

* INTO variable (TAIL-VAR is the hidden tail-pointer gensym): the
  variable is user-visible in the loop body and in FINALLY (CLHS 6.1.3:
  it is \"like a variable established by WITH\"), so it must hold the
  accumulation IN ORDER at every step -- GitHub #19 was `(loop ... nconc
  (list :k e) into acc do (setf (getf acc :k) ...))` seeing a reversed
  plist.  Each clause splices at the tail: COLLECT a fresh cons, NCONC
  the list itself (and re-LASTs it), APPEND a COPY-LIST of it.  No
  epilogue NREVERSE."
  (cond
    ((and tail-var (or (string= kn "COLLECT") (string= kn "COLLECTING")))
     (let ((c (gensym "C")))
       `(let ((,c (list ,expr)))
          (if ,tail-var (rplacd ,tail-var ,c) (setq ,accum-var ,c))
          (setq ,tail-var ,c))))
    ((and tail-var (or (string= kn "APPEND") (string= kn "APPENDING")
                       (string= kn "NCONC") (string= kn "NCONCING")))
     (let ((l (gensym "L")))
       `(let ((,l ,(if (or (string= kn "APPEND") (string= kn "APPENDING"))
                       `(copy-list ,expr)
                       expr)))
          (when ,l
            (if ,tail-var (rplacd ,tail-var ,l) (setq ,accum-var ,l))
            (setq ,tail-var (last ,l))))))
    ((or (string= kn "COLLECT") (string= kn "COLLECTING"))
     `(push ,expr ,accum-var))
    ((or (string= kn "SUM") (string= kn "SUMMING"))
     `(setq ,accum-var (+ ,accum-var ,expr)))
    ((or (string= kn "COUNT") (string= kn "COUNTING"))
     `(when ,expr (setq ,accum-var (+ ,accum-var 1))))
    ((or (string= kn "MAXIMIZE") (string= kn "MAXIMIZING"))
     (let ((tmp (gensym "V")))
       `(let ((,tmp ,expr))
          (when (or (null ,accum-var) (> ,tmp ,accum-var))
            (setq ,accum-var ,tmp)))))
    ((or (string= kn "MINIMIZE") (string= kn "MINIMIZING"))
     (let ((tmp (gensym "V")))
       `(let ((,tmp ,expr))
          (when (or (null ,accum-var) (< ,tmp ,accum-var))
            (setq ,accum-var ,tmp)))))
    ((or (string= kn "APPEND") (string= kn "APPENDING"))
     `(setq ,accum-var (revappend ,expr ,accum-var)))
    (t
     `(setq ,accum-var (nreconc ,expr ,accum-var)))))

(defun %loop-accum-keyword-p (sym)
  "Check if SYM is an accumulation keyword."
  (and (symbolp sym)
       (let ((n (symbol-name sym)))
         (or (string= n "COLLECT") (string= n "COLLECTING")
             (string= n "APPEND") (string= n "APPENDING")
             (string= n "NCONC") (string= n "NCONCING")
             (string= n "SUM") (string= n "SUMMING")
             (string= n "COUNT") (string= n "COUNTING")
             (string= n "MAXIMIZE") (string= n "MAXIMIZING")
             (string= n "MINIMIZE") (string= n "MINIMIZING")))))

(defun %expand-simple-loop (forms)
  "Expand simple (loop body...) into block/tagbody."
  (let ((tag (gensym "LOOP")))
    `(block nil
       (tagbody
         ,tag
         ,@forms
         (go ,tag)))))

(defun %loop-destructure-vars (pattern)
  "Extract all variable names from a destructuring pattern."
  (cond
    ((null pattern) nil)
    ((symbolp pattern) (list pattern))
    ((consp pattern)
     (append (%loop-destructure-vars (car pattern))
             (%loop-destructure-vars (cdr pattern))))
    (t nil)))

(defun %loop-destructure-assigns (pattern source)
  "Generate setq forms to destructure SOURCE into PATTERN variables."
  (cond
    ((null pattern) nil)
    ((symbolp pattern) (list `(setq ,pattern ,source)))
    ((consp pattern)
     (append (%loop-destructure-assigns (car pattern) `(car ,source))
             (%loop-destructure-assigns (cdr pattern) `(cdr ,source))))
    (t nil)))

(defun %loop-destructure-bindings (pattern source)
  "Generate (var init) binding forms to destructure SOURCE into PATTERN variables."
  (cond
    ((null pattern) nil)
    ((symbolp pattern) (list (list pattern source)))
    ((consp pattern)
     (append (%loop-destructure-bindings (car pattern) `(car ,source))
             (%loop-destructure-bindings (cdr pattern) `(cdr ,source))))
    (t nil)))

(defun %loop-all-simple-setq-p (forms)
  "True when every form in FORMS is a plain (SETQ symbol expr)."
  (every (lambda (f)
           (and (consp f) (eq (car f) 'setq)
                (consp (cdr f)) (symbolp (cadr f))
                (consp (cddr f)) (null (cdddr f))))
         forms))

(defun %loop-parallelize-setqs (forms)
  "Turn a list of (SETQ var expr) forms -- the per-iteration steppers of a
group of AND-connected FOR clauses -- into a single form that steps them in
PARALLEL (CLHS 6.1.2.1.4): every step expression is evaluated against the
pre-step values (captured in temporaries) before any variable is assigned.
Returns a one-element list holding that LET form."
  (let* ((temps   (mapcar (lambda (f) (declare (ignore f)) (gensym "PAR")) forms))
         (binds   (mapcar (lambda (tmp f) (list tmp (caddr f))) temps forms))
         (assigns (mapcar (lambda (tmp f) (list 'setq (cadr f) tmp)) temps forms)))
    (list (append (list 'let binds) assigns))))

(defun %loop-build-then-step (flag specs)
  "Build the preamble stepper for a group of FOR var = init THEN step clauses.
SPECS is a list of (var init-expr step-expr).  On the first iteration (FLAG
true) the variables are initialised in order; on subsequent iterations they
are stepped in PARALLEL -- every step expression is read against the previous
values before any variable is assigned (CLHS 6.1.2.1.4).  A single-clause
group reduces to the ordinary sequential init-then-step."
  (let* ((inits (mapcar (lambda (s) (list 'setq (car s) (cadr s))) specs))
         (temps (mapcar (lambda (s) (declare (ignore s)) (gensym "PAR")) specs))
         (tbinds (mapcar (lambda (tmp s) (list tmp (caddr s))) temps specs))
         (tassign (mapcar (lambda (tmp s) (list 'setq (car s) tmp)) temps specs))
         (step-form (if (cdr specs)
                        (append (list 'let tbinds) tassign)   ; parallel
                        (list 'setq (car (car specs)) (caddr (car specs))))))
    `(if ,flag
         (progn ,@inits (setq ,flag nil))
         ,step-form)))

(defun %loop-parse-for (rest var sub-kw end-tag)
  "Parse a FOR/AS clause. Returns (new-rest bindings end-tests pre-body steps)."
  (cond
    ;; FOR var IN list [BY step-fn]
    ((%loop-keyword-p sub-kw "IN")
     (let ((list-expr (car rest))
           (iter-gs (gensym "ITER"))
           (by-fn nil))
       (setq rest (cdr rest))
       (when (and rest (%loop-keyword-p (car rest) "BY"))
         (setq rest (cdr rest))
         (setq by-fn (car rest))
         (setq rest (cdr rest)))
       (list rest
             (list (list iter-gs list-expr) (list var nil))
             (list `(when (endp ,iter-gs) (go ,end-tag)))
             (list `(setq ,var (car ,iter-gs)))
             (list (if by-fn
                       `(setq ,iter-gs (funcall ,by-fn ,iter-gs))
                       `(setq ,iter-gs (cdr ,iter-gs)))))))
    ;; FOR var ON list [BY step-fn]
    ((%loop-keyword-p sub-kw "ON")
     (let ((list-expr (car rest))
           (by-fn nil))
       (setq rest (cdr rest))
       (when (and rest (%loop-keyword-p (car rest) "BY"))
         (setq rest (cdr rest))
         (setq by-fn (car rest))
         (setq rest (cdr rest)))
       (list rest
             (list (list var list-expr))
             ;; CLHS 6.1.2.1.3 (for-as-on-list): the end of the list is tested
             ;; "as if by using atom" -- NOT endp.  This matters when the value
             ;; stepped over is not a proper list (e.g. a dotted list or, as
             ;; some library tests do, a non-list object): atom terminates the
             ;; loop with zero iterations, whereas endp would signal a
             ;; TYPE-ERROR.  (for-as-in-list, above, correctly uses endp.)
             (list `(when (atom ,var) (go ,end-tag)))
             nil
             (list (if by-fn
                       `(setq ,var (funcall ,by-fn ,var))
                       `(setq ,var (cdr ,var)))))))
    ;; FOR var ACROSS vector
    ((%loop-keyword-p sub-kw "ACROSS")
     (let ((vec-expr (car rest))
           (vec-gs (gensym "VEC"))
           (len-gs (gensym "LEN"))
           (idx-gs (gensym "IDX")))
       (setq rest (cdr rest))
       (list rest
             (list (list vec-gs vec-expr) (list len-gs `(length ,vec-gs))
                   (list idx-gs 0) (list var nil))
             (list `(when (>= ,idx-gs ,len-gs) (go ,end-tag)))
             (list `(setq ,var (elt ,vec-gs ,idx-gs)))
             (list `(setq ,idx-gs (+ ,idx-gs 1))))))
    ;; FOR var = expr [THEN step-expr]
    ((and (symbolp sub-kw) (string= (symbol-name sub-kw) "="))
     (let ((init-expr (car rest))
           (step-expr nil)
           (has-then nil))
       (setq rest (cdr rest))
       (when (and rest (%loop-keyword-p (car rest) "THEN"))
         (setq rest (cdr rest))
         (setq step-expr (car rest))
         (setq has-then t)
         (setq rest (cdr rest)))
       (if has-then
           ;; WITH THEN: first iteration uses init-expr, subsequent use
           ;; step-expr — the assignment happens in the PREAMBLE (before the
           ;; body).  We do NOT bake the if-flag form here; instead we hand
           ;; the (var init step) tuple back as the 6th result element so the
           ;; FOR/AND group handler can emit a single shared-flag stepper.
           ;; For a lone clause that stepper is the usual sequential if-flag;
           ;; for AND-connected clauses it steps them in PARALLEL
           ;; (CLHS 6.1.2.1.4).  Bind the variable to NIL up front.
           (list rest
                 (list (list var nil))
                 nil
                 nil
                 nil
                 (list var init-expr step-expr))   ; 6th elt: then-spec
           ;; WITHOUT THEN: re-evaluate expr each iteration
           ;; Assignment in preamble so variable is set before body runs
           (list rest
                 (list (list var nil))
                 (list `(setq ,var ,init-expr))
                 nil
                 nil))))
    ;; FOR var FROM/UPFROM/DOWNFROM start ...
    ((or (%loop-keyword-p sub-kw "FROM")
         (%loop-keyword-p sub-kw "UPFROM")
         (%loop-keyword-p sub-kw "DOWNFROM"))
     (let ((start-expr (car rest))
           (dir (if (%loop-keyword-p sub-kw "DOWNFROM") :down :up))
           (end-expr nil) (end-gs nil) (cmp-fn nil)
           (step-val 1))
       (setq rest (cdr rest))
       (block parse-num-opts
         (tagbody
           num-opt-next
           (when (null rest) (return-from parse-num-opts))
           (cond
             ((or (%loop-keyword-p (car rest) "TO")
                  (%loop-keyword-p (car rest) "UPTO"))
              (setq rest (cdr rest))
              (setq end-expr (car rest))
              (setq rest (cdr rest))
              (setq cmp-fn (if (eq dir :up) '> '<)))
             ((%loop-keyword-p (car rest) "DOWNTO")
              (setq rest (cdr rest))
              (setq end-expr (car rest))
              (setq rest (cdr rest))
              (setq dir :down)
              (setq cmp-fn '<))
             ((%loop-keyword-p (car rest) "BELOW")
              (setq rest (cdr rest))
              (setq end-expr (car rest))
              (setq rest (cdr rest))
              (setq cmp-fn '>=))
             ((%loop-keyword-p (car rest) "ABOVE")
              (setq rest (cdr rest))
              (setq end-expr (car rest))
              (setq rest (cdr rest))
              (setq dir :down)
              (setq cmp-fn '<=))
             ((%loop-keyword-p (car rest) "BY")
              (setq rest (cdr rest))
              (setq step-val (car rest))
              (setq rest (cdr rest)))
             (t (return-from parse-num-opts)))
           (go num-opt-next)))
       (let ((binds (list (list var start-expr)))
             (tests nil))
         (when end-expr
           (setq end-gs (gensym "END"))
           (push (list end-gs end-expr) binds)
           (setq tests (list `(when (,cmp-fn ,var ,end-gs) (go ,end-tag)))))
         (list rest binds tests nil
               (list (if (eq dir :down)
                         `(setq ,var (- ,var ,step-val))
                         `(setq ,var (+ ,var ,step-val))))))))
    ;; FOR var BEING {the|each} {hash-key[s]|hash-value[s]} {of|in} ht [using (...)]
    ;; FOR var BEING {the|each} {symbol[s]|present-symbol[s]|external-symbol[s]} {of|in} pkg
    ((%loop-keyword-p sub-kw "BEING")
     (let ((what nil)
           (using-var nil))
       ;; Skip optional the/each
       (when (and rest (symbolp (car rest))
                  (let ((n (symbol-name (car rest))))
                    (or (string= n "THE") (string= n "EACH"))))
         (setq rest (cdr rest)))
       ;; Parse iteration kind
       (when (null rest) (error "LOOP: missing BEING iteration kind"))
       (let ((n (if (symbolp (car rest)) (symbol-name (car rest)) "")))
         (cond
           ((or (string= n "HASH-KEY") (string= n "HASH-KEYS"))
            (setq what "HASH-KEYS"))
           ((or (string= n "HASH-VALUE") (string= n "HASH-VALUES"))
            (setq what "HASH-VALUES"))
           ((or (string= n "SYMBOL") (string= n "SYMBOLS"))
            (setq what "SYMBOLS"))
           ((or (string= n "PRESENT-SYMBOL") (string= n "PRESENT-SYMBOLS"))
            (setq what "PRESENT-SYMBOLS"))
           ((or (string= n "EXTERNAL-SYMBOL") (string= n "EXTERNAL-SYMBOLS"))
            (setq what "EXTERNAL-SYMBOLS"))
           (t (error "LOOP: unrecognized BEING type ~S" (car rest)))))
       (setq rest (cdr rest))
       ;; Skip of/in
       (when (and rest (symbolp (car rest))
                  (let ((n (symbol-name (car rest))))
                    (or (string= n "OF") (string= n "IN"))))
         (setq rest (cdr rest)))
       ;; Source expression
       (let ((src-expr (car rest)))
         (setq rest (cdr rest))
         ;; Check for USING clause (hash tables only)
         (when (and rest (%loop-keyword-p (car rest) "USING"))
           (setq rest (cdr rest))
           (let ((spec (car rest)))
             (setq rest (cdr rest))
             (setq using-var (cadr spec))))
         ;; Generate expansion
         (cond
           ;; Hash table iteration
           ((or (string= what "HASH-KEYS") (string= what "HASH-VALUES"))
            (let* ((iter-gs (gensym "ITER"))
                   (is-keys (string= what "HASH-KEYS"))
                   (binds (list (list iter-gs `(%hash-table-pairs ,src-expr))
                                (list var nil)))
                   (pre (list `(setq ,var (,(if is-keys 'caar 'cdar) ,iter-gs)))))
              (when using-var
                (setq binds (append binds (list (list using-var nil))))
                (setq pre (append pre
                                  (list `(setq ,using-var
                                               (,(if is-keys 'cdar 'caar) ,iter-gs))))))
              (list rest binds
                    (list `(when (endp ,iter-gs) (go ,end-tag)))
                    pre
                    (list `(setq ,iter-gs (cdr ,iter-gs))))))
           ;; Package symbol iteration
           (t
            (let* ((iter-gs (gensym "ITER"))
                   (fn (if (string= what "EXTERNAL-SYMBOLS")
                           '%package-external-symbols
                           '%package-symbols)))
              (list rest
                    (list (list iter-gs `(,fn (find-package ,src-expr)))
                          (list var nil))
                    (list `(when (endp ,iter-gs) (go ,end-tag)))
                    (list `(setq ,var (car ,iter-gs)))
                    (list `(setq ,iter-gs (cdr ,iter-gs))))))))))
    ;; FOR var BELOW limit — shorthand for FROM 0 BELOW limit
    ((%loop-keyword-p sub-kw "BELOW")
     (let ((end-expr (car rest))
           (end-gs (gensym "END")))
       (setq rest (cdr rest))
       (let ((step-val 1))
         (when (and rest (%loop-keyword-p (car rest) "BY"))
           (setq rest (cdr rest))
           (setq step-val (car rest))
           (setq rest (cdr rest)))
         (list rest
               (list (list var 0) (list end-gs end-expr))
               (list `(when (>= ,var ,end-gs) (go ,end-tag)))
               nil
               (list `(setq ,var (+ ,var ,step-val)))))))
    ;; FOR var ABOVE limit — shorthand for downward from some start
    ((%loop-keyword-p sub-kw "ABOVE")
     (let ((end-expr (car rest))
           (end-gs (gensym "END")))
       (setq rest (cdr rest))
       (let ((step-val 1))
         (when (and rest (%loop-keyword-p (car rest) "BY"))
           (setq rest (cdr rest))
           (setq step-val (car rest))
           (setq rest (cdr rest)))
         (list rest
               (list (list var 0) (list end-gs end-expr))
               (list `(when (<= ,var ,end-gs) (go ,end-tag)))
               nil
               (list `(setq ,var (- ,var ,step-val)))))))
    ;; FOR var TO/UPTO limit — shorthand for FROM 0 TO limit (inclusive)
    ;; Extension: ANSI LOOP (HyperSpec 6.1.2.1.1.2) requires FROM/UPFROM/DOWNFROM as the
    ;; mandatory start keyword; TO/UPTO/DOWNTO as the *first* keyword is an
    ;; implementation-defined extension (also accepted by SBCL and CCL).
    ((or (%loop-keyword-p sub-kw "TO")
         (%loop-keyword-p sub-kw "UPTO"))
     (let ((end-expr (car rest))
           (end-gs (gensym "END")))
       (setq rest (cdr rest))
       (let ((step-val 1))
         (when (and rest (%loop-keyword-p (car rest) "BY"))
           (setq rest (cdr rest))
           (setq step-val (car rest))
           (setq rest (cdr rest)))
         (list rest
               (list (list var 0) (list end-gs end-expr))
               (list `(when (> ,var ,end-gs) (go ,end-tag)))
               nil
               (list `(setq ,var (+ ,var ,step-val)))))))
    ;; FOR var DOWNTO limit — shorthand for FROM 0 DOWNTO limit (inclusive, downward)
    ;; Extension: same implementation-defined extension as TO/UPTO above.
    ((%loop-keyword-p sub-kw "DOWNTO")
     (let ((end-expr (car rest))
           (end-gs (gensym "END")))
       (setq rest (cdr rest))
       (let ((step-val 1))
         (when (and rest (%loop-keyword-p (car rest) "BY"))
           (setq rest (cdr rest))
           (setq step-val (car rest))
           (setq rest (cdr rest)))
         (list rest
               (list (list var 0) (list end-gs end-expr))
               (list `(when (< ,var ,end-gs) (go ,end-tag)))
               nil
               (list `(setq ,var (- ,var ,step-val)))))))
    ;; FOR var OF-TYPE type-spec <sub-clause> — skip type declaration, recurse
    ((%loop-keyword-p sub-kw "OF-TYPE")
     ;; rest = (type-spec real-sub-kw . rest-of-clause)
     ;; Skip the type-spec, delegate to real sub-clause
     (let ((real-sub-kw (cadr rest))
           (real-rest (cddr rest)))
       (%loop-parse-for real-rest var real-sub-kw end-tag)))
    (t
     ;; sub-kw might be an inline type declaration (e.g., FIXNUM in
     ;; "FOR I FIXNUM FROM 0").  Per CL spec, a type-spec can appear
     ;; between the variable and the sub-clause keyword.  Skip it and
     ;; use the next element as the real sub-keyword.
     (let ((next (car rest)))
       (if (and rest (symbolp next)
                (or (%loop-keyword-p next "IN")
                    (%loop-keyword-p next "ON")
                    (%loop-keyword-p next "ACROSS")
                    (%loop-keyword-p next "FROM")
                    (%loop-keyword-p next "UPFROM")
                    (%loop-keyword-p next "DOWNFROM")
                    (%loop-keyword-p next "TO")
                    (%loop-keyword-p next "UPTO")
                    (%loop-keyword-p next "DOWNTO")
                    (%loop-keyword-p next "BELOW")
                    (%loop-keyword-p next "ABOVE")
                    (%loop-keyword-p next "BEING")
                    (and (symbolp next) (string= (symbol-name next) "="))))
           (%loop-parse-for (cdr rest) var next end-tag)
           (error "LOOP: unrecognized FOR sub-clause ~S" sub-kw))))))

(defun %loop-parse-cond-subclause (sub rest block-name
                                    bindings into-vars default-accum
                                    result-form epilogue)
  "Parse one conditional sub-clause. Returns (form rest bindings into-vars
   default-accum result-form epilogue)."
  (let ((form nil))
    (cond
      ;; DO — consume forms until keyword
      ((or (%loop-keyword-p sub "DO") (%loop-keyword-p sub "DOING"))
       (let ((do-forms nil))
         (block consume-cond-do
           (tagbody
             cond-do-next
             (when (null rest) (return-from consume-cond-do))
             (when (%loop-keyword-sym-p (car rest)) (return-from consume-cond-do))
             (push (car rest) do-forms)
             (setq rest (cdr rest))
             (go cond-do-next)))
         (setq form `(progn ,@(nreverse do-forms)))))
      ;; RETURN
      ((%loop-keyword-p sub "RETURN")
       (setq form `(return-from ,block-name ,(car rest)))
       (setq rest (cdr rest)))
      ;; Accumulation
      ((%loop-accum-keyword-p sub)
       (let* ((kn (symbol-name sub))
              (is-list-accum (%loop-list-accum-p kn))
              (is-sum (or (string= kn "SUM") (string= kn "SUMMING")))
              (is-count (or (string= kn "COUNT") (string= kn "COUNTING")))
              (init-val (if (or is-sum is-count) 0 nil))
              (expr (car rest))
              (accum-var nil))
         (setq rest (cdr rest))
         (if (and rest (%loop-keyword-p (car rest) "INTO"))
             (progn
               (setq rest (cdr rest))
               (setq accum-var (car rest))
               (setq rest (cdr rest))
               ;; CLHS 6.1.3.1: INTO var may be followed by OF-TYPE type-spec.
               (when (and rest (%loop-keyword-p (car rest) "OF-TYPE"))
                 (setq rest (cddr rest)))
               ;; INTO-VARS is an alist (var . tail-var); a list
               ;; accumulator gets a hidden tail pointer so the user-visible
               ;; variable stays in order (see %LOOP-ACCUM-BODY).
               (unless (assoc accum-var into-vars)
                 (let ((tail (and is-list-accum (gensym "TAIL"))))
                   (push (cons accum-var tail) into-vars)
                   (push (list accum-var init-val) bindings)
                   (when tail
                     (push (list tail nil) bindings)))))
             (progn
               (unless default-accum
                 (setq default-accum (gensym "ACC"))
                 (push (list default-accum init-val) bindings)
                 (setq result-form (if is-list-accum `(nreverse ,default-accum)
                                       default-accum)))
               (setq accum-var default-accum)))
         (setq form (%loop-accum-body kn expr accum-var
                                      (cdr (assoc accum-var into-vars))))))
      ;; Nested IF/WHEN/UNLESS as sub-clause (standard LOOP syntax)
      ((or (%loop-keyword-p sub "IF") (%loop-keyword-p sub "WHEN")
           (%loop-keyword-p sub "UNLESS"))
       (let* ((inner-test (car rest))
              (negate (%loop-keyword-p sub "UNLESS"))
              (then-forms nil)
              (else-forms nil))
         (setq rest (cdr rest))
         ;; Parse then-branch sub-clause(s) connected by AND
         (block parse-inner-then
           (tagbody
             inner-then-sub
             (let* ((r (%loop-parse-cond-subclause
                        (car rest) (cdr rest) block-name
                        bindings into-vars default-accum
                        result-form epilogue)))
               (push (car r) then-forms)
               (setq rest (cadr r))
               (setq bindings (caddr r))
               (setq into-vars (car (cdddr r)))
               (setq default-accum (cadr (cdddr r)))
               (setq result-form (caddr (cdddr r)))
               (setq epilogue (car (cdddr (cdddr r)))))
             (when (and rest (%loop-keyword-p (car rest) "AND"))
               (setq rest (cdr rest))
               (go inner-then-sub))))
         ;; Check for ELSE branch
         (when (and rest (%loop-keyword-p (car rest) "ELSE"))
           (setq rest (cdr rest))
           (block parse-inner-else
             (tagbody
               inner-else-sub
               (let* ((r (%loop-parse-cond-subclause
                          (car rest) (cdr rest) block-name
                          bindings into-vars default-accum
                          result-form epilogue)))
                 (push (car r) else-forms)
                 (setq rest (cadr r))
                 (setq bindings (caddr r))
                 (setq into-vars (car (cdddr r)))
                 (setq default-accum (cadr (cdddr r)))
                 (setq result-form (caddr (cdddr r)))
                 (setq epilogue (car (cdddr (cdddr r)))))
               (when (and rest (%loop-keyword-p (car rest) "AND"))
                 (setq rest (cdr rest))
                 (go inner-else-sub)))))
         ;; Consume optional END
         (when (and rest (%loop-keyword-p (car rest) "END"))
           (setq rest (cdr rest)))
         ;; Build the form
         (when negate (setq inner-test `(not ,inner-test)))
         (let ((then-body (if (cdr then-forms)
                              `(progn ,@(nreverse then-forms))
                              (car then-forms)))
               (else-body (when else-forms
                            (if (cdr else-forms)
                                `(progn ,@(nreverse else-forms))
                                (car else-forms)))))
           (setq form (if else-body
                          `(if ,inner-test ,then-body ,else-body)
                          `(when ,inner-test ,then-body))))))
      (t (error "LOOP: invalid WHEN/IF sub-clause ~S" sub)))
    (list form rest bindings into-vars default-accum result-form epilogue)))

(defun %expand-extended-loop (forms)
  "Expand extended loop with keywords into block/let*/tagbody."
  (let* ((rest forms)
         (block-name nil)
         (bindings nil)
         (preamble nil)
         (body nil)
         (steps nil)
         (epilogue nil)
         (finally-forms nil)
         (initially-forms nil)
         (result-form nil)
         (default-accum nil)
         (into-vars nil)
         (loop-tag (gensym "LOOP"))
         (end-tag (gensym "END")))
    ;; Parser loop
    (block parse-done
      (tagbody
        parse-next
        (when (null rest) (return-from parse-done))
        (let ((kw (car rest)))
          (setq rest (cdr rest))
          (cond
            ;; WHILE — termination test at the point where it appears (CLHS 6.1.4)
            ((%loop-keyword-p kw "WHILE")
             (push `(unless ,(car rest) (go ,end-tag)) body)
             (setq rest (cdr rest)))
            ;; UNTIL — termination test at the point where it appears (CLHS 6.1.4)
            ((%loop-keyword-p kw "UNTIL")
             (push `(when ,(car rest) (go ,end-tag)) body)
             (setq rest (cdr rest)))
            ;; DO / DOING
            ((or (%loop-keyword-p kw "DO") (%loop-keyword-p kw "DOING"))
             (block consume-do
               (tagbody
                 do-next
                 (when (null rest) (return-from consume-do))
                 (when (%loop-keyword-sym-p (car rest)) (return-from consume-do))
                 (push (car rest) body)
                 (setq rest (cdr rest))
                 (go do-next))))
            ;; REPEAT
            ((%loop-keyword-p kw "REPEAT")
             (let ((ctr (gensym "REP")))
               (push (list ctr (car rest)) bindings)
               (setq rest (cdr rest))
               (push `(when (<= ,ctr 0) (go ,end-tag)) preamble)
               (push `(setq ,ctr (- ,ctr 1)) steps)))
            ;; FOR / AS [var spec] { AND [var spec] }* — parallel iteration.
            ;; CLHS 6.1.2.1: AND introduces another variable whose binding,
            ;; end-test, and stepping happen in parallel with the preceding
            ;; FOR.  For our simple expansion model each sub-clause's
            ;; bindings / preamble / steps are merged at the same level,
            ;; which matches parallel semantics for the iteration forms we
            ;; support (IN, ON, FROM/TO/BY, =, = THEN).
            ((or (%loop-keyword-p kw "FOR") (%loop-keyword-p kw "AS"))
             (let ((grp-steps nil)       ; this AND-group's step forms (reversed)
                   (grp-then-specs nil)   ; (var init step) for = THEN clauses
                   (grp-count 0))
               (block parse-for-and
                 (tagbody
                   for-and-next
                   (let* ((raw-var (car rest))
                          (destructuring (consp raw-var))
                          ;; CLHS 6.1.1.7: NIL as the variable ignores the
                          ;; value -- (loop for nil in list ...).  It still
                          ;; steps, so it gets a variable LET can bind.
                          (var (if destructuring
                                   (gensym "DVAR")
                                   (if (null raw-var) (gensym "IGNORE") raw-var)))
                          ;; CLHS 6.1.2.1: an optional type-spec may follow
                          ;; the variable, e.g. (loop for i fixnum from 0 ...).
                          (after-var (%loop-skip-type-spec (cdr rest)))
                          (sub-kw (car after-var))
                          (r (%loop-parse-for (cdr after-var) var sub-kw end-tag))
                          (then-spec (nth 5 r)))
                     (setq rest (car r))
                     (setq grp-count (+ grp-count 1))
                     (dolist (b (cadr r)) (push b bindings))
                     (when destructuring
                       (dolist (v (%loop-destructure-vars raw-var))
                         (push (list v nil) bindings)))
                     (dolist (e (caddr r)) (push e preamble))
                     (dolist (p (car (cdddr r))) (push p preamble))
                     (when destructuring
                       (dolist (a (%loop-destructure-assigns raw-var var))
                         (push a preamble)))
                     (dolist (s (cadr (cdddr r))) (push s grp-steps))
                     (when then-spec (push then-spec grp-then-specs)))
                   (when (and rest (%loop-keyword-p (car rest) "AND"))
                     (setq rest (cdr rest))
                     (go for-and-next))))
               ;; CLHS 6.1.2.1.4: variables introduced by a single
               ;; FOR ... AND ... AND ... are stepped in PARALLEL — every step
               ;; expression must see pre-step values before any variable is set.
               ;;
               ;; Pure = THEN group: one shared first-iteration flag; parallel
               ;; step via let-temps in preamble (handled by %loop-build-then-step).
               ;;
               ;; Pure FROM/numeric group: all step forms go to `steps`, parallel
               ;; via let-temps when more than one stepper.
               ;;
               ;; Mixed group (= THEN + FROM in the same AND group): putting THEN
               ;; steps in preamble and FROM steps in `steps` violates CLHS 6.1.2.1.4
               ;; because FROM steps (in `steps`) run before the next preamble, so a
               ;; THEN step expression in preamble would see the already-incremented
               ;; FROM value.  Fix: emit only the first-iteration init for each = THEN
               ;; var in preamble; move ALL step forms (FROM + THEN) into `steps` as a
               ;; single parallel let-temp block.
               (setq grp-then-specs (nreverse grp-then-specs))
               (setq grp-steps (nreverse grp-steps))
               (cond
                 ((null grp-then-specs)
                  ;; Pure FROM/numeric group.
                  (if (and (> grp-count 1) (%loop-all-simple-setq-p grp-steps))
                      (dolist (s (%loop-parallelize-setqs grp-steps)) (push s steps))
                      (dolist (s grp-steps) (push s steps))))
                 ((= (length grp-then-specs) grp-count)
                  ;; Pure = THEN group.
                  (let ((flag (gensym "FIRST")))
                    (push (list flag t) bindings)
                    (push (%loop-build-then-step flag grp-then-specs) preamble)))
                 (t
                  ;; Mixed = THEN + FROM group: init only in preamble; all steps
                  ;; (FROM + THEN) combined into one parallel step in `steps`.
                  (dolist (spec grp-then-specs)
                    (let ((flag (gensym "FIRST")))
                      (push (list flag t) bindings)
                      (push `(when ,flag
                               (setq ,(car spec) ,(cadr spec))
                               (setq ,flag nil))
                            preamble)))
                  (let ((all-steps (append grp-steps
                                          (mapcar (lambda (spec)
                                                    `(setq ,(car spec) ,(caddr spec)))
                                                  grp-then-specs))))
                    (if (%loop-all-simple-setq-p all-steps)
                        (dolist (s (%loop-parallelize-setqs all-steps)) (push s steps))
                        (dolist (s all-steps) (push s steps))))))))
            ;; RETURN
            ((%loop-keyword-p kw "RETURN")
             (push `(return-from ,block-name ,(car rest)) body)
             (setq rest (cdr rest)))
            ;; Accumulation (unified)
            ((%loop-accum-keyword-p kw)
             (let* ((kn (symbol-name kw))
                    (is-list-accum (%loop-list-accum-p kn))
                    (is-sum (or (string= kn "SUM") (string= kn "SUMMING")))
                    (is-count (or (string= kn "COUNT") (string= kn "COUNTING")))
                    (init-val (if (or is-sum is-count) 0 nil))
                    (expr (car rest))
                    (accum-var nil))
               (setq rest (cdr rest))
               (if (and rest (%loop-keyword-p (car rest) "INTO"))
                   (progn
                     (setq rest (cdr rest))
                     (setq accum-var (car rest))
                     (setq rest (cdr rest))
                     ;; CLHS 6.1.3.1: accumulation clauses accept an
                     ;; optional OF-TYPE type-spec after INTO var.  We
                     ;; don't use the type for optimization — just
                     ;; consume it so the clause parses.
                     (when (and rest (%loop-keyword-p (car rest) "OF-TYPE"))
                       (setq rest (cddr rest)))
                     ;; INTO-VARS is an alist (var . tail-var); a list
                     ;; accumulator gets a hidden tail pointer so the
                     ;; user-visible variable stays in order (see
                     ;; %LOOP-ACCUM-BODY).
                     (unless (assoc accum-var into-vars)
                       (let ((tail (and is-list-accum (gensym "TAIL"))))
                         (push (cons accum-var tail) into-vars)
                         (push (list accum-var init-val) bindings)
                         (when tail
                           (push (list tail nil) bindings)))))
                   (progn
                     (unless default-accum
                       (setq default-accum (gensym "ACC"))
                       (push (list default-accum init-val) bindings)
                       (setq result-form (if is-list-accum `(nreverse ,default-accum)
                                             default-accum)))
                     (setq accum-var default-accum)))
               (push (%loop-accum-body kn expr accum-var
                                       (cdr (assoc accum-var into-vars)))
                     body)))
            ;; NAMED
            ((%loop-keyword-p kw "NAMED")
             (setq block-name (car rest))
             (setq rest (cdr rest)))
            ;; WITH var [= expr] {AND var [= expr]}*
            ((%loop-keyword-p kw "WITH")
             (block parse-with
               (tagbody
                 with-next
                 (let* ((raw-wvar (car rest))
                        (destructuring-w (consp raw-wvar))
                        (wvar (if destructuring-w (gensym "WDVAR") raw-wvar)))
                   (setq rest (cdr rest))
                   ;; CLHS 6.1.2.4: skip an optional type-spec
                   ;; (OF-TYPE ... or a bare FIXNUM/FLOAT/T/NIL) such as
                   ;; (loop with noctets fixnum = 0 ...).
                   (setq rest (%loop-skip-type-spec rest))
                   (if (and rest (symbolp (car rest))
                            (string= (symbol-name (car rest)) "="))
                       (progn
                         (setq rest (cdr rest))
                         (push (list wvar (car rest)) bindings)
                         (setq rest (cdr rest)))
                       (push (list wvar nil) bindings))
                   (when destructuring-w
                     (dolist (b (%loop-destructure-bindings raw-wvar wvar))
                       (push b bindings))))
                 (when (and rest (%loop-keyword-p (car rest) "AND"))
                   (setq rest (cdr rest))
                   (go with-next)))))
            ;; INITIALLY
            ((%loop-keyword-p kw "INITIALLY")
             (block consume-ini
               (tagbody
                 ini-next
                 (when (null rest) (return-from consume-ini))
                 (when (%loop-keyword-sym-p (car rest)) (return-from consume-ini))
                 (push (car rest) initially-forms)
                 (setq rest (cdr rest))
                 (go ini-next))))
            ;; FINALLY
            ((%loop-keyword-p kw "FINALLY")
             (block consume-fin
               (tagbody
                 fin-next
                 (when (null rest) (return-from consume-fin))
                 (when (%loop-keyword-sym-p (car rest)) (return-from consume-fin))
                 (push (car rest) finally-forms)
                 (setq rest (cdr rest))
                 (go fin-next))))
            ;; ALWAYS
            ((%loop-keyword-p kw "ALWAYS")
             (push `(unless ,(car rest) (return-from ,block-name nil)) body)
             (setq rest (cdr rest))
             (unless result-form (setq result-form t)))
            ;; NEVER
            ((%loop-keyword-p kw "NEVER")
             (push `(when ,(car rest) (return-from ,block-name nil)) body)
             (setq rest (cdr rest))
             (unless result-form (setq result-form t)))
            ;; THEREIS
            ((%loop-keyword-p kw "THEREIS")
             (let ((tmp (gensym "V")))
               (push `(let ((,tmp ,(car rest)))
                        (when ,tmp (return-from ,block-name ,tmp)))
                     body)
               (setq rest (cdr rest))))
            ;; WHEN / IF / UNLESS — conditional with sub-clauses
            ;; Handles flat :when ... :else :when ... chains iteratively
            ((or (%loop-keyword-p kw "WHEN") (%loop-keyword-p kw "IF")
                 (%loop-keyword-p kw "UNLESS"))
             (let ((cond-clauses nil)  ;; list of (test . then-forms) -- built in reverse
                   (final-else nil)
                   (cur-test (car rest))
                   (cur-negate (%loop-keyword-p kw "UNLESS"))
                   (cur-then nil)
                   (it-var (gensym "IT"))   ;; gensym for LOOP IT variable
                   (it-raw-test (car rest))) ;; save raw test before negation
               (setq rest (cdr rest))
               (block parse-cond-chain
                 (tagbody
                   parse-cond-sub
                   ;; Parse one sub-clause
                   (let* ((r (%loop-parse-cond-subclause
                              (car rest) (cdr rest) block-name
                              bindings into-vars default-accum
                              result-form epilogue)))
                     (push (car r) cur-then)
                     (setq rest (cadr r))
                     (setq bindings (caddr r))
                     (setq into-vars (car (cdddr r)))
                     (setq default-accum (cadr (cdddr r)))
                     (setq result-form (caddr (cdddr r)))
                     (setq epilogue (car (cdddr (cdddr r)))))
                   (cond
                     ;; AND — more sub-clauses for same branch
                     ((and rest (%loop-keyword-p (car rest) "AND"))
                      (setq rest (cdr rest))
                      (go parse-cond-sub))
                     ;; ELSE followed by WHEN/IF/UNLESS — new cond clause (iterative)
                     ((and rest (%loop-keyword-p (car rest) "ELSE")
                           (cdr rest)
                           (or (%loop-keyword-p (cadr rest) "WHEN")
                               (%loop-keyword-p (cadr rest) "IF")
                               (%loop-keyword-p (cadr rest) "UNLESS")))
                      ;; Save current clause
                      (when cur-negate (setq cur-test `(not ,cur-test)))
                      (push (cons cur-test (nreverse cur-then)) cond-clauses)
                      ;; Start new clause
                      (setq rest (cdr rest))  ;; skip :else
                      (setq cur-negate (%loop-keyword-p (car rest) "UNLESS"))
                      (setq rest (cdr rest))  ;; skip :when/:if/:unless
                      (setq cur-test (car rest))
                      (setq rest (cdr rest))  ;; skip test-expr
                      (setq cur-then nil)
                      (go parse-cond-sub))
                     ;; ELSE followed by non-conditional — final else branch
                     ((and rest (%loop-keyword-p (car rest) "ELSE"))
                      (setq rest (cdr rest))
                      ;; Save current clause
                      (when cur-negate (setq cur-test `(not ,cur-test)))
                      (push (cons cur-test (nreverse cur-then)) cond-clauses)
                      ;; Parse else sub-clauses
                      (setq cur-then nil)
                      (block parse-final-else
                        (tagbody
                          parse-else-sub
                          (let* ((r (%loop-parse-cond-subclause
                                     (car rest) (cdr rest) block-name
                                     bindings into-vars default-accum
                                     result-form epilogue)))
                            (push (car r) final-else)
                            (setq rest (cadr r))
                            (setq bindings (caddr r))
                            (setq into-vars (car (cdddr r)))
                            (setq default-accum (cadr (cdddr r)))
                            (setq result-form (caddr (cdddr r)))
                            (setq epilogue (car (cdddr (cdddr r)))))
                          (when (and rest (%loop-keyword-p (car rest) "AND"))
                            (setq rest (cdr rest))
                            (go parse-else-sub))))
                      (when (and rest (%loop-keyword-p (car rest) "END"))
                        (setq rest (cdr rest))))
                     ;; END — done
                     ((and rest (%loop-keyword-p (car rest) "END"))
                      (setq rest (cdr rest))
                      ;; Save current clause
                      (when cur-negate (setq cur-test `(not ,cur-test)))
                      (push (cons cur-test (nreverse cur-then)) cond-clauses))
                     ;; No more conditional tokens — done
                     (t
                      (when cur-negate (setq cur-test `(not ,cur-test)))
                      (push (cons cur-test (nreverse cur-then)) cond-clauses)))))
               ;; Build nested if/else from cond-clauses (reverse order) + final-else
               ;; Bind IT to the when-test value per CL spec (LOOP IT variable).
               ;; IT refers to the value of the test-form; we bind IT first, then
               ;; use it as the condition to avoid double evaluation.
               (let ((result (if final-else
                                 `(progn ,@(nreverse final-else))
                                 nil)))
                 (dolist (clause cond-clauses)
                   (let ((test (car clause))
                         (forms (cdr clause)))
                     (if result
                         (setq result `(if ,test (progn ,@forms) ,result))
                         (setq result `(when ,test ,@forms)))))
                 ;; Bind IT to the when-test value per CL spec (LOOP IT
                 ;; variable).  We use a gensym and substitute any reference
                 ;; to the symbol IT (by name) in the body forms.
                 (labels ((%subst-it (form)
                            (cond
                              ((and (symbolp form)
                                    (string= (symbol-name form) "IT"))
                               it-var)
                              ((consp form)
                               (cons (%subst-it (car form))
                                     (%subst-it (cdr form))))
                              (t form))))
                   (if (and (= (length cond-clauses) 1) (null final-else))
                       (let ((forms (%subst-it (cdar cond-clauses))))
                         (push `(let ((,it-var ,it-raw-test))
                                  ,(if cur-negate
                                       `(unless ,it-var ,@forms)
                                       `(when ,it-var ,@forms)))
                               body))
                       ;; Multi-clause: substitute IT and wrap
                       (push `(let ((,it-var ,it-raw-test))
                                ,(%subst-it result))
                             body))))))
            (t
             (error "LOOP: unrecognized clause ~S" kw))))
        (go parse-next)))
    ;; Build the expansion
    `(block ,block-name
       (let* ,(nreverse bindings)
         ,@(nreverse initially-forms)
         (macrolet ((loop-finish () (list 'go ',end-tag)))
           (tagbody
             ,loop-tag
             ,@(nreverse preamble)
             ,@(nreverse body)
             ,@(nreverse steps)
             (go ,loop-tag)
             ,end-tag))
         ,@(nreverse epilogue)
         ,@(nreverse finally-forms)
         ,result-form))))

(defmacro loop (&rest forms)
  "CL LOOP macro -- supports simple and extended forms."
  (if (or (null forms)
          (%loop-keyword-sym-p (car forms)))
      ;; Extended loop (starts with keyword or is empty)
      (%expand-extended-loop forms)
      ;; Simple loop (body forms only)
      (%expand-simple-loop forms)))

;; pprint-logical-block — structured pretty printing
(defmacro pprint-logical-block (header &body body)
  (let* ((stream-sym (car header))
         (list-form (cadr header))
         (keys (cddr header))
         (prefix (getf keys :prefix))
         (per-line-prefix (getf keys :per-line-prefix))
         (suffix (getf keys :suffix))
         (pfx (or prefix per-line-prefix))
         (glist (gensym "LIST")))
    `(let ((,glist ,list-form))
       (%pp-push-block)
       ,(when pfx `(write-string ,pfx ,stream-sym))
       (macrolet ((pprint-pop ()
                    (list 'prog1 (list 'car ',glist)
                          (list 'setq ',glist (list 'cdr ',glist))))
                  (pprint-exit-if-list-exhausted ()
                    (list 'when (list 'null ',glist) '(return nil))))
         (block nil ,@body))
       ,(when suffix `(write-string ,suffix ,stream-sym))
       (%pp-pop-block)
       nil)))

;; pprint-fill — print list elements separated by spaces (fill style)
(defun pprint-fill (stream list &optional (colon-p t) atsign-p)
  (declare (ignore atsign-p))
  (let ((s (or stream *standard-output*)))
    (when colon-p (write-char #\( s))
    (loop for tail on list
          do (write (car tail) :stream s)
          when (cdr tail) do (write-char #\Space s))
    (when colon-p (write-char #\) s))))

;; pprint-linear — print list elements separated by newlines (linear style)
(defun pprint-linear (stream list &optional (colon-p t) atsign-p)
  (declare (ignore atsign-p))
  (let ((s (or stream *standard-output*)))
    (when colon-p (write-char #\( s))
    (loop for tail on list
          do (write (car tail) :stream s)
          when (cdr tail) do (terpri s))
    (when colon-p (write-char #\) s))))

;;; ============================================================
;;; MP atomic operations: COMPARE-AND-SWAP (alias CAS), ATOMIC-INCF,
;;; ATOMIC-DECF
;;; ============================================================
;;; Defined HERE, in the CL section, with every MP name written out as
;;; mp::NAME — not in the (in-package :mp) block below.  That block is
;;; read while the CL macros boot.lisp itself defines (LOOP, UNLESS,
;;; CASE, DOLIST, ...) are not yet external from COMMON-LISP (the export
;;; pass runs after boot), so on a source boot an unqualified LOOP there
;;; would intern a fresh, undefined MP::LOOP.  Read in the CL section
;;; those names resolve to the CL symbols on both load paths.
;;;
;;; The cell primitives mp::%CAS-CAR / %CAS-CDR / %CAS-SVREF /
;;; %CAS-STRUCT-SLOT / %CAS-SYMBOL-VALUE are C builtins
;;; (builtins_thread.c) over the platform CAS: a native compare-exchange
;;; on the host, a Forbid()/Permit() window on the single-core AmigaOS and
;;; MorphOS targets.  Each returns the value the cell held when the CAS
;;; was decided — EQ to OLD exactly when the swap happened.
;;; mp::%CAS-SLOT-VALUE lives in clos.lisp (it needs the slot-location
;;; machinery).

(defun mp::%atomic-place (place env)
  "Analyse PLACE for MP:COMPARE-AND-SWAP / ATOMIC-INCF / ATOMIC-DECF.
Returns three values: a LET* binding list that evaluates the place's
subforms exactly once, left to right (CLHS 5.1.1.1); a form reading the
place through those bindings; and a function of two forms (OLD NEW)
returning the mp::%CAS-* call that swaps it.  Macro and symbol-macro
places are expanded, (THE type place) is unwrapped, and a defstruct
accessor is recognised through the compiler macro DEFSTRUCT installs
for it."
  (let ((form place))
    (loop
      (cond
        ((symbolp form)
         (multiple-value-bind (expansion expanded-p) (macroexpand-1 form env)
           (unless expanded-p
             ;; A variable: only a special (global) one has a cell; the
             ;; builtin follows the thread's dynamic binding if one exists.
             ;; An ordinary lexical has no such cell — CASing it through
             ;; SYMBOL-VALUE would silently hit an unrelated global, so
             ;; reject it here instead (this only sees global specialness;
             ;; a symbol special only by a local DECLARE is indistinguishable
             ;; from a plain lexical without environment introspection this
             ;; implementation doesn't have, and is rejected too).
             (unless (mp::%symbol-specialp form)
               (error "MP:COMPARE-AND-SWAP: ~S is not a special variable~%  ~
A bare symbol place must name a variable declared SPECIAL (DEFVAR, ~
DEFPARAMETER, or PROCLAIM); an ordinary lexical has no shared cell to CAS."
                      form))
             (return (values '()
                             `(symbol-value ',form)
                             (lambda (old new)
                               `(mp::%cas-symbol-value ',form ,old ,new)))))
           (setq form expansion)))
        ((not (consp form))
         (error "MP:COMPARE-AND-SWAP: ~S is not a place" place))
        (t
         (let ((head (car form))
               (args (cdr form)))
           (flet ((cell (nargs reader casser)
                    ;; Bind ARGS to temporaries and build the reader / CAS
                    ;; forms over them.  READER and CASSER take the temps.
                    (unless (= (length args) nargs)
                      (error "MP:COMPARE-AND-SWAP: ~S expects ~D argument~:P, got ~S"
                             head nargs form))
                    (let ((temps '()))
                      (dolist (a args)
                        (declare (ignore a))
                        (push (gensym "PLACE") temps))
                      (setq temps (nreverse temps))
                      (return (values (mapcar #'list temps args)
                                      (apply reader temps)
                                      (lambda (old new)
                                        (apply casser old new temps)))))))
             (case head
               ((car first)
                (cell 1 (lambda (c) `(car ,c))
                        (lambda (o nw c) `(mp::%cas-car ,c ,o ,nw))))
               ((cdr rest)
                (cell 1 (lambda (c) `(cdr ,c))
                        (lambda (o nw c) `(mp::%cas-cdr ,c ,o ,nw))))
               ((svref aref)
                (cell 2 (lambda (v i) `(svref ,v ,i))
                        (lambda (o nw v i) `(mp::%cas-svref ,v ,i ,o ,nw))))
               ((symbol-value)
                (cell 1 (lambda (s) `(symbol-value ,s))
                        (lambda (o nw s) `(mp::%cas-symbol-value ,s ,o ,nw))))
               ((amilisp::%struct-ref)
                (cell 2 (lambda (s i) `(amilisp::%struct-ref ,s ,i))
                        (lambda (o nw s i) `(mp::%cas-struct-slot ,s ,i ,o ,nw))))
               ((slot-value)
                (cell 2 (lambda (i s) `(slot-value ,i ,s))
                        (lambda (o nw i s) `(mp::%cas-slot-value ,i ,s ,o ,nw))))
               ((the)
                (unless (= (length args) 2)
                  (error "MP:COMPARE-AND-SWAP: malformed THE place ~S" form))
                (setq form (car (cdr args))))
               (t
                (let ((expansion
                        (cond ((macro-function head env)
                               (macroexpand-1 form env))
                              ((compiler-macro-function head env)
                               (funcall (compiler-macro-function head env)
                                        form env))
                              (t form))))
                  (when (eq expansion form)
                    (error "MP:COMPARE-AND-SWAP: unsupported place ~S~%  Supported: ~
(CAR c) (CDR c) (FIRST c) (REST c), (SVREF v i) / (AREF v i) on a simple-vector, ~
(SYMBOL-VALUE s) or a special variable, (SLOT-VALUE o 's), a DEFSTRUCT slot accessor, ~
and macros or (THE type ...) wrapping one of these."
                           place))
                  (setq form expansion)))))))))))

(defmacro mp::compare-and-swap (place old new &environment env)
  "Atomically compare the value of PLACE with OLD (under EQ) and, when they
are the same, store NEW.  Returns the value PLACE held when the comparison
was made: EQ to OLD exactly when the swap happened.  PLACE's subforms are
evaluated once, before OLD and NEW.  Atomic with respect to every other
COMPARE-AND-SWAP / ATOMIC-INCF / ATOMIC-DECF on the same cell across all
threads; a plain SETF of the cell is not part of that protocol.  Supported
places: (CAR c) (CDR c) (FIRST c) (REST c), (SVREF v i) / (AREF v i) on a
simple-vector, (SYMBOL-VALUE s) or a special variable (the calling thread's
dynamic binding, if any), (SLOT-VALUE o 's), DEFSTRUCT slot accessors, and
macros or (THE type ...) wrapping one of these."
  (multiple-value-bind (bindings reader casser) (mp::%atomic-place place env)
    (declare (ignore reader))
    (let ((o (gensym "OLD"))
          (nw (gensym "NEW")))
      `(let* (,@bindings (,o ,old) (,nw ,new))
         ,(funcall casser o nw)))))

(defmacro mp::cas (place old new)
  "Alias for MP:COMPARE-AND-SWAP."
  `(mp::compare-and-swap ,place ,old ,new))

(defun mp::%atomic-fixnum-sum (old delta operator)
  "OLD + DELTA for ATOMIC-INCF / ATOMIC-DECF: both must be fixnums, and so
must the result -- an atomic counter never silently becomes a bignum."
  (unless (typep old 'fixnum)
    (error 'type-error :datum old :expected-type 'fixnum))
  (unless (typep delta 'fixnum)
    (error 'type-error :datum delta :expected-type 'fixnum))
  (let ((new (+ old delta)))
    (unless (typep new 'fixnum)
      (error "~S: ~D ~:[-~;+~] ~D overflows a fixnum"
             operator old (>= delta 0) (abs delta)))
    new))

(defun mp::%atomic-add-expansion (place delta env operator)
  ;; CAS retry loop: read the cell, compute the fixnum sum, swap it in if
  ;; the cell still holds what was read; a lost race just goes round again.
  (multiple-value-bind (bindings reader casser) (mp::%atomic-place place env)
    (let ((d (gensym "DELTA"))
          (o (gensym "OLD"))
          (nw (gensym "NEW")))
      `(let* (,@bindings (,d ,delta))
         (loop
           (let* ((,o ,reader)
                  (,nw (mp::%atomic-fixnum-sum ,o ,d ',operator)))
             (when (eq ,o ,(funcall casser o nw))
               (return ,nw))))))))

(defmacro mp::atomic-incf (place &optional (delta 1) &environment env)
  "Atomically add DELTA (default 1) to the fixnum in PLACE and return the
new value.  PLACE and its current value must be fixnums, as must the
result; otherwise an error is signalled and PLACE is left unchanged.  Same
places as MP:COMPARE-AND-SWAP; PLACE's subforms are evaluated once, before
DELTA."
  (mp::%atomic-add-expansion place delta env 'mp::atomic-incf))

(defmacro mp::atomic-decf (place &optional (delta 1) &environment env)
  "Atomically subtract DELTA (default 1) from the fixnum in PLACE and
return the new value.  See MP:ATOMIC-INCF."
  (mp::%atomic-add-expansion place `(- ,delta) env 'mp::atomic-decf))

(export '(mp::compare-and-swap mp::cas mp::atomic-incf mp::atomic-decf) :mp)

;;; ============================================================
;;; EXT package: defglobal
;;; ============================================================

(in-package :ext)

(defmacro defglobal (name value &optional doc)
  (declare (ignore doc))
  `(defvar ,name ,value))

(export '(defglobal))

;; Make socket read/write timeouts settable:
;;   (setf (ext:socket-stream-timeout stream :input)  seconds)
;;   (setf (ext:socket-stream-timeout stream :output) seconds)
;; The setter receives (stream direction new-value) per the defsetf short form.
(defsetf socket-stream-timeout %set-socket-stream-timeout)

;; TLS: keyword front-end over the positional %SOCKET-START-TLS builtin.
;; Upgrades a connected TCP socket stream to TLS IN PLACE and returns the
;; same stream — wrappers already holding it keep working.  Client side by
;; default; :SERVER T accepts instead (then :CERTIFICATE is required).
;; Provider availability: (ext:tls-available-p) / (ext:tls-version).
;; See tests/tls-loopback.lisp for an end-to-end client/server example.
(defun socket-start-tls (stream &key server hostname (verify t)
                                     certificate key key-password
                                     ca-file ca-path (timeout 30))
  (%socket-start-tls stream server hostname verify certificate key
                     key-password ca-file ca-path timeout))

(export '(socket-start-tls))

;;; ============================================================
;;; MP package: threading macros and stubs
;;; ============================================================
;;; Real threading primitives are C builtins (make-thread, make-lock,
;;; acquire-lock, release-lock, etc.).  Convenience macros and stubs
;;; for recursive locks and memory barriers live here.

(in-package :mp)

;; Shadow these names so they are interned in MP, not inherited from CL
;; (cl_package_export_defined_cl_symbols would otherwise claim them)
(shadow '(with-lock-held make-recursive-lock with-recursive-lock-held
          read-memory-barrier write-memory-barrier))

(defmacro with-lock-held ((lock) &body body)
  (let ((l (gensym "LOCK")))
    `(let ((,l ,lock))
       (acquire-lock ,l t)
       (unwind-protect (progn ,@body)
         (release-lock ,l)))))

(defun make-recursive-lock (&optional name)
  (%make-recursive-lock name))

(defmacro with-recursive-lock-held ((lock) &body body)
  `(with-lock-held (,lock) ,@body))

(defun read-memory-barrier ()
  nil)

(defun write-memory-barrier ()
  nil)

(export '(with-lock-held make-recursive-lock
          with-recursive-lock-held
          read-memory-barrier write-memory-barrier))

(in-package :cl-user)

;; JITEXPAND — show the m68k assembly the JIT emitted for a form.
;;
;; Three accepted shapes:
;;
;;   (jitexpand (defun foo (x) x))     ; defines foo, then disassembles
;;   (jitexpand (lambda (x) x))        ; disassembles the lambda
;;   (jitexpand (+ x 1))               ; wraps in (lambda () ...) and
;;                                       disassembles — useful for
;;                                       inspecting an expression in
;;                                       isolation
;;
;; A leading QUOTE is unwrapped so all three also work as:
;;   (jitexpand '(defun foo ...))
;;   (jitexpand '(lambda ...))
;;   (jitexpand '(+ x 1))
;;
;; On host (no JIT_M68K) or for functions/forms the JIT didn't compile,
;; prints "(no native code ...)" and returns NIL.
;;
;; Note: this macro deliberately avoids CADR / SECOND / etc. — at the
;; time boot.lisp compiles, defining a top-level macro that references
;; CADR somehow poisons the CADR function binding for later code (a
;; compiler-macro interaction worth chasing separately).  CAR/CDR
;; combinations are safe.
(defmacro jitexpand (form)
  (let ((f form))
    (if (and (consp f) (eq (car f) 'quote) (consp (cdr f)))
        (setq f (car (cdr f))))
    (cond
      ;; (defun NAME ...) — install the function and disassemble by name.
      ((and (consp f) (eq (car f) 'defun)
            (consp (cdr f)) (symbolp (car (cdr f))))
       (let ((name (car (cdr f))))
         `(progn
            ,f
            (format t "; JIT disassembly of ~A:~%" ',name)
            (amilisp::%jit-disassemble (symbol-function ',name)))))
      ;; (lambda ...) — disassemble the lambda directly.
      ((and (consp f) (eq (car f) 'lambda))
       `(progn
          (format t "; JIT disassembly of ~S:~%" ',f)
          (amilisp::%jit-disassemble ,f)))
      ;; Any other expression — wrap as a thunk and disassemble.  The
      ;; thunk is never invoked, so free variables in F don't have to
      ;; be bound for the inspection to work.
      (t
       `(progn
          (format t "; JIT disassembly of (lambda () ~S):~%" ',f)
          (amilisp::%jit-disassemble (lambda () ,f)))))))
