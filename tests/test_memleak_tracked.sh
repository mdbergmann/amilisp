#!/bin/sh
# Every off-heap byte must be handed back before the process exits.
#
# Requires a -DDEBUG_MEM_TRACK binary (see `make test-memleak`), where every
# platform_alloc is recorded with the file and line that made it and
# AMILISP_MEM_DIAG=1 prints whatever is still outstanding at exit.
#
# Why this is a real test and not hygiene: AmigaOS does not reclaim a
# process's memory.  A block amilisp fails to free is Fast RAM gone from the
# system pool until the machine reboots — this suite exists because amilisp
# lost 3,743,160 bytes on every single launch, measured with `Avail` on a
# Vampire.  A host build cannot observe that at all, so the tracer is the only
# way to keep it fixed from CI.
#
# Each scenario below is run to completion and must report zero blocks live.
#
# Run: sh tests/test_memleak_tracked.sh build/host-memtrack/amilisp

AMILISP="${1:-build/host-memtrack/amilisp}"
case "$AMILISP" in
  /*) ;;
  *) AMILISP="$(pwd)/$AMILISP" ;;
esac
# A Windows program is found by its extension (the delivered executable).
case "$AMILISP" in
  *.exe) EXE_SUFFIX=.exe ;;
  *) EXE_SUFFIX= ;;
esac
passed=0
failed=0
total=0

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# Confirm we were handed a tracked build — otherwise every check below would
# pass vacuously (no report line at all is not "no leak").
probe=$(AMILISP_MEM_DIAG=1 "$AMILISP" --no-userinit --eval '(quit)' --non-interactive </dev/null 2>&1)
case "$probe" in
  *"[mem] leak report:"*) ;;
  *)
    echo "  FAIL  binary_is_a_DEBUG_MEM_TRACK_build"
    echo "        no '[mem] leak report:' line — build with:"
    echo "        make host BUILDDIR=build/host-memtrack DEBUG_FLAGS=-DDEBUG_MEM_TRACK"
    echo ""
    echo "0 passed, 1 failed, 1 total"
    exit 1 ;;
esac

# run_case NAME FILE [ARG...] — run FILE (after the command-line ARGs) and
# require a zero-byte leak report.
run_case() {
    name=$1
    file=$2
    shift 2
    total=$((total + 1))
    out=$(AMILISP_MEM_DIAG=1 "$AMILISP" --no-userinit "$@" --load "$file" </dev/null 2>&1)
    report=$(echo "$out" | sed -n 's/^\[mem\] leak report: \([0-9][0-9]*\) block(s), \([0-9][0-9]*\) bytes.*/\1 \2/p' | tail -1)
    blocks=$(echo "$report" | cut -d' ' -f1)
    bytes=$(echo "$report" | cut -d' ' -f2)
    if [ -z "$report" ]; then
        echo "  FAIL  $name (no leak report — did the run reach shutdown?)"
        echo "$out" | tail -8
        failed=$((failed + 1))
        return
    fi
    case "$out" in
      *"table full"*)
        echo "  FAIL  $name (tracker table overflowed — raise MT_CAP in mem_track.c)"
        failed=$((failed + 1))
        return ;;
      *"SCENARIO-FAILED"*)
        # the scenario did not do what it exists to measure
        echo "  FAIL  $name"
        echo "$out" | grep 'SCENARIO-FAILED'
        failed=$((failed + 1))
        return ;;
    esac
    if [ "$blocks" = "0" ] && [ "$bytes" = "0" ]; then
        echo "  ok  $name"
        passed=$((passed + 1))
    else
        echo "  FAIL  $name ($bytes bytes in $blocks block(s) never freed)"
        echo "$out" | grep '^\[mem\]   ' | head -10
        failed=$((failed + 1))
    fi
}

# --- boot only: the whole live image must still be released ----------------
cat > "$WORK/nul.lisp" <<'LISPEOF'
(quit)
LISPEOF
run_case "no_leak_after_bare_boot" "$WORK/nul.lisp"

# --- compile churn: dead bytecode payloads must be swept -------------------
cat > "$WORK/churn.lisp" <<'LISPEOF'
(dotimes (i 500)
  (eval (list 'defun 'churn-victim (list 'x)
              (list '+ 'x i i i i i i i i))))
(gc)
(quit)
LISPEOF
run_case "no_leak_after_compile_churn" "$WORK/churn.lisp"

# --- compiler buffers: grown, kept, dropped, and lost to an overflow -------
# The pool keeps small bytecode / constants buffers across compiles, frees the
# oversized ones on release and all of them at shutdown; a compile that
# overflows the limit leaves its grown buffer to the unwind path.
cat > "$WORK/compbuf.lisp" <<'LISPEOF'
(defun dag (leaf k)
  (if (= k 0) leaf (let ((s (dag leaf (1- k)))) (list 'progn s s))))
(defun big-fn (k)
  `(lambda (x) (let ((acc x)) ,(dag '(setq acc (+ acc 1)) k) acc)))
(dolist (k '(6 9 11 13)) (funcall (compile nil (big-fn k)) 0))
(handler-case (compile nil (big-fn 15)) (error () nil))
(let ((objs (loop for i below 1500 collect (format nil "c~D" i))))
  (funcall (compile nil `(lambda () (list ,@(subseq objs 0 200)))))
  (compile nil `(lambda ()
                  (let ((v (make-array 1500)))
                    ,@(loop for o in objs for i from 0
                            collect `(setf (svref v ,i) ,o))
                    v))))
(handler-case
    (compile nil `(lambda ()
                    (let ((v (make-array 8300)))
                      ,@(loop for i below 8300
                              collect `(setf (svref v ,i) ,(code-char (+ 256 i))))
                      v)))
  (error () nil))
(quit)
LISPEOF
run_case "no_leak_after_compiler_buffer_growth" "$WORK/compbuf.lisp"

# --- bignum scratch buffers above the stack sizes (2026-09) ----------------
# Division of a 4,096+-bit dividend or by a >2,048-bit divisor, the bit
# operations past 2,048 bits, a negative bit-op result past 4,096 bits and
# ASH right of a 4,096+-bit result each platform_alloc a scratch block that
# must be handed back on the same call.
cat > "$WORK/bignum.lisp" <<LISPEOF
(mod (expt 2 8000) 1000003)
(mod (expt 2 8000) (+ (expt 2 3000) 1))
(gcd (expt 2 5000) (* 3 (expt 2 4500)))
(logand (expt 2 5000) (1- (expt 2 5000)))
(logior (- (expt 2 5000)) (- (expt 2 4999)))
(logxor (expt 2 5000) (expt 2 4000))
(ash (expt 2 5000) -7)
(ash (- (expt 2 5000)) -7)
(quit)
LISPEOF
run_case "no_leak_after_bignum_scratch" "$WORK/bignum.lisp"

# --- CLOS, streams, locks, threads, string output, a dropped stream --------
cat > "$WORK/heavy.lisp" <<LISPEOF
(defclass pt () ((x :initarg :x :accessor px) (y :initarg :y :accessor py)))
(defmethod norm ((p pt)) (+ (abs (px p)) (abs (py p))))
(norm (make-instance 'pt :x 3 :y 4))
(let ((s (open "$WORK/o1.tmp" :direction :output :if-exists :supersede)))
  (format s "hello~%")
  (close s))
;; deliberately dropped without CLOSE — the sweep finalizer must reclaim it
(open "$WORK/o2.tmp" :direction :output :if-exists :supersede)
(with-open-file (s "$WORK/o1.tmp") (read-line s nil nil))
(let ((l (mp:make-lock))) (mp:with-lock-held (l) t))
(mp:join-thread (mp:make-thread (lambda () (loop for i from 1 to 500 sum i))))
(with-output-to-string (s) (format s "~a" (list 1 2 3)))
(gc)
(quit)
LISPEOF
run_case "no_leak_after_clos_streams_threads" "$WORK/heavy.lisp"

# --- MP locks / condition variables are heap words: no off-heap primitive --
# 200,000 of each, some left held or with a registered-then-woken waiter,
# plus thread create/exit cycles (each worker allocates and frees its own
# park handle) must leave nothing outstanding.
cat > "$WORK/locks.lisp" <<'LISPEOF'
(let ((v (make-array 400000)))
  (dotimes (i 200000)
    (setf (aref v i) (mp:make-lock)
          (aref v (+ i 200000)) (mp:make-condition-variable)))
  (mp:acquire-lock (aref v 0))            ; left held: still just garbage
  (setf v nil))
(let* ((lk (mp:make-lock)) (cv (mp:make-condition-variable)) (go nil)
       (th (mp:make-thread (lambda ()
                             (mp:acquire-lock lk)
                             (loop until go do (mp:condition-wait cv lk))
                             (mp:release-lock lk)))))
  (sleep 0.1)
  (mp:with-lock-held (lk) (setf go t) (mp:condition-notify cv))
  (mp:join-thread th))
(dotimes (i 50)
  (mp:join-thread (mp:make-thread (lambda ()
                                    (let ((l (mp:make-lock)))
                                      (mp:with-lock-held (l)
                                        (mp:condition-wait (mp:make-condition-variable) l 0.001)))))))
(gc)
(quit)
LISPEOF
run_case "no_leak_after_lock_condvar_churn" "$WORK/locks.lisp"

# --- a thread that finished but was never joined ------------------------------
# Its wrapper stays reachable (a global, as an editor's port struct or a
# thread registry holds one) and the program only polls THREAD-ALIVE-P, so
# neither JOIN-THREAD nor the wrapper's finalizer frees the worker's stacks;
# cl_thread_shutdown reaps it.  Before the reap this leaked ~1.2 MB per such
# thread on the host at every exit (the Clamacs editor had four).
cat > "$WORK/unjoined.lisp" <<'LISPEOF'
(defvar *workers* '())
(dotimes (i 3)
  (push (mp:make-thread (lambda () (loop for i from 1 to 100 sum i))) *workers*))
(loop while (some #'mp:thread-alive-p *workers*) do (sleep 0.01))
(unless (every (lambda (th) (not (mp:thread-alive-p th))) *workers*)
  (error "workers still alive"))
(quit)
LISPEOF
run_case "no_leak_after_unjoined_finished_threads" "$WORK/unjoined.lisp"

# --- compile-file + FASL load: the reader's own allocations ----------------
cat > "$WORK/src.lisp" <<'LISPEOF'
(defun fl-a (x) (+ x 1))
(defun fl-b (x &key (k 2) j) (list x k j))
(defmacro fl-m (x) `(list ,x))
(defun fl-c (x) (fl-m (fl-a x)))
LISPEOF
cat > "$WORK/fasl.lisp" <<LISPEOF
(compile-file "$WORK/src.lisp" :output-file "$WORK/src.fasl")
(load "$WORK/src.fasl")
(load "$WORK/src.fasl")
(gc)
(quit)
LISPEOF
run_case "no_leak_after_compile_file_and_fasl_load" "$WORK/fasl.lisp"

# --- (QUIT) from inside LOAD: the early-exit shutdown path -----------------
cat > "$WORK/inner.lisp" <<'LISPEOF'
(defun q-a (x) (+ x 1))
(quit)
LISPEOF
cat > "$WORK/quitload.lisp" <<LISPEOF
(load "$WORK/inner.lisp")
(format t "NOT-REACHED~%")
LISPEOF
run_case "no_leak_when_quit_inside_load" "$WORK/quitload.lisp"

# --- a FASL load abandoned by a non-local exit ------------------------------
# The unit signals while the reader holds its off-heap shared-object table
# (the literal below is one object referenced twice).  HANDLER-CASE, RETURN-FROM
# and THROW leave through the VM's NLX landing, the last load through LOAD's own
# per-form error frame — either way fasl_load never reaches its cleanup, so the
# registry has to hand the table back.
cat > "$WORK/boom.lisp" <<'LISPEOF'
(defvar *boom-shared* '(#1=(1 2 3) #1# #2="shared" #2#))
(error "boom from inside a FASL unit")
LISPEOF
cat > "$WORK/boomload.lisp" <<LISPEOF
(load "$WORK/boom.fasl")
LISPEOF
cat > "$WORK/abandon.lisp" <<LISPEOF
(compile-file "$WORK/boom.lisp" :output-file "$WORK/boom.fasl")
(handler-case (load "$WORK/boom.fasl") (error () :caught))
(block out
  (handler-bind ((error (lambda (e) (declare (ignore e)) (return-from out nil))))
    (load "$WORK/boom.fasl")))
(catch 'tag
  (handler-bind ((error (lambda (e) (declare (ignore e)) (throw 'tag nil))))
    (load "$WORK/boom.fasl")))
(load "$WORK/boomload.lisp")
(gc)
(quit)
LISPEOF
run_case "no_leak_after_abandoned_fasl_load" "$WORK/abandon.lisp"

# --- a source LOAD's auto-cache writer abandoned by a non-local exit -------
# bi_load's auto-cache heap-allocates a CL_FaslWriter (fw) plus a fasl_buf
# and unit_buf, and registers fw as a GC root for the whole source load — the
# writer counterpart of the reader case above.  HANDLER-CASE, RETURN-FROM and
# THROW leave through the NLX landing before bi_load's own end-of-function
# cleanup runs, so all three had to be handed back from the registry instead.
cat > "$WORK/wboom.lisp" <<'LISPEOF'
(defvar *wboom-shared* '(#1=(1 2 3) #1# #2="shared" #2#))
(error "wboom from inside a source LOAD unit")
LISPEOF
cat > "$WORK/wabandon.lisp" <<LISPEOF
(handler-case (load "$WORK/wboom.lisp") (error () :caught))
(block out
  (handler-bind ((error (lambda (e) (declare (ignore e)) (return-from out nil))))
    (load "$WORK/wboom.lisp")))
(catch 'tag
  (handler-bind ((error (lambda (e) (declare (ignore e)) (throw 'tag nil))))
    (load "$WORK/wboom.lisp")))
(gc)
(quit)
LISPEOF
run_case "no_leak_after_abandoned_source_load_writer" "$WORK/wabandon.lisp"

# --- LOADs nested past the static table sizes ---------------------------------
# The C-buffer stream table and the FASL reader/writer registries start static
# and grow with platform_alloc when LOADs nest deeper (8 / 16 / 8 entries);
# the grown blocks are handed back at exit.  The bottom file signals, so the
# whole chain is abandoned once and completed once.
i=1
while [ $i -le 24 ]; do
    printf '(load "%s/deep%s.lisp")\n' "$WORK" "$((i + 1))" > "$WORK/deep$i.lisp"
    i=$((i + 1))
done
printf '(when *deep-boom* (error "deep boom"))\n' > "$WORK/deep25.lisp"
cat > "$WORK/deep.lisp" <<LISPEOF
(defvar *deep-boom* t)
(handler-case (load "$WORK/deep1.lisp") (error () :caught))
(setq *deep-boom* nil)
(load "$WORK/deep1.lisp")
(compile-file "$WORK/deep25.lisp" :output-file "$WORK/deep25.fasl")
(gc)
(quit)
LISPEOF
run_case "no_leak_after_deeply_nested_loads" "$WORK/deep.lisp"

# --- the native string-key hook (AMIGA.MUI:MAKE-STRING-KEY-HOOK) ------------
# builtins_amiga.c parks the hook's state, its entry table and a platform
# closure off-heap; FREE-STRING-KEY-HOOK must hand all of it back, also
# when the hook was made with no entries and when its entry was asked for
# (a registration per pointer object, released by the finalizer).
cat > "$WORK/skh.lisp" <<'LISPEOF'
(require "amiga/mui")
(dotimes (i 3)
  (let ((h (amiga.mui:make-string-key-hook 1 2 3 '((#x42 #x19 0 9) (#x24 #x19 8 10)))))
    (amiga.mui:string-key-hook-entry h)
    (amiga.mui:string-key-hook-stats h)
    (amiga.mui:free-string-key-hook h)))
(amiga.mui:free-string-key-hook (amiga.mui:make-string-key-hook 1 2 3 '()))
(gc)
(quit)
LISPEOF
run_case "no_leak_after_string_key_hooks" "$WORK/skh.lisp"

# --- a method-filtered dispatcher (AMIGA.FFI:MAKE-DISPATCHER :METHODS) ------
# The callback slot parks the method table off-heap; FREE-DISPATCHER hands
# it back, and a dispatcher with an empty list holds none.
cat > "$WORK/dispfilter.lisp" <<'LISPEOF'
(require "amiga/ffi")
(dotimes (i 3)
  (let ((d (amiga.ffi:make-dispatcher (lambda (c o m) c o m 0)
                                      :methods '(#x80426D66 #x8C1A0002 #x103))))
    (amiga.ffi:dispatcher-stats d)
    (amiga.ffi:free-dispatcher d)))
(amiga.ffi:free-dispatcher (amiga.ffi:make-dispatcher (lambda (c o m) c o m 0) :methods '()))
(gc)
(quit)
LISPEOF
run_case "no_leak_after_method_dispatchers" "$WORK/dispfilter.lisp"

# --- AArch64 JIT: stubs installed, replaced and swept ----------------------
# The code heap maps its chunks itself, but its chunk bookkeeping is
# platform_alloc'd and goes back in cl_jit_shutdown; a stub replaced or
# swept with its function frees its block.  Without the backend
# %JIT-COMPILE-STUB returns NIL and the scenario is a plain churn.
cat > "$WORK/jitstub.lisp" <<'LISPEOF'
(defun jit-leak-a () 1)
(amilisp::%jit-compile-stub #'jit-leak-a)
(amilisp::%jit-compile-stub #'jit-leak-a)
(dotimes (i 50)
  (amilisp::%jit-compile-stub (compile nil `(lambda () ,i))))
(defun jit-leak-a () 2)
(gc)
(quit)
LISPEOF
run_case "no_leak_after_jit_stubs" "$WORK/jitstub.lisp"

# --- AArch64 JIT: the walker (phase 1) --------------------------------------
# Every compile builds label, depth and literal tables off-heap and frees
# them whether it installs code or declines the function; installed code is
# freed with a redefined or dead function.  Eager, so every DEFUN compiles;
# a function with more than 16 helper and data addresses grows the literal
# table.  Without the backend this is a plain churn.
cat > "$WORK/jitwalk.lisp" <<'LISPEOF'
(amilisp::%jit-set-hot-threshold 0)
(defun jit-walk-probe () 1)
(defvar *jit-walk-backend* (amilisp::%jit-compile-stub #'jit-walk-probe))
(defvar *jit-walk-g* 0)
(defun jit-walk-loop (n) (let ((s 0)) (dotimes (i n s) (setq s (+ s i)))))
(defun jit-walk-many (x)
  (list (car x) (cdr x) (cons x x) (+ 1 2) (- 3 1) (* 2 2) (< 1 2) (> 1 2)
        (<= 1 2) (>= 1 2) (= 1 1) (setq *jit-walk-g* (1+ *jit-walk-g*))
        (symbol-function 'car) (list 1 2 3) (eq x x) (not x) 'a "b" 3.5))
(defun jit-walk-declined (x &optional y) (catch 'k (list x y)))
(when (and *jit-walk-backend*
           (not (and (amilisp::%jit-dump-bytes #'jit-walk-loop)
                     (amilisp::%jit-dump-bytes #'jit-walk-many))))
  (format t "SCENARIO-FAILED: the walker compiled nothing~%"))
(jit-walk-loop 1000)
(jit-walk-many '(1 . 2))
(jit-walk-declined 1)
(dotimes (i 50)
  (funcall (compile nil `(lambda (x) (if (< x ,i) (list x ,i) (cons ,i x)))) 3))
(defun jit-walk-loop (n) n)
(gc)
(quit)
LISPEOF
run_case "no_leak_after_jit_walker" "$WORK/jitwalk.lisp"

# --- heap image save + restore ----------------------------------------------
# The save builds an off-heap source-file table; the restore attaches every
# function's bytecode side buffers and interns each source-file name once
# through a scratch table.  Both processes must hand it all back.
cat > "$WORK/imgfns.lisp" <<'LISPEOF'
(defun leak-img-a (x) (lambda (y) (+ x y)))
(defun leak-img-b (x) (* x 2))
LISPEOF
cat > "$WORK/imgsave.lisp" <<LISPEOF
(load "$WORK/imgfns.lisp")
(ext:save-image "$WORK/leak.img" :quit t)
LISPEOF
run_case "no_leak_after_image_save" "$WORK/imgsave.lisp" --no-image
cat > "$WORK/imgrestore.lisp" <<'LISPEOF'
(unless (and ext:*image-restored-p* (= (funcall (leak-img-a 40) (leak-img-b 1)) 42))
  (format t "SCENARIO-FAILED: the image did not restore the session~%"))
(quit)
LISPEOF
run_case "no_leak_after_image_restore" "$WORK/imgrestore.lisp" --image "$WORK/leak.img"

# --- delivered executable ----------------------------------------------------
# (ext:save-image f :executable t) copies the runtime through a scratch
# buffer; the executable it writes looks at its own tail before anything
# else, restores from there and ends through its :TOPLEVEL function -- by
# returning, by (quit), and by an error nothing handled.  The executable is a
# copy of this tracked binary, so it reports its own leaks.
cat > "$WORK/exesave.lisp" <<LISPEOF
(load "$WORK/imgfns.lisp")
(defun leak-exe-main ()
  (let ((cmd (first ext:*command-line-args*)))
    (format t "EXE-RAN ~a~%" (funcall (leak-img-a 40) (leak-img-b 1)))
    (cond ((equal cmd "quit") (quit 3))
          ((equal cmd "boom") (error "exe-boom")))))
(ext:save-image "$WORK/leak-exe$EXE_SUFFIX" :executable t :toplevel 'leak-exe-main :quit t)
LISPEOF
run_case "no_leak_after_executable_save" "$WORK/exesave.lisp" --no-image

# run_exe_case NAME WANT-RC [ARG...] — start the delivered executable.
run_exe_case() {
    name=$1
    want=$2
    shift 2
    total=$((total + 1))
    out=$(AMILISP_MEM_DIAG=1 "$WORK/leak-exe$EXE_SUFFIX" "$@" </dev/null 2>&1)
    rc=$?
    report=$(echo "$out" | sed -n 's/^\[mem\] leak report: \([0-9][0-9]*\) block(s), \([0-9][0-9]*\) bytes.*/\1 \2/p' | tail -1)
    case "$out" in
      *"EXE-RAN 42"*) ;;
      *) echo "  FAIL  $name (the executable did not run its toplevel, rc=$rc)"
         echo "$out" | tail -6
         failed=$((failed + 1))
         return ;;
    esac
    if [ "$rc" -ne "$want" ]; then
        echo "  FAIL  $name (exit code $rc, want $want)"
        failed=$((failed + 1))
    elif [ "$report" = "0 0" ]; then
        echo "  ok  $name"
        passed=$((passed + 1))
    else
        echo "  FAIL  $name (leak report: ${report:-none} -- blocks bytes)"
        echo "$out" | grep '^\[mem\]' | head -10
        failed=$((failed + 1))
    fi
}
run_exe_case "no_leak_after_executable_run" 0 some args
run_exe_case "no_leak_after_executable_quit" 3 quit
run_exe_case "no_leak_after_executable_error" 1 boom

# A damaged executable (image bytes missing, trailer intact) leaves before
# the command line is parsed -- with the file table its look at itself opened.
total=$((total + 1))
exe_size=$(wc -c < "$WORK/leak-exe$EXE_SUFFIX")
head -c $((exe_size - 120)) "$WORK/leak-exe$EXE_SUFFIX" > "$WORK/leak-cut$EXE_SUFFIX"
tail -c 20 "$WORK/leak-exe$EXE_SUFFIX" >> "$WORK/leak-cut$EXE_SUFFIX"
chmod +x "$WORK/leak-cut$EXE_SUFFIX"
out=$(AMILISP_MEM_DIAG=1 "$WORK/leak-cut$EXE_SUFFIX" </dev/null 2>&1)
rc=$?
report=$(echo "$out" | sed -n 's/^\[mem\] leak report: \([0-9][0-9]*\) block(s), \([0-9][0-9]*\) bytes.*/\1 \2/p' | tail -1)
case "$rc:$report:$out" in
  1:"0 0":*"cannot be used"*)
    echo "  ok  no_leak_when_damaged_executable_refuses"
    passed=$((passed + 1)) ;;
  *)
    echo "  FAIL  no_leak_when_damaged_executable_refuses (rc=$rc, leak report: ${report:-none})"
    echo "$out" | tail -6
    failed=$((failed + 1)) ;;
esac

# --- startup failures: the exits that never reach a REPL --------------------
# run_fail_case NAME EXPECT [ENV=VAL ...] -- ARG...: the run must fail (rc 1),
# say EXPECT, and still end with zero bytes live.  AmigaOS does not reclaim
# what a failed start forgets: a Clamacs start whose 8 MB heap did not fit
# on a 24 MB 68020 lost 2.5 MB per attempt through a bare exit().
run_fail_case() {
    name=$1
    expect=$2
    shift 2
    envs=""
    while [ "$1" != "--" ]; do envs="$envs $1"; shift; done
    shift
    total=$((total + 1))
    out=$(env AMILISP_MEM_DIAG=1 $envs "$AMILISP" --no-userinit --non-interactive "$@" </dev/null 2>&1)
    rc=$?
    report=$(echo "$out" | sed -n 's/^\[mem\] leak report: \([0-9][0-9]*\) block(s), \([0-9][0-9]*\) bytes.*/\1 \2/p' | tail -1)
    case "$out" in
      *"$expect"*) ;;
      *) echo "  FAIL  $name (no \"$expect\" in the output, rc=$rc)"
         echo "$out" | tail -6
         failed=$((failed + 1))
         return ;;
    esac
    if [ "$rc" -ne 1 ]; then
        echo "  FAIL  $name (exit code $rc, want 1)"
        failed=$((failed + 1))
    elif [ "$report" = "0 0" ]; then
        echo "  ok  $name"
        passed=$((passed + 1))
    else
        echo "  FAIL  $name (leak report: ${report:-none} -- blocks bytes)"
        echo "$out" | grep '^\[mem\]' | head -10
        failed=$((failed + 1))
    fi
}

# The arena refused (the tracer's AMILISP_MEM_FAIL_OVER stands in for a
# fragmented Fast RAM; AMILISP_GENGC=0 so the arena comes from platform_alloc
# as on AmigaOS), with an image staged and without.
run_fail_case "no_leak_when_heap_allocation_fails" "Failed to allocate 8388608-byte heap" \
    AMILISP_GENGC=0 AMILISP_MEM_FAIL_OVER=8388608 -- --no-image --heap 8M --eval '(quit)'
run_fail_case "no_leak_when_heap_allocation_fails_with_image" "Failed to allocate 8388608-byte heap" \
    AMILISP_GENGC=0 AMILISP_MEM_FAIL_OVER=8388608 -- --image "$WORK/leak.img" --heap 8M --eval '(quit)'

# An explicit --image whose header stages but whose payload is refused at
# restore (truncated): the arena and the whole C init exist by then, and the
# exit must go through the normal teardown.
head -c 200 "$WORK/leak.img" > "$WORK/leak-trunc.img"
run_fail_case "no_leak_when_explicit_image_restore_fails" "is truncated" \
    -- --image "$WORK/leak-trunc.img" --eval '(quit)'

echo ""
echo "$passed passed, $failed failed, $total total"
[ "$failed" -eq 0 ] || exit 1
exit 0
