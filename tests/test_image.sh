#!/bin/sh
# End-to-end tests for heap images (EXT:SAVE-IMAGE / --image) — the
# two-process legs the in-process units (tests/test_image.c) cannot cover:
# a full-boot session saves an image, a separate process restores it and
# must find every kind of session state intact.
#
# Covered:
#   save from --eval, restore via --image: functions, CLOS dispatch,
#     macros, EQ-hashtable identity, closures, readtable customization
#   restore into a larger --heap than the save used
#   (ext:save-image f :quit t): image written after the enclosing form,
#     later actions skipped, EXT:*EXIT-HOOKS* still run
#   deferred-dump semantics at the REPL (batch): the session continues
#     after the dump, state created before the save is in the image
#   corrupt image → clean refusal (explicit --image exits 1)
#   auto-discovery of amilisp.img in the cwd and in an install prefix's
#     lib/amilisp/; --no-image bypasses it
#   process state is re-derived on restore, not carried from the saver:
#     *default-pathname-defaults* names the RESTORING process's cwd,
#     *random-state* is freshly seeded
#   ~/.amilisprc runs after a restore with EXT:*IMAGE-RESTORED-P* = T
#   :shake-bindings — the delivery mode: binding tables shed before the dump,
#     touched names intact, untouched ones gone with a reader error saying why
#   source files: each name stored once, every function's file restored
#   AMIGA.FFI:DEFINE-LIBRARY-VARIABLE — library bases re-derived before
#     ~/.amilisprc, in definition order, once each; a failing one reported;
#     a zeroed (plain DEFVAR) base makes a library call signal "NULL
#     pointer" on the VM and the stub path instead of jumping to -LVO
#
# Run: sh tests/test_image.sh build/host/amilisp

AMILISP="${1:-build/host/amilisp}"
case "$AMILISP" in
    /*) : ;;
    *) AMILISP="$(pwd)/$AMILISP" ;;
esac

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)
if [ -z "$TIMEOUT" ]; then
    echo "SKIP test_image: neither timeout nor gtimeout on PATH"
    exit 0
fi

passed=0
failed=0

WORK=$(mktemp -d "${TMPDIR:-/tmp}/amilisp_image_XXXXXX") || exit 1
trap 'rm -rf "$WORK"' EXIT INT TERM
cd "$WORK" || exit 1

CLI="--no-userinit --no-image"

fail() {
    desc="$1"; why="$2"; out="$3"
    failed=$((failed + 1))
    echo "  FAIL  $desc ($why)"
    echo "    output: $(echo "$out" | head -8)"
}

ok() { passed=$((passed + 1)); echo "  ok  $desc"; }

check() {
    desc="$1"; want_ec="$2"; ec="$3"; out="$4"
    shift 4
    if [ "$ec" -eq 124 ]; then
        fail "$desc" "timed out" "$out"
        return 1
    fi
    if [ "$ec" -ne "$want_ec" ]; then
        fail "$desc" "exit code $ec, wanted $want_ec" "$out"
        return 1
    fi
    for marker in "$@"; do
        if ! echo "$out" | grep -q "$marker"; then
            fail "$desc" "missing marker $marker" "$out"
            return 1
        fi
    done
    ok
    return 0
}

check_absent() {
    # last argument style: assert marker NOT in output
    desc="$1"; out="$2"; marker="$3"
    if echo "$out" | grep -q "$marker"; then
        fail "$desc" "unexpected marker $marker" "$out"
        return 1
    fi
    ok
    return 0
}

# --- Build a full-boot session and save it -------------------------------

cat > state.lisp <<'EOF'
(defun im-fib (n) (if (< n 2) n (+ (im-fib (- n 1)) (im-fib (- n 2)))))
(defmacro im-twice (x) `(* 2 ,x))
(defclass im-animal () ((name :initarg :name :accessor im-name)))
(defmethod im-speak ((a im-animal)) (format nil "~a speaks" (im-name a)))
(defvar *im-pet* (make-instance 'im-animal :name "Rex"))
(defvar *im-ht* (make-hash-table :test 'eq))
(defvar *im-key* (cons 'k 'v))
(setf (gethash *im-key* *im-ht*) 'im-hit)
(defvar *im-close* (let ((n 31)) (lambda (x) (+ x n))))
(set-macro-character #\! (lambda (s c) (declare (ignore s c)) 4242))
;; FFI stubs (DEFCFUN / DEFCSTRUCT binding descriptors): pure heap data,
;; must come back callable with their fields intact.
(require "amiga/ffi")
(defvar *im-base* nil)
(amiga.ffi:defcfun im-lc *im-base* -30 (:a0 x :d0 y) :result :u16)
(ffi:defcstruct (im-pt :size 8) (x :i16 0) (arr (:array :u8 4) 4))
;; A demand-interned binding table (bindtab.c): the blob hangs off the
;; package (CL_Package.bindings), one name built before the save, the rest
;; must still materialise lazily after the restore.
(defpackage "IM-BT" (:use "CL"))
(amiga.ffi:define-binding-table "IM-BT" (:base *im-base*)
  (:const "+BEFORE+" 11) (:const "+AFTER+" 22) (:fn "IM-BT-CALL" -36 (:a0) :bool)
  (:struct "IM-NODE" 4 ("X" :i16 0)))
(defvar *im-bt-before* 'im-bt:+before+)
(format t "STATE-LOADED~%")
EOF

cat > verify.lisp <<'EOF'
(format t "FIB=~a~%" (im-fib 12))
(format t "MACRO=~a~%" (im-twice 21))
(format t "CLOS=~a~%" (im-speak *im-pet*))
(format t "HT=~a~%" (gethash *im-key* *im-ht*))
(format t "CLOSURE=~a~%" (funcall *im-close* 11))
(format t "READER=~a~%" (read-from-string "!"))
(format t "RESTOREDP=~a~%" ext:*image-restored-p*)
(format t "STUB=~a~%" (list (getf (ffi::%ffi-stub-info #'im-lc) :result)
                            (getf (ffi::%ffi-stub-info #'im-lc) :lvo)
                            (let ((m (ffi:alloc-foreign 8)))
                              (setf (im-pt-x m) -7 (im-pt-arr m 3) 9)
                              (prog1 (list (im-pt-x m) (im-pt-arr m 3))
                                (ffi:free-foreign m)))
                            (handler-case (im-lc 1 2)
                              (error (e) (if (search "not open" (format nil "~a" e)) :not-open e)))))
(format t "BT=~a~%" (list (getf (amilisp::%binding-table-info "IM-BT") :entries)
                          (eq *im-bt-before* 'im-bt:+before+)
                          (multiple-value-bind (s st) (find-symbol "+AFTER+" "IM-BT") (list st (symbol-value s)))
                          (getf (ffi::%ffi-stub-info #'im-bt:im-bt-call) :lvo)
                          im-bt:*im-node-size*))
(dotimes (i 20000) (cons i i))
(ext:gc)
(format t "GC-OK=~a~%" (im-fib 10))
EOF

out=$("$TIMEOUT" 60 "$AMILISP" $CLI --heap 8M --non-interactive \
    --load state.lisp --eval '(ext:save-image "session.img")' </dev/null 2>&1)
ec=$?
check "save_full_boot_session" 0 "$ec" "$out" STATE-LOADED "Image saved"

# --- Restore and verify every state class --------------------------------

out=$("$TIMEOUT" 60 "$AMILISP" --no-userinit --image session.img \
    --non-interactive --load verify.lisp </dev/null 2>&1)
ec=$?
check "restore_verifies_state" 0 "$ec" "$out" \
    "FIB=144" "MACRO=42" "CLOS=Rex speaks" "HT=IM-HIT" "CLOSURE=42" \
    "READER=4242" "RESTOREDP=T" "STUB=(U16 -30 (-7 9) NOT-OPEN)" \
    "BT=(5 T (EXTERNAL 22) -36 4)" "GC-OK=55"

# --- Restore into a larger heap ------------------------------------------

out=$("$TIMEOUT" 60 "$AMILISP" --no-userinit --image session.img --heap 48M \
    --non-interactive --eval '(format t "BIGHEAP=~a~%" (im-fib 12))' </dev/null 2>&1)
ec=$?
check "restore_into_larger_heap" 0 "$ec" "$out" "BIGHEAP=144"

# --- :quit t: dump after the form, skip later actions, run exit hooks ----

out=$("$TIMEOUT" 60 "$AMILISP" $CLI --heap 8M --non-interactive \
    --eval '(ext:add-exit-hook (lambda () (format t "EXIT-HOOK~%")))' \
    --eval '(defvar *im-q* 7)' \
    --eval '(ext:save-image "quit.img" :quit t)' \
    --eval '(format t "NOT-REACHED~%")' </dev/null 2>&1)
ec=$?
check "save_quit_writes_and_exits" 0 "$ec" "$out" "Image saved" "EXIT-HOOK"
desc="save_quit_skips_later_actions"
check_absent "$desc" "$out" "NOT-REACHED"

out=$("$TIMEOUT" 60 "$AMILISP" --no-userinit --image quit.img \
    --non-interactive --eval '(format t "QVAR=~a~%" *im-q*)' </dev/null 2>&1)
ec=$?
check "quit_image_restores" 0 "$ec" "$out" "QVAR=7"

# --- Deferred dump at the (batch) REPL: session continues after save -----

out=$(printf '(defvar *im-r* 5)\n(ext:save-image "repl.img")\n(format t "AFTER-SAVE=~a~%%" (+ *im-r* 1))\n(quit)\n' \
    | "$TIMEOUT" 60 "$AMILISP" $CLI --heap 8M --batch 2>&1)
ec=$?
check "repl_save_session_continues" 0 "$ec" "$out" "Image saved" "AFTER-SAVE=6"

out=$("$TIMEOUT" 60 "$AMILISP" --no-userinit --image repl.img \
    --non-interactive --eval '(format t "RVAR=~a~%" *im-r*)' </dev/null 2>&1)
ec=$?
check "repl_image_restores" 0 "$ec" "$out" "RVAR=5"

# --- Locks and condition variables restore free (heap words) -------------
# The image is written while a recursive lock is HELD at depth 2 by the
# saving thread, after a condvar has been waited on and notified; the
# restored objects must be free with zero waiters and usable
# (specs/mp-locks-heap-words.md: restore zeroes state/depth/waiters — a
# saved image cannot meaningfully contain a held lock, and SAVE-IMAGE
# refuses while worker threads run, so no waiter can be registered).

cat > lockimg.lisp <<'EOF'
(defvar *im-lk* (mp:make-recursive-lock "img-lock"))
(defvar *im-cv* (mp:make-condition-variable "img-cv"))
(defvar *im-go* nil)
(let ((th (mp:make-thread (lambda ()
                            (let ((l (mp:make-lock)))
                              (mp:acquire-lock l)
                              (loop until *im-go* do (mp:condition-wait *im-cv* l))
                              (mp:release-lock l))))))
  (sleep 0.2)
  (setf *im-go* t)
  (mp:condition-broadcast *im-cv*)
  (mp:join-thread th))
(mp:acquire-lock *im-lk*)
(mp:acquire-lock *im-lk*)
(format t "PRE-SAVE waiters=~a held=~a~%" (mp::%condvar-waiters *im-cv*) (mp::%lock-held-p *im-lk*))
(ext:save-image "locks.img")
(mp:release-lock *im-lk*)
(mp:release-lock *im-lk*)
EOF
out=$("$TIMEOUT" 60 "$AMILISP" $CLI --heap 8M --non-interactive \
    --load lockimg.lisp </dev/null 2>&1)
ec=$?
check "save_with_held_lock" 0 "$ec" "$out" "PRE-SAVE waiters=0 held=T" "Image saved"

out=$("$TIMEOUT" 60 "$AMILISP" --no-userinit --image locks.img --non-interactive \
    --eval '(format t "POST-RESTORE waiters=~a held=~a ok=~a~%" (mp::%condvar-waiters *im-cv*) (mp::%lock-held-p *im-lk*) (progn (mp:acquire-lock *im-lk*) (mp:acquire-lock *im-lk*) (mp:condition-notify *im-cv*) (mp:release-lock *im-lk*) (mp:release-lock *im-lk*) (mp:join-thread (mp:make-thread (lambda () (mp:acquire-lock *im-lk* nil))))))' </dev/null 2>&1)
ec=$?
check "restored_lock_and_condvar_are_free" 0 "$ec" "$out" "POST-RESTORE waiters=0 held=NIL ok=T"

# --- Live worker threads: refused, and the refusal NAMES them -------------
# "1 worker thread(s) are still running" told the user nothing when the
# thread was a service a module started for them -- the ARexx port from
# S:.amilisprc (thread "arexx-port") was exactly that, 2026-10-06.  The
# message lists each live worker by name (an unnamed one by id), keeps the
# main thread out, and says where the rc-started one comes from.
cat > thrimg.lisp <<'EOF2'
(defvar *im-go* nil)
(defvar *im-named* (mp:make-thread (lambda () (loop until *im-go* do (sleep 0.02))) :name "arexx-port"))
(defvar *im-anon* (mp:make-thread (lambda () (loop until *im-go* do (sleep 0.02)))))
(sleep 0.1)
(handler-case (ext:save-image "threads.img")
  (error (e) (format t "REFUSED: ~a~%" e)))
(setf *im-go* t)
(mp:join-thread *im-named*)
(mp:join-thread *im-anon*)
(format t "SAVED-P ~a~%" (probe-file "threads.img"))
EOF2
out=$("$TIMEOUT" 60 "$AMILISP" $CLI --heap 8M --non-interactive \
    --load thrimg.lisp </dev/null 2>&1)
ec=$?
check "save_refused_names_live_threads" 0 "$ec" "$out" \
    "REFUSED: SAVE-IMAGE: 2 worker thread(s) are still running (" \
    '"arexx-port"' "unnamed thread" "no-userinit" "SAVED-P NIL"
desc="save_refusal_omits_main_thread"
case $out in
    *'"main"'*) fail "$desc" "main thread listed" "$out" ;;
    *) ok ;;
esac

# --- Corrupt image: explicit --image refuses cleanly ---------------------

# Flip a fingerprint byte (offset 12 is inside the 32-byte fingerprint).
cp session.img corrupt.img
printf '\377' | dd of=corrupt.img bs=1 seek=12 count=1 conv=notrunc 2>/dev/null
out=$("$TIMEOUT" 60 "$AMILISP" --no-userinit --image corrupt.img \
    --non-interactive --eval '(print 1)' </dev/null 2>&1)
ec=$?
check "corrupt_image_refused" 1 "$ec" "$out" "different build"

# Truncated file: refused, exit 1.
head -c 50000 session.img > trunc.img
out=$("$TIMEOUT" 60 "$AMILISP" --no-userinit --image trunc.img \
    --non-interactive --eval '(print 1)' </dev/null 2>&1)
ec=$?
check "truncated_image_refused" 1 "$ec" "$out" "corrupt"

# --- Auto-discovery of amilisp.img in the cwd ----------------------------

cp session.img amilisp.img
out=$("$TIMEOUT" 60 "$AMILISP" --no-userinit --non-interactive \
    --eval '(format t "DISC=~a~%" ext:*image-restored-p*)' </dev/null 2>&1)
ec=$?
check "auto_discovery_cwd" 0 "$ec" "$out" "DISC=T"

out=$("$TIMEOUT" 60 "$AMILISP" --no-userinit --no-image --non-interactive \
    --eval '(format t "NODISC=~a~%" ext:*image-restored-p*)' </dev/null 2>&1)
ec=$?
check "no_image_bypasses_discovery" 0 "$ec" "$out" "NODISC=NIL"

# A corrupt discovered image falls back to a normal boot (exit 0), unlike
# an explicit --image.
cp corrupt.img amilisp.img
out=$("$TIMEOUT" 60 "$AMILISP" --no-userinit --non-interactive \
    --eval '(format t "FALLBACK=~a~%" ext:*image-restored-p*)' </dev/null 2>&1)
ec=$?
check "corrupt_discovery_falls_back" 0 "$ec" "$out" "FALLBACK=NIL" "different build"
rm -f amilisp.img

# The pre-rename name: a clamiga.img saved before the runtime became
# AmiLisp (0.12) is still discovered at every location ...
cp session.img clamiga.img
out=$("$TIMEOUT" 60 "$AMILISP" --no-userinit --non-interactive \
    --eval '(format t "DISC-OLD=~a~%" ext:*image-restored-p*)' </dev/null 2>&1)
ec=$?
check "auto_discovery_pre_rename_name" 0 "$ec" "$out" "DISC-OLD=T"
# ... but amilisp.img wins when both are there (the corrupt one as
# clamiga.img would have forced a fallback boot had it been tried first).
cp corrupt.img clamiga.img
cp session.img amilisp.img
out=$("$TIMEOUT" 60 "$AMILISP" --no-userinit --non-interactive \
    --eval '(format t "DISC-NEW-FIRST=~a~%" ext:*image-restored-p*)' </dev/null 2>&1)
ec=$?
check "auto_discovery_new_name_first" 0 "$ec" "$out" "DISC-NEW-FIRST=T"
rm -f amilisp.img clamiga.img

# Install prefix (`make install`-style layout): <prefix>/bin/amilisp picks up an image
# in <prefix>/lib/amilisp/, beside the runtime library it belongs to — the
# same executable-relative locations the lib/ search uses.
mkdir -p prefix/bin prefix/lib/amilisp
cp "$AMILISP" prefix/bin/amilisp
cp session.img prefix/lib/amilisp/amilisp.img
out=$(cd prefix && "$TIMEOUT" 60 bin/amilisp --no-userinit --non-interactive \
    --eval '(format t "DISC-FHS=~a~%" ext:*image-restored-p*)' </dev/null 2>&1)
ec=$?
check "auto_discovery_installed_layout" 0 "$ec" "$out" "DISC-FHS=T"

# ... and the prefix layout as installed before the rename: <prefix>/lib/clamiga/clamiga.img
mkdir -p prefix-old/bin prefix-old/lib/clamiga
cp "$AMILISP" prefix-old/bin/amilisp
cp session.img prefix-old/lib/clamiga/clamiga.img
out=$(cd prefix-old && "$TIMEOUT" 60 bin/amilisp --no-userinit --non-interactive \
    --eval '(format t "DISC-FHS-OLD=~a~%" ext:*image-restored-p*)' </dev/null 2>&1)
ec=$?
check "auto_discovery_pre_rename_installed_layout" 0 "$ec" "$out" "DISC-FHS-OLD=T"

# --- Process state is re-derived on restore, not carried from the saver --
# *DEFAULT-PATHNAME-DEFAULTS* is heap state: carried over, a restored
# session merges relative paths against the SAVING process's cwd (for a
# shipped amilisp.img, a directory on the build machine) while REQUIRE/LOAD
# check existence against the real cwd — "found" a file, then failed to open
# it.  *RANDOM-STATE* likewise: the saver's clock-seeded state would replay
# the same (random) sequence on every restore.  Save in dpd-a/, restore
# from dpd-b/.
mkdir -p dpd-a dpd-b
out=$(cd dpd-a && "$TIMEOUT" 60 "$AMILISP" $CLI --non-interactive \
    --eval '(format t "RND-NEXT=~a~%" (random 1000000 (make-random-state nil)))' \
    --eval '(ext:save-image "dpd.img")' </dev/null 2>&1)
ec=$?
check "process_state_save_in_dir_a" 0 "$ec" "$out" "RND-NEXT=" "Image saved"
rnd_saved=$(echo "$out" | sed -n 's/^RND-NEXT=//p')
out=$(cd dpd-b && "$TIMEOUT" 60 "$AMILISP" --no-userinit --image ../dpd-a/dpd.img \
    --non-interactive \
    --eval '(format t "DPD=~a~%" (namestring *default-pathname-defaults*))' \
    --eval '(format t "RND-FIRST=~a~%" (random 1000000))' </dev/null 2>&1)
ec=$?
check "dpd_is_the_restoring_cwd" 0 "$ec" "$out" "^DPD=.*/dpd-b/$"
check_absent "dpd_not_the_saving_cwd" "$out" "^DPD=.*dpd-a"
rnd_restored=$(echo "$out" | sed -n 's/^RND-FIRST=//p')
if [ -n "$rnd_saved" ] && [ -n "$rnd_restored" ] && [ "$rnd_saved" != "$rnd_restored" ]; then
    desc="random_state_reseeded_on_restore"; ok
else
    fail "random_state_reseeded_on_restore" \
        "saver's next draw $rnd_saved, restored first draw $rnd_restored" "$out"
fi

# --- ~/.amilisprc runs after restore with *IMAGE-RESTORED-P* = T ---------

mkdir -p rc-home
cat > rc-home/.amilisprc <<'EOF'
(format t "RC-RESTORED=~a~%" ext:*image-restored-p*)
EOF
out=$(HOME="$WORK/rc-home" AMILISP_NO_USERINIT= "$TIMEOUT" 60 "$AMILISP" \
    --image session.img --non-interactive \
    --eval '(format t "RC-DONE~%")' </dev/null 2>&1)
ec=$?
check "amilisprc_sees_restored_p" 0 "$ec" "$out" "RC-RESTORED=T" "RC-DONE"

# --- :SHAKE-BINDINGS — image delivery sheds the binding tables -----------
# (specs/raw-bindings-footprint.md Phase 3.)  A delivered image has a CLOSED
# set of referenced names, so the tables that answer "a name nobody has asked
# for yet" are dead weight.  Shedding drops them: what was touched before the
# save keeps working, what was not stops existing — with an error that says
# so rather than looking like a typo.

cat > shake-state.lisp <<'EOF'
(require "amiga/ffi")
(defvar *sh-base* nil)
(defpackage "SH-BT" (:use "CL"))
;; 400 rows: big enough that shedding is visible in the image's size
(eval (list* 'amiga.ffi:define-binding-table "SH-BT" '((:base *sh-base*))
             (loop for i below 400 collect (list :const (format nil "+SH-C~D+" i) i))))
(defvar *sh-touched* 'sh-bt:+sh-c7+)
(format t "SHAKE-STATE=~a~%" (getf (amilisp::%binding-table-info "SH-BT") :entries))
EOF

cat > shake-verify.lisp <<'EOF'
(format t "SH-INFO=~a~%" (list (getf (amilisp::%binding-table-info "SH-BT") :shed)
                               (getf (amilisp::%binding-table-info "SH-BT") :entries)))
(format t "SH-TOUCHED=~a~%" (list (eq *sh-touched* 'sh-bt:+sh-c7+) sh-bt:+sh-c7+))
(format t "SH-UNTOUCHED=~a~%" (multiple-value-list (find-symbol "+SH-C9+" "SH-BT")))
(format t "SH-READ=~a~%" (handler-case (read-from-string "sh-bt:+sh-c9+")
                           (error (e) (if (search "shed by SAVE-IMAGE" (format nil "~a" e))
                                          :hinted :plain))))
EOF

out=$("$TIMEOUT" 60 "$AMILISP" $CLI --heap 8M --non-interactive \
    --load shake-state.lisp \
    --eval '(ext:save-image "shake-plain.img" :quit t)' </dev/null 2>&1)
ec=$?
check "shake_baseline_save" 0 "$ec" "$out" "SHAKE-STATE=400" "Image saved"
desc="save_without_shake_keeps_the_tables"
check_absent "$desc" "$out" "shed"

out=$("$TIMEOUT" 60 "$AMILISP" $CLI --heap 8M --non-interactive \
    --load shake-state.lisp \
    --eval '(ext:save-image "shake-shaken.img" :quit t :shake-bindings t)' </dev/null 2>&1)
ec=$?
check "shake_bindings_save_reports_what_it_shed" 0 "$ec" "$out" \
    "shed 1 binding table" "resolve only the names already referenced" "Image saved"

desc="shake_bindings_shrinks_the_image"
sz_plain=$(wc -c < shake-plain.img)
sz_shaken=$(wc -c < shake-shaken.img)
if [ "$sz_shaken" -lt "$sz_plain" ]; then
    ok
else
    fail "$desc" "shaken image $sz_shaken not smaller than $sz_plain" ""
fi

out=$("$TIMEOUT" 60 "$AMILISP" --no-userinit --image shake-shaken.img \
    --non-interactive --load shake-verify.lisp </dev/null 2>&1)
ec=$?
check "shaken_image_closed_at_referenced_names" 0 "$ec" "$out" \
    "SH-INFO=(T 0)" "SH-TOUCHED=(T 7)" "SH-UNTOUCHED=(NIL NIL)" "SH-READ=HINTED"

# The unshaken twin is the control: the same untouched name still resolves.
out=$("$TIMEOUT" 60 "$AMILISP" --no-userinit --image shake-plain.img \
    --non-interactive --load shake-verify.lisp </dev/null 2>&1)
ec=$?
check "unshaken_image_still_materialises_on_demand" 0 "$ec" "$out" \
    "SH-INFO=(NIL 400)" "SH-UNTOUCHED=(+SH-C9+ EXTERNAL)" "SH-READ=+SH-C9+"

# Nothing lazy loaded: a clean no-op, not an error and not a stray message.
out=$("$TIMEOUT" 60 "$AMILISP" $CLI --heap 8M --non-interactive \
    --eval '(defvar *im-n* 3)' \
    --eval '(ext:save-image "noshake.img" :quit t :shake-bindings t)' </dev/null 2>&1)
ec=$?
check "shake_bindings_without_any_table_is_a_no_op" 0 "$ec" "$out" "Image saved"
desc="no_op_shake_says_nothing"
check_absent "$desc" "$out" "shed"

out=$("$TIMEOUT" 60 "$AMILISP" --no-userinit --image noshake.img \
    --non-interactive --eval '(format t "NOSHAKE=~a~%" *im-n*)' </dev/null 2>&1)
ec=$?
check "no_op_shake_image_restores" 0 "$ec" "$out" "NOSHAKE=3"

# An unknown keyword names the ones that exist.
out=$("$TIMEOUT" 60 "$AMILISP" $CLI --heap 8M --non-interactive \
    --eval '(handler-case (ext:save-image "x.img" :bogus t)
              (error (e) (format t "KWERR=~a~%" e)))' </dev/null 2>&1)
ec=$?
check "unknown_keyword_lists_the_accepted_ones" 0 "$ec" "$out" \
    "KWERR=.*:QUIT, :SHAKE-BINDINGS, :EXECUTABLE, :TOPLEVEL and :HEAP-SIZE"

# --- Source files: each name once in the image, every function's back ----
# A function keeps the file it was compiled from (EXT:FUNCTION-SOURCE-LOCATION,
# M-., backtraces).  The image holds each file's name once and a function
# names it by index; it used to carry a copy per function -- 2,019 copies of
# 46 paths in the editor's image.

cat > im-src-alpha.lisp <<'EOF'
(defun im-src-a1 (x) (+ x 1))
(defun im-src-a2 (x) (lambda (y) (+ x y)))
(defmacro im-src-am (x) `(list ,x))
EOF
cat > im-src-beta.lisp <<'EOF'
(defun im-src-b1 (x) (- x 1))
EOF

out=$("$TIMEOUT" 60 "$AMILISP" $CLI --heap 8M --non-interactive \
    --load im-src-alpha.lisp --load im-src-beta.lisp \
    --eval '(defun im-src-repl (x) x)' \
    --eval '(ext:save-image "src.img" :quit t)' </dev/null 2>&1)
ec=$?
check "source_files_save" 0 "$ec" "$out" "Image saved"

desc="source_file_names_stored_once"
n_alpha=$(LC_ALL=C grep -a -o 'im-src-alpha\.lisp' src.img | wc -l | tr -d ' ')
n_beta=$(LC_ALL=C grep -a -o 'im-src-beta\.lisp' src.img | wc -l | tr -d ' ')
if [ "$n_alpha" = 1 ] && [ "$n_beta" = 1 ]; then
    ok
else
    fail "$desc" "im-src-alpha.lisp $n_alpha times, im-src-beta.lisp $n_beta times" ""
fi

# The file of each function (the nested lambda's too) and, for one of them,
# the whole location -- only the file is the image's business.
out=$("$TIMEOUT" 60 "$AMILISP" --no-userinit --image src.img --non-interactive \
    --eval '(format t "SRC=~s~%" (mapcar (lambda (f)
                                          (let ((loc (ext:function-source-location f)))
                                            (if (consp loc) (car loc) loc)))
                                        (list (function im-src-a1) (im-src-a2 1)
                                              (macro-function (quote im-src-am))
                                              (function im-src-b1) (function im-src-repl))))' \
    --eval '(format t "LOC=~s~%" (ext:function-source-location (function im-src-b1)))' \
    </dev/null 2>&1)
ec=$?
check "source_files_restored_per_function" 0 "$ec" "$out" \
    'SRC=("im-src-alpha.lisp" "im-src-alpha.lisp" "im-src-alpha.lisp" "im-src-beta.lisp" :NOT-AVAILABLE)' \
    'LOC=("im-src-beta.lisp" 1)'

# --- AMIGA.FFI:DEFINE-LIBRARY-VARIABLE: OS state re-derived on restore ----
# A restore zeroes every foreign pointer of the image (it belonged to the
# saving process), so an Amiga module's library base would come back as
# address 0 and the first library call would jump to -LVO.  A variable
# declared with DEFINE-LIBRARY-VARIABLE has its init form run again in the
# restoring process, in definition order, BEFORE ~/.amilisprc; a plain
# foreign-pointer base now makes the call signal instead.

cat > libvar-state.lisp <<'EOF'
(require "amiga/ffi")
(defvar *lv-evals* 0)
;; A base, as OPEN-LIBRARY returns one: its form runs at load, and exactly
;; once more in the restoring process (counted there only).
(amiga.ffi:define-library-variable *lv-base*
  (progn (when ext:*image-restored-p* (incf *lv-evals*))
         (ffi:make-foreign-pointer #x1000)))
;; Derived from the base: must be re-derived after it.
(amiga.ffi:define-library-variable *lv-version*
  (and *lv-base* (+ 1 (ffi:foreign-pointer-address *lv-base*))))
;; Fails in the restoring process: reported, left NIL, the rest go on.
(amiga.ffi:define-library-variable *lv-broken*
  (if ext:*image-restored-p* (error "no such library here") :loaded))
(amiga.ffi:define-library-variable *lv-after*
  (if ext:*image-restored-p* :rederived :loaded)
  "The last library variable.")
;; The contrast: a plain DEFVAR'd base, zeroed by the restore.
(defvar *lv-plain* (ffi:make-foreign-pointer #x2000))
(amiga.ffi:defcfun lv-call *lv-plain* -30 (:d0 x))
(defun lv-direct () (lv-call 1))
(format t "LV-LOADED=~a~%" (list *lv-evals* *lv-version* *lv-broken* *lv-after*))
EOF

cat > libvar-verify.lisp <<'EOF'
(defun lv-null-p (thunk)
  (handler-case (progn (funcall thunk) :called)
    (error (e) (if (search "NULL pointer" (format nil "~a" e)) :null e))))
(format t "LV=~a~%" (list *lv-evals* (ffi:foreign-pointer-address *lv-base*)
                          *lv-version* *lv-broken* *lv-after*
                          (length amiga.ffi::*library-variables*)))
(format t "LV-DOC=~a~%" (documentation '*lv-after* 'variable))
(format t "LV-PLAIN=~a~%" (list (ffi:foreign-pointer-address *lv-plain*)
                                (lv-null-p #'lv-direct)
                                (lv-null-p (lambda () (funcall #'lv-call 1)))))
EOF

mkdir -p lv-home
cat > lv-home/.amilisprc <<'EOF'
(format t "RC-LV=~a~%" (list (ffi:foreign-pointer-address *lv-base*) *lv-version*))
EOF

# Loaded twice: DEFVAR keeps the value and the registry keeps one entry per
# variable, so the restore runs each form exactly once.
out=$("$TIMEOUT" 60 "$AMILISP" $CLI --heap 8M --non-interactive \
    --load libvar-state.lisp --load libvar-state.lisp \
    --eval '(ext:save-image "libvar.img")' </dev/null 2>&1)
ec=$?
check "library_variables_save" 0 "$ec" "$out" \
    "LV-LOADED=(0 4097 LOADED LOADED)" "Image saved"

out=$(HOME="$WORK/lv-home" AMILISP_NO_USERINIT= "$TIMEOUT" 60 "$AMILISP" \
    --image libvar.img --non-interactive --load libvar-verify.lisp </dev/null 2>&1)
ec=$?
check "library_variables_rederived_before_amilisprc" 0 "$ec" "$out" \
    "RC-LV=(4096 4097)"
check "library_variables_rederived_in_order_once_each" 0 "$ec" "$out" \
    "LV=(1 4096 4097 NIL REDERIVED 4)" "LV-DOC=The last library variable."
check "library_variable_that_fails_is_reported" 0 "$ec" "$out" \
    "\*LV-BROKEN\* could not be re-derived after the image restore -- no such library here"
check "null_library_base_signals_on_both_call_paths" 0 "$ec" "$out" \
    "LV-PLAIN=(0 NULL NULL)"

# --- Report --------------------------------------------------------------

echo "test_image: $passed passed, $failed failed"
[ "$failed" -eq 0 ]
