#!/bin/sh
# Executable specification for EXT.DEV:HANDLE-COMMAND -- the command layer
# behind the ARexx development port (lib/dev-commands.lisp).
#
# The whole reason that layer is a separate, platform-neutral file is so it
# can be tested here, on the host, where there is no ARexx at all.  The
# Amiga side (lib/amiga/arexx.lisp) only moves these strings across a
# message port, and is covered by the arexx block in
# tests/amiga/run-tests.lisp.
#
# What matters most: a LOAD of a file with SEVERAL bad top-level forms must
# come back with a diagnostic for EACH of them, each carrying file:line.
# That is the editor's whole workflow, and it depends on two runtime fixes
# guarded by tests/test_unwind_load.c.
#
# Run: sh tests/test_dev_commands.sh build/host/amilisp

AMILISP="${1:-build/host/amilisp}"
passed=0
failed=0
total=0
TMPD=$(mktemp -d "${TMPDIR:-/tmp}/test_dev_commands_XXXXXX") || exit 1
trap 'rm -rf "$TMPD"' EXIT INT TERM

# Each case runs in a fresh image: no FASL cache carry-over between checks.
run_cmd() {
    # $1 = command string (already Lisp-escaped)
    cat <<EOF | "$AMILISP" --no-userinit --batch 2>&1
(require "dev-commands")
(multiple-value-bind (rc text) (ext.dev:handle-command "$1")
  (format t "~&<<RC=~d>>~%~a~%<<END>>~%" rc text))
EOF
}

check() {
    desc="$1"; pattern="$2"; out="$3"
    total=$((total + 1))
    if echo "$out" | grep -q "$pattern"; then
        echo "  ok  $desc"
        passed=$((passed + 1))
    else
        echo "  FAIL  $desc (missing: $pattern)"
        echo "    output: $(echo "$out" | head -12)"
        failed=$((failed + 1))
    fi
}

# Run a Lisp script FILE in a fresh image.  Scripts go through a file rather
# than an inline heredoc inside $( ): a heredoc nested in a command
# substitution re-parses quotes, and an apostrophe in Lisp source (quote) is
# enough to break the shell parse.
run_script() {
    "$AMILISP" --no-userinit --batch < "$1" 2>&1
}

check_not() {
    desc="$1"; pattern="$2"; out="$3"
    total=$((total + 1))
    if echo "$out" | grep -q "$pattern"; then
        echo "  FAIL  $desc (unexpected: $pattern)"
        echo "    output: $(echo "$out" | head -12)"
        failed=$((failed + 1))
    else
        echo "  ok  $desc"
        passed=$((passed + 1))
    fi
}

# --- fixture files ---------------------------------------------------------

# Three bad forms among good ones: the multi-diagnostic case.
cat > "$TMPD/three-errors.lisp" <<'EOF'
(defun dev-ok-1 () 1)
(error "first bad form")
(defun dev-ok-2 () 2)
(undefined-function-one)
(defun dev-ok-3 () 3)
(undefined-function-two)
(defvar *dev-reached-end* t)
EOF

cat > "$TMPD/clean.lisp" <<'EOF'
(defvar *dev-clean-loaded* t)
EOF

# Unbalanced parens: the half-saved-buffer case.  The reader error takes the
# rest of the file with it, which the reply has to say out loud.
cat > "$TMPD/unbalanced.lisp" <<'EOF'
(defvar *dev-unbalanced-first* t)
(defun broken (
EOF

# --- PING / VERSION --------------------------------------------------------

out=$(run_cmd 'PING')
check "PING returns rc 0"            '<<RC=0>>' "$out"
check "PING answers PONG"            'PONG'     "$out"

out=$(run_cmd 'ping')
check "verbs are case-insensitive"   'PONG'     "$out"

out=$(run_cmd 'VERSION')
check "VERSION returns rc 0"         '<<RC=0>>' "$out"
check "VERSION names the implementation" 'AmiLisp' "$out"

# --- unknown command -------------------------------------------------------

out=$(run_cmd 'FLURB some args')
check "unknown command is fatal"     '<<RC=20>>'            "$out"
check "unknown command names itself" 'unknown command: FLURB' "$out"
check "unknown command lists verbs"  'Known commands:.*LOAD' "$out"

# --- EVAL ------------------------------------------------------------------

out=$(run_cmd 'EVAL (+ 1 2)')
check "EVAL returns rc 0"            '<<RC=0>>' "$out"
check "EVAL prints the value"        '^3$'      "$out"

out=$(run_cmd '(list 1 2 3)')
check "bare form is evaluated"       '(1 2 3)'  "$out"

out=$(run_cmd 'EVAL (values 1 2)')
check "EVAL shows multiple values"   '1 ; 2'    "$out"

out=$(run_cmd 'EVAL (/ 1 0)')
check "EVAL error gives rc 10"       '<<RC=10>>'      "$out"
check "EVAL error is reported"       'ERROR: division by zero' "$out"
# A form typed into a command string has no source file; inventing one from
# whatever the compiler last stamped would send the editor to a random file.
check_not "EVAL error has no bogus location" 'lisp:[0-9]*: ERROR: division' "$out"

# --- LOAD ------------------------------------------------------------------

out=$(run_cmd "LOAD $TMPD/clean.lisp")
check "clean LOAD returns rc 0"      '<<RC=0>>'              "$out"
check "clean LOAD reports no errors" '0 error(s), 0 warning(s)' "$out"

out=$(run_cmd "LOAD $TMPD/three-errors.lisp")
check "failing LOAD returns rc 10"   '<<RC=10>>'             "$out"
check "LOAD reports ALL three errors" '3 error(s), 0 warning(s)' "$out"
check "first error has file:line"    'three-errors.lisp:2: ERROR: first bad form' "$out"
check "second error has file:line"   'three-errors.lisp:4: ERROR: Undefined function: UNDEFINED-FUNCTION-ONE' "$out"
check "third error has file:line"    'three-errors.lisp:6: ERROR: Undefined function: UNDEFINED-FUNCTION-TWO' "$out"

# Recovery is not just cosmetic: the forms after each bad one must have run.
cat > "$TMPD/reached-end.lisp" <<EOF
(require "dev-commands")
(ext.dev:handle-command "LOAD $TMPD/three-errors.lisp")
(format t "~&<<REACHED-END=~a>>~%" (and (boundp (quote cl-user::*dev-reached-end*)) t))
EOF
out=$(run_script "$TMPD/reached-end.lisp")
check "LOAD kept going to the last form" '<<REACHED-END=T>>' "$out"

out=$(run_cmd "LOAD $TMPD/unbalanced.lisp")
check "reader error returns rc 10"   '<<RC=10>>'  "$out"
check "reader error says it aborted" 'aborted'    "$out"

out=$(run_cmd "LOAD $TMPD/no-such-file-here.lisp")
check "missing file returns rc 10"   '<<RC=10>>'         "$out"
check "missing file is reported"     'does not exist'    "$out"

out=$(run_cmd 'LOAD')
check "LOAD with no argument is fatal" '<<RC=20>>'            "$out"
check "LOAD with no argument explains" 'requires a file name' "$out"

# Editors quote paths that contain spaces.
mkdir -p "$TMPD/dir with space"
cp "$TMPD/clean.lisp" "$TMPD/dir with space/quoted.lisp"
out=$(run_cmd "LOAD \\\"$TMPD/dir with space/quoted.lisp\\\"")
check "quoted path with spaces loads" '0 error(s), 0 warning(s)' "$out"

# --- the port survives a bad command ---------------------------------------
# The handler loop keeps serving after anything a command can do to it; if a
# failing LOAD could take the image down, the editor would lose the session.
cat > "$TMPD/survives.lisp" <<EOF
(require "dev-commands")
(ext.dev:handle-command "LOAD $TMPD/unbalanced.lisp")
(ext.dev:handle-command "EVAL (/ 1 0)")
(ext.dev:handle-command "LOAD $TMPD/no-such-file-here.lisp")
(multiple-value-bind (rc text) (ext.dev:handle-command "PING")
  (format t "~&<<STILL-ALIVE rc=~d ~a>>~%" rc text))
EOF
out=$(run_script "$TMPD/survives.lisp")
check "handler survives a run of bad commands" '<<STILL-ALIVE rc=0 PONG>>' "$out"

# --- LASTRESULT ------------------------------------------------------------
# ARexx only transmits RESULT when rc is 0, so a failing command's text is
# unreachable without this.
cat > "$TMPD/lastresult.lisp" <<EOF
(require "dev-commands")
(ext.dev:handle-command "LOAD $TMPD/three-errors.lisp")
(multiple-value-bind (rc text) (ext.dev:handle-command "LASTRESULT")
  (format t "~&<<RC=~d>>~%~a~%<<END>>~%" rc text))
EOF
out=$(run_script "$TMPD/lastresult.lisp")
check "LASTRESULT returns rc 0"       '<<RC=0>>'  "$out"
check "LASTRESULT replays diagnostics" '3 error(s), 0 warning(s)' "$out"

# --- IN-PACKAGE ------------------------------------------------------------

cat > "$TMPD/in-package.lisp" <<'EOF'
(require "dev-commands")
(defpackage "DEV-CMD-TEST" (:use "CL"))
(ext.dev:handle-command "IN-PACKAGE DEV-CMD-TEST")
(multiple-value-bind (rc text) (ext.dev:handle-command "EVAL (package-name *package*)")
  (format t "~&<<RC=~d>>~%~a~%<<END>>~%" rc text))
EOF
out=$(run_script "$TMPD/in-package.lisp")
check "IN-PACKAGE switches the command package" 'DEV-CMD-TEST' "$out"

out=$(run_cmd 'IN-PACKAGE NO-SUCH-PACKAGE-HERE')
check "IN-PACKAGE rejects unknown packages" 'no such package' "$out"

# --- DEFINE-RAW-COMMAND -----------------------------------------------------
#
# An editor's port receives the REPL thread's `OUTPUT <chunk>' through this
# layer too, and a chunk is text, not syntax: the indentation of an
# indented line and the newline a chunk ends with must survive, where every
# other command is trimmed at both ends.  Exactly what the C editor's
# replmsg.c did outside ReadArgs (clamacs tests/test_replmsg.c).

cat > "$TMPD/raw.lisp" <<'EOF'
(require "dev-commands")
(defvar *chunks* '())
(ext.dev:define-raw-command "OUTPUT" (arg)
  (push arg *chunks*)
  (values ext.dev:+rc-ok+ ""))
(ext.dev:define-command "TRIMMED" (arg)
  (push arg *chunks*)
  (values ext.dev:+rc-ok+ ""))
(dolist (c (list (format nil "OUTPUT hello~%")
                 (format nil "OUTPUT   indented~%")
                 "OUTPUT say \"hi"
                 (format nil "OUTPUT a~%b~%c~%")
                 "OUTPUT "
                 "OUTPUT"
                 "output lower"
                 "OUTPUTS x"
                 (format nil "TRIMMED   both ends   ~%")))
  (ext.dev:handle-command c))
;; A newline inside a grep pattern is an alternation, not a newline, so show
;; every newline as ~ and assert on single-line patterns.
(format t "~&<<CHUNKS=~s>>~%"
        (mapcar (lambda (c) (substitute #\~ #\Newline c)) (reverse *chunks*)))
(multiple-value-bind (rc text) (ext.dev:handle-command "OUTPUTS x")
  (format t "~&<<RC=~d>>~%~a~%<<END>>~%" rc text))
EOF
out=$(run_script "$TMPD/raw.lisp")
check "a raw command keeps the chunk's newline"      '"hello~"' "$out"
check "a raw command keeps leading blanks"           '"  indented~"' "$out"
check "a raw command takes quotes as text"           '"say \\"hi"' "$out"
check "a raw command keeps several lines"            '"a~b~c~"' "$out"
check "OUTPUT with a blank is the empty chunk"       '"" ""' "$out"
check "raw verbs are case-insensitive"               '"lower"' "$out"
check "an ordinary command is still trimmed"         '"both ends"' "$out"
check "OUTPUTS is not OUTPUT"                        'unknown command: OUTPUTS' "$out"

# --- result cap ------------------------------------------------------------
# An unbounded compiler log cannot be shipped as an ARexx argstring.
cat > "$TMPD/truncate.lisp" <<'EOF'
(require "dev-commands")
(let ((ext.dev:*max-result-length* 200))
  (multiple-value-bind (rc text)
      (ext.dev:handle-command "EVAL (dotimes (i 200) (format t \"filler line ~d~%\" i))")
    (format t "~&<<RC=~d LEN=~d>>~%~a~%" rc (length text) text)))
EOF
out=$(run_script "$TMPD/truncate.lisp")
check "over-long reply is truncated"   'truncated at 200 characters' "$out"

# ===========================================================================
# Introspection: ARGLIST / COMPLETE / DESCRIBE / APROPOS / SOURCE-LOCATION /
# MACROEXPAND -- the editor's phase-2 commands.  Every case that names a
# symbol resolves it against the command package without interning, so an
# unknown name is rc 10 with a reason.
# ===========================================================================

# A file of definitions, loaded from its FASL: SOURCE-LOCATION and the
# docstrings must survive COMPILE-FILE + LOAD in a fresh session, which is
# the load-time-call design of specs/documentation-introspection.md.
cat > "$TMPD/intro.lisp" <<'EOF'
(in-package :cl-user)

(defun intro-fn (a b &key (c 3))
  "Intro doc."
  (list a b c))
(defmacro intro-mac (x &body body)
  "Intro macro doc."
  `(let ((y ,x)) ,@body))
(defgeneric intro-gf (a) (:documentation "Intro gf doc."))
(defmethod intro-gf ((a string)) a)
(defvar *intro-var* 1 "Intro var doc.")
(defun intro-other () 2)
EOF
cat > "$TMPD/intro-compile.lisp" <<EOF
(compile-file "$TMPD/intro.lisp" :output-file "$TMPD/intro.fasl")
EOF
run_script "$TMPD/intro-compile.lisp" >/dev/null 2>&1

# Runs COMMAND after loading the FASL (a fresh image each time).
run_intro() {
    cat <<EOF | "$AMILISP" --no-userinit --batch 2>&1
(require "dev-commands")
(load "$TMPD/intro.fasl")
(multiple-value-bind (rc text) (ext.dev:handle-command "$1")
  (format t "~&<<RC=~d>>~%~a~%<<END>>~%" rc text))
EOF
}

# --- ARGLIST ---------------------------------------------------------------

out=$(run_intro 'ARGLIST intro-fn')
check "ARGLIST returns rc 0"                 '<<RC=0>>' "$out"
check "ARGLIST is the lambda list as written, lower case" '^(a b &key (c 3))$' "$out"

out=$(run_intro 'ARGLIST intro-mac')
check "ARGLIST of a macro is its written lambda list" '^(x &body body)$' "$out"

out=$(run_intro 'ARGLIST intro-gf')
check "ARGLIST of a generic function"        '^(a)$' "$out"

out=$(run_intro 'ARGLIST cl:mapcar')
check "ARGLIST of a builtin has bare placeholders" '^(arg0 arg1 &rest arg2)$' "$out"

out=$(run_intro 'ARGLIST if')
check "ARGLIST of a special operator comes from the table" '^(test then &optional else)$' "$out"

out=$(run_intro 'ARGLIST no-such-symbol-here')
check "ARGLIST of an unknown name is rc 10"  '<<RC=10>>' "$out"
check "ARGLIST names the missing symbol"     'no such symbol: no-such-symbol-here' "$out"

out=$(run_intro 'ARGLIST nopkg:foo')
check "ARGLIST with an unknown package is rc 10" 'no such package: nopkg' "$out"

out=$(run_intro 'ARGLIST *intro-var*')
check "ARGLIST of a variable is rc 10"       '<<RC=10>>' "$out"

out=$(run_intro 'ARGLIST')
check "ARGLIST without a name is fatal"      '<<RC=20>>' "$out"

# --- COMPLETE --------------------------------------------------------------

out=$(run_intro 'COMPLETE intro-')
check "COMPLETE lists every candidate"       '^intro-fn$' "$out"
check "COMPLETE lists the macro too"         '^intro-mac$' "$out"
check "COMPLETE candidates are sorted"       'intro-fn.*intro-gf.*intro-mac.*intro-other' "$(echo "$out" | tr '\n' ' ')"

out=$(run_intro 'COMPLETE multiple-value-b')
check "COMPLETE sees symbols inherited from CL" '^multiple-value-bind$' "$out"

out=$(run_intro 'COMPLETE cl:mapc')
check "COMPLETE keeps the package prefix as typed" '^cl:mapcar$' "$out"

out=$(run_intro 'COMPLETE :for')
check "COMPLETE handles keywords"            '^:format-control$' "$out"

out=$(run_intro 'COMPLETE handle ext.dev')
check "COMPLETE takes an explicit package"   '^handle-command$' "$out"

out=$(run_intro 'COMPLETE zzz-nothing')
check "COMPLETE with no candidate is rc 0"   '<<RC=0>>' "$out"

# No symbol starts with `tro-': the ones that contain it (and only then,
# checked below with the complete-order script).
out=$(run_intro 'COMPLETE tro-')
check "COMPLETE falls back to names containing the text" '^intro-fn$' "$out"
check "COMPLETE fallback lists the macro too" '^intro-mac$' "$out"
check "COMPLETE fallback is sorted"          'intro-fn.*intro-gf.*intro-mac.*intro-other' "$(echo "$out" | tr '\n' ' ')"
out=$(run_intro 'COMPLETE cl:tiple-value-b')
check "COMPLETE fallback keeps the package prefix" '^cl:multiple-value-bind$' "$out"

out=$(run_intro 'COMPLETE x nopkg')
check "COMPLETE with an unknown package is rc 10" 'no such package: nopkg' "$out"

# Exported symbols come first: MAPCAR (external in CL) before a same-prefix
# internal of the command package.
cat > "$TMPD/complete-order.lisp" <<EOF
(require "dev-commands")
(load "$TMPD/intro.fasl")
(defun mapcar-intro-internal () 1)
(multiple-value-bind (rc text) (ext.dev:handle-command "COMPLETE mapcar")
  (format t "~&<<RC=~d>>~%~a~%<<END>>~%" rc (substitute #\\Space #\\Newline text)))
(let ((ext.dev:*max-completions* 2))
  (multiple-value-bind (rc text) (ext.dev:handle-command "COMPLETE m")
    (format t "~&<<CAP rc=~d lines=~d>>~%" rc (1+ (count #\\Newline text)))))
;; intro- has prefix matches: MAPCAR-INTRO-INTERNAL, which only contains
;; it, is not among them.
(multiple-value-bind (rc text) (ext.dev:handle-command "COMPLETE intro-")
  (format t "~&<<PREFIX rc=~d diluted=~a>>~%" rc (and (search "mapcar" text) t)))
EOF
out=$(run_script "$TMPD/complete-order.lisp")
check "COMPLETE puts exported candidates first" 'mapcar mapcar-intro-internal' "$out"
check "COMPLETE caps the candidate list"     '<<CAP rc=0 lines=2>>' "$out"
check "COMPLETE prefix matches are not diluted with substring matches" '<<PREFIX rc=0 diluted=NIL>>' "$out"

# --- DESCRIBE --------------------------------------------------------------

out=$(run_intro 'DESCRIBE intro-fn')
check "DESCRIBE returns rc 0"                '<<RC=0>>' "$out"
check "DESCRIBE shows the lambda list"       'Lambda-list: (A B &KEY (C 3))' "$out"
check "DESCRIBE shows the docstring loaded from the FASL" 'Documentation: Intro doc.' "$out"
check "DESCRIBE shows the source location"   'Source: .*intro.lisp:3' "$out"

out=$(run_intro 'DESCRIBE intro-mac')
check "DESCRIBE of a macro names it a macro" 'Macro: ' "$out"
check "DESCRIBE of a macro shows its docstring" 'Documentation: Intro macro doc.' "$out"

out=$(run_intro 'DESCRIBE *intro-var*')
check "DESCRIBE shows a variable docstring"  'Variable documentation: Intro var doc.' "$out"

out=$(run_intro 'DESCRIBE no-such-symbol-here')
check "DESCRIBE of an unknown name is rc 10" '<<RC=10>>' "$out"

# --- APROPOS ---------------------------------------------------------------

out=$(run_intro 'APROPOS intro-')
check "APROPOS finds the user's own definitions" '^intro-fn function$' "$out"
check "APROPOS tags a macro"                 '^intro-mac macro$' "$out"
check "APROPOS tags a variable"              '^\*intro-var\* variable$' "$out"

out=$(run_intro 'APROPOS make-hash cl')
check "APROPOS takes a package"              '^make-hash-table function$' "$out"

out=$(run_intro 'APROPOS x nopkg')
check "APROPOS with an unknown package is rc 10" 'no such package: nopkg' "$out"

out=$(run_intro 'APROPOS')
check "APROPOS without a string is fatal"    '<<RC=20>>' "$out"

# --- SOURCE-LOCATION -------------------------------------------------------

out=$(run_intro 'SOURCE-LOCATION intro-fn')
check "SOURCE-LOCATION returns rc 0"         '<<RC=0>>' "$out"
check "SOURCE-LOCATION is file:line of the DEFUN" 'intro.lisp:3$' "$out"

out=$(run_intro 'SOURCE-LOCATION intro-mac')
check "SOURCE-LOCATION of a macro"           'intro.lisp:6$' "$out"

# A generic function has no body of its own: the first method with a
# recorded location stands in.  From the source, since a method body
# loaded from a FASL is compiled at load time and carries no line -- and
# then the honest answer is rc 10, not "file:0".
cat > "$TMPD/gf-loc.lisp" <<EOF
(require "dev-commands")
(load "$TMPD/intro.lisp")
(multiple-value-bind (rc text) (ext.dev:handle-command "SOURCE-LOCATION intro-gf")
  (format t "~&<<SRC rc=~d>>~%~a~%<<END>>~%" rc text))
EOF
out=$(run_script "$TMPD/gf-loc.lisp")
check "SOURCE-LOCATION of a generic function is its first method" 'intro.lisp:10$' "$out"
out=$(run_intro 'SOURCE-LOCATION intro-gf')
check "SOURCE-LOCATION without a line is rc 10, not file:0" '<<RC=10>>' "$out"

out=$(run_intro 'SOURCE-LOCATION cl:mapcar')
check "SOURCE-LOCATION of a builtin is rc 10" '<<RC=10>>' "$out"
check "SOURCE-LOCATION explains"             'no source location recorded for mapcar' "$out"

# --- MACROEXPAND -----------------------------------------------------------

out=$(run_intro 'MACROEXPAND-1 (intro-mac (+ 1 2) (print y))')
check "MACROEXPAND-1 returns rc 0"           '<<RC=0>>' "$out"
check "MACROEXPAND-1 expands once, lower case" '^(let ((y (+ 1 2))) (print y))$' "$out"

# A long expansion is laid out like code: body forms indented by two under
# WITH-OPEN-FILE's LET, not filled to the margin.
cat > "$TMPD/mx.lisp" <<'EOF'
(require "dev-commands")
(multiple-value-bind (rc text)
    (ext.dev:handle-command "MACROEXPAND (with-open-file (s \"foo\" :direction :output :if-exists :supersede) (format s \"hello ~a\" 1) (format s \"world\"))")
  (format t "~&<<RC=~d>>~%~a~%<<END>>~%" rc text))
EOF
out=$(run_script "$TMPD/mx.lisp")
check "MACROEXPAND lays the expansion out"   '^(let ((s (open "foo" :direction :output :if-exists :supersede)))$' "$out"
check "MACROEXPAND indents the body by two"  '^  (unwind-protect' "$out"

out=$(run_intro 'MACROEXPAND (intro-fn 1 2)')
check "MACROEXPAND of a non-macro form returns it" '^(intro-fn 1 2)$' "$out"

out=$(run_intro 'MACROEXPAND (')
check "MACROEXPAND of an unreadable form is rc 10" '<<RC=10>>' "$out"

out=$(run_intro 'MACROEXPAND')
check "MACROEXPAND without a form is fatal"  '<<RC=20>>' "$out"

# --- REPL (lib/dev-repl.lisp) ----------------------------------------------
#
# The REPL runs on its own thread and talks back to the editor's port through
# EXT.DEV:*REPL-SEND*.  Here a Lisp function stands in for the editor: it
# records what the REPL thread sends (OUTPUT, READLINE, RESULT) and answers a
# READLINE with REPL-INPUT, as the editor would.  One image for the whole
# conversation, since the thread's state is the point.

cat > "$TMPD/repl.lisp" <<'EOF'
(require "dev-commands")
(defvar *sent* '())
(defvar *sent-lock* (mp:make-lock))
(defvar *editor*
  (lambda (port command)
    (mp:with-lock-held (*sent-lock*) (push (cons port command) *sent*))
    (when (string= command "READLINE")
      (ext.dev:handle-command "REPL-INPUT typed line"))
    (values 0 "")))
(setf ext.dev:*repl-send* *editor*)
(defun result-p (command)
  (and (>= (length command) 6) (string= "RESULT" command :end2 6)))
(defun wait-result ()
  (loop repeat 500
        do (when (mp:with-lock-held (*sent-lock*) (find-if #'result-p *sent* :key #'cdr))
             (return t))
           (sleep 0.02)))
(defun show (label)
  (format t "~&<<~a>>~%" label)
  (dolist (s (reverse *sent*))
    (format t "SENT ~a|~a~%" (car s) (cdr s)))
  (format t "<<END ~a>>~%" label)
  (setf *sent* '()))
(defun cmd (label c)
  (multiple-value-bind (rc text) (ext.dev:handle-command c)
    (format t "~&<<~a RC=~d>>~%~a~%<<END>>~%" label rc text)))
(defun thread-alive ()
  (and ext.dev::*repl-thread* (mp:thread-alive-p ext.dev::*repl-thread*) t))

(cmd "eval-unattached" "REPL-EVAL (+ 1 2)")
(cmd "attach-noport" "REPL-ATTACH")
(cmd "attach" "REPL-ATTACH EDITOR")
(cmd "eval" "REPL-EVAL (progn (princ \"hello\") (terpri) (princ \"there\") (+ 2 3))")
(wait-result) (show "eval")
(cmd "history" "REPL-EVAL (* * 10)")
(wait-result) (show "history")
;; The REPL thread's history variables (*, **, ... see %REPL-EVAL) must be
;; bound locally to that thread, not the global slot the physical console's
;; REPL (src/core/repl.c's cl_repl_update_history) also writes through.
;; This script's own top-level forms run under cl_repl_batch, which never
;; touches these symbols, so the global * stays NIL unless %REPL-LOOP fails
;; to shadow it -- in which case the REPL-EVAL above would have set it to 50.
(format t "<<MAIN-STAR=~a>>~%" *)
(cmd "two-values" "REPL-EVAL (values 1 2)")
(wait-result) (show "two values")
(cmd "no-values" "REPL-EVAL (values)")
(wait-result) (show "no values")
(cmd "in-package" "REPL-EVAL (in-package :ext.dev)")
(wait-result) (show "in-package")
(format t "<<COMMAND-PACKAGE=~a>>~%" (package-name ext.dev:*command-package*))
(cmd "editor-in-package" "IN-PACKAGE CL-USER")
(cmd "error" "REPL-EVAL (progn (princ \"before\") (error \"boom\"))")
(wait-result) (show "error")
(cmd "warning" "REPL-EVAL (warn \"careful\")")
(wait-result) (show "warning")
(cmd "readline" "REPL-EVAL (read-line)")
(wait-result) (show "readline")
(cmd "stray" "REPL-INPUT stray")
(cmd "chunks" "REPL-EVAL (princ (make-string 2500 :initial-element #\\x))")
(wait-result)
(format t "<<CHUNKS=~d>>~%"
        (count-if (lambda (s) (string= "OUTPUT" (cdr s) :end2 6)) *sent*))
(format t "<<TOTAL=~d>>~%"
        (reduce #'+ (mapcar (lambda (s) (if (string= "OUTPUT" (cdr s) :end2 6)
                                            (- (length (cdr s)) 7)
                                            0))
                            *sent*)))
(setf *sent* '())
(cmd "interrupt-idle" "REPL-INTERRUPT")
(cmd "eval-loop" "REPL-EVAL (loop)")
(sleep 0.3)
(cmd "eval-busy" "REPL-EVAL (+ 1 1)")
(cmd "interrupt" "REPL-INTERRUPT")
(wait-result) (show "interrupt")
(cmd "eval-after" "REPL-EVAL (+ 1 2)")
(wait-result) (show "after interrupt")
(cmd "reattach" "REPL-ATTACH EDITOR2")
(cmd "eval-reattached" "REPL-EVAL (+ 20 22)")
(wait-result) (show "reattached")
(cmd "detach" "REPL-DETACH")
(cmd "eval-detached" "REPL-EVAL (+ 1 2)")
(format t "<<THREAD-ALIVE-AFTER-DETACH=~a>>~%" (thread-alive))
;; The editor goes away: the transport fails, the REPL stops itself.
(cmd "attach2" "REPL-ATTACH EDITOR")
(setf ext.dev:*repl-send* (lambda (port command) (error "port ~a is gone (~a)" port command)))
(cmd "eval-gone" "REPL-EVAL (+ 1 1)")
(loop repeat 250 while (thread-alive) do (sleep 0.02))
(format t "<<THREAD-ALIVE-AFTER-FAILURE=~a>>~%" (thread-alive))
(setf ext.dev:*repl-send* *editor*)
(cmd "attach3" "REPL-ATTACH EDITOR")
(cmd "eval-recovered" "REPL-EVAL (+ 1 1)")
(wait-result) (show "recovered")
;; An editor that sends the next form the moment RESULT arrives -- inside
;; the send, as a macro or an in-process editor does -- must find the REPL
;; idle: RESULT is the REPL's promise that it is.
(defvar *chained* nil)
(setf ext.dev:*repl-send*
      (lambda (port command)
        (funcall *editor* port command)
        (when (and (result-p command) (null *chained*))
          (setf *chained* (multiple-value-list (ext.dev:handle-command "REPL-EVAL (+ 40 2)"))))
        (values 0 "")))
(cmd "eval-chain" "REPL-EVAL (+ 1 1)")
(loop repeat 500
      until (>= (mp:with-lock-held (*sent-lock*) (count-if #'result-p *sent* :key #'cdr)) 2)
      do (sleep 0.02))
(format t "<<CHAINED=~a ~a>>~%" (first *chained*) (second *chained*))
(show "chain")
(setf ext.dev:*repl-send* *editor*)
(cmd "detach2" "REPL-DETACH")
EOF
# Not $out: check() assigns the global `out` itself, so a block extracted
# from it would be read from the previous check's argument.
repl_out=$(run_script "$TMPD/repl.lisp")
block() { echo "$repl_out" | sed -n "/<<$1>>/,/<<END $1>>/p"; }

check "REPL-EVAL before the REPL is loaded is unknown" '<<eval-unattached RC=20>>' "$repl_out"
check "REPL-ATTACH needs a port"                '<<attach-noport RC=20>>' "$repl_out"
check "REPL-ATTACH returns rc 0"                '<<attach RC=0>>'  "$repl_out"
check "REPL-ATTACH answers the package"         '^CL-USER$'        "$repl_out"
check "REPL-EVAL replies at once"               '<<eval RC=0>>'    "$repl_out"
check "output is streamed line by line"         'SENT EDITOR|OUTPUT hello$' "$(block eval)"
check "the last line is flushed before RESULT"  'SENT EDITOR|OUTPUT there$' "$(block eval)"
check "RESULT carries rc and package"           '^SENT EDITOR|RESULT 0 CL-USER$' "$(block eval)"
check "RESULT carries the value"                '^5$'              "$(block eval)"
check "output precedes RESULT" 'OUTPUT there' "$(block eval | sed -n '1,/RESULT/p')"
check "the history variables are kept"          '^50$'             "$(block history)"
check "the REPL thread's history is private, not the console's global *" '<<MAIN-STAR=NIL>>' "$repl_out"
check "several values, one per line"            '^1$'              "$(block 'two values')"
check "no values says so"                       '; No values'      "$(block 'no values')"
check "IN-PACKAGE at the REPL changes the prompt" 'RESULT 0 EXT.DEV' "$(block in-package)"
check "IN-PACKAGE at the REPL reaches the commands" '<<COMMAND-PACKAGE=EXT.DEV>>' "$repl_out"
check "output before an error still arrives"    'OUTPUT before'    "$(block error)"
check "an error is rc 10 in RESULT"             'RESULT 10 CL-USER' "$(block error)"
check "the editor's IN-PACKAGE reaches the REPL" 'RESULT 10 CL-USER' "$(block error)"
check "RESULT carries the error text"           'ERROR: boom'      "$(block error)"
check "a warning is printed as output"          'OUTPUT WARNING: careful' "$(block warning)"
check "a warning does not fail the form"        'RESULT 0 CL-USER' "$(block warning)"
check "READ-LINE asks the editor"               '^SENT EDITOR|READLINE$' "$(block readline)"
check "REPL-INPUT is the line read"             '"typed line"'     "$(block readline)"
check "REPL-INPUT with no READLINE outstanding is rc 10" '<<stray RC=10>>' "$repl_out"
check "a big write goes out in chunks"          '<<CHUNKS=3>>'     "$repl_out"
check "the chunks add up to the text"           '<<TOTAL=2500>>'   "$repl_out"
check "REPL-INTERRUPT when idle is rc 0"        'the REPL is idle' "$repl_out"
check "REPL-EVAL while a form runs is rc 10"    '<<eval-busy RC=10>>' "$repl_out"
check "REPL-INTERRUPT is rc 0"                  '<<interrupt RC=0>>' "$repl_out"
check "the interrupted form is rc 10"           'RESULT 10 CL-USER' "$(block interrupt)"
check "the interrupted form says so"            'ERROR: Interrupted' "$(block interrupt)"
check "the REPL works after an interrupt"       '^3$'              "$(block 'after interrupt')"
check "REPL-ATTACH again moves to the new port" 'SENT EDITOR2|RESULT 0 CL-USER' "$(block reattached)"
check "REPL-DETACH is rc 0"                     '<<detach RC=0>>'  "$repl_out"
check "REPL-EVAL after detach is rc 10"         '<<eval-detached RC=10>>' "$repl_out"
check "REPL-DETACH stops the thread"            '<<THREAD-ALIVE-AFTER-DETACH=NIL>>' "$repl_out"
check "a transport failure stops the REPL"      '<<THREAD-ALIVE-AFTER-FAILURE=NIL>>' "$repl_out"
check "REPL-ATTACH works again after that"      '<<attach3 RC=0>>' "$repl_out"
check "and so does the REPL"                    '^2$'              "$(block recovered)"
check "RESULT means idle: the next REPL-EVAL sent on it is taken" '<<CHAINED=0 >>' "$repl_out"
check "and that form runs"                      '^42$'             "$(block chain)"

# --- A REPL thread that dies ------------------------------------------------
#
# An exhausted heap is the one error %REPL-RUN's handler never sees: the
# runtime drops the thread's handlers and leaves through the thread's entry.
# The form still ends with RESULT (rc 10, the error), sent from the dying
# thread through MP:*THREAD-DEATH-HOOKS*, and a new REPL thread has taken
# its place by then -- with DEBUG attached too, where the debugger must not
# be what the editor hears of instead.

cat > "$TMPD/repl-death.lisp" <<'EOF'
(require "dev-commands")
(defvar *keep* nil)
(defvar *sent* '())
(defvar *sent-lock* (mp:make-lock))
(defun result-p (command)
  (and (>= (length command) 6) (string= "RESULT" command :end2 6)))
(setf ext.dev:*repl-send*
      (lambda (port command)
        (mp:with-lock-held (*sent-lock*) (push (cons port command) *sent*))
        (values 0 "")))
(defun cmd (label command)
  (multiple-value-bind (rc text) (ext.dev:handle-command command)
    (format t "<<~a RC=~d>>~%~a~%" label rc text)))
(defun results ()
  (mp:with-lock-held (*sent-lock*) (count-if #'result-p *sent* :key #'cdr)))
(defun show (label expected)
  (loop repeat 1200 until (>= (results) expected) do (sleep 0.05))
  (format t "<<~a>>~%" label)
  (dolist (entry (reverse (mp:with-lock-held (*sent-lock*)
                            (prog1 *sent* (setf *sent* '())))))
    (format t "SENT ~a|~a~%" (car entry) (cdr entry)))
  (format t "<<END ~a>>~%" label))
(defun eat (option)
  (cmd "attach" (format nil "REPL-ATTACH EDITOR~a" option))
  (let ((before ext.dev::*repl-thread*))
    (cmd "eat" "REPL-EVAL (loop (push (make-string 10000) cl-user::*keep*))")
    (show (format nil "died~a" option) 1)
    (format t "<<OLD-ALIVE~a=~a NEW=~a NEW-ALIVE=~a>>~%" option
            (mp:thread-alive-p before)
            (not (eq before ext.dev::*repl-thread*))
            (mp:thread-alive-p ext.dev::*repl-thread*)))
  (setf *keep* nil)
  (cmd (format nil "eval-after~a" option) "REPL-EVAL (+ 1 2)")
  (show (format nil "after~a" option) 1))
(eat "")
(eat " DEBUG")
(cmd "detach" "REPL-DETACH")
(format t "<<ALIVE-AFTER-DETACH=~a>>~%" (and ext.dev::*repl-thread* t))
EOF
death_out=$("$AMILISP" --heap 4M --no-userinit --batch < "$TMPD/repl-death.lisp" 2>&1)
dblock() { echo "$death_out" | sed -n "/<<$1>>/,/<<END $1>>/p"; }

check "a REPL thread out of heap still sends RESULT, rc 10" \
      '^SENT EDITOR|RESULT 10 CL-USER$' "$(dblock died)"
check "RESULT carries what it died of"          '^ERROR: Heap exhausted' "$(dblock died)"
check "RESULT says a new thread took over"      'a new one has taken its place' "$(dblock died)"
check "the old thread is gone, a new one runs"  '<<OLD-ALIVE=NIL NEW=T NEW-ALIVE=T>>' "$death_out"
check "the next REPL-EVAL is taken"             '<<eval-after RC=0>>' "$death_out"
check "and evaluated by the new thread"         '^3$'              "$(dblock after)"
check "the same under DEBUG: RESULT, rc 10"     '^SENT EDITOR|RESULT 10 CL-USER$' "$(dblock 'died DEBUG')"
check_not "no debugger is announced for it"     'DEBUGGER'         "$(dblock 'died DEBUG')"
check "a new thread there too"                  '<<OLD-ALIVE DEBUG=NIL NEW=T NEW-ALIVE=T>>' "$death_out"
check "which evaluates"                         '^3$'              "$(dblock 'after DEBUG')"
check "REPL-DETACH stops the new thread"        '<<ALIVE-AFTER-DETACH=NIL>>' "$death_out"

# --- The debugger and the inspector (phase 4 of the editor) -----------------
#
# With REPL-ATTACH <port> DEBUG an unhandled error parks the REPL thread on
# the erring stack and announces DEBUGGER <level> to the editor; the editor
# then asks BACKTRACE / FRAME / FRAME-EVAL and picks a RESTART (or ABORT,
# CONTINUE) -- see the header of lib/dev-repl.lisp.  INSPECT / PART / POP
# are synchronous on the handler thread and do not need the REPL, except
# that `INSPECT *' sees the REPL thread's last value.

cat > "$TMPD/debug.lisp" <<'EOF'
(require "dev-commands")
(defvar *sent* '())
(defvar *sent-lock* (mp:make-lock))
(defvar *editor*
  (lambda (port command)
    (mp:with-lock-held (*sent-lock*) (push (cons port command) *sent*))
    (when (string= command "READLINE")
      (ext.dev:handle-command "REPL-INPUT typed line"))
    (values 0 "")))
(setf ext.dev:*repl-send* *editor*)
(defun sent-p (pred)
  (mp:with-lock-held (*sent-lock*) (find-if pred *sent* :key #'cdr)))
(defun prefix-p (prefix s)
  (and (>= (length s) (length prefix)) (string= prefix s :end2 (length prefix))))
(defun wait-for (pred)
  (loop repeat 500 do (when (sent-p pred) (return t)) (sleep 0.02)))
(defun wait-result () (wait-for (lambda (c) (prefix-p "RESULT" c))))
(defun wait-debugger (level)
  (wait-for (lambda (c) (prefix-p (format nil "DEBUGGER ~d" level) c))))
(defun wait-output (needle)
  (wait-for (lambda (c) (and (prefix-p "OUTPUT" c) (search needle c)))))
(defun show (label)
  (format t "~&<<~a>>~%" label)
  (dolist (s (reverse *sent*)) (format t "SENT ~a|~a~%" (car s) (cdr s)))
  (format t "<<END ~a>>~%" label)
  (setf *sent* '()))
(defun cmd (label c)
  (multiple-value-bind (rc text) (ext.dev:handle-command c)
    (format t "~&<<~a RC=~d>>~%~a~%<<END>>~%" label rc text)))
(defun thread-alive ()
  (and ext.dev::*repl-thread* (mp:thread-alive-p ext.dev::*repl-thread*) t))
(defun dbg-fn (a b) (let ((c (* a b))) (error "bad ~a" c)))

(cmd "backtrace-idle" "BACKTRACE")
(cmd "attach-bad-option" "REPL-ATTACH EDITOR VERBOSE")
(cmd "attach-debug" "REPL-ATTACH EDITOR DEBUG")
(cmd "debug-error" "REPL-EVAL (progn (princ \"pre\") (dbg-fn 3 4))")
(wait-debugger 1) (show "debugger")
(cmd "eval-in-debugger" "REPL-EVAL (+ 1 1)")
(cmd "restarts" "RESTARTS")
(format t "<<REPL-DEBUG=~a>>~%" ext.dev::*repl-debug*)
(cmd "backtrace" "BACKTRACE")
(cmd "frame0" "FRAME 0")
(cmd "frame-none" "FRAME 99")
(cmd "frame-eval" "FRAME-EVAL 0 (list arg0 arg1 local3)")
(wait-output "(3 4 12)") (show "frame-eval")
(cmd "frame-eval-error" "FRAME-EVAL 0 (error \"nested\")")
(wait-debugger 2) (show "nested")
(cmd "backtrace2" "BACKTRACE")
(cmd "abort-nested" "ABORT")
(wait-output "Aborted") (sleep 0.1) (show "abort-nested")
(cmd "restart-bad" "RESTART 99")
(cmd "continue-none" "CONTINUE")
(cmd "restart-abort" "RESTART 0")
(wait-result) (show "restart-abort")
(cmd "debug-cerror" "REPL-EVAL (progn (cerror \"Go on\" \"stop\") :went-on)")
(wait-debugger 1) (show "cerror")
(cmd "continue" "CONTINUE")
(wait-result) (show "continue")
(cmd "debug-use-value" "REPL-EVAL (restart-case (error \"x\") (use-value (v) :interactive (lambda () (list (read-line))) v))")
(wait-debugger 1) (show "use-value-entered")
(cmd "restart-use-value" "RESTART 0")
(wait-result) (show "use-value")
(cmd "debug-error2" "REPL-EVAL (error \"y\")")
(wait-debugger 1) (setf *sent* '())
(cmd "interrupt-debugger" "REPL-INTERRUPT")
(wait-result) (show "interrupt-debugger")
(cmd "eval-list" "REPL-EVAL (list :a :b)")
(wait-result) (setf *sent* '())
(cmd "inspect-star" "INSPECT *")
(cmd "inspect-cons" "INSPECT (list 1 (list 2 3))")
(cmd "part-1" "PART 1")
(cmd "pop" "POP")
(cmd "pop-root" "POP")
(cmd "part-bad" "PART 7")
(cmd "inspect-error" "INSPECT (error \"no\")")
(cmd "inspect-none" "INSPECT")
(setf ext.dev:*max-inspect-parts* 3)
(cmd "inspect-limit" "INSPECT (make-array 10 :initial-element 0)")
;; An error raised by the runtime (an unbound variable: cl_error from C,
;; not CL:ERROR) inside a frame eval, a Lisp error nested inside THAT, then
;; ABORT twice: each ABORT must land one level down, and a CONTINUE
;; afterwards must still work.  The host does this right; the m68k build
;; did not (2026-09-11, clamacs's phase-4 leg), so this pins what the JIT
;; fix has to reach.
(cmd "unbound-in-frame" "REPL-EVAL (dbg-fn 3 4)")
(wait-debugger 1) (setf *sent* '())
(cmd "fe-unbound" "FRAME-EVAL 0 (list nosuchvar)")
(wait-debugger 2) (show "unbound-level2")
(cmd "fe-nested" "FRAME-EVAL 0 (dbg-fn 1 2)")
(wait-debugger 3) (setf *sent* '())
(cmd "abort-3" "ABORT")
(wait-for (lambda (c) (prefix-p "OUTPUT ; Aborted" c))) (sleep 0.1)
(cmd "restarts-after-3" "RESTARTS")
(setf *sent* '())
(cmd "abort-2" "ABORT")
(wait-for (lambda (c) (prefix-p "OUTPUT ; Aborted" c))) (sleep 0.1)
(cmd "restarts-after-2" "RESTARTS")
(cmd "abort-1" "ABORT")
(wait-result) (setf *sent* '())
(cmd "cerror-after" "REPL-EVAL (progn (cerror \"Go on\" \"stop\") :still-continues)")
(wait-debugger 1) (setf *sent* '())
(cmd "continue-after" "CONTINUE")
(wait-result) (show "continue-after")
(cmd "debug-error3" "REPL-EVAL (error \"z\")")
(wait-debugger 1) (setf *sent* '())
(cmd "detach-in-debugger" "REPL-DETACH")
(format t "<<THREAD-ALIVE-AFTER-DETACH=~a>>~%" (thread-alive))
;; Without DEBUG an error ends the form as it did before.
(cmd "attach-plain" "REPL-ATTACH EDITOR")
(cmd "plain-error" "REPL-EVAL (error \"plain\")")
(wait-result) (show "plain")
(cmd "detach2" "REPL-DETACH")
EOF
debug_out=$(run_script "$TMPD/debug.lisp")
dblock() { echo "$debug_out" | sed -n "/<<$1>>/,/<<END $1>>/p"; }
# A command reply: `<<label RC=n>>' up to its `<<END>>'.
dreply() { echo "$debug_out" | sed -n "/<<$1 RC=/,/<<END>>/p"; }

check "BACKTRACE before the REPL is loaded is unknown" '<<backtrace-idle RC=20>>' "$debug_out"
check "REPL-ATTACH rejects an unknown option"    '<<attach-bad-option RC=20>>' "$debug_out"
check "REPL-ATTACH takes DEBUG"                  '<<attach-debug RC=0>>' "$debug_out"
check "output before the error still arrives"    'OUTPUT pre'       "$(dblock debugger)"
check "an error announces DEBUGGER 1"            'DEBUGGER 1 CL-USER' "$(dblock debugger)"
check "the condition follows on the next line"   'SIMPLE-ERROR: bad 12' "$(dblock debugger)"
check "the REPL's own ABORT restart is offered"  '0: ABORT Return to the REPL' "$(dblock debugger)"
check "REPL-EVAL in the debugger is rc 10"       '<<eval-in-debugger RC=10>>' "$debug_out"
check "and says so"                              'in the debugger'  "$debug_out"
check "RESTARTS names the level and the condition" '^level 1: SIMPLE-ERROR: bad 12$' "$(dreply restarts)"
check "RESTARTS lists the restarts as DEBUGGER did" '^0: ABORT Return to the REPL$' "$(dreply restarts)"
check "a DEBUG attach is remembered" '<<REPL-DEBUG=T>>' "$debug_out"
check "BACKTRACE lists the erring function first" '^0: dbg-fn' "$(dreply backtrace)"
check "BACKTRACE leaves the REPL's own frames out" '^1: <anonymous>$' "$(dreply backtrace)"
check "FRAME 0 names the arguments"              '^ARG0 = 3$'       "$(dreply frame0)"
check "FRAME 0 shows the LET variable"           '^LOCAL3 = 12$'    "$(dreply frame0)"
check "FRAME of a frame that is not there says so" '; no such frame' "$(dreply frame-none)"
check "FRAME-EVAL replies at once"               '<<frame-eval RC=0>>' "$debug_out"
check "FRAME-EVAL binds the locals by name"      'OUTPUT (3 4 12)'  "$(dblock frame-eval)"
check "an error in FRAME-EVAL is DEBUGGER 2"     'DEBUGGER 2 CL-USER' "$(dblock nested)"
check "level 2 offers a return to level 1"       '0: ABORT Return to debugger level 1' "$(dblock nested)"
check "the nested backtrace still shows level 1's frames" '^1: dbg-fn' "$(dreply backtrace2)"
check "ABORT at level 2 re-announces level 1"    'DEBUGGER 1 CL-USER' "$(dblock abort-nested)"
check "and prints that it aborted"               'OUTPUT ; Aborted' "$(dblock abort-nested)"
check "RESTART out of range is rc 10"            '<<restart-bad RC=10>>' "$debug_out"
check "CONTINUE without a CONTINUE restart is rc 10" 'no CONTINUE restart' "$debug_out"
check "RESTART 0 leaves the debugger"            'DEBUGGER 0 CL-USER' "$(dblock restart-abort)"
check "and the form ends with RESULT"            'RESULT 0 CL-USER'  "$(dblock restart-abort)"
check "aborted, as the values say"               '; Aborted'        "$(dblock restart-abort)"
check "CERROR offers CONTINUE first"             '0: CONTINUE Go on' "$(dblock cerror)"
check "CONTINUE goes on with the form"           ':WENT-ON'         "$(dblock continue)"
check "a restart without a report shows its name alone" '0: USE-VALUE $' "$(dblock use-value-entered)"
check "an interactive restart reads through READLINE" 'READLINE'    "$(dblock use-value)"
check "and its value is the form's"              '"typed line"'     "$(dblock use-value)"
check "REPL-INTERRUPT in the debugger ends the form" 'ERROR: Interrupted' "$(dblock interrupt-debugger)"
check "INSPECT * sees the REPL's last value"     '^(:A :B)$'        "$(dreply inspect-star)"
check "INSPECT starts with a header"             '^CONS 1 2$'       "$(dreply inspect-cons)"
check "INSPECT numbers the parts"                '^1: Cdr = ((2 3))$' "$(dreply inspect-cons)"
check "PART descends and the depth grows"        '^CONS 2 2$'       "$(dreply part-1)"
check "PART shows the part's own parts"          '^0: Car = (2 3)$' "$(dreply part-1)"
check "POP comes back"                           '^CONS 1 2$'       "$(dreply pop)"
check "POP at the root is rc 10"                 '<<pop-root RC=10>>' "$debug_out"
check "PART out of range is rc 10"               'no part 7 (the object has 2)' "$debug_out"
check "INSPECT of a failing form is rc 10"       '<<inspect-error RC=10>>' "$debug_out"
check "INSPECT without a form is rc 20"          '<<inspect-none RC=20>>' "$debug_out"
check "the part list is capped, the header says the count" '^SIMPLE-VECTOR 1 10$' "$(dreply inspect-limit)"
check "and lists only that many"                 '^2: \[2\] = 0$'   "$(dreply inspect-limit)"
check "a runtime error in a frame eval is a nested level" 'UNBOUND-VARIABLE: Unbound variable: NOSUCHVAR' "$(dblock unbound-level2)"
check "ABORT from level 3 lands at level 2"       '^level 2: UNBOUND-VARIABLE' "$(dreply restarts-after-3)"
check "ABORT from level 2 lands at level 1"       '^level 1: SIMPLE-ERROR: bad 12' "$(dreply restarts-after-2)"
check "CONTINUE still works after that"           ':STILL-CONTINUES' "$(dblock continue-after)"
check "REPL-DETACH while parked stops the thread" '<<THREAD-ALIVE-AFTER-DETACH=NIL>>' "$debug_out"
check "without DEBUG an error is RESULT 10 as before" 'ERROR: plain' "$(dblock plain)"

# --- Systems (LOAD-ASD-SYSTEM / TEST-ASD-SYSTEM) ----------------------------
#
# What an editor's Load System evaluates: the system is named by its .asd
# file, ASDF is loaded on first use, and the system loaded is the one the
# file is named after -- or the one the caller names, of the file's
# others (an editor that read the file's defsystems and asked).

mkdir "$TMPD/devsys"
cat > "$TMPD/devsys/devsys.asd" <<'EOF'
(asdf:defsystem "devsys"
  :components ((:file "one") (:file "two" :depends-on ("one")))
  :in-order-to ((asdf:test-op (asdf:test-op "devsys/tests"))))
(asdf:defsystem "devsys/tests"
  :depends-on ("devsys")
  :components ((:file "tests"))
  :perform (asdf:test-op (o c) (uiop:symbol-call :devsys :run-tests)))
EOF
cat > "$TMPD/devsys/one.lisp" <<'EOF'
(defpackage :devsys (:use :cl) (:export #:one #:two #:run-tests))
(in-package :devsys)
(defvar *one-loads* 0)
(incf *one-loads*)
(defun one () 1)
EOF
cat > "$TMPD/devsys/two.lisp" <<'EOF'
(in-package :devsys)
(defun two () (1+ (one)))
EOF
cat > "$TMPD/devsys/tests.lisp" <<'EOF'
(in-package :devsys)
(defun run-tests () (format t "~&<<DEVSYS-TESTS two=~a>>~%" (two)))
EOF

# A list of directories in the environment, which is what ASDF reads its
# source registry from (XDG_DATA_DIRS): entries are separated by ":", and by
# ";" on Windows, where each of them has a colon after its drive letter.
# What Windows itself puts there is spelled with "\", so the second entry is.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) dirsep=';'; dir2=$(printf '%s' "$TMPD/" | tr '/' '\\') ;;
    *)                    dirsep=':'; dir2="$TMPD/" ;;
esac
cat > "$TMPD/dirlist.lisp" <<EOF
(require "asdf")
(let ((dirs (uiop:getenv-pathnames "AMILISP_TEST_DIRS" :want-absolute t)))
  (format t "~&<<DIRS=~d>>~%" (length dirs))
  (format t "~&<<DIRS-EXIST=~a>>~%"
          (and dirs (every (function uiop:directory-exists-p) dirs) t)))
EOF
dirs_out=$(AMILISP_TEST_DIRS="$TMPD/devsys/$dirsep$dir2" run_script "$TMPD/dirlist.lisp")
check "a directory list in the environment is split at the separator" '<<DIRS=2>>' "$dirs_out"
check "and each entry is a directory that exists" '<<DIRS-EXIST=T>>' "$dirs_out"

cat > "$TMPD/systems.lisp" <<EOF
(require "dev-commands")
(format t "~&<<ASDF-BEFORE=~a>>~%" (and (find-package "ASDF") t))
(multiple-value-bind (rc text)
    (ext.dev:handle-command "EVAL (ext.dev:load-asd-system \"$TMPD/devsys/one.lisp\")")
  (format t "~&<<not-asd RC=~d>>~%~a~%" rc text))
(multiple-value-bind (rc text)
    (ext.dev:handle-command "EVAL (ext.dev:load-asd-system \"$TMPD/devsys/nope.asd\")")
  (format t "~&<<missing RC=~d>>~%~a~%" rc text))
(multiple-value-bind (rc text)
    (ext.dev:handle-command "EVAL (ext.dev:load-asd-system \"$TMPD/devsys/devsys.asd\" 42)")
  (format t "~&<<bad-name RC=~d>>~%~a~%" rc text))
(format t "~&<<ASDF-AFTER-REFUSALS=~a>>~%" (and (find-package "ASDF") t))
(format t "~&<<LOADED=~s>>~%" (ext.dev:load-asd-system "$TMPD/devsys/devsys.asd"))
(format t "~&<<TWO=~a>>~%" (funcall (find-symbol "TWO" "DEVSYS")))
(format t "~&<<TESTS-LOADED-BY-LOAD=~a>>~%" (and (fboundp (find-symbol "RUN-TESTS" "DEVSYS")) t))
(format t "~&<<LOADED-NAMED=~s>>~%" (ext.dev:load-asd-system "$TMPD/devsys/devsys.asd" "devsys/tests"))
(format t "~&<<TESTS-LOADED-BY-NAME=~a>>~%" (and (fboundp (find-symbol "RUN-TESTS" "DEVSYS")) t))
(format t "~&<<TESTED=~s>>~%" (ext.dev:test-asd-system "$TMPD/devsys/devsys.asd"))
;; The .asd was loaded once for all of that: ASDF has the systems, and
;; loads and tests what it has incrementally.  Loading the file on every
;; call redefined them, and ASDF then reloaded every file each time.
(defun one-loads () (symbol-value (find-symbol "*ONE-LOADS*" "DEVSYS")))
(format t "~&<<ONE-LOADS=~a>>~%" (one-loads))
(format t "~&<<LOADED-AGAIN=~s>>~%" (ext.dev:load-asd-system "$TMPD/devsys/devsys.asd"))
(format t "~&<<TESTED-AGAIN=~s>>~%" (ext.dev:test-asd-system "$TMPD/devsys/devsys.asd"))
(format t "~&<<ONE-LOADS-AGAIN=~a>>~%" (one-loads))
;; A changed .asd ASDF reloads by itself (FIND-SYSTEM compares the date),
;; and with it the system.  A second later, for the date to differ.
(sleep 1.1)
(with-open-file (o "$TMPD/devsys/devsys.asd" :direction :output :if-exists :append)
  (format o "~%;; changed~%"))
(format t "~&<<LOADED-CHANGED=~s>>~%" (ext.dev:load-asd-system "$TMPD/devsys/devsys.asd"))
(format t "~&<<ONE-LOADS-CHANGED=~a>>~%" (one-loads))
EOF
sys_out=$(run_script "$TMPD/systems.lisp")
sys_failed_before=$failed
check "ASDF is not loaded before a system is asked for" '<<ASDF-BEFORE=NIL>>' "$sys_out"
check "a file that is no .asd is refused"        '<<not-asd RC=10>>'  "$sys_out"
check "and the refusal names it"                 'Not a system definition file (.asd): .*one.lisp' "$sys_out"
check "a missing .asd is refused"                '<<missing RC=10>>'  "$sys_out"
check "and that refusal names it"                'No such system definition file: .*nope.asd' "$sys_out"
check "a system name that is no string is refused" '<<bad-name RC=10>>' "$sys_out"
check "and that refusal says so"                 'Not a system name: 42' "$sys_out"
check "a refusal did not load ASDF"              '<<ASDF-AFTER-REFUSALS=NIL>>' "$sys_out"
check "LOAD-ASD-SYSTEM says what it loads"       '; loading system devsys from .*devsys.asd' "$sys_out"
check "and returns the system's name"            '<<LOADED="devsys">>' "$sys_out"
check "the system's files are loaded in order"   '<<TWO=2>>'          "$sys_out"
check "the test system is not loaded by a load"  '<<TESTS-LOADED-BY-LOAD=NIL>>' "$sys_out"
check "a named system of the file is loaded instead" '; loading system devsys/tests from .*devsys.asd' "$sys_out"
check "and its name is returned"                 '<<LOADED-NAMED="devsys/tests">>' "$sys_out"
check "and it is the one loaded"                 '<<TESTS-LOADED-BY-NAME=T>>' "$sys_out"
check "TEST-ASD-SYSTEM runs the test-op"         '<<DEVSYS-TESTS two=2>>' "$sys_out"
check "and returns the tested system's name"     '<<TESTED="devsys">>' "$sys_out"
check "the .asd was loaded once for the load, the named load and the test" '<<ONE-LOADS=1>>' "$sys_out"
check "a second load does not reload the .asd"   '<<LOADED-AGAIN="devsys">>' "$sys_out"
check "nor does a second test"                   '<<TESTED-AGAIN="devsys">>' "$sys_out"
check "and neither reloads the system's files"   '<<ONE-LOADS-AGAIN=1>>' "$sys_out"
check "but the second test ran the test-op again" '<<DEVSYS-TESTS two=2>>.*<<DEVSYS-TESTS two=2>>' "$(echo "$sys_out" | tr '\n' ' ')"
check "a changed .asd is reloaded, by ASDF, with the system" '<<ONE-LOADS-CHANGED=2>>' "$sys_out"
check "and the file's second load is the only other" '^2$' "$(echo "$sys_out" | grep -c 'Loading .*devsys\.asd')"
# check shows the head of the output only, and what went wrong with a system
# is said after the refusals -- so a failure here prints all of it.
if [ "$failed" -ne "$sys_failed_before" ]; then
    echo "    --- full output of the systems script ---"
    echo "$sys_out" | sed 's/^/    | /'
fi

echo ""
echo "test_dev_commands: $passed passed, $failed failed, $total total"
[ "$failed" -eq 0 ]
