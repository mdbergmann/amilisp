#!/bin/sh
# A FASL is only valid against the structure layouts it was compiled with.
#
# DEFSTRUCT's compiler macros bake a layout into the CALLER: an accessor call
# becomes OP_STRUCT_REF/SET with a constant slot index, a keyword constructor
# call becomes a positional %MAKE-STRUCT with the defaults spliced in, and an
# (:INCLUDE parent) copies the parent's slot specs.  LOAD caches a compiled
# file keyed by that file's OWN mtime, so editing the DEFSTRUCT in a.lisp left
# the cached b.lisp reading the wrong slot (2026-09-17, clamacs: a slot added
# in front of EDITOR's made the untouched test file fail with "CDR: argument
# is not of type LIST (got STRUCTURE)"; through SETF it is silent corruption).
#
# Every FASL now carries a DEPS trailer — a layout hash per structure it
# depends on — that is compared against the live registry before any unit
# runs.  LOAD's cache recompiles on a mismatch; a FASL loaded by name signals.
# --no-fasl-cache / AMILISP_FASL_CACHE=0 switch the implicit cache off, which
# is the answer for what the trailer cannot see (a changed macro or inline
# function in another file).
#
# Run: sh tests/test_fasl_struct_deps.sh [path-to-amilisp]

AMILISP="${1:-build/host/amilisp}"
case "$AMILISP" in
    /*|[A-Za-z]:/*) ;;
    *) AMILISP="$(cd "$(dirname "$AMILISP")" && pwd)/$(basename "$AMILISP")" ;;
esac

passed=0
failed=0
TMP="${TMPDIR:-/tmp}/clamiga-structdeps-$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

check() {
    desc="$1"; cond="$2"
    if [ "$cond" = "yes" ]; then
        echo "  ok  $desc"; passed=$((passed + 1))
    else
        echo "  FAIL  $desc"; failed=$((failed + 1))
        [ -n "$out" ] && echo "$out" | sed 's/^/      | /'
    fi
}
has()  { echo "$out" | grep -q -- "$1" && echo yes || echo no; }
hasnt() { echo "$out" | grep -q -- "$1" && echo no || echo yes; }

# run DIR [extra amilisp args...] — load run.lisp from inside DIR with a cache
# private to DIR; relative names keep the generated Lisp free of host paths.
run() {
    dir="$1"; shift
    out=$(cd "$dir" && AMILISP_FASL_CACHE_DIR="$dir/cache" \
          "$AMILISP" --no-userinit --non-interactive "$@" --load run.lisp \
          </dev/null 2>&1)
}
# An edit must land in a later second than the cache entry written before it:
# the cache's own mtime test has one-second resolution.
edit_pause() { sleep 1; }
nfasl() { find "$1/cache" -name '*.fasl' 2>/dev/null | wc -l | tr -d ' '; }

# --- 1. accessor read, SETF, keyword constructor -----------------------------
d="$TMP/acc"; mkdir -p "$d"
printf '(defstruct editor documents (name "n"))\n' > "$d/a.lisp"
cat > "$d/b.lisp" <<'EOF'
(defun docs (e) (editor-documents e))
(defun set-docs (e v) (setf (editor-documents e) v))
(defun mk () (make-editor :documents '(d1)))
EOF
cat > "$d/run.lisp" <<'EOF'
(load "a.lisp")
(load "b.lisp")
(let ((e (mk)))
  (format t "READ ~S~%" (docs e))
  (set-docs e '(d2))
  (format t "WRITE ~S ~S~%" (editor-documents e) (editor-name e))
  (format t "SLOTS ~S~%" (amilisp::%struct-slot-count 'editor)))
EOF
run "$d"
check "fresh_load_is_correct" "$(has 'READ (D1)')"
run "$d"
check "unchanged_struct_loads_b_from_cache" "$(has 'Loading .*cache.*b\.fasl')"
check "unchanged_struct_no_recompile" "$(hasnt 'Recompiling')"
check "cached_load_is_correct" "$(has 'WRITE (D2) "n"')"

edit_pause
printf '(defstruct editor frontend documents (name "n"))\n' > "$d/a.lisp"
run "$d"
check "slot_prepended_recompiles_untouched_b" "$(has 'Recompiling .*b\.lisp: structure COMMON-LISP-USER::EDITOR changed')"
check "slot_prepended_read_hits_right_slot" "$(has 'READ (D1)')"
check "slot_prepended_setf_hits_right_slot" "$(has 'WRITE (D2) "n"')"
check "slot_prepended_new_layout_live" "$(has 'SLOTS 3')"
check "slot_prepended_no_error" "$(hasnt 'not of type')"
run "$d"
check "recompiled_b_is_cached_again" "$(has 'Loading .*cache.*b\.fasl')"
check "recompiled_b_not_recompiled_twice" "$(hasnt 'Recompiling')"

# --- 2. keyword constructor: a changed DEFAULT is inlined into callers ------
d="$TMP/ctor"; mkdir -p "$d"
printf '(defstruct pt (x 1) (y 2))\n' > "$d/a.lisp"
printf '(defun mk () (make-pt :x 10))\n' > "$d/b.lisp"
cat > "$d/run.lisp" <<'EOF'
(load "a.lisp")
(load "b.lisp")
(format t "PT ~S ~S~%" (pt-x (mk)) (pt-y (mk)))
EOF
run "$d"
check "ctor_fresh" "$(has 'PT 10 2')"
edit_pause
printf '(defstruct pt (x 1) (y 99))\n' > "$d/a.lisp"
run "$d"
check "ctor_default_changed_recompiles_caller" "$(has 'Recompiling .*b\.lisp')"
check "ctor_default_changed_new_default_used" "$(has 'PT 10 99')"

# --- 2b. same, but the default is a heap-boxed number: the layout hash used
# to fall back to "heap type tag only" for anything that wasn't a fixnum,
# character, symbol, string or cons -- so a FLOAT or BIGNUM default's own
# VALUE never affected the hash and a changed default silently kept loading
# the stale caller from cache. ---------------------------------------------
edit_pause
printf '(defstruct pt (x 1) (y 2.0))\n' > "$d/a.lisp"
run "$d"
check "ctor_float_default_recompiles_caller" "$(has 'Recompiling .*b\.lisp')"
check "ctor_float_default_used" "$(has 'PT 10 2\.0')"
edit_pause
printf '(defstruct pt (x 1) (y 3.0))\n' > "$d/a.lisp"
run "$d"
check "ctor_float_default_changed_recompiles_caller" "$(has 'Recompiling .*b\.lisp')"
check "ctor_float_default_changed_new_default_used" "$(has 'PT 10 3\.0')"

edit_pause
printf '(defstruct pt (x 1) (y 100000000000))\n' > "$d/a.lisp"
run "$d"
check "ctor_bignum_default_recompiles_caller" "$(has 'Recompiling .*b\.lisp')"
check "ctor_bignum_default_used" "$(has 'PT 10 100000000000')"
edit_pause
printf '(defstruct pt (x 1) (y 200000000000))\n' > "$d/a.lisp"
run "$d"
check "ctor_bignum_default_changed_recompiles_caller" "$(has 'Recompiling .*b\.lisp')"
check "ctor_bignum_default_changed_new_default_used" "$(has 'PT 10 200000000000')"

# --- 2c. same, but the default is a non-keyword symbol: the layout hash used
# to hash such a symbol by NAME ONLY, ignoring its home package -- so two
# same-named symbols from different packages hashed identically and a
# default changed from one package's X to another's went undetected. -------
d="$TMP/symdef"; mkdir -p "$d"
cat > "$d/a.lisp" <<'EOF'
(defpackage :symdef-p1 (:use :cl) (:export #:x))
(defpackage :symdef-p2 (:use :cl) (:export #:x))
(defstruct sd (tag 'symdef-p1:x))
EOF
printf '(defun mk () (make-sd))\n' > "$d/b.lisp"
cat > "$d/run.lisp" <<'EOF'
(load "a.lisp")
(load "b.lisp")
(format t "SD ~S~%" (sd-tag (mk)))
EOF
run "$d"
check "ctor_symbol_default_fresh" "$(has 'SD SYMDEF-P1:X')"
edit_pause
cat > "$d/a.lisp" <<'EOF'
(defpackage :symdef-p1 (:use :cl) (:export #:x))
(defpackage :symdef-p2 (:use :cl) (:export #:x))
(defstruct sd (tag 'symdef-p2:x))
EOF
run "$d"
check "ctor_symbol_default_package_changed_recompiles_caller" "$(has 'Recompiling .*b\.lisp')"
check "ctor_symbol_default_package_changed_new_default_used" "$(has 'SD SYMDEF-P2:X')"

# --- 3. (:include parent): the child's file depends on the parent's layout ---
d="$TMP/incl"; mkdir -p "$d"
printf '(defstruct base id)\n' > "$d/a.lisp"
cat > "$d/b.lisp" <<'EOF'
(defstruct (child (:include base)) tag)
EOF
cat > "$d/run.lisp" <<'EOF'
(load "a.lisp")
(load "b.lisp")
(let ((c (make-child :id 1 :tag :t1)))
  (format t "CHILD ~S ~S ~S~%" (child-id c) (child-tag c)
          (amilisp::%struct-slot-count 'child)))
EOF
run "$d"
check "include_fresh" "$(has 'CHILD 1 :T1 2')"
edit_pause
printf '(defstruct base rev id)\n' > "$d/a.lisp"
run "$d"
check "include_parent_changed_recompiles_child_file" "$(has 'Recompiling .*b\.lisp: structure COMMON-LISP-USER::BASE changed')"
check "include_parent_changed_child_has_new_slot" "$(has 'CHILD 1 :T1 3')"

# --- 4. a file that defines the structure it uses ---------------------------
# At check time the structure does not exist yet: not a mismatch.
d="$TMP/self"; mkdir -p "$d"
cat > "$d/s.lisp" <<'EOF'
(defpackage :selfpkg (:use :cl))
(in-package :selfpkg)
(defstruct own a b)
(defun own-sum (o) (+ (own-a o) (own-b o)))
(format t "OWN ~S~%" (own-sum (make-own :a 1 :b 2)))
EOF
printf '(load "s.lisp")\n' > "$d/run.lisp"
run "$d"
check "self_defining_fresh" "$(has 'OWN 3')"
run "$d"
check "self_defining_cached" "$(has 'Loading .*cache.*s\.fasl')"
check "self_defining_cached_correct" "$(has 'OWN 3')"
check "self_defining_no_recompile" "$(hasnt 'Recompiling')"
# ... and loading it twice in ONE process (structure now registered, same
# layout) is not a mismatch either.
printf '(load "s.lisp")\n(load "s.lisp")\n' > "$d/run.lisp"
run "$d"
check "self_defining_reload_same_process" "$(hasnt 'Recompiling')"

# --- 5. an explicit FASL is not recompiled behind the caller's back ---------
d="$TMP/cf"; mkdir -p "$d"
printf '(defstruct rec k v)\n' > "$d/a.lisp"
printf '(defun rec-val (r) (rec-v r))\n' > "$d/b.lisp"
cat > "$d/run.lisp" <<'EOF'
(load "a.lisp")
(compile-file "b.lisp" :output-file "b-out.fasl")
(load "b-out.fasl")
(format t "CF ~S~%" (rec-val (make-rec :k 1 :v 2)))
EOF
run "$d"
check "compile_file_fresh" "$(has 'CF 2')"
edit_pause
printf '(defstruct rec pad k v)\n' > "$d/a.lisp"
cat > "$d/run.lisp" <<'EOF'
(load "a.lisp")
(handler-case (progn (load "b-out.fasl") (format t "LOADED-STALE~%"))
  (error (e) (format t "REFUSED ~A~%" e)))
(format t "DEFINED ~S~%" (fboundp 'rec-val))
;; The refusal leaves through HANDLER-CASE, a non-local exit that passes no C
;; error frame: the loader must have dropped its (stack-local) reader from
;; the GC's registry itself, or the next collections mark a dead frame.
(dotimes (i 3)
  (load "a.lisp")
  (make-list 2000)
  (ext:gc))
(format t "GC-AFTER-REFUSAL-OK~%")
EOF
run "$d"
check "stale_explicit_fasl_signals" "$(has 'REFUSED .*stale.*COMMON-LISP-USER::REC')"
check "stale_explicit_fasl_says_recompile" "$(has 'recompile')"
check "stale_explicit_fasl_ran_no_unit" "$(has 'DEFINED NIL')"
check "stale_explicit_fasl_gc_survives_refusal" "$(has 'GC-AFTER-REFUSAL-OK')"

# --- 6. the switch ----------------------------------------------------------
d="$TMP/off"; mkdir -p "$d"
printf '(defun f () 1)\n' > "$d/a.lisp"
printf '(load "a.lisp")\n(format t "F ~S~%%" (f))\n' > "$d/run.lisp"
run "$d" --no-fasl-cache
check "flag_loads_source" "$(has 'F 1')"
check "flag_writes_no_cache" "$([ "$(nfasl "$d")" -eq 0 ] && echo yes || echo no)"
out=$(cd "$d" && AMILISP_FASL_CACHE=0 AMILISP_FASL_CACHE_DIR="$d/cache" \
      "$AMILISP" --no-userinit --non-interactive --load run.lisp </dev/null 2>&1)
check "env_loads_source" "$(has 'F 1')"
check "env_writes_no_cache" "$([ "$(nfasl "$d")" -eq 0 ] && echo yes || echo no)"
run "$d"
check "default_writes_cache" "$([ "$(nfasl "$d")" -ge 1 ] && echo yes || echo no)"
run "$d" --no-fasl-cache
check "flag_ignores_existing_cache" "$(hasnt 'Loading .*cache.*a\.fasl')"
run "$d"
check "default_reads_cache" "$(has 'Loading .*cache.*a\.fasl')"
out=$("$AMILISP" --help 2>&1)
check "help_mentions_flag" "$(has '--no-fasl-cache')"

echo ""
echo "test_fasl_struct_deps: $passed passed, $failed failed"
[ "$failed" -eq 0 ]
