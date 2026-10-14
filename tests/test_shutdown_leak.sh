#!/bin/sh
# Regression test for off-heap (non-arena) memory that was never handed back.
#
# amilisp keeps a lot of memory outside the GC arena: every compiled function
# owns a bytecode body, a constants pool, &key arrays, a line map and (on
# m68k) JIT code; the compiler pool holds eight blocks and their buffers;
# the struct, condition and setf/deftype indexes are hash tables; the stream
# layer has segmented directories.  None of it is visible to ROOM, and none of
# it used to be released.
#
# On a host that was invisible — the kernel reclaims the address space.  On
# AmigaOS nothing reclaims it, so each run permanently cost the machine
# 3,743,160 bytes of Fast RAM (measured with `Avail` on a Vampire before and
# after; the leak reproduced exactly, run after run, and was independent of
# --heap, which is what proved it was not the arena).
#
# Two separate defects, both covered here:
#   1. the GC swept dead bytecode objects without freeing their off-heap
#      buffers, so the leak also grew *during* a run (function redefinition,
#      COMPILE, reloading a file);
#   2. shutdown freed the arena but nothing hanging off it, so the entire
#      live boot image leaked at exit.
#
# The strong form of this test (every byte accounted for) needs the leak
# tracer: `make test-memleak`.  What runs here works in an ordinary build.
#
# Also runs under the gc-stress binary (make test-gc-stress, CLAMIGA_GC_STRESS=1)
# so the churn scenario's coalesce/finalize interaction (bytecode_release_offheap
# invoked from gc_finalize_dead) is exercised under forced compaction, not just
# whatever compaction pattern an ordinary run happens to trigger.
#
# Run: sh tests/test_shutdown_leak.sh build/host/amilisp

CLAMIGA="${1:-build/host/amilisp}"
case "$CLAMIGA" in
  /*) ;;
  *) CLAMIGA="$(pwd)/$CLAMIGA" ;;
esac
passed=0
failed=0
total=0

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

chk() {
    total=$((total + 1))
    if [ "$2" = "yes" ]; then
        echo "  ok  $1"
        passed=$((passed + 1))
    else
        echo "  FAIL  $1"
        failed=$((failed + 1))
    fi
}

# --- 1. The GC must reclaim a dead bytecode's off-heap payload -------------
#
# Redefining a function orphans its old CL_Bytecode.  Sweeping that object
# must also free the buffers it owns; before the fix the counter stayed at
# zero no matter how much code churned.

cat > "$WORK/churn.lisp" <<'LISPEOF'
(let ((before (first (ext:%bytecode-offheap-stats))))
  (dotimes (i 400)
    (eval (list 'defun 'churn-victim (list 'x)
                (list '+ 'x i i i i i i i i))))
  (gc)
  (gc)
  (let* ((after (first (ext:%bytecode-offheap-stats)))
         (grown (- after before)))
    (format t "CHURN-RECLAIMED ~a~%" grown)
    ;; 400 redefinitions of an 8-constant function: each dead bytecode owns
    ;; at least a body and a constants pool, so this is far above any
    ;; plausible noise floor while staying well under the real figure.
    (format t "CHURN-VERDICT ~a~%" (if (> grown 4000) "PASS" "FAIL"))))
(quit)
LISPEOF

out=$("$CLAMIGA" --no-userinit --load "$WORK/churn.lisp" </dev/null 2>&1)

case "$out" in
  *"CHURN-VERDICT PASS"*) chk "gc_reclaims_dead_bytecode_offheap_payload" yes ;;
  *) chk "gc_reclaims_dead_bytecode_offheap_payload" no
     echo "$out" | tail -5 ;;
esac

# The counter must also report a nonzero object count, not just bytes.
cat > "$WORK/count.lisp" <<'LISPEOF'
(dotimes (i 200) (eval (list 'defun 'cv2 (list 'x) (list '+ 'x i))))
(gc)
(format t "COUNT ~a~%" (second (ext:%bytecode-offheap-stats)))
(quit)
LISPEOF
out=$("$CLAMIGA" --no-userinit --load "$WORK/count.lisp" </dev/null 2>&1)
n=$(echo "$out" | sed -n 's/^COUNT \([0-9][0-9]*\)$/\1/p' | tail -1)
if [ -n "$n" ] && [ "$n" -gt 100 ]; then chk "offheap_stats_reports_object_count" yes
else chk "offheap_stats_reports_object_count" no; echo "$out" | tail -5; fi

# --- 2. Shutdown must release what is still live ---------------------------
#
# CLAMIGA_MEM_DIAG=1 makes the shutdown paths report what they hand back.
# A trivial program still has the whole boot image live, so both figures
# below must be substantial.

cat > "$WORK/nul.lisp" <<'LISPEOF'
(quit)
LISPEOF

out=$(CLAMIGA_MEM_DIAG=1 "$CLAMIGA" --no-userinit --load "$WORK/nul.lisp" </dev/null 2>&1)

case "$out" in
  *"[mem] compiler pool:"*"released"*) chk "shutdown_releases_compiler_pool" yes ;;
  *) chk "shutdown_releases_compiler_pool" no; echo "$out" | tail -6 ;;
esac

# The pool pre-warms 8 blocks and releases every one of them, with the
# buffers they grew.  A block used to hold its bytecode and constants as
# fixed arrays (366 KB each, 3 MB for the pool); what is left is ~80 KB, so
# the whole release must now be well under a megabyte but cover all eight.
line=$(echo "$out" | grep '\[mem\] compiler pool:' | tail -1)
nblk=$(echo "$line" | sed -n 's/.*pool: \([0-9][0-9]*\) block(s) x .*/\1/p')
bsize=$(echo "$line" | sed -n 's/.* block(s) x \([0-9][0-9]*\) bytes .*/\1/p')
pool=$(echo "$line" | sed -n 's/.*= \([0-9][0-9]*\) bytes released.*/\1/p')
if [ "$nblk" = "8" ] && [ -n "$bsize" ] && [ -n "$pool" ] \
   && [ "$pool" -ge $((nblk * bsize)) ]; then chk "compiler_pool_release_is_whole_pool" yes
else chk "compiler_pool_release_is_whole_pool" no; echo "  (got '$line')"; fi
if [ -n "$bsize" ] && [ "$bsize" -lt 160000 ] && [ -n "$pool" ] && [ "$pool" -lt 1000000 ]; then
    chk "compiler_pool_blocks_are_small" yes
else chk "compiler_pool_blocks_are_small" no; echo "  (got '$line')"; fi

live=$(echo "$out" | sed -n 's/.*during the run, \([0-9][0-9]*\) released at shutdown.*/\1/p' | tail -1)
if [ -n "$live" ] && [ "$live" -gt 50000 ]; then chk "shutdown_releases_live_bytecode_payload" yes
else chk "shutdown_releases_live_bytecode_payload" no; echo "  (got '$live')"; fi

# --- 3. Source paths survive the FASL round-trip ---------------------------
#
# The FASL reader used to platform_alloc a fresh copy of the source path for
# every bytecode it read instead of interning it, so loading boot.fasl and
# clos.fasl alone left ~1400 duplicates of two strings (~104 KB) that nothing
# ever freed.  The fix routes the path through cl_intern_source_file, the
# same pool the compiler and image restore use — which also means the reader
# no longer owns the buffer it read into.
#
# The byte-level proof that the duplicates are gone is `make test-memleak`.
# What this guards is the behaviour that fix could have broken: a function
# loaded FROM A FASL must still report the file it was compiled from, and
# must report the same one as its neighbours in that file.

cat > "$WORK/src.lisp" <<'LISPEOF'
(defun sf-a (x) (+ x 1))
(defun sf-b (x) (+ x 2))
(defun sf-c (x) (+ x 3))
LISPEOF

cat > "$WORK/srccheck.lisp" <<LISPEOF
;; Compile to a FASL, then load THAT — so the paths come back through the
;; FASL reader, not the compiler.
(compile-file "$WORK/src.lisp" :output-file "$WORK/src.fasl")
(fmakunbound 'sf-a) (fmakunbound 'sf-b) (fmakunbound 'sf-c)
(load "$WORK/src.fasl")
(let ((a (ext:function-source-location #'sf-a))
      (b (ext:function-source-location #'sf-b))
      (c (ext:function-source-location #'sf-c)))
  (format t "SRC ~a ~a ~a~%"
          (if (consp a) "HAVE" a)
          (if (and (consp a) (consp b) (equal (first a) (first b))) "SAME" "DIFF")
          (if (and (consp b) (consp c) (equal (first b) (first c))) "SAME" "DIFF")))
(quit)
LISPEOF

out=$("$CLAMIGA" --no-userinit --load "$WORK/srccheck.lisp" </dev/null 2>&1)
case "$out" in
  *"SRC HAVE SAME SAME"*) chk "fasl_loaded_functions_keep_source_path" yes ;;
  *) chk "fasl_loaded_functions_keep_source_path" no
     echo "$out" | tail -6 ;;
esac

echo ""
echo "$passed passed, $failed failed, $total total"
[ "$failed" -eq 0 ] || exit 1
exit 0
