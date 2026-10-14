;;; test-jit.lisp — m68k template JIT verification.
;;;
;;; Loaded from run-tests.lisp via (load ...) inside the #+amigaos
;;; block.  Kept separate so JIT coverage can grow on its own cadence
;;; without bloating the core test file, and so it can be run in
;;; isolation while iterating on codegen.
;;;
;;; Tests cover three things end-to-end on real m68k:
;;;   - byte emission (encoder pipeline writes the expected opcodes)
;;;   - dispatch engagement (cl_jit_invoke counter bumps on call)
;;;   - behavioral correctness (native return value matches bytecode)
;;;
;;; Relies on `*pass-count*` / `*fail-count*` and the `check` helper
;;; established by run-tests.lisp.
;;;
;;; The behavioural checks also run on an arm64 host against the AArch64
;;; walker (tests/test_jit_a64_walk.sh).  The checks marked #+m68k are the
;;; m68k backend's own: its byte goldens and pattern matchers, the
;;; conservative-scan header index, the direct-call sites -- and the
;;; "this compiled" checks for shapes the AArch64 walker leaves to the
;;; interpreter until its phase 3 (NLX frames, dynamic binding, PROGV,
;;; multiple values, closures, &key; specs/native-backend-a64.md).

; The byte-emission goldens below document the exact native code the JIT
; produces for KNOWN speed-1 bytecode shapes.  run-tests.lisp declaims
; (optimize (speed 3)) long before loading this file, which would route
; every defun here through the peephole post-pass (spec 1.8) and change
; the input bytecode out from under the goldens (e.g. the STORE;POP;LOAD
; result tail collapses to STORE, so fast-path template matchers no longer
; fire and walker output shrinks).  Pin the input; peephole-vs-JIT
; interaction is covered by the "peep ..." checks in run-tests.lisp, which
; run at speed 3 with the JIT active.  The suite's speed-3 baseline is
; restored at the end of this file.
(declaim (optimize (speed 1)))
; Everything below inspects a function's native code right after its DEFUN,
; so it runs in eager mode (every function compiles at definition, the way
; the JIT worked before 0.12).  The default -- compile once hot -- is
; restored, and covered, in the "Hot compilation" section at the end.
(defparameter *jit-suite-hot-threshold* (amilisp::%jit-set-hot-threshold 0))

; --- Byte-pipeline smoke: %JIT-COMPILE-STUB writes NOP+RTS into a
; function's native_code slot, %JIT-DUMP-BYTES reads it back. ---

; A function defined with the JIT off stays bytecode (see "hot: defined
; with the JIT off stays bytecode"), which gives a clean "no native_code
; yet" baseline to verify %JIT-COMPILE-STUB attaches the stub bytes.
; (This used to rely on a shape the walkers decline -- (and x y), then
; &optional -- and each one went away when a walker learned it.)
(amilisp::%jit-set-active nil)
(defun jit-stub-test-fn (x &optional y) (or x y))
(amilisp::%jit-set-active t)
(check "jit-dump-before-stub" nil (amilisp::%jit-dump-bytes #'jit-stub-test-fn))
(check "jit-compile-stub-succeeds" t (amilisp::%jit-compile-stub #'jit-stub-test-fn))
; NOP = 0x4E71, RTS = 0x4E75 → bytes 78 113 78 117
#+m68k (check "jit-dump-after-stub" '(78 113 78 117) (amilisp::%jit-dump-bytes #'jit-stub-test-fn))
; The AArch64 stub: movz w0, #0 ; ret, little-endian.
#-m68k (check "jit-dump-after-stub-a64" '(0 0 128 82 192 3 95 214) (amilisp::%jit-dump-bytes #'jit-stub-test-fn))

; --- Round-trip: trivial `() -> NIL` function actually runs as
; native code.  Compiler emits moveq #0,d0 ; rts; OP_CALL dispatches
; into it; counter bumps prove the native path was taken (since the
; bytecode interpreter would return the same value). ---
(defun jit-roundtrip-nil () nil)
; MOVEQ #0,d0 = 0x7000 → 0x70 0x00 = 112 0
; RTS         = 0x4E75 → 0x4E 0x75 = 78 117
#+m68k (check "jit-roundtrip-bytes" '(112 0 78 117)
  (amilisp::%jit-dump-bytes #'jit-roundtrip-nil))
(check "jit-roundtrip-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count)))
    (jit-roundtrip-nil)
    (> (amilisp::%jit-invoke-count) before)))
(check "jit-roundtrip-returns-nil" nil (jit-roundtrip-nil))

; --- Literal-leaf coverage: OP_T, OP_CONST fixnum (fits moveq),
; OP_CONST fixnum (needs move.l), negative fixnum (sign-extension
; round-trip through moveq).  Each verifies exact emitted bytes plus
; behavioral correctness. ---

; Small positive fixnum 42: tagged = (42<<1)|1 = 85 = 0x55.
; Fits signed 8-bit → moveq #85,d0 ; rts → bytes 0x70 0x55 0x4E 0x75.
(defun jit-rt-fix-small () 42)
#+m68k (check "jit-rt-fix-small-bytes" '(112 85 78 117)
  (amilisp::%jit-dump-bytes #'jit-rt-fix-small))
(check "jit-rt-fix-small-returns" 42 (jit-rt-fix-small))

; Negative fixnum -5: tagged = ((-5)<<1)|1 = 0xFFFFFFF7 = signed -9.
; Fits signed 8-bit as 0xF7 → moveq #-9,d0 ; rts → bytes
; 0x70 0xF7 0x4E 0x75 (sign-extended back to 0xFFFFFFF7 on execute).
(defun jit-rt-fix-neg () -5)
#+m68k (check "jit-rt-fix-neg-bytes" '(112 247 78 117)
  (amilisp::%jit-dump-bytes #'jit-rt-fix-neg))
(check "jit-rt-fix-neg-returns" -5 (jit-rt-fix-neg))

; Larger fixnum 1000: tagged = 2001 = 0x000007D1.  Doesn't fit
; signed 8-bit, falls back to move.l #imm32,d0 ; rts:
;   0x20 0x3C  0x00 0x00 0x07 0xD1  0x4E 0x75
(defun jit-rt-fix-big () 1000)
#+m68k (check "jit-rt-fix-big-bytes" '(32 60 0 0 7 209 78 117)
  (amilisp::%jit-dump-bytes #'jit-rt-fix-big))
(check "jit-rt-fix-big-returns" 1000 (jit-rt-fix-big))

; OP_T → CL_T is a heap pointer (runtime-allocated symbol), so the
; exact bytes vary across runs.  Verify the *shape*: 8 bytes,
; starts with the move.l-immediate-to-d0 opcode (0x203C), ends with
; RTS (0x4E75) — and that the function actually returns T.
(defun jit-rt-t () t)
(check "jit-rt-t-returns" t (jit-rt-t))
#+m68k (check "jit-rt-t-shape" t
  (let ((bs (amilisp::%jit-dump-bytes #'jit-rt-t)))
    (and (= 8 (length bs))
         (= 32 (nth 0 bs)) (= 60 (nth 1 bs))   ; 0x20 0x3C
         (= 78 (nth 6 bs)) (= 117 (nth 7 bs))))) ; 0x4E 0x75

; --- 1-arg identity: (defun f (x) x) compiles to
;   jsr cl_jit_runtime_mv_reset  ; 0x4E 0xB9 + abs32 target
;   move.l 8(a7),d0              ; 0x20 0x2F 0x00 0x08
;   rts                          ; 0x4E 0x75
; (12 bytes).  C ABI on m68k puts the first arg at 4(sp) after JSR;
; cl_jit_invoke pushes `func_obj` below the arguments (so OP_UPVAL
; can reach the active closure's upvalues), and the arguments sit in
; operand-stack order above it: the LAST one at 8(sp), argument j of n
; at (8 + 4*(n-1-j))(sp).  The returned CL_Obj is whatever bit pattern
; the caller passed — fixnums, symbols, conses all round-trip
; without reinterpretation.
;
; The leading JSR writes cl_mv_count = 1: OP_LOAD leaves the
; multiple-value buffer describing whatever ran before the call, so
; without it (f (values 1 2)) would return TWO values.  The bytecode
; body carries an explicit OP_MV_RESET for exactly that reason (see
; matches_passthrough / cl_mv_normalize), and the helper's absolute
; address differs per build — so these check the SHAPE and the stack
; displacement, which is what the template is really about. ---
(defun jit-passthrough-shape-p (bs disp)
  (and (= 12 (length bs))
       (= 78 (nth 0 bs)) (= 185 (nth 1 bs))       ; 0x4E 0xB9  jsr abs.l
       (= 32 (nth 6 bs)) (= 47 (nth 7 bs))        ; 0x20 0x2F  move.l d(a7),d0
       (= 0 (nth 8 bs))  (= disp (nth 9 bs))      ; big-endian 16-bit disp
       (= 78 (nth 10 bs)) (= 117 (nth 11 bs))))   ; 0x4E 0x75  rts
(defun jit-id (x) x)
#+m68k (check "jit-id-bytes" t
  (jit-passthrough-shape-p (amilisp::%jit-dump-bytes #'jit-id) 8))
(check "jit-id-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count)))
    (jit-id 42)
    (> (amilisp::%jit-invoke-count) before)))
(check "jit-id-fixnum-small" 7   (jit-id 7))
(check "jit-id-fixnum-neg"   -3  (jit-id -3))
(check "jit-id-fixnum-big"   1000 (jit-id 1000))
(check "jit-id-nil"          nil (jit-id nil))
(check "jit-id-t"            t   (jit-id t))
(check "jit-id-symbol"       'foo (jit-id 'foo))
(check "jit-id-cons"         '(1 2 3) (jit-id '(1 2 3)))
(check "jit-id-string"       "hello" (jit-id "hello"))
; The template's leading mv-reset is load-bearing, not decoration: a
; pass-through returns exactly ONE value even when its argument form
; produced several (CLHS 3.1.7).  Checked warm, so it is the native
; template answering — the interpreter side is covered by the
; "mv hygiene" checks in run-tests.lisp.
(check "jit-id-single-value" '(1)
  (progn (dotimes (i 60) (jit-id 1))
         (multiple-value-list (jit-id (values 1 2)))))

; --- 2-arg pass-through: same template as 1-arg identity (mv-reset
; JSR + load + rts), just a different stack displacement.  The
; arguments are in operand-stack order, so of two the second sits at
; 8(a7) and the first at 12(a7).  The behavioral test then proves
; cl_jit_invoke's 2-arg dispatch loads both args off the VM stack and
; passes them in the right order. ---
(defun jit-2arg-fst (x y) x)
(defun jit-2arg-snd (x y) y)
#+m68k (check "jit-2arg-fst-bytes" t
  (jit-passthrough-shape-p (amilisp::%jit-dump-bytes #'jit-2arg-fst) 12))
#+m68k (check "jit-2arg-snd-bytes" t
  (jit-passthrough-shape-p (amilisp::%jit-dump-bytes #'jit-2arg-snd) 8))
(check "jit-2arg-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count)))
    (jit-2arg-fst 1 2)
    (jit-2arg-snd 3 4)
    (= (+ before 2) (amilisp::%jit-invoke-count))))
(check "jit-2arg-fst-fixnum" 11 (jit-2arg-fst 11 22))
(check "jit-2arg-snd-fixnum" 22 (jit-2arg-snd 11 22))
(check "jit-2arg-fst-mixed" 'a   (jit-2arg-fst 'a "b"))
(check "jit-2arg-snd-mixed" "b"  (jit-2arg-snd 'a "b"))
(check "jit-2arg-fst-distinguishes-args"
       'left  (jit-2arg-fst 'left 'right))
(check "jit-2arg-snd-distinguishes-args"
       'right (jit-2arg-snd 'left 'right))

; --- Higher arities: same matcher / template, different switch case
; in cl_jit_invoke.  Cover arity 3 (middle slot), 6, and 9 (over the
; old cap of six).  Each emits move.l (8+4*(n-1-j))(a7),d0 ; rts where j
; is the source slot (user-arg index) of an arity-n function:
; operand-stack order puts the last argument at 8(a7).  The middle of
; three stays at 12; the 6-arg case puts the first at 28 and the last at
; 8; the 9-arg one the first at 40. ---
(defun jit-3arg-mid (x y z) y)
#+m68k (check "jit-3arg-mid-bytes" t
  (jit-passthrough-shape-p (amilisp::%jit-dump-bytes #'jit-3arg-mid) 12))
(check "jit-3arg-mid-returns" 'b (jit-3arg-mid 'a 'b 'c))

(defun jit-6arg-1 (a b c d e f) a)
(defun jit-6arg-6 (a b c d e f) f)
#+m68k (check "jit-6arg-1-bytes" t
  (jit-passthrough-shape-p (amilisp::%jit-dump-bytes #'jit-6arg-1) 28))
#+m68k (check "jit-6arg-6-bytes" t
  (jit-passthrough-shape-p (amilisp::%jit-dump-bytes #'jit-6arg-6) 8))
(check "jit-6arg-1-returns" 'first  (jit-6arg-1 'first 2 3 4 5 'last))
(check "jit-6arg-6-returns" 'last   (jit-6arg-6 'first 2 3 4 5 'last))
(defun jit-9arg-1 (a b c d e f g h i) a)
#+m68k (check "jit-9arg-1-bytes" t
  (jit-passthrough-shape-p (amilisp::%jit-dump-bytes #'jit-9arg-1) 40))
(check "jit-9arg-1-returns" 'first (jit-9arg-1 'first 2 3 4 5 6 7 8 'last))

; --- Per-opcode walker.  Fires only for shapes the whole-function
; matchers reject.  Uses LINK/UNLK to set up an A6 frame and the m68k
; hardware stack as the operand stack — much larger native code per
; function (≥22 bytes vs the 4–8 bytes the matchers produce) but
; covers arbitrary compositions of the supported opcodes (OP_NIL,
; OP_T, OP_CONST, OP_LOAD, OP_STORE, OP_POP, OP_RET).
;
; (defun walker-nil-1arg (x) nil) bytecode:
;   NIL ; RET   (2 bytes — the compiler emits NIL ; STORE 1 ; POP ;
;   LOAD 1 ; RET, and the peephole pass (spec 4.3) folds the block-result
;   store-reload and the dead store before the RET away)
; arity=1, n_locals=2 → 1 extra local → LINK A6,#-4.
; Slot 1 is the (now unused) block-return local at -4(a6).
;
; With the 3-slot rotating stack cache, the cache_head index advances
; D7 → D5 → D6 → D7 on each push.  This function starts with head=7;
; the first push (OP_NIL) lands in D{next(7)} = D5.  The RET pops it
; back through D5 to D0 (no register shifts — the rotation reclaims
; D5 implicitly).  The prologue saves D5/D6/D7 to A6-relative slots;
; the epilogue restores via A6-relative loads.
;
; Expected native (30 bytes):
;   78 86 255 252   ; link a6,#-4
;   47 7            ; move.l d7,-(a7)         — save D7
;   47 6            ; move.l d6,-(a7)         — save D6
;   47 5            ; move.l d5,-(a7)         — save D5
;   122 0           ; moveq #0,d5             — OP_NIL → D5 (new TOS)
;   32 5            ; move.l d5,d0           \
;   46 46 255 248   ; move.l -8(a6),d7        — restore D7
;   44 46 255 244   ; move.l -12(a6),d6       — restore D6
;   42 46 255 240   ; move.l -16(a6),d5       — restore D5
;   78 94           ; unlk a6                 } — OP_RET
;   78 117          ; rts                    /
(defun walker-nil-1arg (x) nil)
#+m68k (check "walker-nil-1arg-bytes"
  '(78 86 255 252  47 7  47 6  47 5  122 0  32 5
    46 46 255 248  44 46 255 244  42 46 255 240
    78 94  78 117)
  (amilisp::%jit-dump-bytes #'walker-nil-1arg))
(check "walker-nil-1arg-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count)))
    (walker-nil-1arg 99)
    (> (amilisp::%jit-invoke-count) before)))
(check "walker-nil-1arg-returns-nil"    nil (walker-nil-1arg 99))
(check "walker-nil-1arg-ignores-arg"    nil (walker-nil-1arg 'anything))

; Constant return via OP_CONST: (defun walker-fix-1arg (x) 42).
; Fixnum 42 tagged = (42<<1)|1 = 85 = 0x55.  Cache pushes D5 (head
; rotates 7→5), and 85 fits MOVEQ's 8-bit signed range, so the load
; is a 2-byte `moveq #85,d5` (bytes 122 85) rather than the 6-byte
; `move.l #imm32`.  Otherwise the shape mirrors walker-nil-1arg.
(defun walker-fix-1arg (x) 42)
#+m68k (check "walker-fix-1arg-bytes"
  '(78 86 255 252  47 7  47 6  47 5  122 85  32 5
    46 46 255 248  44 46 255 244  42 46 255 240
    78 94  78 117)
  (amilisp::%jit-dump-bytes #'walker-fix-1arg))
(check "walker-fix-1arg-returns" 42 (walker-fix-1arg 'ignored))

; OP_T: shape is identical to OP_NIL except OP_T loads CL_T (a heap
; pointer that doesn't fit in MOVEQ's 8-bit signed range) via the
; 6-byte `move.l #imm32,d7` instead of MOVEQ's 2 bytes.  CL_T's
; address varies across boots so the embedded immediate isn't stable
; — verify total size (4 bytes longer than walker-nil-1arg's 30) and
; behavior.
(defun walker-t-1arg (x) t)
#+m68k (check "walker-t-1arg-size" 34
  (length (amilisp::%jit-dump-bytes #'walker-t-1arg)))
(check "walker-t-1arg-returns-t" t (walker-t-1arg nil))

; Real local-slot use: LET binds an extra slot above the block-return.
; (defun walker-let-id (x) (let ((y x)) y))
;   arity=1, n_locals=3 (x=slot 0, block-return=slot 1, y=slot 2)
;   bytecode as emitted: LOAD 0 ; STORE 2 ; POP ; LOAD 2 ; STORE 1 ; POP ;
;   LOAD 1 ; MV_RESET ; RET — the peephole pass (spec 4.3) folds both
;   store-reloads and the dead stores away, leaving LOAD_MV_RESET 0 ; RET,
;   which the pass-through matcher handles.  Behavior is what matters.
(defun walker-let-id (x) (let ((y x)) y))
(check "walker-let-id-fixnum" 7        (walker-let-id 7))
(check "walker-let-id-symbol" 'banana  (walker-let-id 'banana))
(check "walker-let-id-cons"   '(1 . 2) (walker-let-id (cons 1 2)))
(check "walker-let-id-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count)))
    (walker-let-id 1)
    (> (amilisp::%jit-invoke-count) before)))

; OP_MV_RESET (emitted between AND/OR arms) now compiles via a JSR to
; cl_jit_runtime_mv_reset, which sets cl_mv_count = 1 on the current
; thread.  Previously the walker bailed on the op and `(and x y)` ran
; through the interpreter — landing this is what makes step-line in
; bouncing-lines (which uses `(when (or A B) …)` four times) JIT in
; the first place.  Verify the function compiles AND that AND's
; short-circuit/value semantics still match the interpreter.
(defun walker-mv-reset-and (x y) (and x y))
(check "walker-mv-reset-and-has-native" t
  (let ((bytes (amilisp::%jit-dump-bytes #'walker-mv-reset-and)))
    (and (consp bytes) (> (length bytes) 0))))
(check "walker-mv-reset-and-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count)))
    (walker-mv-reset-and 'a 'b)
    (> (amilisp::%jit-invoke-count) before)))
(check "walker-mv-reset-and-truthy"      'b  (walker-mv-reset-and 'a 'b))
(check "walker-mv-reset-and-short-nil"   nil (walker-mv-reset-and nil 'b))
(check "walker-mv-reset-and-second-nil"  nil (walker-mv-reset-and 'a nil))
(check "walker-mv-reset-and-both-nil"    nil (walker-mv-reset-and nil nil))

; OR variant — same emission path (OP_MV_RESET sits between OR's arms
; in compiler_extra.c:102), but the surrounding control flow uses
; OP_JTRUE instead of OP_JNIL.
(defun walker-mv-reset-or (x y) (or x y))
(check "walker-mv-reset-or-has-native" t
  (let ((bytes (amilisp::%jit-dump-bytes #'walker-mv-reset-or)))
    (and (consp bytes) (> (length bytes) 0))))
(check "walker-mv-reset-or-first"   'a  (walker-mv-reset-or 'a 'b))
(check "walker-mv-reset-or-second"  'b  (walker-mv-reset-or nil 'b))
(check "walker-mv-reset-or-both-nil" nil (walker-mv-reset-or nil nil))

; After an OP_MV_RESET fires, calling (values-list ...) immediately
; should see mv_count = 1 — the walker's JSR-helper handling matches
; the bytecode VM's `cl_mv_count = 1` exactly.  This is the case the
; jit-mv-count-no-per-opcode-reset memory flagged as the failure mode
; if the walker had simply ignored OP_MV_RESET — stale mv state from
; a prior (values ...) leaking into a later consumer.
(defun walker-mv-reset-after-and (x)
  ;; The `(and t x)` arm emits OP_MV_RESET, then `(values 1)` is
  ;; the function's tail.  `nth-value 0` reads value 0 and depends
  ;; on cl_mv_count being a stable 1 by the time of consumption.
  (and t x))
(check "walker-mv-reset-tail-value" 7 (walker-mv-reset-after-and 7))

; --- Branches (OP_JMP / OP_JNIL / OP_JTRUE) ------------------------------
;
; `(if x 1 2)` compiles to LOAD 0 ; JNIL else ; CONST 1 ; JMP end ;
; else: CONST 2 ; end: STORE/POP/LOAD/RET — i.e. exercises both
; forward JNIL and forward JMP plus the patch-resolution loop.
;
; CONST indices and tagged-fixnum bytes depend on the constant pool's
; layout, so byte-exact would be brittle.  Instead verify two stable
; properties: (1) the function JITs (counter bumps), (2) both
; branch arms produce the right return value across argument types
; that exercise the truthiness path (NIL → else, anything else →
; then).

(defun walker-if-1-2 (x) (if x 1 2))
(check "walker-if-1-2-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count)))
    (walker-if-1-2 t)
    (> (amilisp::%jit-invoke-count) before)))
(check "walker-if-1-2-then-t"      1 (walker-if-1-2 t))
(check "walker-if-1-2-then-fixnum" 1 (walker-if-1-2 42))
(check "walker-if-1-2-then-symbol" 1 (walker-if-1-2 'anything))
(check "walker-if-1-2-then-cons"   1 (walker-if-1-2 (cons 1 2)))
(check "walker-if-1-2-else-nil"    2 (walker-if-1-2 nil))

; `(if x x y)` — then-branch returns the test value (so JIT'd code
; threads the same arg through both LOAD-and-test and the result),
; else-branch returns y (the last parameter, which lives at 12(a6)).
(defun walker-if-x-y (x y) (if x x y))
(check "walker-if-x-y-then-fixnum" 7   (walker-if-x-y 7 99))
(check "walker-if-x-y-then-symbol" 'a  (walker-if-x-y 'a 'b))
(check "walker-if-x-y-then-cons"   '(1 . 2) (walker-if-x-y '(1 . 2) nil))
(check "walker-if-x-y-else-fixnum" 99  (walker-if-x-y nil 99))
(check "walker-if-x-y-else-sym"    'b  (walker-if-x-y nil 'b))

; `(when x v)` collapses to (if x v nil) — the NIL else-branch
; exercises the OP_NIL walker case as the else-target rather than
; OP_CONST.  Returns v on truthy x, NIL otherwise.
(defun walker-when-v (x) (when x 'taken))
(check "walker-when-v-truthy" 'taken (walker-when-v t))
(check "walker-when-v-nil"    nil    (walker-when-v nil))

; `(unless x v)` → (if x nil v).  Symmetric coverage to when.
(defun walker-unless-v (x) (unless x 'skipped))
(check "walker-unless-v-truthy" nil      (walker-unless-v t))
(check "walker-unless-v-nil"    'skipped (walker-unless-v nil))

; --- OP_DUP behavioral coverage via cond's empty-body clause ---------
;
; `(cond (x))` compiles to LOAD x ; DUP ; JNIL else ; JMP end ;
; else: NIL ; end: ... — so the test value is returned itself when
; truthy, NIL otherwise.  This is the only Lisp shape that emits
; OP_DUP without also emitting OP_MV_RESET (which the walker still
; rejects), so it doubles as the OP_DUP regression test.
(defun walker-cond-empty-body (x) (cond (x)))
(check "walker-cond-empty-body-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count)))
    (walker-cond-empty-body t)
    (> (amilisp::%jit-invoke-count) before)))
(check "walker-cond-empty-body-truthy-t"      t      (walker-cond-empty-body t))
(check "walker-cond-empty-body-truthy-fixnum" 7      (walker-cond-empty-body 7))
(check "walker-cond-empty-body-truthy-sym"    'foo   (walker-cond-empty-body 'foo))
(check "walker-cond-empty-body-falsey-nil"    nil    (walker-cond-empty-body nil))

; Two-clause cond — exercises the cascade of JMPs plus a NIL fall-
; through at the end.  Use (cond (x 1)) — JNIL else ; CONST 1 ; JMP
; end ; else: NIL ; end: ... , same building blocks as if-1-2 minus
; the second CONST.  (cond ((eq x 'a) 1) ((eq x 'b) 2)) shapes are
; covered separately by the OP_EQ tests below.
(defun walker-cond-only-true (x) (cond (x 1)))
(check "walker-cond-only-true-truthy" 1   (walker-cond-only-true 'anything))
(check "walker-cond-only-true-falsey" nil (walker-cond-only-true nil))

; --- Negative: branch range overflow.  The walker bails when a
; 16-bit branch displacement won't fit.  There's no realistic way to
; provoke this from hand-written Lisp at this scale, so the test is
; left implicit: any function the walker accepts has fit within
; range, and the full Amiga test suite running 2321+ tests through
; the JIT'd pipeline is the regression net.

; --- Arithmetic & comparison (OP_ADD, OP_LT) -----------------------------
;
; `(defun add2 (a b) (+ a b))` compiles to LOAD a; LOAD b; ADD ; postlude.
; The ADD template inlines a fixnum fast path (tag-check via AND+BTST,
; signed add with BVS overflow detect, surplus-tag strip via SUBQ#1)
; with a JSR slow path to cl_jit_runtime_add for non-fixnum / overflow.
; Behavioral tests pin down both the fast path and the round-trip
; through the slow-path JSR.

(defun walker-add2 (a b) (+ a b))
(check "walker-add2-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count)))
    (walker-add2 1 2)
    (> (amilisp::%jit-invoke-count) before)))
(check "walker-add2-small"     5    (walker-add2 2 3))
(check "walker-add2-negative" -1    (walker-add2 -3 2))
(check "walker-add2-zero"      0    (walker-add2 0 0))
(check "walker-add2-pos-pos"   1000 (walker-add2 700 300))

; Fixnum overflow: 2^30 - 1 + 2 = 2^30 + 1, still fixnum range.
; But 2^30 - 1 + 2^30 = 2^31 - 1, beyond CL_FIXNUM_MAX (2^30 - 1).
; The BVS path triggers and the slow-path helper produces a bignum.
; This crosses the JIT/runtime boundary — the result still matches
; what the bytecode VM would produce.
(check "walker-add2-overflow-up"   2147483647 (walker-add2 1073741823 1073741824))
(check "walker-add2-overflow-down" -2147483648 (walker-add2 -1073741824 -1073741824))

; `(defun lt2 (a b) (< a b))` — same template shape for OP_LT.
; Fast path: cmp.l + BLT → push CL_T else push CL_NIL.
(defun walker-lt2 (a b) (< a b))
(check "walker-lt2-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count)))
    (walker-lt2 1 2)
    (> (amilisp::%jit-invoke-count) before)))
(check "walker-lt2-yes"            t   (walker-lt2 1 2))
(check "walker-lt2-no-greater"     nil (walker-lt2 2 1))
(check "walker-lt2-no-equal"       nil (walker-lt2 5 5))
(check "walker-lt2-negatives-yes"  t   (walker-lt2 -10 -3))
(check "walker-lt2-negatives-no"   nil (walker-lt2 -3 -10))
(check "walker-lt2-mixed-sign-yes" t   (walker-lt2 -1 1))
(check "walker-lt2-zero-yes"       t   (walker-lt2 0 1))
(check "walker-lt2-zero-no"        nil (walker-lt2 1 0))

; Slow-path validation: integer compared against a float — the fast
; path's AND+BTST sees bit 0 = 0 for floats (heap pointer), bails, JSR
; cl_jit_runtime_lt routes through cl_arith_compare which handles the
; cross-type compare correctly.
(check "walker-lt2-slow-int-float-yes" t   (walker-lt2 1 1.5))
(check "walker-lt2-slow-int-float-no"  nil (walker-lt2 2 1.5))

; --- OP_SUB.  Mirror of OP_ADD: same fast-path template with SUB.L
; and ADDQ #1 (vs ADD.L / SUBQ #1) and a different slow-path helper.
; Overflow recovery reconstructs original a via ADD d1,d0.
(defun walker-sub2 (a b) (- a b))
(check "walker-sub2-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count)))
    (walker-sub2 5 3)
    (> (amilisp::%jit-invoke-count) before)))
(check "walker-sub2-small"        2    (walker-sub2 5 3))
(check "walker-sub2-zero"         0    (walker-sub2 7 7))
(check "walker-sub2-negative"    -5    (walker-sub2 3 8))
(check "walker-sub2-double-neg"   5    (walker-sub2 -3 -8))
; Fixnum overflow: CL_FIXNUM_MIN - 1 = bignum.
(check "walker-sub2-overflow-min" -1073741825 (walker-sub2 -1073741824 1))
(check "walker-sub2-overflow-max" 2147483647  (walker-sub2 1073741823 -1073741824))
; Slow path through int↔float.
(check "walker-sub2-slow-int-float" 0.5 (walker-sub2 2 1.5))

; --- OP_GT, OP_LE, OP_GE.  Same template as OP_LT, different Bcc.
(defun walker-gt2 (a b) (> a b))
(check "walker-gt2-yes"        t   (walker-gt2 2 1))
(check "walker-gt2-no-less"    nil (walker-gt2 1 2))
(check "walker-gt2-no-equal"   nil (walker-gt2 5 5))
(check "walker-gt2-negatives"  t   (walker-gt2 -3 -10))
(check "walker-gt2-slow-float" t   (walker-gt2 2 1.5))

(defun walker-le2 (a b) (<= a b))
(check "walker-le2-less"        t   (walker-le2 1 2))
(check "walker-le2-equal"       t   (walker-le2 5 5))
(check "walker-le2-greater"     nil (walker-le2 3 1))
(check "walker-le2-negatives"   t   (walker-le2 -10 -3))
(check "walker-le2-slow-float"  t   (walker-le2 1 1.5))

(defun walker-ge2 (a b) (>= a b))
(check "walker-ge2-greater"     t   (walker-ge2 3 1))
(check "walker-ge2-equal"       t   (walker-ge2 5 5))
(check "walker-ge2-less"        nil (walker-ge2 1 2))
(check "walker-ge2-negatives"   t   (walker-ge2 -3 -10))
(check "walker-ge2-slow-float"  nil (walker-ge2 1 1.5))

; --- OP_NUMEQ.  Fixnum fast path with BEQ; slow path validates as
; NUMBER (not REAL — `=` accepts complex per CLHS 12.1.4.1) and
; falls through to cl_numeric_equal for cross-type compares.
(defun walker-numeq2 (a b) (= a b))
(check "walker-numeq2-yes-fix"     t   (walker-numeq2 7 7))
(check "walker-numeq2-no-fix"      nil (walker-numeq2 7 8))
(check "walker-numeq2-yes-neg"     t   (walker-numeq2 -3 -3))
(check "walker-numeq2-slow-int-float-yes" t (walker-numeq2 2 2.0))
(check "walker-numeq2-slow-int-float-no"  nil (walker-numeq2 2 2.5))

; --- OP_EQ.  Pure pointer compare, no slow path.  Lisp `eq` returns
; T iff the two arguments are the same object — true for fixnums
; (immediate, identical tagged value) and identical symbols, false
; for distinct conses / strings / floats even with equal contents.
(defun walker-eq2 (a b) (eq a b))
(check "walker-eq2-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count)))
    (walker-eq2 'a 'a)
    (> (amilisp::%jit-invoke-count) before)))
(check "walker-eq2-same-symbol"     t   (walker-eq2 'foo 'foo))
(check "walker-eq2-different-syms"  nil (walker-eq2 'foo 'bar))
(check "walker-eq2-same-fixnum"     t   (walker-eq2 42 42))
(check "walker-eq2-different-fix"   nil (walker-eq2 42 43))
(check "walker-eq2-nil-self"        t   (walker-eq2 nil nil))
(check "walker-eq2-t-self"          t   (walker-eq2 t t))
(check "walker-eq2-distinct-conses" nil (walker-eq2 (cons 1 2) (cons 1 2)))
(check "walker-eq2-shared-cons"     t   (let ((c (cons 1 2))) (walker-eq2 c c)))

; --- OP_NOT.  Pop value; push CL_T iff NIL, else CL_NIL.  Same shape
; as OP_EQ minus the second pop and CMP — relies on MOVE.L setting Z
; from the popped value.
(defun walker-not (x) (not x))
(check "walker-not-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count)))
    (walker-not t)
    (> (amilisp::%jit-invoke-count) before)))
(check "walker-not-nil"        t   (walker-not nil))
(check "walker-not-t"          nil (walker-not t))
(check "walker-not-fixnum"     nil (walker-not 42))
(check "walker-not-zero"       nil (walker-not 0))    ; 0 is truthy in CL
(check "walker-not-symbol"     nil (walker-not 'foo))
(check "walker-not-cons"       nil (walker-not '(1 2 3)))
(check "walker-not-empty-list" t   (walker-not '()))  ; () is NIL

; --- OP_STRUCT_REF / OP_STRUCT_SET.  Defstruct accessors compile to
; `(%struct-ref obj <idx>)` and `(%struct-set obj <idx> val)`, which
; emit the OP_STRUCT_REF/OP_STRUCT_SET opcodes.  The walker's template
; is JSR-based: pop, push args, JSR cl_jit_runtime_struct_{ref,set},
; clean up the stack, push the helper's return value.  Helper mirrors
; the VM (same STRUCTURE type-check + bounds-check + error messages),
; and is non-allocating so no GC concerns even without precise stack
; scanning.

(defstruct jit-point x y z)

; Accessor reader — get-x emits LOAD 0 ; STRUCT_REF 0 ; postlude.
(defun walker-point-x (p) (jit-point-x p))
(defun walker-point-y (p) (jit-point-y p))
(defun walker-point-z (p) (jit-point-z p))

(check "walker-point-x-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count))
        (p (make-jit-point :x 10 :y 20 :z 30)))
    (walker-point-x p)
    (> (amilisp::%jit-invoke-count) before)))

(check "walker-point-x-fixnum" 10
  (walker-point-x (make-jit-point :x 10 :y 20 :z 30)))
(check "walker-point-y-fixnum" 20
  (walker-point-y (make-jit-point :x 10 :y 20 :z 30)))
(check "walker-point-z-fixnum" 30
  (walker-point-z (make-jit-point :x 10 :y 20 :z 30)))
(check "walker-point-x-nil"    nil
  (walker-point-x (make-jit-point :x nil :y 20 :z 30)))
(check "walker-point-x-symbol" 'hello
  (walker-point-x (make-jit-point :x 'hello :y 20 :z 30)))
(check "walker-point-x-cons"   '(1 2 3)
  (walker-point-x (make-jit-point :x '(1 2 3) :y 20 :z 30)))

; Type error on non-struct: the helper signals STRUCTURE type-error.
; handler-case lets us assert the signal happens without aborting tests.
(check "walker-point-x-type-error" :caught
  (handler-case (progn (walker-point-x 42) :no-error)
    (type-error () :caught)))
(check "walker-point-x-type-error-nil" :caught
  (handler-case (progn (walker-point-x nil) :no-error)
    (type-error () :caught)))

; --- Setter: setf-of-accessor emits OP_STRUCT_SET.
(defun walker-set-point-x (p v) (setf (jit-point-x p) v))
(defun walker-set-point-z (p v) (setf (jit-point-z p) v))

(check "walker-set-point-x-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count))
        (p (make-jit-point :x 0 :y 0 :z 0)))
    (walker-set-point-x p 99)
    (> (amilisp::%jit-invoke-count) before)))

; The setter returns the stored value (CL `setf` semantics).
(check "walker-set-point-x-returns-val" 99
  (walker-set-point-x (make-jit-point :x 0 :y 0 :z 0) 99))

; Round-trip: store via walker-set, read via walker-point.
(check "walker-struct-roundtrip-x" 99
  (let ((p (make-jit-point :x 0 :y 0 :z 0)))
    (walker-set-point-x p 99)
    (walker-point-x p)))
(check "walker-struct-roundtrip-z" 'tag
  (let ((p (make-jit-point :x 0 :y 0 :z 0)))
    (walker-set-point-z p 'tag)
    (walker-point-z p)))
; Setting one slot doesn't disturb the others.
(check "walker-struct-set-leaves-others" '(99 20 30)
  (let ((p (make-jit-point :x 10 :y 20 :z 30)))
    (walker-set-point-x p 99)
    (list (jit-point-x p) (jit-point-y p) (jit-point-z p))))

(check "walker-set-point-x-type-error" :caught
  (handler-case (progn (walker-set-point-x 'not-a-struct 1) :no-error)
    (type-error () :caught)))

; --- OP_MUL.  JSR-only template (no inline fixnum fast path): pop b,
; pop a, push b, push a (C-ABI right-to-left), JSR cl_jit_runtime_mul,
; drop, push result.  The helper preserves the VM's fixnum fast path
; inside cl_arith_mul, so cost vs. bytecode is one extra JSR per call.
; Coverage exercises in-range fixnum, fast-path-fitting cross-products,
; bignum overflow, int↔float promotion, and the NUMBER type-error
; path so behaviour matches the bytecode VM exactly.  OP_DIV is left
; out for now — the compiler doesn't emit it (no `/` case in
; inline_builtin_opcode), so adding a walker template would be dead
; code.
(defun walker-mul2 (a b) (* a b))
(check "walker-mul2-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count)))
    (walker-mul2 6 7)
    (> (amilisp::%jit-invoke-count) before)))
(check "walker-mul2-fix"          42  (walker-mul2 6 7))
(check "walker-mul2-zero"         0   (walker-mul2 0 12345))
(check "walker-mul2-zero-rhs"     0   (walker-mul2 12345 0))
(check "walker-mul2-neg"         -42  (walker-mul2 -6 7))
(check "walker-mul2-double-neg"   42  (walker-mul2 -6 -7))
; Mid-range fixnum × fixnum that fits in fixnum: 1000 * 2000 = 2 000 000.
(check "walker-mul2-mid-fix"      2000000  (walker-mul2 1000 2000))
; Just past the VM's 15-bit fast-path guard: 65535 * 16384 = ~1.07e9 — still
; CL_FIXNUM_MAX-fitting (< 1 073 741 823), so cl_arith_mul returns a fixnum.
(check "walker-mul2-large-fix"    1073725440 (walker-mul2 65535 16384))
; Overflow to bignum: 100000 * 100000 = 1e10 — well past CL_FIXNUM_MAX.
(check "walker-mul2-overflow-bignum" 10000000000 (walker-mul2 100000 100000))
; Cross-type: int * float → float.
(check "walker-mul2-slow-int-float" 7.5 (walker-mul2 3 2.5))
; Non-NUMBER → type-error.
(check "walker-mul2-type-error" :caught
  (handler-case (progn (walker-mul2 'foo 7) :no-error)
    (type-error () :caught)))

; --- OP_CAR / OP_CDR.  JSR-only one-arg templates routing through
; cl_jit_runtime_car / _cdr, which forward to cl_car / cl_cdr.  Those
; already implement the full spec: NIL→NIL, LIST type-error with the
; same diagnostic the VM prints, and unbound-variable detection.
; Non-allocating → GC-safe in all cases.
(defun walker-car1 (lst) (car lst))
(defun walker-cdr1 (lst) (cdr lst))
(check "walker-car1-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count)))
    (walker-car1 '(1 2 3))
    (> (amilisp::%jit-invoke-count) before)))
(check "walker-car1-list"    1   (walker-car1 '(1 2 3)))
(check "walker-car1-single"  'x  (walker-car1 '(x)))
(check "walker-car1-pair"    'a  (walker-car1 (cons 'a 'b)))
(check "walker-car1-nil"     nil (walker-car1 nil))
(check "walker-car1-empty"   nil (walker-car1 '()))
(check "walker-car1-type-error" :caught
  (handler-case (progn (walker-car1 42) :no-error)
    (type-error () :caught)))

(check "walker-cdr1-list"    '(2 3) (walker-cdr1 '(1 2 3)))
(check "walker-cdr1-single"  nil    (walker-cdr1 '(x)))
(check "walker-cdr1-pair"    'b     (walker-cdr1 (cons 'a 'b)))
(check "walker-cdr1-nil"     nil    (walker-cdr1 nil))
(check "walker-cdr1-empty"   nil    (walker-cdr1 '()))
(check "walker-cdr1-type-error" :caught
  (handler-case (progn (walker-cdr1 'foo) :no-error)
    (type-error () :caught)))

; --- OP_CONS: first allocating opcode the walker handles directly.
;
; Compiler inlines (cons a b) (2 args) to OP_CONS rather than the
; FLOAD+CALL path, so a body of `(cons x y)` is a direct test of the
; OP_CONS emitter — pop cdr/car, cache_flush, JSR cl_jit_runtime_cons,
; push result back.  GC during cl_cons is reached by the conservative
; m68k-stack scan; the cache flush is what keeps any residual cached
; heap pointers visible to the scan.
(defun walker-cons1 (x y) (cons x y))
(check "walker-cons1-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count)))
    (walker-cons1 1 2)
    (> (amilisp::%jit-invoke-count) before)))
(check "walker-cons1-fixnums"  '(1 . 2)         (walker-cons1 1 2))
(check "walker-cons1-symbols"  '(a . b)         (walker-cons1 'a 'b))
(check "walker-cons1-mixed"    '(1 . a)         (walker-cons1 1 'a))
(check "walker-cons1-nil-cdr"  '(x)             (walker-cons1 'x nil))
(check "walker-cons1-cons-car" '((1 . 2) . 3)   (walker-cons1 (cons 1 2) 3))
(check "walker-cons1-list-cdr" '(0 1 2 3)       (walker-cons1 0 '(1 2 3)))

; GC stress: build a long list inside the JIT'd body so allocations
; accumulate and the collector is virtually guaranteed to run with
; live operand-stack references on the m68k stack (the partially-
; built list head sits in a local that the conservative scan must
; reach across each cl_cons call).  Returns the list length so the
; check is robust against in-place GC-induced rewrites — if the scan
; missed a root, the list would be truncated or corrupted.
(defun walker-cons-stress (n)
  (let ((lst nil)
        (i 0))
    (tagbody
       loop-top
       (if (< i n)
           (progn
             (setq lst (cons i lst))
             (setq i (+ i 1))
             (go loop-top))))
    lst))
(check "walker-cons-stress-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count)))
    (walker-cons-stress 10)
    (> (amilisp::%jit-invoke-count) before)))
(check "walker-cons-stress-10-length" 10
  (length (walker-cons-stress 10)))
(check "walker-cons-stress-10-head"   9
  (car (walker-cons-stress 10)))
(check "walker-cons-stress-10-last"   0
  (car (last (walker-cons-stress 10))))
; 500 conses (~6 KB) is enough to trip a young-arena GC on the small-
; heap test config; primary goal is to prove the scan keeps the
; growing list rooted across collections, not benchmark speed.
(check "walker-cons-stress-500-length" 500
  (length (walker-cons-stress 500)))

; --- OP_EQ in conditional context: `(cond ((eq x 'a) 1) ((eq x 'b) 2))`.
; Pulls together OP_EQ + OP_DUP + OP_JNIL + branch resolution within a
; single function.
(defun walker-cond-eq (x)
  (cond ((eq x 'a) 1)
        ((eq x 'b) 2)
        (t 99)))
(check "walker-cond-eq-a"        1  (walker-cond-eq 'a))
(check "walker-cond-eq-b"        2  (walker-cond-eq 'b))
(check "walker-cond-eq-fallback" 99 (walker-cond-eq 'c))

; --- Self-contained loop: sum 0..(N-1) via tagbody+go --------------------
;
; This is the JIT's first "real" benchmark shape — every opcode in the
; loop body now JITs (LOAD/STORE/POP/CONST + JNIL/JMP from the prior
; commit + ADD/LT from this one).  The loop runs end-to-end in native
; code with zero re-entry into the bytecode interpreter until OP_RET.
;
; sum-to(N) = N*(N-1)/2.  For N=10: 45.  For N=100: 4950.
(defun walker-sum-to (n)
  (let ((s 0) (i 0))
    (tagbody
       loop-top
       (if (< i n)
           (progn
             (setq s (+ s i))
             (setq i (+ i 1))
             (go loop-top))))
    s))
(check "walker-sum-to-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count)))
    (walker-sum-to 10)
    (> (amilisp::%jit-invoke-count) before)))
(check "walker-sum-to-0"   0     (walker-sum-to 0))
(check "walker-sum-to-1"   0     (walker-sum-to 1))
(check "walker-sum-to-10"  45    (walker-sum-to 10))
(check "walker-sum-to-100" 4950  (walker-sum-to 100))

; --- OP_FLOAD / OP_CALL.  General Lisp call sequencing:
;
;   FLOAD <sym>      ; push symbol's function value
;   <push args>
;   CALL <nargs>     ; pops func + N args, pushes result
;
; The walker emits OP_FLOAD as a JSR to cl_jit_runtime_fload with the
; symbol literal baked in (CL_Obj from constants[idx] at compile
; time), and OP_CALL as a JSR to cl_jit_runtime_call which copies the
; m68k operand-stack args into a local CL_Obj[] and dispatches via
; cl_vm_apply — so closures, builtins, and JIT'd callees all route
; through the standard call path.
;
; OP_TAILCALL has its own walker case (covered in the section below);
; the tests in *this* section use a let-binding wrapper `(let ((r
; expr)) r)` to force the call out of tail position so the OP_CALL
; emitter is exercised specifically.
;
; Coverage: 0/1/3-arg calls, calls to builtins (CL_FUNCTION_P branch
; in cl_vm_apply), JIT-to-JIT chains, recursive fixnum loops (fib),
; undefined-function diagnostic, recovery after longjmp.

; --- Trivial 0-arg call wrapped in a let so the call site is OP_CALL,
; not OP_TAILCALL.  Callee returns a literal; CALL just bounces
; through cl_vm_apply.
(defun walker-call-target-0 () 17)
(defun walker-call-0 () (let ((r (walker-call-target-0))) r))
(check "walker-call-0-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count)))
    (walker-call-0)
    (> (amilisp::%jit-invoke-count) before)))
(check "walker-call-0-returns" 17 (walker-call-0))

; --- 1-arg call wrapped in let.  Identity callee; verifies arg-
; passing order (the helper's reverse-copy of operand_top must
; preserve arg(0)).
(defun walker-call-target-id (x) x)
(defun walker-call-id (x) (let ((r (walker-call-target-id x))) r))
(check "walker-call-id-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count)))
    (walker-call-id 1)
    (> (amilisp::%jit-invoke-count) before)))
(check "walker-call-id-fix"    42     (walker-call-id 42))
(check "walker-call-id-sym"    'q     (walker-call-id 'q))
(check "walker-call-id-nil"    nil    (walker-call-id nil))
(check "walker-call-id-cons"   '(a b) (walker-call-id '(a b)))

; --- 3-arg calls, each selecting a different slot.  A reversed-copy
; or off-by-one bug in argument passing would immediately surface as
; the wrong slot's value coming back.
(defun walker-call-target-3-first (a b c) a)
(defun walker-call-target-3-mid   (a b c) b)
(defun walker-call-target-3-last  (a b c) c)
(defun walker-call-3-first (a b c)
  (let ((r (walker-call-target-3-first a b c))) r))
(defun walker-call-3-mid   (a b c)
  (let ((r (walker-call-target-3-mid   a b c))) r))
(defun walker-call-3-last  (a b c)
  (let ((r (walker-call-target-3-last  a b c))) r))
(check "walker-call-3-first" 'one   (walker-call-3-first 'one 'two 'three))
(check "walker-call-3-mid"   'two   (walker-call-3-mid   'one 'two 'three))
(check "walker-call-3-last"  'three (walker-call-3-last  'one 'two 'three))
(check "walker-call-3-mid-fix" 20   (walker-call-3-mid   10 20 30))

; --- Call to a CL builtin (LIST).  cl_vm_apply takes the
; CL_FUNCTION_P branch, dispatching directly to call_builtin
; without a stub frame.  Same JIT call site, different runtime
; tail.  LIST is used here because CONS now inlines to OP_CONS
; rather than FLOAD+CALL — the call-path coverage moved to a
; builtin the compiler doesn't intrinsify.
(defun walker-call-list2 (a b) (let ((r (list a b))) r))
(check "walker-call-list2-fixnums" '(1 2)     (walker-call-list2 1 2))
(check "walker-call-list2-symbols" '(a b)     (walker-call-list2 'a 'b))

; --- Chained JIT-to-JIT call: caller and callees all JIT'd.  The
; outer caller's body is wrapped in let to keep all three call sites
; OP_CALL; the inner two are already non-tail (their results feed
; the outer call's argument list).
(defun walker-call-add1 (x) (+ x 1))
(defun walker-call-chained (n)
  (let ((r (walker-call-add1 (walker-call-add1 (walker-call-add1 n)))))
    r))
(check "walker-call-chained-fix" 13 (walker-call-chained 10))
(check "walker-call-chained-neg" -2 (walker-call-chained -5))

; --- Recursion: fib(N).  In the else-branch the two recursive calls
; feed `+`, so they're naturally non-tail and emit OP_CALL.  No let
; wrapper needed — exercises OP_CALL twice per non-base case.
(defun walker-call-fib (n)
  (if (< n 2)
      n
      (+ (walker-call-fib (- n 1)) (walker-call-fib (- n 2)))))
(check "walker-call-fib-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count)))
    (walker-call-fib 5)
    (> (amilisp::%jit-invoke-count) before)))
(check "walker-call-fib-0"   0  (walker-call-fib 0))
(check "walker-call-fib-1"   1  (walker-call-fib 1))
(check "walker-call-fib-2"   1  (walker-call-fib 2))
(check "walker-call-fib-7"  13  (walker-call-fib 7))
(check "walker-call-fib-10" 55  (walker-call-fib 10))

; --- Undefined function: FLOAD signals via cl_error → longjmp out
; of the JIT'd frame.  handler-case catches; the JIT frame must
; unwind cleanly without corrupting subsequent calls.
(defun walker-call-undef ()
  (let ((r (no-such-function-defined-here-please))) r))
(check "walker-call-undef-signals" :caught
  (handler-case (progn (walker-call-undef) :no-error)
    (undefined-function () :caught)
    (error            () :caught)))
; Further calls still work after the longjmp unwind.
(check "walker-call-recover-after-error" 42
  (walker-call-id 42))

; --- OP_TAILCALL.  Two paths in the emitter (since 2026-05-15):
;
;   1. Self-recursive TCO.  When nargs == arity and bc->name is a
;      SYMBOL, the emitter prefixes the helper sequence with a
;      runtime guard: compare the func value sitting at 4*N(a7)
;      against this bytecode's CL_Obj.  On match, copy the N args
;      from the operand stack into the A6 frame slots, drop the
;      operand stack, and bra.w back to entry-after-prologue —
;      same LINK frame is reused, zero m68k-stack growth.
;
;   2. Fallback / non-self / redefined.  Guard misses → continue
;      with the helper-based sequence (cache_flush already done at
;      the top, marshal args, JSR cl_jit_runtime_call, drop frame,
;      restore D5/D6/D7, UNLK, RTS).  This is the path cross-function
;      tail calls and post-redefinition self-calls take.
;
; The OP_RET the compiler always emits after OP_TAILCALL becomes
; dead unreachable native code on either branch (still emitted by
; the walker in case other branches land there, just never reached).

; Tail call to a plain user function.  Bare `(walker-tail-target x)` in
; tail position emits OP_TAILCALL; arg-passing order check (single
; arg).
(defun walker-tail-target (x) x)
(defun walker-tail-id (x) (walker-tail-target x))
(check "walker-tail-id-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count)))
    (walker-tail-id 1)
    (> (amilisp::%jit-invoke-count) before)))
(check "walker-tail-id-fix"  42    (walker-tail-id 42))
(check "walker-tail-id-sym"  'q    (walker-tail-id 'q))
(check "walker-tail-id-nil"  nil   (walker-tail-id nil))
(check "walker-tail-id-cons" '(a b) (walker-tail-id '(a b)))

; 3-arg tail call selecting different slots — same arg-passing
; coverage as the OP_CALL version above, just from tail position.
(defun walker-tail-3-first (a b c)
  (walker-call-target-3-first a b c))
(defun walker-tail-3-mid (a b c)
  (walker-call-target-3-mid a b c))
(defun walker-tail-3-last (a b c)
  (walker-call-target-3-last a b c))
(check "walker-tail-3-first" 'one   (walker-tail-3-first 'one 'two 'three))
(check "walker-tail-3-mid"   'two   (walker-tail-3-mid   'one 'two 'three))
(check "walker-tail-3-last"  'three (walker-tail-3-last  'one 'two 'three))

; Tail call to a CL builtin — tail position picks the same OP_TAILCALL
; opcode regardless of callee kind; cl_vm_apply routes to call_builtin.
(defun walker-tail-cons (a b) (cons a b))
(check "walker-tail-cons-fixnums" '(1 . 2) (walker-tail-cons 1 2))
(check "walker-tail-cons-symbols" '(a . b) (walker-tail-cons 'a 'b))

; Tail call inside an IF branch — the most common shape in practice.
; The IF emits a conditional JNIL/JTRUE branch over the two arms; the
; tail-position arms each emit OP_TAILCALL.  Cache invariant: every
; branch target lands with cache_depth=0, and the OP_TAILCALL emitter
; flushes before its JSR, so the post-flush state on either arm
; matches the canonical empty cache.
(defun walker-tail-if (flag x y)
  (if flag (walker-tail-target x) (walker-tail-target y)))
(check "walker-tail-if-true"  'a (walker-tail-if t   'a 'b))
(check "walker-tail-if-false" 'b (walker-tail-if nil 'a 'b))
(check "walker-tail-if-fix-t" 10 (walker-tail-if t   10 20))
(check "walker-tail-if-fix-f" 20 (walker-tail-if nil 10 20))

; Self-recursion accumulator — every recursive call is in tail
; position, so the loop runs entirely through OP_TAILCALL with the
; **native self-TCO path** (landed 2026-05-15): runtime guard at the
; tail-call site compares the func value against this bytecode's
; CL_Obj; on match, args are copied to A6 frame slots and execution
; bra.w's back to entry-after-prologue.  Same LINK frame is reused —
; zero m68k-stack growth.  N can now go deep without blowing the 65
; KB Amiga stack (the bytecode VM's frame reuse on cl_vm.stack is
; matched).  Σ 1..N = N*(N+1)/2.
(defun walker-tail-sum (n acc)
  (if (zerop n)
      acc
      (walker-tail-sum (- n 1) (+ acc n))))
(check "walker-tail-sum-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count)))
    (walker-tail-sum 5 0)
    (> (amilisp::%jit-invoke-count) before)))
(check "walker-tail-sum-0"   0     (walker-tail-sum 0   0))
(check "walker-tail-sum-1"   1     (walker-tail-sum 1   0))
(check "walker-tail-sum-10"  55    (walker-tail-sum 10  0))
(check "walker-tail-sum-20"  210   (walker-tail-sum 20  0))
; Deep self-recursion: without native TCO this would blow the 65 KB
; Amiga stack at N≈50.  N=1000 (sum 500500) proves the LINK frame is
; really being reused; N=5000 (sum 12502500) — well past 5 MB of
; "would-have-grown" stack — proves it's not just lucky inlining.
(check "walker-tail-sum-1000" 500500    (walker-tail-sum 1000 0))
(check "walker-tail-sum-5000" 12502500  (walker-tail-sum 5000 0))

; Self-TCO doesn't fire when the tail-call target isn't `self`.  Two
; cooperating functions exercise the fallback path: `walker-tail-ping`
; ends with a call to `walker-tail-pong` (not self), so its OP_TAILCALL
; emits the guard, the runtime cmp fails (func != ping_bc), and
; control falls through to the helper.  Same for pong.  Each round-
; trip costs one C frame, so we keep the depth modest.  Result chains
; through both layers — proves the guard's mismatch arm works.
(defun walker-tail-pong (x) x)
(defun walker-tail-ping (x) (walker-tail-pong x))
(check "walker-tail-ping-cross"   42 (walker-tail-ping 42))
(check "walker-tail-ping-sym"   'foo (walker-tail-ping 'foo))

; Self-TCO with redefinition.  `defun` of the same name replaces the
; symbol's function cell with a fresh CL_Bytecode.  The original JIT'd
; code's baked-in self-CL_Obj points at the OLD bytecode; the guard
; cmp at runtime sees that the new func != old bc, falls back to the
; helper, which dispatches to the new definition.  Semantics match
; the bytecode VM exactly (`(setf (symbol-function 'foo) #'bar)`-style
; redefinition is honored).
;
; Walk it: define `walker-redef-tco` as a self-recursive countdown
; that JITs through self-TCO; verify it works; redefine it to a
; non-recursive form; verify the new body runs.
(defun walker-redef-tco (n)
  (if (zerop n) :hit-bottom (walker-redef-tco (- n 1))))
(check "walker-redef-tco-self"   :hit-bottom (walker-redef-tco 50))
(defun walker-redef-tco (n) (list :replaced n))
(check "walker-redef-tco-after"  '(:replaced 7) (walker-redef-tco 7))
; And one more time the other direction — redefining back to a
; self-recursive shape proves the symbol cell really is what's being
; consulted at each call.
(defun walker-redef-tco (n)
  (if (zerop n) :back-again (walker-redef-tco (- n 1))))
(check "walker-redef-tco-self-2" :back-again (walker-redef-tco 30))

; Tail call to undefined function: OP_FLOAD signals via cl_error →
; longjmp out of the JIT'd frame.  handler-case catches; subsequent
; JIT'd calls must still work after the unwind (the LINK frame and
; saved D5/D6/D7 don't get restored on the longjmp path, but the
; m68k C-ABI doesn't require it across the unwind because the
; caller's setjmp restored its own context).
(defun walker-tail-undef ()
  (no-such-tailcall-target-please))
(check "walker-tail-undef-signals" :caught
  (handler-case (progn (walker-tail-undef) :no-error)
    (undefined-function () :caught)
    (error              () :caught)))
(check "walker-tail-recover-after-error" 42
  (walker-tail-id 42))

; --- OP_GLOAD / OP_GSTORE.  Global/special-variable load and store:
;
;   GLOAD  <sym>   ; push symbol's dynamic value (TLV first, else cell)
;   GSTORE <sym>   ; write TOS to symbol's dynamic value (peek, no pop)
;
; The walker bakes constants[idx] (a SYMBOL) into the emitted code as a
; 32-bit literal — same JIT-time soundness argument as OP_FLOAD: the
; constants[] slot doesn't get re-bound after compilation, only the
; symbol's value cell, which the helper dereferences on every call.
;
; OP_GLOAD template: push sym, JSR cl_jit_runtime_gload, drop arg,
; push helper's D0 result.  OP_GSTORE template: duplicate TOS as the
; C-ABI's val arg (pushed first right-to-left), push sym as the first
; arg, JSR cl_jit_runtime_gstore, drop the 8-byte arg frame.  The TOS
; survives untouched — matching the VM's "store without pop" semantics.
;
; cl_jit_runtime_gstore mirrors the VM's *PACKAGE* sync (calls
; cl_sync_current_package_from_dynamic when the symbol is *PACKAGE*),
; so SETQ *PACKAGE* through JIT'd code is indistinguishable from the
; bytecode path.

(defvar *walker-glo* 100)

; Reader: function body is a bare special reference, which the
; compiler emits as OP_GLOAD <*walker-glo*>.
(defun walker-gload () *walker-glo*)
(check "walker-gload-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count)))
    (walker-gload)
    (> (amilisp::%jit-invoke-count) before)))
(check "walker-gload-fix" 100 (walker-gload))

; Writer: SETQ on a special emits OP_GSTORE.  Returns the stored value
; (setq's value is the new value), so the JIT'd return must equal the
; argument.
(defun walker-gstore (v) (setq *walker-glo* v))
(check "walker-gstore-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count)))
    (walker-gstore 100)
    (> (amilisp::%jit-invoke-count) before)))
(check "walker-gstore-returns-val" 7 (walker-gstore 7))

; Round-trip: store via walker-gstore, read back via walker-gload.
; Both functions exercise the JIT'd template; if either helper had the
; wrong argument order or the GSTORE emitter popped the TOS by mistake,
; the value coming back would be wrong or the stack would underflow.
(check "walker-glo-roundtrip-fix" 999
  (progn (walker-gstore 999) (walker-gload)))
(check "walker-glo-roundtrip-sym" 'tag
  (progn (walker-gstore 'tag) (walker-gload)))
(check "walker-glo-roundtrip-cons" '(a b c)
  (progn (walker-gstore '(a b c)) (walker-gload)))
(check "walker-glo-roundtrip-nil" nil
  (progn (walker-gstore nil) (walker-gload)))

; Restore so later tests aren't affected by the leftover state.
(setq *walker-glo* 100)

; --- Unbound-variable: GLOAD on a special with no value signals
; UNBOUND-VARIABLE via cl_error → longjmp out of the JIT'd frame.
; makunbound clears the value cell; the next read must signal, and
; subsequent JIT'd calls must still work after the unwind.
(defvar *walker-glo-unbound* :placeholder)
(makunbound '*walker-glo-unbound*)
(defun walker-gload-unbound () *walker-glo-unbound*)
(check "walker-gload-unbound-signals" :caught
  (handler-case (progn (walker-gload-unbound) :no-error)
    (unbound-variable () :caught)
    (error            () :caught)))
; Further calls still work after the longjmp unwind.
(check "walker-gload-recover-after-error" 100
  (walker-gload))

; --- Dynamic binding (LET on a special) participates correctly: the
; JIT'd GLOAD goes through cl_symbol_value which checks TLV first, so
; rebinding *walker-glo* in an outer LET must be visible inside the
; JIT'd reader.
(check "walker-gload-tlv-rebind" 55
  (let ((*walker-glo* 55)) (walker-gload)))
; And after the LET unwinds, the outer global value is restored.
(check "walker-gload-tlv-restore" 100 (walker-gload))

; --- OP_DYNBIND / OP_DYNUNBIND.  `(let ((*special* val)) body)` on a
; defvar'd symbol compiles to a value-producer + OP_DYNBIND + body +
; OP_DYNUNBIND <count>.
;
; OP_DYNBIND template: the value is already on the operand stack (TOS),
; so we push the symbol literal above it and JSR
; cl_jit_runtime_dynbind(sym, val); the cleanup ADDQ #8 drops both —
; matching the VM's "pop value" semantic.  OP_DYNUNBIND template: push
; the u8 count, JSR cl_jit_runtime_dynunbind, drop arg.  Both helpers
; are non-allocating (dyn_stack + TLV table are preallocated).
;
; cl_jit_runtime_dynbind mirrors the VM's *PACKAGE* sync.  An error
; raised through the helper (dyn-stack overflow) longjmps out of the
; JIT'd frame the same way OP_FLOAD's unbound-function path does;
; cl_dynbind_restore_to runs on the error path via the existing
; runtime, so the binding stack stays consistent regardless of how
; control leaves the JIT'd frame.

; Reuse *walker-glo* (defvar'd above to 100).
;
; Basic: bind *walker-glo* to 999 around a body that reads it.  Inside
; the body the JIT'd OP_GLOAD must see 999; after the LET unwinds, the
; outer cell must still read 100.
(defun walker-dyn-let-read ()
  (let ((*walker-glo* 999)) *walker-glo*))
#+m68k (check "walker-dyn-let-read-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count)))
    (walker-dyn-let-read)
    (> (amilisp::%jit-invoke-count) before)))
(check "walker-dyn-let-read-inner"  999 (walker-dyn-let-read))
(check "walker-dyn-let-read-outer-restored" 100 *walker-glo*)

; Bind then mutate inside the body — SETQ on the special hits the
; freshly-rebound TLV, not the outer cell.  After unwind, the outer
; cell must still be 100 (the post-mutation value lived in the TLV
; that the OP_DYNUNBIND restored away).
(defun walker-dyn-let-setq ()
  (let ((*walker-glo* 999))
    (setq *walker-glo* 1234)
    *walker-glo*))
(check "walker-dyn-let-setq-inner" 1234 (walker-dyn-let-setq))
(check "walker-dyn-let-setq-outer-restored" 100 *walker-glo*)

; Two specials bound in one LET → OP_DYNUNBIND with count 2.  Also
; covers the LET semantic that all RHS forms evaluate in the outer
; scope before any binding takes effect.
(defvar *walker-glo2* 200)
(defun walker-dyn-let-two ()
  (let ((*walker-glo*  111)
        (*walker-glo2* 222))
    (+ *walker-glo* *walker-glo2*)))
#+m68k (check "walker-dyn-let-two-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count)))
    (walker-dyn-let-two)
    (> (amilisp::%jit-invoke-count) before)))
(check "walker-dyn-let-two-inner"        333 (walker-dyn-let-two))
(check "walker-dyn-let-two-outer1-rest"  100 *walker-glo*)
(check "walker-dyn-let-two-outer2-rest"  200 *walker-glo2*)

; Nested LETs on the same special — inner shadows outer; after inner
; unwinds, outer's binding (still itself a dyn-binding) is visible.
; After outer unwinds, the global value is back.
(defun walker-dyn-let-nested ()
  (let ((*walker-glo* 11))
    (let ((*walker-glo* 22))
      (let ((*walker-glo* 33))
        *walker-glo*))))
(check "walker-dyn-let-nested-inner"  33  (walker-dyn-let-nested))
(check "walker-dyn-let-nested-outer-restored" 100 *walker-glo*)

; Sequenced: read after inner unwinds, while outer is still bound.
; Verifies OP_DYNUNBIND 1 restores exactly the previous TLV (not
; collapsing both LET layers).
(defun walker-dyn-let-restore-mid ()
  (let ((*walker-glo* 10))
    (let ((*walker-glo* 20)) *walker-glo*)
    *walker-glo*))
(check "walker-dyn-let-restore-mid" 10 (walker-dyn-let-restore-mid))
(check "walker-dyn-let-restore-mid-outer" 100 *walker-glo*)

; --- OP_BLOCK_PUSH / OP_BLOCK_POP / OP_BLOCK_RETURN -----------------------
;
; The compiler only emits these when needs_nlx is true — i.e. when a
; (return-from <tag> ...) actually crosses a closure boundary
; (tree_needs_nlx_block in compiler_special.c).  These tests construct
; that shape via mapcar / mapc closures, then assert (a) the function
; that owns the block JITs (counter bump) and (b) the return-from
; semantics match what the bytecode VM would produce.
;
; The walker emits an inline JSR setjmp at OP_BLOCK_PUSH so the
; captured frame belongs to the JIT'd function itself.  When
; OP_BLOCK_RETURN's helper longjmps, control returns to the
; instruction after the JSR with D0 != 0; the NLX shim then JSRs
; cl_jit_runtime_block_post_longjmp (restores marks + mv_values),
; pushes the result onto the operand stack, and branches to the
; landing IP.

; Simple early-exit: scan a list, return-from on first match.  The
; lambda closes over TARGET so the return-from crosses a closure
; boundary, forcing NLX emission.  No counter-bump assertion: a lambda
; with captured upvalues forces the outer function to emit OP_CLOSURE,
; which is not in the walker's switch — so the function itself runs
; through the bytecode interpreter even though the bytecode contains
; OP_BLOCK_PUSH.  The behaviour-correctness tests below still exercise
; the path because the *bytecode VM* hits BLOCK_PUSH / BLOCK_RETURN
; with the same semantics.
(defun walker-block-find-first (list target)
  (block found
    (mapc (lambda (x) (when (eql x target) (return-from found x))) list)
    nil))
(check "walker-block-find-first-hit"  3   (walker-block-find-first '(1 3 5) 3))
(check "walker-block-find-first-miss" nil (walker-block-find-first '(1 2 3) 9))
(check "walker-block-find-first-first" 1  (walker-block-find-first '(1 2 3) 1))

; No return-from is taken: normal exit through OP_BLOCK_POP, the
; landing receives the implicit body result.  Exercises the
; "setjmp returns 0 → commit → run body → BLOCK_POP" path without
; ever firing longjmp.
(defun walker-block-no-return (list)
  (block tag
    (mapc (lambda (x) (declare (ignore x)) nil) list)
    :normal-exit))
(check "walker-block-no-return-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count)))
    (walker-block-no-return '(1 2 3))
    (> (amilisp::%jit-invoke-count) before)))
(check "walker-block-no-return" :normal-exit (walker-block-no-return '(1 2 3)))
(check "walker-block-no-return-empty" :normal-exit (walker-block-no-return nil))

; Return-from with a non-fixnum value — exercises the result-push
; through the NLX shim's `move.l d0,-(a7)` (no fixnum tag assumed by
; the JIT, so cons cells and symbols round-trip the same way).
(defun walker-block-return-cons ()
  (block b
    (mapc (lambda (x) (return-from b (cons x x))) '(:a))
    :unreached))
(check "walker-block-return-cons" '(:a . :a) (walker-block-return-cons))

(defun walker-block-return-sym ()
  (block b
    (mapc (lambda (x) (return-from b x)) '(:tag))
    :unreached))
(check "walker-block-return-sym" :tag (walker-block-return-sym))

; OP_DYNUNBIND interaction: dyn-bindings established between
; BLOCK_PUSH and the return-from must be unwound when the longjmp
; fires.  cl_jit_runtime_block_post_longjmp restores cl_dyn_top to
; the mark saved by block_alloc; verify by reading the special after
; the function returns.
(defvar *walker-block-dyn* :outer)
(defun walker-block-dyn-unwind ()
  (block b
    (let ((*walker-block-dyn* :inner))
      (mapc (lambda (x) (declare (ignore x))
                         (return-from b *walker-block-dyn*))
            '(t)))
    :unreached))
(check "walker-block-dyn-unwind-inner" :inner (walker-block-dyn-unwind))
(check "walker-block-dyn-unwind-restored" :outer *walker-block-dyn*)

; Loop with explicit RETURN.  The CL `loop` macro emits OP_BLOCK_PUSH
; (anonymous block NIL) and the inner closure that walks the
; collection triggers needs_nlx.  This is the shape `loop ... thereis`
; / `loop ... when ... return` use under the hood.
(defun walker-block-loop-return (list)
  (loop for x in list
        when (and (numberp x) (oddp x))
          do (return x)))
(check "walker-block-loop-return-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count)))
    (walker-block-loop-return '(2 4 5 6))
    (> (amilisp::%jit-invoke-count) before)))
(check "walker-block-loop-return-hit" 5  (walker-block-loop-return '(2 4 5 6)))
(check "walker-block-loop-return-all-even" nil (walker-block-loop-return '(2 4 6)))

; ---- Walker: OP_UPVAL / OP_CELL_SET_UPVAL (closure read + mutation) ----
;
; `sum` is captured AND mutated by the inner lambda, so the boxing
; analysis emits OP_MAKE_CELL for it in the outer and OP_UPVAL +
; OP_CELL_REF (read) / OP_UPVAL + OP_CELL_SET_UPVAL (write) in the
; inner.  With the n_upvalues>0 gate lifted in phase B, the inner
; lambda itself JITs and reaches `sum`'s cell via the func_obj-first
; ABI.  Behaviour test verifies the final accumulated value matches
; the bytecode VM's; the explicit invoke-count bump on both the
; outer (which contains OP_CLOSURE) and the inner (which contains
; OP_UPVAL / OP_CELL_SET_UPVAL) is implicit — mapc invokes the
; inner once per list element.
(defun walker-closure-mutate (list)
  (let ((sum 0))
    (mapc (lambda (x) (setq sum (+ sum x))) list)
    sum))
(check "walker-closure-mutate-empty" 0   (walker-closure-mutate '()))
(check "walker-closure-mutate-one"   7   (walker-closure-mutate '(7)))
(check "walker-closure-mutate-sum"   15  (walker-closure-mutate '(1 2 3 4 5)))
(check "walker-closure-mutate-negs"  0   (walker-closure-mutate '(-3 -2 -1 1 2 3)))

; Same shape but the captured slot is a non-integer accumulator
; (the cell holds a list).  Exercises OP_CELL_REF/OP_CELL_SET_UPVAL
; round-tripping a heap-allocated value through the cell — proves
; we're not accidentally treating the cell payload as a fixnum.
(defun walker-closure-collect (list)
  (let ((acc nil))
    (mapc (lambda (x) (setq acc (cons x acc))) list)
    acc))
(check "walker-closure-collect" '(3 2 1) (walker-closure-collect '(1 2 3)))

; ---- Walker: OP_UWPROT / OP_UWPOP / OP_UWRETHROW + OP_MV_TO_LIST ----
;
; unwind-protect compiles to:
;   OP_UWPROT <i32 offset>
;     <protected body>
;     OP_MV_TO_LIST
;     OP_STORE list_slot
;     OP_POP
;     OP_UWPOP
;     OP_JMP cleanup_start
;   <cleanup_landing/cleanup_start>:
;     <cleanup forms, each OP_POP'd>
;     OP_UWRETHROW
;     OP_FLOAD VALUES-LIST ; OP_LOAD list_slot ; OP_CALL 1
;
; The walker now emits inline JSR setjmp at OP_UWPROT (mirroring
; OP_BLOCK_PUSH), JSRs to runtime helpers for OP_UWPOP/OP_UWRETHROW,
; and routes OP_MV_TO_LIST through cl_jit_runtime_mv_to_list.

; Normal-exit: protected form returns a value, cleanup runs once,
; result is the protected value.
(defvar *walker-uwp-cleanup-count* 0)
(defun walker-uwp-normal ()
  (setq *walker-uwp-cleanup-count* 0)
  (unwind-protect
    (+ 1 2)
    (incf *walker-uwp-cleanup-count*)))
#+m68k (check "walker-uwp-normal-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count)))
    (walker-uwp-normal)
    (> (amilisp::%jit-invoke-count) before)))
(check "walker-uwp-normal-result" 3 (walker-uwp-normal))
(check "walker-uwp-normal-cleanup-ran-once" 1
  (progn (walker-uwp-normal) *walker-uwp-cleanup-count*))

; Error through UWPROT.  The handler-case lives in the outer driver
; so its closure forms don't block JIT of the UWP-bearing inner; the
; inner contains the unwind-protect alone and is JIT-compilable.  The
; counter-bump assertion verifies the inner actually ran as native
; code, so the asserted cleanup behavior is exercising the JIT path
; (not falling back to the bytecode VM's OP_UWPROT).
(defvar *walker-uwp-cleanup-err* 0)
(defun walker-uwp-error-inner ()
  (unwind-protect
    (error "boom from JIT'd uwp")
    (incf *walker-uwp-cleanup-err*)))
(defun walker-uwp-error ()
  (setq *walker-uwp-cleanup-err* 0)
  (handler-case (walker-uwp-error-inner)
    (error (c) (declare (ignore c)) :caught)))
#+m68k (check "walker-uwp-error-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count)))
    (walker-uwp-error)
    (> (amilisp::%jit-invoke-count) before)))
(check "walker-uwp-error-result" :caught (walker-uwp-error))
(check "walker-uwp-error-cleanup-ran" 1
  (progn (walker-uwp-error) *walker-uwp-cleanup-err*))

; Two JIT'd UWPROT frames stacked on the NLX stack.  Error unwinds
; through both: inner cleanup runs first (via cl_jit_runtime_uwprot_
; post_longjmp / OP_UWRETHROW pending==2 walking to outer), then
; outer cleanup, then the unhandled error reaches the driver's
; handler-case.
(defvar *walker-uwp-order* nil)
(defun walker-uwp-nested-inner ()
  (unwind-protect
    (unwind-protect
      (error "force unwind")
      (push :inner *walker-uwp-order*))
    (push :outer *walker-uwp-order*)))
(defun walker-uwp-nested ()
  (setq *walker-uwp-order* nil)
  (handler-case (walker-uwp-nested-inner)
    (error (c) (declare (ignore c)) :caught)))
#+m68k (check "walker-uwp-nested-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count)))
    (walker-uwp-nested)
    (> (amilisp::%jit-invoke-count) before)))
(check "walker-uwp-nested-result" :caught (walker-uwp-nested))
(check "walker-uwp-nested-order" '(:outer :inner)
  (progn (walker-uwp-nested) *walker-uwp-order*))

; Multiple-value round-trip on the normal-exit path.  Protected form
; returns three values via (values …); OP_MV_TO_LIST in JIT'd code
; captures them into the unwind-protect's stash slot, cleanup runs
; (here a nop), then VALUES-LIST republishes them at exit.  Inner
; function is JIT-compilable; outer wraps it in multiple-value-list
; for the test harness.
(defun walker-uwp-mv-inner ()
  (unwind-protect
    (values 1 2 3)
    nil))
(defun walker-uwp-mv ()
  (multiple-value-list (walker-uwp-mv-inner)))
#+m68k (check "walker-uwp-mv-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count)))
    (walker-uwp-mv)
    (> (amilisp::%jit-invoke-count) before)))
(check "walker-uwp-mv-result" '(1 2 3) (walker-uwp-mv))

; Multiple values surviving a RETURN-FROM that unwinds THROUGH a JIT'd
; unwind-protect.  Regression for the JIT NLX-MV drop: block_return's
; interposing-UWPROT branch only saved the primary value into the
; pending record (not cl_pending_mv_*), and uwprot_rethrow never
; restored the mv set into the target block frame — so the frame kept
; its mv_count=1 baseline and the secondary value was lost.  Expected
; (NIL T); the bug produced (NIL).  The unwind-protect lives in its own
; JIT-compilable function so the throw really crosses a native UWPROT.
(defun walker-uwp-mv-nlx-cleanup (fn)
  (unwind-protect (funcall fn) nil))
(defun walker-uwp-mv-nlx ()
  (block done
    (walker-uwp-mv-nlx-cleanup (lambda () (return-from done (values nil t))))))
#+m68k (check "walker-uwp-mv-nlx-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count)))
    (walker-uwp-mv-nlx)
    (> (amilisp::%jit-invoke-count) before)))
(check "walker-uwp-mv-nlx-result" '(nil t)
  (multiple-value-list (walker-uwp-mv-nlx)))

; Same shape but with three values and a non-trivial cleanup form, to
; confirm the cleanup running between the throw and the rethrow does
; not disturb the carried multiple-value set.
(defvar *walker-uwp-mv-nlx3-ran* 0)
(defun walker-uwp-mv-nlx3-cleanup (fn)
  (unwind-protect (funcall fn) (incf *walker-uwp-mv-nlx3-ran*)))
(defun walker-uwp-mv-nlx3 ()
  (setq *walker-uwp-mv-nlx3-ran* 0)
  (block done
    (walker-uwp-mv-nlx3-cleanup
      (lambda () (return-from done (values 10 20 30))))))
(check "walker-uwp-mv-nlx3-result" '(10 20 30)
  (multiple-value-list (walker-uwp-mv-nlx3)))
(check "walker-uwp-mv-nlx3-cleanup-ran" 1
  (progn (walker-uwp-mv-nlx3) *walker-uwp-mv-nlx3-ran*))

; THROW (rather than RETURN-FROM) carrying multiple values through a
; JIT'd unwind-protect — exercises the CATCH target arm of the same
; uwprot_rethrow mv-restore fix.
(defun walker-uwp-mv-throw-cleanup (fn)
  (unwind-protect (funcall fn) nil))
(defun walker-uwp-mv-throw ()
  (catch 'tag
    (walker-uwp-mv-throw-cleanup (lambda () (throw 'tag (values :a :b))))))
(check "walker-uwp-mv-throw-result" '(:a :b)
  (multiple-value-list (walker-uwp-mv-throw)))

; --- OP_CATCH / OP_UNCATCH -----------------------------------------------
;
; Same JIT-inline-setjmp protocol as OP_BLOCK_PUSH; differences are the
; tag is a runtime value (popped from the operand stack) and the
; matching pop is OP_UNCATCH (search-backward for CL_NLX_CATCH).  Catch
; frames live on the same NLX stack as block / tagbody / uwprot, so
; throws from either VM or JIT code reach catches on either side via
; the buf field captured at setjmp.
;
; No counter-bump on the catch-owning function: the throw site lives in
; an inner closure (so the throw crosses a closure boundary, like
; return-from), and the outer body that contains OP_CATCH also contains
; the OP_CLOSURE for the lambda — closure with captures is walker-
; supported now, but counter-bumps for these tests are not the
; objective.  The behavioural correctness covers the JIT path: the
; outer function's OP_CATCH / OP_UNCATCH sequence is the substrate the
; throw lands in, and any incorrect emit would corrupt the operand
; stack or the NLX top.

; Normal exit: body returns without throwing.  Exercises the
; setjmp==0 → commit → body → OP_UNCATCH path.
(defun walker-catch-normal ()
  (catch 'tag
    (+ 1 2)))
(check "walker-catch-normal" 3 (walker-catch-normal))

; Throw from a closure captured inside the catch body.  The closure
; closes over the catch tag implicitly (it's a literal), and the
; throw call site is inside the lambda — same shape as the
; walker-block-find-first test, applied to catch/throw.
(defun walker-catch-throw (list target)
  (catch 'found
    (mapc (lambda (x) (when (eql x target) (throw 'found x))) list)
    nil))
(check "walker-catch-throw-hit"   3   (walker-catch-throw '(1 3 5) 3))
(check "walker-catch-throw-miss"  nil (walker-catch-throw '(1 2 3) 9))
(check "walker-catch-throw-first" 1   (walker-catch-throw '(1 2 3) 1))

; Non-fixnum throw value — exercises the result-push through the
; NLX shim's `move.l d0,-(a7)` for a heap-allocated cons cell.
(defun walker-catch-throw-cons ()
  (catch 'tag
    (mapc (lambda (x) (throw 'tag (cons x x))) '(:a))
    :unreached))
(check "walker-catch-throw-cons" '(:a . :a) (walker-catch-throw-cons))

; OP_DYNUNBIND interaction: dyn-bindings established between OP_CATCH
; and the throw must be unwound when the longjmp fires.  Mirrors the
; walker-block-dyn-unwind shape.
(defvar *walker-catch-dyn* :outer)
(defun walker-catch-dyn-unwind ()
  (catch 'tag
    (let ((*walker-catch-dyn* :inner))
      (mapc (lambda (x) (declare (ignore x))
                         (throw 'tag *walker-catch-dyn*))
            '(t)))
    :unreached))
(check "walker-catch-dyn-unwind-inner" :inner (walker-catch-dyn-unwind))
(check "walker-catch-dyn-unwind-restored" :outer *walker-catch-dyn*)

; Runtime tag value (not a literal symbol): the tag is computed and
; the same value is passed to throw.  Exercises the cache_pop_to_dn
; → JSR catch_alloc path with a non-baked-in tag value.
(defun walker-catch-runtime-tag (tag)
  (catch tag
    (mapc (lambda (x) (throw tag x)) '(42))
    :unreached))
(check "walker-catch-runtime-tag-int"  42  (walker-catch-runtime-tag 99))
(check "walker-catch-runtime-tag-sym"  42  (walker-catch-runtime-tag 'foo))

; Nested catches: outer tag should not match an inner throw to the
; outer tag from inside the inner catch body — the inner catch frame
; doesn't intercept tags it doesn't own.  Same NLX search-backward
; semantics as block_return.
(defun walker-catch-nested ()
  (catch 'outer
    (catch 'inner
      (mapc (lambda (x) (declare (ignore x))
                         (throw 'outer :from-inner))
            '(t))
      :inner-fall-through)
    :outer-fall-through))
(check "walker-catch-nested-outer" :from-inner (walker-catch-nested))

; Unwind-protect interposition: a throw across a UWPROT frame must
; run cleanup first.  The JIT'd catch frame here is the throw target;
; the UWPROT in between is owned by the inner closure.  Cleanup ran
; if the counter incremented.
(defvar *walker-catch-cleanup* 0)
(defun walker-catch-uwp ()
  (setq *walker-catch-cleanup* 0)
  (catch 'tag
    (mapc (lambda (x) (declare (ignore x))
                       (unwind-protect
                         (throw 'tag :thrown)
                         (incf *walker-catch-cleanup*)))
          '(t))
    :unreached))
(check "walker-catch-uwp-result"  :thrown (walker-catch-uwp))
(check "walker-catch-uwp-cleanup" 1
  (progn (walker-catch-uwp) *walker-catch-cleanup*))

; --- &key support: native kw-prologue ----------------------------------
;
; The walker emits a kw prologue (LINK + save D5/D6/D7 + JSR
; cl_jit_runtime_kw_prologue) for bytecodes whose lambda-list carries
; &key.  The entry is the positional ABI (the count in D1, the
; arguments above A6) and the helper NIL-initialises every slot then
; populates key_slots[]/key_suppliedp_slots[] via the same matcher
; logic as vm.c's OP_CALL.  Each test below proves a different facet:
; default-when-missing, value-when-supplied, suppliedp tracking,
; right-to-left "leftmost duplicate wins", unknown-key signal,
; :allow-other-keys, odd-kwarg signal.

; Two-key function; defaults exercise the OP_LOAD-of-suppliedp /
; OP_JTRUE-skip pattern the compiler emits for key defaults.
(defun walker-key-2 (&key (a 10) (b 20))
  (+ a b))
#+m68k (check "walker-key-2-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count)))
    (walker-key-2)
    (> (amilisp::%jit-invoke-count) before)))
(check "walker-key-2-defaults"      30 (walker-key-2))
(check "walker-key-2-supplied-a"    23 (walker-key-2 :a 3))
(check "walker-key-2-supplied-b"    15 (walker-key-2 :b 5))
(check "walker-key-2-both-supplied"  9 (walker-key-2 :a 4 :b 5))
(check "walker-key-2-both-reversed"  9 (walker-key-2 :b 5 :a 4))

; Suppliedp tracking — user-declared supplied-p var.  When the key is
; not passed, the suppliedp slot stays NIL (kw_prologue's NIL-init);
; when passed, the helper writes CL_T into the slot.
(defun walker-key-suppliedp (&key (x 0 xp))
  (if xp (cons :given x) (cons :missing x)))
(check "walker-key-suppliedp-missing" '(:missing . 0) (walker-key-suppliedp))
(check "walker-key-suppliedp-given-zero" '(:given . 0) (walker-key-suppliedp :x 0))
(check "walker-key-suppliedp-given-nil" '(:given) (walker-key-suppliedp :x nil))
(check "walker-key-suppliedp-given-value" '(:given . 42) (walker-key-suppliedp :x 42))

; Leftmost duplicate keyword wins per CLHS 3.4.1.4.1.  The helper
; walks the pairs right-to-left so each overwrite is shadowed by the
; next (= leftmost) occurrence — the final slot value is the
; leftmost one.
(defun walker-key-dup (&key v) v)
(check "walker-key-dup-leftmost" 1 (walker-key-dup :v 1 :v 2 :v 3))

; Unknown keyword without :allow-other-keys → CL_ERR_ARGS via
; cl_error.  longjmp out of the JIT frame; handler-case catches it.
(defun walker-key-strict (&key a) a)
(check "walker-key-strict-known" 7 (walker-key-strict :a 7))
(check "walker-key-strict-unknown" :caught
  (handler-case (progn (walker-key-strict :b 1) :no-error)
    (error () :caught)))

; :allow-other-keys baked into the lambda list → flags bit 1 set →
; helper skips the unknown-keyword check.
(defun walker-key-allow (&key a &allow-other-keys) a)
(check "walker-key-allow-known"   9 (walker-key-allow :a 9))
(check "walker-key-allow-unknown" 9 (walker-key-allow :a 9 :extra 'whatever))

; :allow-other-keys T passed by the caller turns on the per-call
; bypass even when the callee didn't declare &allow-other-keys.
(defun walker-key-caller-bypass (&key a) a)
(check "walker-key-caller-bypass" 5
  (walker-key-caller-bypass :a 5 :extra 99 :allow-other-keys t))

; Odd number of keyword arguments → CL_ERR_ARGS.  The kw_prologue's
; n_extra & 1 check is reached only after positional filling, so this
; specifically exercises the post-positional path.  (apply ensures
; the malformed call survives any compile-time argument analysis.)
(defun walker-key-odd (&key a) a)
(check "walker-key-odd-signals" :caught
  (handler-case (progn (apply #'walker-key-odd '(:a)) :no-error)
    (error () :caught)))

; Required arg + &key combination: positional copies into slot 0,
; key slots live above it.  Verifies the helper handles
; `arity = 1, n_keys = 2` correctly.
(defun walker-key-req+2 (x &key (a 10) (b 20))
  (list x a b))
(check "walker-key-req+2-defaults"    '(1 10 20) (walker-key-req+2 1))
(check "walker-key-req+2-supplied"    '(1  3  4) (walker-key-req+2 1 :a 3 :b 4))
(check "walker-key-req+2-reversed"    '(1  3  4) (walker-key-req+2 1 :b 4 :a 3))

; --- OP_FSTORE.  Compiler emits this for `(defun ...)` to install the
; closure in the symbol's function cell — a nested defun therefore
; lands an OP_FSTORE inside the enclosing JIT'd function's body.
; Walker pattern is identical to OP_GSTORE: cache-flush so TOS lives
; at (a7), push it, push the baked symbol literal, JSR helper, drop.
(defun walker-fstore-installer ()
  (defun walker-fstore-installed-fn (x) (* x x))
  'installed)
#+m68k (check "walker-fstore-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count)))
    (walker-fstore-installer)
    (> (amilisp::%jit-invoke-count) before)))
(check "walker-fstore-returns-tag"    'installed (walker-fstore-installer))
; The nested defun must have actually installed the function — call it.
(check "walker-fstore-installs-fn"    25
  (progn (walker-fstore-installer) (walker-fstore-installed-fn 5)))
; And re-running the installer should re-install (idempotent in observable
; behaviour — the body still returns 'installed and the inner still works).
(check "walker-fstore-reinstall"      49
  (progn (walker-fstore-installer) (walker-fstore-installed-fn 7)))

; --- OP_LIST.  Compiler emits this only from `(trace ...)` / `(untrace
; ...)` — both forms call %{TRACE,UNTRACE}-FUNCTION on each name and
; then collect the per-call return values into a list.  %UNTRACE-FUNCTION
; is the cleaner test substrate: it returns the symbol unchanged whether
; or not the symbol was previously traced, so the OP_LIST result is
; exactly the literal symbol list with no side effects on tests that
; care about the trace state.
(defun walker-op-list-3 ()
  (untrace walker-untrace-tag-a walker-untrace-tag-b walker-untrace-tag-c))
(check "walker-op-list-3-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count)))
    (walker-op-list-3)
    (> (amilisp::%jit-invoke-count) before)))
(check "walker-op-list-3-result"
  '(walker-untrace-tag-a walker-untrace-tag-b walker-untrace-tag-c)
  (walker-op-list-3))

; n=1 — exercises the no-LEA-drop edge of the walker emit.
(defun walker-op-list-1 () (untrace walker-untrace-tag-a))
(check "walker-op-list-1-result" '(walker-untrace-tag-a)
  (walker-op-list-1))

; Larger n triggers a 28-byte LEA drop and stresses the conservative-
; scan path across multiple cl_cons allocations: 7 elements means 7
; cons cells, building bottom-up from operand_top[0] = tag-g.
(defun walker-op-list-7 ()
  (untrace walker-untrace-tag-a walker-untrace-tag-b walker-untrace-tag-c
           walker-untrace-tag-d walker-untrace-tag-e walker-untrace-tag-f
           walker-untrace-tag-g))
(check "walker-op-list-7-result"
  '(walker-untrace-tag-a walker-untrace-tag-b walker-untrace-tag-c
    walker-untrace-tag-d walker-untrace-tag-e walker-untrace-tag-f
    walker-untrace-tag-g)
  (walker-op-list-7))

; --- OP_RPLACA.  Compiler inlines `(rplaca cons new-car)` ... actually
; `rplaca` isn't in the inline_builtin_opcode table; OP_RPLACA is what
; (setf (car ...) ...) and (setf (first ...) ...) lower to.  Tested via
; the setf form so the test stays neutral to potential inline expansion
; changes.
(defun walker-rplaca-1 (c v) (setf (car c) v))
(check "walker-rplaca-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count))
        (c (cons 1 2)))
    (walker-rplaca-1 c 99)
    (> (amilisp::%jit-invoke-count) before)))
; rplaca returns the new car (CLHS setf semantics).
(check "walker-rplaca-returns-val" 99
  (walker-rplaca-1 (cons 1 2) 99))
; Mutation reaches the cons cell.
(check "walker-rplaca-mutates" '(99 . 2)
  (let ((c (cons 1 2))) (walker-rplaca-1 c 99) c))
; Non-cons → type-error.
(check "walker-rplaca-type-error" :caught
  (handler-case (progn (walker-rplaca-1 42 'x) :no-error)
    (type-error () :caught)))
(check "walker-rplaca-type-error-nil" :caught
  (handler-case (progn (walker-rplaca-1 nil 'x) :no-error)
    (type-error () :caught)))

; --- OP_ASET.  Emitted by `(setf (aref v idx) val)` on the 1D fast
; path; dispatches across general-vector, simple-string, and bit-vector
; in a single helper.
(defun walker-aset-1 (v idx val) (setf (aref v idx) val))
(check "walker-aset-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count))
        (v (make-array 3 :initial-element 0)))
    (walker-aset-1 v 0 99)
    (> (amilisp::%jit-invoke-count) before)))
; General vector — value type unconstrained.
(check "walker-aset-vector-returns-val" 99
  (walker-aset-1 (make-array 3 :initial-element 0) 1 99))
(check "walker-aset-vector-mutates" t
  (equalp #(0 99 0)
          (let ((v (make-array 3 :initial-element 0)))
            (walker-aset-1 v 1 99) v)))
(check "walker-aset-vector-symbol" 'tag
  (let ((v (make-array 3 :initial-element 0)))
    (walker-aset-1 v 2 'tag)
    (aref v 2)))
; Simple string — value must be a CHARACTER.
(check "walker-aset-string-mutates" "axc"
  (let ((s (copy-seq "abc")))
    (walker-aset-1 s 1 #\x) s))
(check "walker-aset-string-wrong-type" :caught
  (handler-case (progn (walker-aset-1 (copy-seq "abc") 0 42) :no-error)
    (type-error () :caught)))
; Bit-vector — value must be 0 or 1.
(check "walker-aset-bv-mutates" #*101
  (let ((bv (make-array 3 :element-type 'bit :initial-element 0)))
    (walker-aset-1 bv 0 1)
    (walker-aset-1 bv 2 1)
    bv))
(check "walker-aset-bv-out-of-range-value" :caught
  (handler-case
      (progn
        (walker-aset-1 (make-array 3 :element-type 'bit :initial-element 0) 0 2)
        :no-error)
    (type-error () :caught)))
; Bounds + base-type errors.
(check "walker-aset-out-of-range" :caught
  (handler-case
      (progn (walker-aset-1 (make-array 3 :initial-element 0) 99 'x) :no-error)
    (error () :caught)))
(check "walker-aset-not-a-vector" :caught
  (handler-case (progn (walker-aset-1 42 0 'x) :no-error)
    (type-error () :caught)))

; --- OP_RPLACD.  Mirror of RPLACA via (setf (cdr ...) ...).
(defun walker-rplacd-1 (c v) (setf (cdr c) v))
(check "walker-rplacd-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count))
        (c (cons 1 2)))
    (walker-rplacd-1 c 99)
    (> (amilisp::%jit-invoke-count) before)))
(check "walker-rplacd-returns-val" 99
  (walker-rplacd-1 (cons 1 2) 99))
(check "walker-rplacd-mutates" '(1 . 99)
  (let ((c (cons 1 2))) (walker-rplacd-1 c 99) c))
(check "walker-rplacd-type-error" :caught
  (handler-case (progn (walker-rplacd-1 42 'x) :no-error)
    (type-error () :caught)))

; --- OP_ARGC.  The compiler emits it only inside the &optional
; prologue: see the "&optional" section after the direct-call sites.

; --- OP_MV_LOAD / OP_NTH_VALUE.  Both come from multiple-value-bind
; / nth-value.  multiple-value-bind expands to OP_MV_LOAD for vars
; 1+; nth-value uses OP_NTH_VALUE.
(defun walker-mv-load-1 (a b)
  (multiple-value-bind (x y) (values a b)
    (+ x y)))
#+m68k (check "walker-mv-load-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count)))
    (walker-mv-load-1 10 20)
    (> (amilisp::%jit-invoke-count) before)))
(check "walker-mv-load-primary" 30 (walker-mv-load-1 10 20))
(defun walker-mv-load-2 (a b c)
  (multiple-value-bind (x y z) (values a b c)
    (list x y z)))
(check "walker-mv-load-three" '(1 2 3) (walker-mv-load-2 1 2 3))
; missing values default to NIL.
(defun walker-mv-load-missing (a)
  (multiple-value-bind (x y) (values a)
    (list x y)))
(check "walker-mv-load-missing-is-nil" '(7 nil) (walker-mv-load-missing 7))

(defun walker-nth-value-1 (n) (nth-value n (values 'a 'b 'c)))
#+m68k (check "walker-nth-value-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count)))
    (walker-nth-value-1 0)
    (> (amilisp::%jit-invoke-count) before)))
(check "walker-nth-value-0" 'a (walker-nth-value-1 0))
(check "walker-nth-value-1" 'b (walker-nth-value-1 1))
(check "walker-nth-value-2" 'c (walker-nth-value-1 2))
(check "walker-nth-value-out-of-range" nil (walker-nth-value-1 99))
(check "walker-nth-value-type-error" :caught
  (handler-case (progn (walker-nth-value-1 'not-a-number) :no-error)
    (type-error () :caught)))

; --- OP_ASSERT_TYPE.  Emitted by `the` and by `check-type` (which
; expands to a typep + restart-case loop; the inner type check uses
; OP_ASSERT_TYPE indirectly).  The cleanest emitter is `(the TYPE
; FORM)` which lowers to OP_ASSERT_TYPE on the result of FORM.
(defun walker-assert-type-fixnum (x) (the fixnum x))
(check "walker-assert-type-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count)))
    (walker-assert-type-fixnum 42)
    (> (amilisp::%jit-invoke-count) before)))
(check "walker-assert-type-pass" 42 (walker-assert-type-fixnum 42))
(check "walker-assert-type-fail" :caught
  (handler-case (progn (walker-assert-type-fixnum "not a fixnum") :no-error)
    (type-error () :caught)))
(defun walker-assert-type-symbol (x) (the symbol x))
(check "walker-assert-type-symbol-pass" 'foo (walker-assert-type-symbol 'foo))
(check "walker-assert-type-symbol-fail" :caught
  (handler-case (progn (walker-assert-type-symbol 42) :no-error)
    (type-error () :caught)))

; --- OP_APPLY.  Compiler emits OP_APPLY for `(apply fn ... list)`.  The
; walker pops arglist + func, JSRs cl_jit_runtime_apply (which flattens
; the arglist and routes through cl_vm_apply).
(defun walker-apply-1 (fn args) (apply fn args))
(check "walker-apply-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count)))
    (walker-apply-1 #'+ '(1 2 3))
    (> (amilisp::%jit-invoke-count) before)))
; Simple builtin via apply.
(check "walker-apply-builtin" 6 (walker-apply-1 #'+ '(1 2 3)))
; Empty arglist.
(check "walker-apply-empty" 0 (walker-apply-1 #'+ nil))
; User-defined function via apply.
(defun walker-apply-helper (a b c) (list c b a))
(check "walker-apply-user-fn" '(3 2 1)
  (walker-apply-1 #'walker-apply-helper '(1 2 3)))
; (apply fn arg1 ... arglist) — leading args plus a trailing list.
(defun walker-apply-leading (a fn args) (apply fn a args))
(check "walker-apply-leading-args" 10
  (walker-apply-leading 1 #'+ '(2 3 4)))
; Symbol as function — resolves through symbol-function.
(check "walker-apply-symbol-fn" 6
  (walker-apply-1 '+ '(1 2 3)))

; --- OP_PROGV_BIND / OP_PROGV_UNBIND.  `progv` lowers to PROGV_BIND
; (consume two lists, dyn-bind in lockstep, push fixnum mark) then
; PROGV_UNBIND on body exit.  Walker round-trips both through helpers
; that mirror the VM exactly.

(defvar walker-progv-not-special 0)  ; ensure symbol exists for symbol-value lookup

(defun walker-progv-1 (syms vals)
  (progv syms vals
    (symbol-value (car syms))))

#+m68k (check "walker-progv-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count)))
    (walker-progv-1 '(walker-progv-not-special) '(42))
    (> (amilisp::%jit-invoke-count) before)))

; Single binding visible inside body.
(check "walker-progv-single" 42
  (walker-progv-1 '(walker-progv-not-special) '(42)))

; Bindings unwind: outer value restored after progv exit.
(setf walker-progv-not-special 7)
(check "walker-progv-restored-after-exit" 7
  (progn (walker-progv-1 '(walker-progv-not-special) '(99))
         walker-progv-not-special))

; Multiple bindings + access via SYMBOL-VALUE inside.
(defvar walker-progv-a 0)
(defvar walker-progv-b 0)
(defun walker-progv-multi ()
  (progv '(walker-progv-a walker-progv-b) '(10 20)
    (+ (symbol-value 'walker-progv-a)
       (symbol-value 'walker-progv-b))))
(check "walker-progv-multi-sum" 30 (walker-progv-multi))

; Fewer values than symbols → trailing symbols are bound to no-value.
; Reading the unbound symbol inside the progv signals UNBOUND-VARIABLE
; per CLHS PROGV semantics.
(defun walker-progv-short-vals ()
  (progv '(walker-progv-a walker-progv-b) '(5)
    (handler-case (symbol-value 'walker-progv-b)
      (error () :unbound))))
(check "walker-progv-short-values" :unbound (walker-progv-short-vals))

; Non-symbol in symbols list → type-error from the helper.
(defun walker-progv-bad-syms ()
  (progv '(42) '(1) :body))
(check "walker-progv-type-error" :caught
  (handler-case (progn (walker-progv-bad-syms) :no-error)
    (error () :caught)))

; --- OP_DIV.  Today's compiler routes `/` through OP_FLOAD+OP_CALL, not
; OP_DIV, so this opcode isn't reached by user code via the standard
; `/` path — but the walker's prescan and emitter are in place so any
; future inliner that emits OP_DIV picks them up immediately.  No direct
; test reaches the emitter through Lisp source today.

; --- GC compaction reloc: a JIT'd body bakes heap CL_Obj references as
; 32-bit immediate operands (here the OP_FLOAD symbol for the helper
; call).  A *moving* compaction relocates those symbols; the immediates
; buried in the platform_alloc'd code buffer are invisible to the
; conservative stack scan and would go stale unless cl_jit_compile's
; reloc table + the compactor's forwarding pass (mem.c, TYPE_BYTECODE)
; rewrite them in place.  Pre-fix this crashed with "OP_FLOAD: JIT call
; site has non-symbol constant 0x........" once a GC fired mid-loop.
;
; Heap-pressure recipe mirrors run-tests.lisp's "rational/float compare
; gc-safe": a retained filler array keeps the heap full so the
; per-iteration consing forces compaction while the JIT'd loop runs.
(defun jit-reloc-add1 (x) (+ x 1))
(defun jit-reloc-sum (n)
  (let ((acc 0))
    (dotimes (i n acc)
      (setq acc (+ acc (jit-reloc-add1 i)))
      (list i i i))))            ; per-iteration alloc to pressure the heap

; Confirm the body actually runs as native code (else the test wouldn't
; exercise the baked-immediate path at all).
(check "jit-reloc-sum runs native" t
  (let ((before (amilisp::%jit-invoke-count)))
    (jit-reloc-sum 3)
    (> (amilisp::%jit-invoke-count) before)))

; sum_{i=0}^{99} (i+1) = 5050.  A stale FLOAD symbol would crash or call
; the wrong function; the retained filler keeps the heap pressured so a
; compaction relocates jit-reloc-add1 mid-run.
(check "jit-reloc-sum gc-safe result" 5050
  (let ((filler (make-array 50000 :initial-element 1)))
    (+ (jit-reloc-sum 100) (aref filler 0) -1)))

; --- GC sweep frees a dead bytecode's JIT artifacts.  gc_finalize_dead's
; TYPE_BYTECODE arm platform_frees native_code + native_relocs (they were
; leaked for good before); host builds never attach native code, so this
; Amiga run is the only end-to-end coverage of the free itself.  The loop
; churns out short-lived JIT'd leaves, drops every reference, and
; compacts each batch — a double-free or use-after-free would guru the
; suite here.  The second compact re-walks the corpses (NULLed fields
; must make re-finalization a no-op), and the probe proves fresh JIT
; compilation still works afterwards.
(check "jit-dead-bytecode-native-freed" 42
  (progn
    (dotimes (i 24)
      (let ((name (intern (format nil "JIT-DEAD-FN-~D" i))))
        (eval (list 'defun name '() i))
        ; must actually be JIT'd, else the finalizer has nothing to free
        (unless (amilisp::%jit-dump-bytes (symbol-function name))
          (error "expected ~S to be JIT-compiled" name))
        (funcall name)
        (fmakunbound name)
        (unintern name))
      (ext:gc-compact))
    (ext:gc-compact)
    (defun jit-after-free-probe () 42)
    (jit-after-free-probe)))

; --- Block-start index (mem.c gc_hdr_page[]): the conservative native-
; stack scan validates each spilled word against it instead of walking
; the whole arena per collection.  The JIT'd loops above compacted and
; swept with live native frames (pins, gaps, splits, bump resets); the
; index must still agree with the arena's header chain everywhere.  A
; violation here is the bug class that would surface as a phantom mark
; or a missed pin under the JIT — on the host the C unit test
; tests/test_gc_hdr_index.c covers the same invariant without native
; frames.  (-1 would mean no index at all: never on the Amiga, whose
; collector is always the classic one.)
#+m68k (check "gc block-start index clean after JIT GC stress" 0
  (progn
    (let ((filler (make-array 30000 :initial-element 1)))
      (jit-reloc-sum 200)
      (ext:gc-compact)
      (jit-reloc-sum 200)
      (ext:gc)
      (aref filler 0))
    (ext:%gc-audit-hdr-index)))

; ---- Walker: OP_HANDLER_CASE_PUSH / OP_HANDLER_CASE_POP ----
;
; HANDLER-CASE is the special form AMILISP::%HANDLER-CASE (Tier-4 phase
; 2): one NLX frame plus one clause binding per clause, and a landing
; table of per-clause OP_JMPs.  The walker emits the same inline JSR
; setjmp shape as OP_BLOCK_PUSH; the longjmp arm pushes the condition and
; dispatches on the matched clause index (cl_jit_runtime_handler_case_
; clause) to that clause's table entry.  Every function below must attach
; native code (%jit-dump-bytes non-NIL: the walker no longer bails on the
; opcode) so the asserted behaviour is the native path's.
(defun walker-hc-normal (x)
  (handler-case (* x 2)
    (error (c) (declare (ignore c)) :err)))
#+m68k (check "walker-hc-normal-compiled" t (not (null (amilisp::%jit-dump-bytes #'walker-hc-normal))))
#+m68k (check "walker-hc-normal-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count)))
    (walker-hc-normal 21)
    (> (amilisp::%jit-invoke-count) before)))
(check "walker-hc-normal-result" 42 (walker-hc-normal 21))

(defun walker-hc-error (x)
  (handler-case (error "hc boom ~A" x)
    (error (c) (list :caught (search "hc boom" (princ-to-string c))))))
#+m68k (check "walker-hc-error-compiled" t (not (null (amilisp::%jit-dump-bytes #'walker-hc-error))))
(check "walker-hc-error-result" '(:caught 0) (walker-hc-error 1))

; Three clauses: the dispatch must reach the second and third table
; entries, not only the first.
(define-condition walker-hc-c1 (error) ())
(define-condition walker-hc-c2 (error) ())
(defun walker-hc-dispatch (which)
  (handler-case (cond ((eql which 1) (error 'walker-hc-c1))
                      ((eql which 2) (error 'walker-hc-c2))
                      ((eql which 3) (error "plain"))
                      (t :none))
    (walker-hc-c1 () :first)
    (walker-hc-c2 (c) (declare (ignore c)) :second)
    (error () :third)))
#+m68k (check "walker-hc-dispatch-compiled" t (not (null (amilisp::%jit-dump-bytes #'walker-hc-dispatch))))
(check "walker-hc-dispatch" '(:first :second :third :none)
  (list (walker-hc-dispatch 1) (walker-hc-dispatch 2)
        (walker-hc-dispatch 3) (walker-hc-dispatch 4)))

; The error is raised in a JIT'd callee and lands in the JIT'd caller's
; clause: cl_handler_case_transfer → cl_nlx_jump into our setjmp buf.
(defun walker-hc-callee (x) (if (> x 0) (error "neg wanted") (- x)))
(defun walker-hc-caller (x)
  (handler-case (walker-hc-callee x)
    (error (c) (declare (ignore c)) :from-callee)))
#+m68k (check "walker-hc-caller-compiled" t (not (null (amilisp::%jit-dump-bytes #'walker-hc-caller))))
(check "walker-hc-across-frames" '(5 :from-callee)
  (list (walker-hc-caller -5) (walker-hc-caller 5)))

; An interposed unwind-protect in the same JIT'd function: the cleanup
; runs first (the transfer is parked as pending kind 3 and re-initiated by
; uwprot_rethrow), then the clause sees the count.
(defvar *walker-hc-cleanups* 0)
(defun walker-hc-uwp ()
  (setq *walker-hc-cleanups* 0)
  (handler-case
      (unwind-protect (error "through cleanup")
        (incf *walker-hc-cleanups*))
    (error () (list :caught *walker-hc-cleanups*))))
#+m68k (check "walker-hc-uwp-compiled" t (not (null (amilisp::%jit-dump-bytes #'walker-hc-uwp))))
(check "walker-hc-uwp-cleanup-first" '(:caught 1) (walker-hc-uwp))

; Nested: the inner clause does not match and the outer does, then the
; inner matches and the outer frame is popped normally afterwards.
(defun walker-hc-nested (which)
  (handler-case
      (handler-case (if (eql which :inner) (error 'walker-hc-c1) (error 'walker-hc-c2))
        (walker-hc-c1 () :inner-caught))
    (walker-hc-c2 () :outer-caught)))
#+m68k (check "walker-hc-nested-compiled" t (not (null (amilisp::%jit-dump-bytes #'walker-hc-nested))))
(check "walker-hc-nested" '(:inner-caught :outer-caught)
  (list (walker-hc-nested :inner) (walker-hc-nested :outer)))

; Multiple values on the normal path, and a :no-error clause.
(defun walker-hc-mv () (handler-case (values 1 2 3) (error () :err)))
(check "walker-hc-mv" '(1 2 3) (multiple-value-list (walker-hc-mv)))
(defun walker-hc-noerr (x)
  (handler-case (* x 3)
    (error () :err)
    (:no-error (v) (list :ok v))))
(check "walker-hc-no-error" '(:ok 9) (walker-hc-noerr 3))

; Both exits pop the frame and the clause bindings: 300 rounds of each
; path would overflow a leaked NLX or handler stack.
(defun walker-hc-loop (n)
  (let ((hits 0))
    (dotimes (i n hits)
      (handler-case (if (oddp i) (error "odd") i)
        (error () (incf hits))))))
#+m68k (check "walker-hc-loop-compiled" t (not (null (amilisp::%jit-dump-bytes #'walker-hc-loop))))
(check "walker-hc-loop-no-leak" 150 (walker-hc-loop 300))

;; --- The direct call path (jit_dispatch, src/jit/runtime_m68k.c).  A JIT'd
;; caller reaches C builtins, FFI stubs and native callees straight from its
;; operand stack; only interpreted callees, generic functions, arity
;; mismatches and traced functions still take the cl_vm_apply trampoline
;; (with its stub interpreter frame).  Each check pins one arm; the numbers
;; behind the change are in trunk/bench-jit-call.lisp.

;; A native callee.  With the direct-call sites off (the kill switch, see
;; the jit-site-* block below): the caller's own entry plus one
;; cl_jit_invoke per call.  With them on, only the call that fills the
;; caller's site goes through cl_jit_invoke; the rest JSR straight in.
(defun jdc-leaf (a b) (+ a b))
(defun jdc-caller (n) (let ((s 0)) (dotimes (i n) (setq s (jdc-leaf s 1))) s))
(check "jit-direct-native-callee-compiled" '(t t)
  (list (not (null (amilisp::%jit-dump-bytes #'jdc-leaf)))
        (not (null (amilisp::%jit-dump-bytes #'jdc-caller)))))
(check "jit-direct-native-callee-invokes" 11
  (progn
    (amilisp::%jit-set-direct-calls nil)
    (unwind-protect
         (let ((before (amilisp::%jit-invoke-count)))
           (jdc-caller 10)
           (- (amilisp::%jit-invoke-count) before))
      (amilisp::%jit-set-direct-calls t))))
#+m68k (check "jit-direct-native-callee-invokes-direct" 2
  (progn
    (amilisp::%jit-set-direct-calls t)  ; a fresh generation: the site refills
    (let ((before (amilisp::%jit-invoke-count)))
      (jdc-caller 10)
      (- (amilisp::%jit-invoke-count) before))))
(check "jit-direct-native-callee-value" 1000 (jdc-caller 1000))

;; A native callee's multiple values survive the direct entry, as do a
;; builtin's (the MV buffer is the per-thread one either way).
(defun jdc-mv-leaf (a) (values a (+ a 1)))
(defun jdc-mv-caller () (multiple-value-list (jdc-mv-leaf 1)))
(check "jit-direct-native-callee-mv" '(1 2) (jdc-mv-caller))
(defun jdc-builtin-mv () (multiple-value-list (floor 7 2)))
(check "jit-direct-builtin-mv" '(3 1) (jdc-builtin-mv))

;; A &key native callee is reached from native code too (since "Direct
;; calls into &key callees" below, through a filled site).
(defun jdc-key (a &key (b 10)) (+ a b))
(defun jdc-key-caller () (list (jdc-key 1) (jdc-key 1 :b 2)))
#+m68k (check "jit-direct-key-callee-compiled" t
  (not (null (amilisp::%jit-dump-bytes #'jdc-key))))
(check "jit-direct-key-callee" '(11 3) (jdc-key-caller))

;; OP_CALL with a function value (FUNCALL) takes the same path.
(defun jdc-funcall (f a b) (funcall f a b))
(check "jit-direct-funcall-native" 7 (jdc-funcall #'jdc-leaf 3 4))
(check "jit-direct-funcall-builtin" '(3 . 4) (jdc-funcall #'cons 3 4))

;; Arity mismatches keep the VM's diagnostics: a builtin's from
;; validate_builtin, a native callee's from OP_CALL (the direct path only
;; takes a call the callee's lambda list accepts).
(defun jdc-call0 (f) (funcall f))
(check "jit-direct-builtin-arity-error" t
  (handler-case (progn (jdc-call0 #'car) nil)
    (error (e) (not (null (search "too few arguments" (format nil "~A" e)))))))
(defun jdc-call1 (f a) (funcall f a))
(check "jit-direct-native-arity-error" t
  (handler-case (progn (jdc-call1 #'jdc-leaf 1) nil)
    (error (e) (not (null (search "Too few arguments to JDC-LEAF" (format nil "~A" e)))))))

;; An FFI stub (DEFCFUN) from a JIT'd caller: IoErr() through dos.library.
;; AmigaOS only: on a host the AMIGA.FFI package does not exist, and the
;; reader error would end the LOAD of this file right here.
#+amigaos
(progn
  (defvar *jdc-dos* (amiga:open-library "dos.library" 36))
  (amiga.ffi:defcfun jdc-ioerr *jdc-dos* -132 ())
  (defun jdc-ffi-caller () (integerp (jdc-ioerr)))
  (check "jit-direct-ffi-stub" t (jdc-ffi-caller))
  (amiga:close-library *jdc-dos*))

;; A traced callee still traces from a JIT'd caller (tracing routes every
;; call through the trampoline).
(defun jdc-traced-leaf (x) (* x x))
(defun jdc-traced-caller (x) (jdc-traced-leaf x))
(trace jdc-traced-leaf)
(let* ((s (make-string-output-stream))
       (captured (progn (let ((*trace-output* s)) (jdc-traced-caller 5))
                        (get-output-stream-string s))))
  (check "jit-direct-traced-callee" t
    (not (null (search "JDC-TRACED-LEAF" captured)))))
(untrace jdc-traced-leaf)
(check "jit-direct-untraced-callee" 25 (jdc-traced-caller 5))

;; Native frames nest on the C stack, so runaway recursion through the
;; direct path must reach the C-stack guard and signal, not crash.
(defun jdc-deep (n) (if (= n 0) 0 (+ 1 (jdc-deep (- n 1)))))
(check "jit-direct-deep-recursion-guarded" :caught
  (handler-case (progn (jdc-deep 400000) :finished)
    (error () :caught)))
(check "jit-direct-recursion-after-guard" 100 (jdc-deep 100))

;; Allocating builtins called directly, with collections in between: the
;; arguments live on the rooted VM stack for the call and the caller's
;; operand stack is scanned conservatively.
(defun jdc-alloc (n) (let ((acc nil)) (dotimes (i n) (setq acc (list i (car acc)))) acc))
;; m68k: the host's heap (32M in the AArch64 walker test) needs no
;; collection for 300000 conses, so "a collection happened" is the Amiga's.
#+m68k
(stress-check "jit-direct-builtin-across-gc" '(t (299999 299998))
  (let ((g0 (amilisp::%get-gc-count)))
    (let ((r (jdc-alloc 300000)))
      (list (> (amilisp::%get-gc-count) g0) r))))

;; Regression: jit_dispatch's native-callee fast path (the TYPE_BYTECODE /
;; TYPE_CLOSURE arm above, src/jit/runtime_m68k.c) used to resolve the callee's
;; raw CL_Bytecode* before polling thr->gc_requested / calling
;; cl_gc_safepoint(), then dereference that same stale pointer afterwards.
;; A peer thread's concurrent EXT:GC-COMPACT runs exactly inside that poll
;; (that's what gc_requested/cl_gc_safepoint coordinate) and can relocate
;; the CL_Bytecode/CL_Closure the pointer was resolved from, since it's a
;; raw pointer and not a CL_Obj compaction fixes up.  Several workers
;; hammer a JIT-compiled native leaf through the direct path while another
;; thread compacts concurrently -- a stale pointer shows up as a wrong
;; result or a guru, not a hang, so this only needs to run enough rounds
;; to land calls inside the compaction window.
(defun jdc-mt-leaf (a b) (+ a b))
(defun jdc-mt-caller (n)
  (let ((s 0))
    (dotimes (i n) (setq s (jdc-mt-leaf s 1)))
    s))
(stress-check "jit-direct-native-callee-across-concurrent-gc" t
  (progn
    (unless (amilisp::%jit-dump-bytes #'jdc-mt-leaf)
      (error "jdc-mt-leaf did not JIT-compile"))
    (let ((workers nil))
      (dotimes (w 4)
        (push (mp:make-thread
                (lambda ()
                  (let ((good t))
                    (dotimes (i 50)
                      (unless (= (jdc-mt-caller 200) 200)
                        (setq good nil)))
                    good))
                :name "jdc-mt-worker")
              workers))
      (let ((compactor (mp:make-thread
                          (lambda () (dotimes (i 200) (ext:gc-compact)))
                          :name "jdc-mt-compactor")))
        (let ((results (mapcar #'mp:join-thread workers)))
          (mp:join-thread compactor)
          (every #'identity results))))))

;; --- Positional native ABI: argument order (specs/jit-direct-calls.md
;; phase 1).  A positional native function finds its parameters above A6 in
;; operand-stack order (the last argument nearest the return address), so a
;; native caller will be able to JSR into it without re-copying them.  Every
;; reader of a parameter slot has to agree on that order: OP_LOAD / OP_STORE,
;; closure captures, the pass-through template, the self-tail-call argument
;; rewrites and cl_jit_invoke's entry.  Each callee returns all of its
;; arguments, reached from an interpreted caller (cl_jit_invoke straight from
;; the VM), from a native caller (jit_dispatch) and by FUNCALL.  Distinct
;; argument types make a swapped pair show.
(defun abi-0 () (list))
(defun abi-1 (a) (list a))
(defun abi-2 (a b) (list a b))
(defun abi-3 (a b c) (list a b c))
(defun abi-4 (a b c d) (list a b c d))
(defun abi-5 (a b c d e) (list a b c d e))
(defun abi-6 (a b c d e f) (list a b c d e f))
;; OP_STORE into the first and the last parameter slot.
(defun abi-store-6 (a b c d e f)
  (setq a (list :a a) f (list :f f))
  (list a b c d e f))
;; Parameters captured by a closure (OP_CLOSURE reads them via slot_disp).
(defun abi-capture-3 (a b c) (funcall (lambda () (list c b a))))
;; Self tail calls through OP_TAILCALL_GLOBAL and OP_TAILCALL that rotate
;; their parameters: the rewrite into the parameter slots must land each
;; argument where the next round's OP_LOAD reads it.
(defun abi-rot-global (n a b c d)
  (if (= n 0) (list a b c d) (abi-rot-global (- n 1) b c d a)))
(defun abi-rot-funcall (n a b c d)
  (if (= n 0) (list a b c d) (funcall #'abi-rot-funcall (- n 1) b c d a)))
;; Native callers.
(defun abi-native-caller ()
  (list (abi-0) (abi-1 1) (abi-2 1 'b) (abi-3 1 'b "c")
        (abi-4 1 'b "c" #\d) (abi-5 1 'b "c" #\d '(e))
        (abi-6 1 'b "c" #\d '(e) 6.5)))
(defun abi-native-funcall (f g)
  (list (funcall f 1 'b "c") (funcall g 1 'b "c" #\d '(e) 6.5)))
;; An interpreted caller: defined with the JIT off, it stays bytecode.
(amilisp::%jit-set-active nil)
(defun abi-vm-caller (&optional (x 1))
  (list (abi-0) (abi-1 x) (abi-2 x 'b) (abi-3 x 'b "c")
        (abi-4 x 'b "c" #\d) (abi-5 x 'b "c" #\d '(e))
        (abi-6 x 'b "c" #\d '(e) 6.5)))
(amilisp::%jit-set-active t)
(defparameter *abi-expected*
  '(() (1) (1 b) (1 b "c") (1 b "c" #\d) (1 b "c" #\d (e))
    (1 b "c" #\d (e) 6.5)))
(check "jit-abi-all-native" t
  (every (lambda (f) (not (null (amilisp::%jit-dump-bytes f))))
         (list #'abi-0 #'abi-1 #'abi-2 #'abi-3 #'abi-4 #'abi-5 #'abi-6
               #'abi-store-6 #'abi-capture-3 #'abi-rot-global
               #'abi-rot-funcall #'abi-native-caller #'abi-native-funcall)))
(check "jit-abi-vm-caller-not-native" nil (amilisp::%jit-dump-bytes #'abi-vm-caller))
(check "jit-abi-from-vm" *abi-expected* (abi-vm-caller))
(check "jit-abi-from-native" *abi-expected* (abi-native-caller))
(check "jit-abi-funcall-native" '((1 b "c") (1 b "c" #\d (e) 6.5))
  (abi-native-funcall #'abi-3 #'abi-6))
(check "jit-abi-apply" '(1 b "c" #\d (e) 6.5)
  (apply #'abi-6 '(1 b "c" #\d (e) 6.5)))
(check "jit-abi-store" '((:a 1) b "c" #\d (e) (:f 6.5))
  (abi-store-6 1 'b "c" #\d '(e) 6.5))
(check "jit-abi-capture" '("c" b 1) (abi-capture-3 1 'b "c"))
(check "jit-abi-self-tco-global" '((1 b "c" #\d) (b "c" #\d 1) (#\d 1 b "c"))
  (list (abi-rot-global 0 1 'b "c" #\d) (abi-rot-global 1 1 'b "c" #\d)
        (abi-rot-global 1003 1 'b "c" #\d)))
(check "jit-abi-self-tco-funcall" '((1 b "c" #\d) (b "c" #\d 1) (#\d 1 b "c"))
  (list (abi-rot-funcall 0 1 'b "c" #\d) (abi-rot-funcall 1 1 'b "c" #\d)
        (abi-rot-funcall 1003 1 'b "c" #\d)))

;; --- The native entry trampoline and the C-stack floor
;; (specs/jit-direct-calls.md phase 2).  cl_jit_invoke enters native code
;; through cl_jit_enter (src/jit/jit_enter_m68k.s), which loads A3 with the
;; CL_Thread and pushes the arguments; the jit-abi-* checks above go
;; through it for every positional arity, the walker-key-* checks for the
;; keyword ABI.  The outermost entry computes CL_Thread.jit_c_floor once:
;; %JIT-C-FLOOR returns (FLOOR SP) from inside native code, NIL outside.
(defun jef-floor () (amilisp::%jit-c-floor))
(defun jef-deep (n)
  (if (= n 0) (jef-floor) (car (list (jef-deep (- n 1))))))
(defun jef-error (n) (if (= n 0) (error "jef") (car (list (jef-error (- n 1))))))
;; Interpreted (defined with the JIT off): calls the builtin with no native
;; frame on the stack.
(amilisp::%jit-set-active nil)
(defun jef-vm-floor (&optional x) (declare (ignore x)) (amilisp::%jit-c-floor))
(amilisp::%jit-set-active t)
(check "jit-enter-all-native" t
  (every (lambda (f) (not (null (amilisp::%jit-dump-bytes f))))
         (list #'jef-floor #'jef-deep #'jef-error)))
(check "jit-enter-floor-outside-native" nil (jef-vm-floor))
;; The floor is the m68k entry trampoline's (cl_jit_enter); AArch64 has
;; none -- %JIT-C-FLOOR is NIL there, and a direct call checks the C stack
;; itself (tests/test_jit_a64_walk.sh part 4).
#+m68k
(let ((r (jef-floor)))
  (check "jit-enter-floor-shape" t
    (and (consp r) (integerp (first r)) (integerp (second r)) t))
  ;; The floor lies below the native frame, by less than any Amiga stack
  ;; (the test runner's is 128K..800K) minus the 16K margin.
  (check "jit-enter-floor-below-sp" t
    (and (< 0 (first r) (second r))
         (< (- (second r) (first r)) (* 4 1024 1024))))
  ;; Computed once, at the outermost entry: fifty nested native frames
  ;; deeper the floor is the same and the stack pointer lower.
  (let ((d (jef-deep 50)))
    (check "jit-enter-floor-same-when-nested" (first r) (first d))
    (check "jit-enter-floor-sp-deeper" t (< (second d) (second r))))
  ;; An error unwinding out of native code leaves no stale floor behind:
  ;; the next outermost entry finds the same one.
  (check "jit-enter-error-unwinds" :caught
    (handler-case (jef-error 20) (error () :caught)))
  (check "jit-enter-floor-after-unwind" (first r) (first (jef-floor))))
(check "jit-enter-floor-outside-after-unwind" nil (jef-vm-floor))

;; Every frame that snapshots jit_depth snapshots jit_c_floor beside it and
;; a landing restores both: a nested entry on a foreign stack parks the
;; floor, and a THROW / error / MUFFLE-WARNING out of it must hand back the
;; outer one (tests/test_nlx_jit_restore.c parks it; here the landings of
;; native code -- src/jit/runtime_m68k.c, which the host never builds -- are run
;; inside one outermost native call and must restore the floor exactly).
(defun jfu-thrower (n)
  (if (= n 0) (throw 'jfu :thrown) (car (list (jfu-thrower (- n 1))))))
(defun jfu-catch ()
  (let* ((before (first (jef-floor)))
         (r (catch 'jfu (jfu-thrower 10))))
    (list r (eql before (first (jef-floor))))))
(defun jfu-handler-case ()
  (let* ((before (first (jef-floor)))
         (r (handler-case (jef-error 10) (error () :caught))))
    (list r (eql before (first (jef-floor))))))
(defun jfu-muffle ()
  (let ((before (first (jef-floor))))
    (handler-bind ((warning #'muffle-warning)) (warn "jfu"))
    (list :muffled (eql before (first (jef-floor))))))
(check "jit-floor-unwind-all-native" t
  (every (lambda (f) (not (null (amilisp::%jit-dump-bytes f))))
         (list #'jfu-thrower #'jfu-catch #'jfu-handler-case #'jfu-muffle)))
(check "jit-floor-restored-after-throw" '(:thrown t) (jfu-catch))
(check "jit-floor-restored-after-handler-case" '(:caught t) (jfu-handler-case))
(check "jit-floor-restored-after-muffle-warning" '(:muffled t) (jfu-muffle))
(check "jit-floor-outside-after-landings" nil (jef-vm-floor))

;; --- Direct native-to-native call sites (specs/jit-direct-calls.md phase 4).
;; Every call in native code goes through a site whose 12-byte cell caches
;; (gen, func, entry): while gen equals the call generation (%CALL-GEN) and
;; the C stack is above CL_Thread.jit_c_floor, the call JSRs straight into
;; the callee's native code; anything else is a miss, which dispatches as
;; before and fills the cell for a positional native callee of the call's
;; arity.  %JIT-DIRECT-CALL-STATS counts the miss path only -- a hit runs no
;; C -- so "N more calls cost no more misses" is how a hit shows.  Every
;; bump of the call generation (a definition, a collection, TRACE, ...)
;; empties every site at once; these checks pin that no stale cell is ever
;; used, through every way the cached pair can go wrong.
(defun jds-stat (key) (getf (amilisp::%jit-direct-call-stats) key))
(defun jds-leaf (a b) (+ a b))
(defun jds-loop (n) (let ((s 0)) (dotimes (i n s) (setq s (jds-leaf s 1)))))
(defun jds-misses-for (n)
  (let ((m0 (jds-stat :misses)))
    (jds-loop n)
    (- (jds-stat :misses) m0)))
;; Misses that 1000 calls cost beyond what 10 cost: 0 when the sites hit.
;; Best of three, so that a collection landing inside one measurement (it
;; empties the sites: one refill) does not fail the check.
(defun jds-extra-misses ()
  (jds-misses-for 10)
  (let ((best nil))
    (dotimes (k 3 best)
      (let ((d (- (jds-misses-for 1000) (jds-misses-for 10))))
        (when (or (null best) (< (abs d) (abs best))) (setq best d))))))
(defun jds-fills-for (n)
  (amilisp::%jit-set-direct-calls t)   ; a new generation: every site is empty
  (let ((f0 (jds-stat :fills)))
    (jds-loop n)
    (- (jds-stat :fills) f0)))

(check "jit-site-all-native" t
  (every (lambda (f) (not (null (amilisp::%jit-dump-bytes f))))
         (list #'jds-stat #'jds-leaf #'jds-loop #'jds-misses-for
               #'jds-extra-misses #'jds-fills-for)))
(check "jit-site-enabled-by-default" t
  (getf (amilisp::%jit-direct-call-stats) :enabled))
(check "jit-site-hit-value" 1000 (jds-loop 1000))
(check "jit-site-hits-cost-no-misses" 0 (jds-extra-misses))
;; One fill per site per generation, however many calls go through it.
(check "jit-site-fills-once-per-generation" t
  (let ((a (jds-fills-for 10)) (b (jds-fills-for 1000)))
    (and (= a b) (> a 0))))
;; The kill switch: nothing fills, every call misses, values unchanged.
(check "jit-site-kill-switch" '(nil 1000 990 0)
  (progn
    (amilisp::%jit-set-direct-calls nil)
    (unwind-protect
         (let ((f0 (jds-stat :fills)))
           (list (getf (amilisp::%jit-direct-call-stats) :enabled)
                 (jds-loop 1000)
                 (- (jds-misses-for 1000) (jds-misses-for 10))
                 (- (jds-stat :fills) f0)))
      (amilisp::%jit-set-direct-calls t))))
(check "jit-site-kill-switch-bumps" t
  (let ((g (amilisp::%call-gen)))
    (amilisp::%jit-set-direct-calls t)
    (/= g (amilisp::%call-gen))))

;; Redefinition through every public path between two calls of the same
;; native caller: the second call runs the new definition.
(defun jds-r () 1)
(defun jds-r-caller () (jds-r))
(check "jit-site-redef-filled" '(1 1) (list (jds-r-caller) (jds-r-caller)))
(defun jds-r () 2)
(check "jit-site-redef-defun" '(2 2) (list (jds-r-caller) (jds-r-caller)))
(setf (fdefinition 'jds-r) (lambda () 3))
(check "jit-site-redef-setf-fdefinition" '(3 3) (list (jds-r-caller) (jds-r-caller)))
(setf (symbol-function 'jds-r) (let ((k (list 4))) (lambda () (car k))))
(check "jit-site-redef-closure" '(4 4) (list (jds-r-caller) (jds-r-caller)))
(fmakunbound 'jds-r)
(check "jit-site-redef-fmakunbound" "Undefined function: JDS-R"
  (handler-case (progn (jds-r-caller) :returned)
    (undefined-function (e) (format nil "~A" e))))
(defun jds-r () 5)
(check "jit-site-redef-after-fmakunbound" '(5 5) (list (jds-r-caller) (jds-r-caller)))

;; CLHS CELL-ERROR: the JIT's own raise paths (FLOAD / GLOAD helpers, the
;; APPLY trampoline) name the cell; the report text is unchanged.
(defun jcen-call () (jcen-no-such-fn 1))
(defun jcen-gload () jcen-unbound-var)
(defun jcen-apply (s) (apply s '(1 2)))
(check "jit-cell-error-callers-native" '(t t t)
  (list (and (amilisp::%jit-dump-bytes #'jcen-call) t)
        (and (amilisp::%jit-dump-bytes #'jcen-gload) t)
        (and (amilisp::%jit-dump-bytes #'jcen-apply) t)))
(check "jit-cell-error-name-undefined-function"
  '(jcen-no-such-fn "Undefined function: JCEN-NO-SUCH-FN")
  (handler-case (jcen-call)
    (undefined-function (c) (list (cell-error-name c) (princ-to-string c)))))
(check "jit-cell-error-name-unbound-variable"
  '(jcen-unbound-var "Unbound variable: JCEN-UNBOUND-VAR")
  (handler-case (jcen-gload)
    (unbound-variable (c) (list (cell-error-name c) (princ-to-string c)))))
(check "jit-cell-error-name-apply-symbol" 'jcen-no-such-fn-2
  (handler-case (jcen-apply 'jcen-no-such-fn-2)
    (undefined-function (c) (cell-error-name c))))

;; A redefinition with another arity at a filled site: the call no longer
;; fits, so it misses and keeps OP_CALL's diagnostic.
(defun jds-ar (a) a)
(defun jds-ar-caller () (jds-ar 1))
(check "jit-site-arity-filled" '(1 1) (list (jds-ar-caller) (jds-ar-caller)))
(defun jds-ar (a b) (+ a b))
(check "jit-site-arity-mismatch" t
  (handler-case (progn (jds-ar-caller) nil)
    (error (e) (not (null (search "Too few arguments to JDS-AR" (format nil "~A" e)))))))

;; FUNCALL (OP_CALL) of two closures over the same code at one site: the
;; site's func is compared too, so the other closure is a miss and runs
;; with its own upvalues.
(defun jds-adder (k) (lambda (x) (+ x k)))
(defun jds-fc (f x) (funcall f x))
(defun jds-alternate (n)
  (let ((a (jds-adder 1)) (b (jds-adder 100)) (s 0))
    (dotimes (i n s) (setq s (jds-fc (if (evenp i) a b) s)))))
(check "jit-site-funcall-alternating" 50500 (jds-alternate 1000))
(check "jit-site-funcall-other-function" '(7 (3 . 4) 30)
  (list (jds-fc (jds-adder 6) 1) (funcall #'jds-fc (lambda (x) (cons 3 x)) 4)
        (jds-fc (jds-adder 10) 20)))

;; Multiple values through a filled site, and no values at all.
(defun jds-mv (a) (values a (+ a 1) (+ a 2)))
(defun jds-mv-caller (a) (multiple-value-list (jds-mv a)))
(defun jds-none () (values))
(defun jds-none-caller () (multiple-value-list (jds-none)))
(check "jit-site-mv" '((1 2 3) (5 6 7)) (list (jds-mv-caller 1) (jds-mv-caller 5)))
(check "jit-site-mv-none" '(nil nil) (list (jds-none-caller) (jds-none-caller)))

;; Collections between calls through one site: each one empties the site
;; (the cached func is no GC root and is not relocated), so it refills
;; once per collection, not once per call, and every value is right.
(defun jds-gc-loop (n)
  (let ((s 0)) (dotimes (i n s) (make-string 2000) (setq s (jds-leaf s 1)))))
(stress-check "jit-site-across-gc" '(20000 t t)
  (let ((g0 (amilisp::%get-gc-count)) (f0 (jds-stat :fills)))
    (let ((r (jds-gc-loop 20000)))
      (let ((gcs (- (amilisp::%get-gc-count) g0))
            (fills (- (jds-stat :fills) f0)))
        (list r (> gcs 0) (<= fills (+ (* 3 gcs) 6)))))))

;; ABA: a closure called through a site, dropped, collected; a new closure
;; of the same shape through the same site must run, not the old one.
(defun jds-call0 (f) (funcall f))
(defun jds-aba (v) (jds-call0 (let ((c (list v))) (lambda () (car c)))))
(check "jit-site-aba" '(1 1 2 3)
  (list (jds-aba 1) (jds-aba 1)
        (progn (ext:gc-compact) (jds-aba 2))
        (progn (ext:gc-compact) (jds-aba 3))))

;; TRACE after the site has filled: the traced call takes the trampoline
;; (and is reported); after UNTRACE the site fills again.
(defun jds-tr-leaf (x) (* x 3))
(defun jds-tr-caller (x) (jds-tr-leaf x))
(check "jit-site-trace-filled" '(3 3) (list (jds-tr-caller 1) (jds-tr-caller 1)))
(trace jds-tr-leaf)
(let* ((r0 (jds-stat :refused-trace))
       (s (make-string-output-stream))
       (v (let ((*trace-output* s)) (jds-tr-caller 2)))
       (captured (get-output-stream-string s)))
  (check "jit-site-trace-reports" '(6 t t)
    (list v (not (null (search "JDS-TR-LEAF" captured)))
          (> (jds-stat :refused-trace) r0))))
(untrace jds-tr-leaf)
;; The jit-site-* fill counts and refusals are the m68k cells' rules (the
;; AArch64 ones -- refused while frames are OFF, per-thread counters -- are
;; tests/test_jit_a64_walk.sh part 4's).
#+m68k
(check "jit-site-untrace-refills" '(12 t)
  (let ((f0 (jds-stat :fills)))
    (list (jds-tr-caller 4) (> (jds-stat :fills) f0))))

;; Native frames, on by default: a filled site's hit path pushes the
;; callee's CL_Frame, so EXT:BACKTRACE lists both native functions.  The
;; leaf's call is a hit when it costs no cl_jit_invoke entry: the caller's
;; own entry (from this interpreted form) is the only one.  A collection
;; between fill and call empties the site, so best of three.
(defun jds-bt-leaf () (ext:backtrace))
(defun jds-bt-caller () (let ((r (jds-bt-leaf))) r))
(defun jds-fl-leaf (x) (declare (ignore x)) (ext:frame-locals 0))
(defun jds-fl-caller () (let ((r (jds-fl-leaf 7))) r))
(defun jds-bt-hit ()
  (let ((r nil))
    (dotimes (k 3 r)
      (jds-bt-caller)                   ; fill the site
      (let* ((i0 (amilisp::%jit-invoke-count))
             (bt (jds-bt-caller))
             (di (- (amilisp::%jit-invoke-count) i0)))
        (setq r (list (symbol-name (second (first bt)))
                      (symbol-name (second (second bt)))
                      di))
        (when (eql di 1) (return r))))))
#+m68k
(check "jit-frames-on-by-default" t (amilisp::%jit-frames-p))
;; Forced compaction (AMILISP_GC_STRESS, make test-gc-stress) collects at
;; every allocation, which empties the sites before any call can hit: there
;; only the frames are checked.
(check "jit-site-frames-backtrace" '("JDS-BT-LEAF" "JDS-BT-CALLER" t)
  (let ((r (jds-bt-hit)))
    (list (first r) (second r)
          (or (eql (third r) 1) (not (null (ext:getenv "AMILISP_GC_STRESS")))))))
;; An m68k frame pushed by the hit path has no locals on the VM stack (the
;; arguments are on the m68k stack): FRAME-LOCALS answers with an empty
;; list, not with whatever the reused slot held before.  (AArch64 keeps its
;; locals on the VM stack and shows them.)
#+m68k
(check "jit-site-frame-locals-empty" '(nil nil)
  (progn (jds-fl-caller) (list (jds-fl-caller) (jds-fl-caller))))
;; Frames off: no frame to push, so no site fills.
#+m68k
(check "jit-site-frames-off-refused" '(t 3)
  (progn
    (amilisp::%jit-set-frames nil)
    (unwind-protect
         (let ((r0 (jds-stat :refused-shadow)))
           (list (progn (jds-loop 3) (> (jds-stat :refused-shadow) r0))
                 (jds-loop 3)))
      (amilisp::%jit-set-frames t))))
;; Runaway native recursion now fills the frame stack, the interpreter's
;; limit, before it reaches the C-stack floor -- the same error either way.
(defun jds-runaway (n) (if (= n 0) 0 (+ 1 (jds-runaway (- n 1)))))
(check "jit-site-recursion-frame-limit" '(500 t 500)
  (list (jds-runaway 500)
        (handler-case (progn (jds-runaway 400000) nil)
          (error (e) (and (search "Call stack overflow" (princ-to-string e)) t)))
        (jds-runaway 500)))

;; Runaway recursion through filled sites still meets the C-stack guard:
;; below CL_Thread.jit_c_floor every site misses, and the miss path
;; signals.  (jit-direct-deep-recursion-guarded above runs the same shape.)
(defun jds-deep (n) (if (= n 0) 0 (+ 1 (jds-deep (- n 1)))))
(check "jit-site-deep-recursion" '(200 :caught 200)
  (list (jds-deep 200)
        (handler-case (progn (jds-deep 400000) :finished) (error () :caught))
        (jds-deep 200)))

;; MP:INTERRUPT-THREAD reaches a thread that loops on direct calls: a hit
;; never polls, so the interrupt has to bump the generation to make the
;; next call miss.  Five seconds without delivery is a failure, and the
;; loop is then stopped from here, so a regression cannot hang the suite.
(defvar *jds-stop* nil)
(defun jds-spin ()
  (let ((s 0)) (loop until *jds-stop* do (setq s (jds-leaf s 1))) s))
(check "jit-site-interrupt-delivered" :interrupted
  (progn
    (setq *jds-stop* nil)
    (let ((th (mp:make-thread #'jds-spin :name "jds-spin")))
      (sleep 0.2)
      (mp:interrupt-thread th (lambda () (setq *jds-stop* :interrupted)))
      (loop repeat 50 until *jds-stop* do (sleep 0.1))
      (let ((r *jds-stop*))
        (unless r (setq *jds-stop* :forced))
        (mp:join-thread th)
        r))))
;; (A peer's stop-the-world collection while threads loop on direct calls:
;; jit-direct-native-callee-across-concurrent-gc above.)

;; --- Tail calls into other functions (m68k: emit_tail_call_site).  A
;; filled tail site hands the caller's activation and CL_Frame to the
;; callee and jumps, so a chain of tail calls runs in constant stack, as in
;; the interpreter.  100000 is far past the 1024 frames a chain of calls
;; would need: each one would push a frame and fail with "Call stack
;; overflow".  A tail call passing more arguments than its function
;; received stays a call (the caller drops only what it pushed).
(defun jtc-even (n) (if (= n 0) t (jtc-odd (- n 1))))
(defun jtc-odd (n) (if (= n 0) nil (jtc-even (- n 1))))
;; A state machine of three functions, consing as it goes, so collections
;; and the loop poll land inside the chain.
(defun jtc-st-a (n acc) (if (= n 0) (length acc) (jtc-st-b (- n 1) (cons n acc))))
(defun jtc-st-b (n acc) (if (evenp n) (jtc-st-a n acc) (jtc-st-c n acc)))
(defun jtc-st-c (n acc) (jtc-st-a n (cdr acc)))
;; Fewer arguments than received is a jump too.
(defun jtc-one (a) (list a))
(defun jtc-drop (a b) (declare (ignore b)) (jtc-one a))
;; More arguments than received: a call, and the right values.
(defun jtc-wide (a b c) (list a b c))
(defun jtc-grow (n) (if (= n 0) :done (jtc-grow2 n 1)))
(defun jtc-grow2 (n k) (jtc-grow3 n k :x))
(defun jtc-grow3 (n k x) (declare (ignore x)) (jtc-grow (- n k)))
;; Nine arguments: the copy is a DBF loop above eight.
(defun jtc-nine (n a b c d e f g h)
  (if (= n 0) (list a b c d e f g h)
      (jtc-nine2 (- n 1) b c d e f g h a)))
(defun jtc-nine2 (n a b c d e f g h) (jtc-nine n a b c d e f g h))
;; Into a closure (its upvalues come from the func the jump passes), an
;; &optional, a &rest and a &key callee, and multiple values back out.
(defun jtc-make-adder (k) (lambda (x) (+ x k)))
(defun jtc-funcall (f x) (funcall f x))
(defun jtc-opt (a &optional (b 10)) (list a b))
(defun jtc-to-opt (a) (jtc-opt a))
(defun jtc-rest (&rest r) r)
(defun jtc-to-rest (a b) (jtc-rest a b))
(defun jtc-key (a &key (k :dflt)) (list a k))
(defun jtc-to-key (a b c) (jtc-key a b c))
(defun jtc-mv (a) (values a (1+ a) (+ a 2)))
(defun jtc-to-mv (a) (jtc-mv a))
;; An error in a function reached by a jump unwinds cleanly.
(defun jtc-err-a (n) (if (= n 0) (error "jtc bottom") (jtc-err-b (- n 1))))
(defun jtc-err-b (n) (jtc-err-a n))
;; A special binding around the tail-positioned call: the binding must be
;; seen by the callee and undone after it, so this one never jumps.
(defvar *jtc-special* :outer)
(defun jtc-read-special () *jtc-special*)
(defun jtc-bind (v) (let ((*jtc-special* v)) (jtc-read-special)))

(check "jtc-all-native" t
  (every (lambda (f) (not (null (amilisp::%jit-dump-bytes f))))
         (list #'jtc-even #'jtc-odd #'jtc-st-a #'jtc-st-b #'jtc-st-c
               #'jtc-grow #'jtc-grow2 #'jtc-grow3 #'jtc-nine #'jtc-nine2)))
(check "jtc-mutual-small" '(t nil t) (list (jtc-even 10) (jtc-even 7) (jtc-odd 7)))
(check "jtc-mutual-constant-stack" '(t nil)
  (list (jtc-even 100000) (jtc-odd 100000)))
(check "jtc-state-machine" '(5 10001 10001)
  (list (jtc-st-a 10 nil) (jtc-st-a 20001 nil) (jtc-st-a 20001 nil)))
(check "jtc-fewer-args" '((1) (2)) (list (jtc-drop 1 :x) (jtc-drop 2 :y)))
(check "jtc-more-args-stays-a-call" '(:done :done) (list (jtc-grow 50) (jtc-grow 300)))
(check "jtc-nine-args" '((b c d e f g h a) (a b c d e f g h))
  (list (jtc-nine 1 'a 'b 'c 'd 'e 'f 'g 'h)
        (jtc-nine 20000 'a 'b 'c 'd 'e 'f 'g 'h)))
(check "jtc-closure-callee" '(15 107)
  (list (jtc-funcall (jtc-make-adder 5) 10)
        (jtc-funcall (jtc-make-adder 100) 7)))
(check "jtc-optional-rest-key-callees" '((1 10) (1 10) (1 2) (1 2) (1 :v) (1 :v))
  (list (jtc-to-opt 1) (jtc-to-opt 1) (jtc-to-rest 1 2) (jtc-to-rest 1 2)
        (jtc-to-key 1 :k :v) (jtc-to-key 1 :k :v)))
(check "jtc-multiple-values" '((1 2 3) (5 6 7))
  (list (multiple-value-list (jtc-to-mv 1)) (multiple-value-list (jtc-to-mv 5))))
(check "jtc-error-unwinds" '(:caught :caught t)
  (list (handler-case (jtc-err-a 5000) (error () :caught))
        (handler-case (jtc-err-a 5000) (error () :caught))
        (jtc-even 100000)))
(check "jtc-special-binding" '(:inner :inner :outer)
  (list (jtc-bind :inner) (jtc-bind :inner) *jtc-special*))
;; A jump makes the callee's frame the caller's own, as the interpreter's
;; frame reuse does: the backtrace shows the callee where the caller was.
(defun jtc-bt-leaf () (ext:backtrace))
(defun jtc-bt-mid () (jtc-bt-leaf))
(defun jtc-bt-top () (let ((r (jtc-bt-mid))) r))
;; A collection between fill and call empties the sites, so best of three.
(defun jtc-bt-names ()
  (let ((r nil))
    (dotimes (k 3 r)
      (jtc-bt-top)                      ; fill the sites
      (let ((bt (jtc-bt-top)))
        (setq r (list (symbol-name (second (first bt)))
                      (symbol-name (second (second bt)))))
        (when (equal (second r) "JTC-BT-TOP") (return r))))))
#+m68k
(check "jtc-backtrace-frame-replaced" '("JTC-BT-LEAF" "JTC-BT-TOP")
  (jtc-bt-names))
;; Frames off: no site fills, so every tail call is a call again -- the
;; values do not change (and stay within the frame stack).
#+m68k
(check "jtc-frames-off" '(t nil (1 10))
  (progn
    (amilisp::%jit-set-frames nil)
    (unwind-protect (list (jtc-even 300) (jtc-odd 300) (jtc-to-opt 1))
      (amilisp::%jit-set-frames t))))

;; --- The line of a native frame in a backtrace: the line of the call the
;; function is in, as for an interpreted one (m68k: the backtrace finds the
;; frame's return address on the stack and looks it up in the function's
;; line table; AArch64 stores the ip before each call).  Each native
;; function below has an interpreted twin laid out the same way, the
;; twin's DEFUN exactly 5 lines further down, so the twin's line is the
;; native one's plus 5 -- wherever this file puts them.  A native frame
;; stuck at its function's first line gives 6.
(defun jln-name= (name) (lambda (e) (string= name (symbol-name (second e)))))
(defun jln-line (name bt) (fourth (find-if (jln-name= name) bt)))
(defun jln-leaf () (ext:backtrace))
;; A direct call into a native leaf.
(defun jln-outer (x)
  (let ((a (car x)))
    (list a
          (jln-leaf))))
(amilisp::%jit-set-active nil)
(defun jln-outer-i (x)
  (let ((a (car x)))
    (list a
          (jln-leaf-i))))
(defun jln-leaf-i () (ext:backtrace))
(amilisp::%jit-set-active t)
;; A call to a builtin (the call site's miss path).
(defun jln-builtin (x)
  (let ((a (car x)))
    (list a
          (ext:backtrace))))
(amilisp::%jit-set-active nil)
(defun jln-builtin-i (x)
  (let ((a (car x)))
    (list a
          (ext:backtrace))))
(amilisp::%jit-set-active t)
;; An error signalled by a builtin the native function calls, seen from
;; the handler: there the frame is the innermost native one.
(defun jln-err (h)
  (let ((y (list h)))
    (gethash
     y h)))
(amilisp::%jit-set-active nil)
(defun jln-err-i (h)
  (let ((y (list h)))
    (gethash
     y h)))
(amilisp::%jit-set-active t)
(defun jln-err-line (fn name)
  (block nil
    (handler-bind ((error (lambda (c) (declare (ignore c))
                            (return (jln-line name (ext:backtrace))))))
      (funcall fn 5))))

(check "jln-native" '(t t t nil nil nil nil)
  (mapcar (lambda (f) (not (null (amilisp::%jit-dump-bytes f))))
          (list #'jln-outer #'jln-builtin #'jln-err
                #'jln-outer-i #'jln-leaf-i #'jln-builtin-i #'jln-err-i)))
(check "jln-direct-call-line" 5
  (- (jln-line "JLN-OUTER-I" (second (jln-outer-i '(1))))
     (jln-line "JLN-OUTER" (second (jln-outer '(1))))))
(check "jln-builtin-call-line" 5
  (- (jln-line "JLN-BUILTIN-I" (second (jln-builtin-i '(1))))
     (jln-line "JLN-BUILTIN" (second (jln-builtin '(1))))))
(check "jln-error-in-builtin-line" 5
  (- (jln-err-line #'jln-err-i "JLN-ERR-I")
     (jln-err-line #'jln-err "JLN-ERR")))
;; A second backtrace resolves the same frames again.
(check "jln-repeatable" t
  (let ((a (jln-line "JLN-OUTER" (second (jln-outer '(1)))))
        (b (jln-line "JLN-OUTER" (second (jln-outer '(1))))))
    (and (integerp a) (= a b))))

;; --- &optional (the m68k walker: emit_opt_prologue; AArch64: its inline
;; prologue).  The caller passes the argument count -- in D1 on m68k, from
;; cl_jit_enter and from a call site's hit path -- and the prologue copies
;; the arguments into the frame, NILs the slots after them and keeps the
;; count for OP_ARGC, which the compiler's &optional prologue tests.  Values
;; are the HyperSpec's (3.4.1): a default sees the earlier parameters and
;; their supplied-p variables, and runs only for a missing argument.
(defun opt-k () (list "k"))
(defvar *opt-s* :global)
(defun opt-o (a &optional (b 10) (c (list a b) cp)) (list a b c cp))
(defun opt-heap (&optional (x (opt-k)) (y (opt-k))) (list x y))
(defun opt-special (&optional (s *opt-s*)) s)
(defun opt-err (&optional (x (error "default ~A" 1))) x)
(defun opt-nil (&optional a b) (list a b))
(defun opt-sees-sp (&optional (a 1 ap) (b (if ap :given :missing))) (list a b))
(defun opt-key (a &optional b &key (k 7 kp)) (list a b k kp))
;; Six and seven positional parameters: seven was over the m68k walker's
;; old limit (see "More than six positional parameters").
(defun opt-6 (a b c &optional (d :d) (e :e) (f :f)) (list a b c d e f))
(defun opt-7 (a b c d &optional (e :e) (f :f) (g :g)) (list a b c d e f g))
;; Self tail calls that change the count: a loop in one frame.  A
;; supplied-p variable set by one round must read NIL again in the next.
(defun opt-acc (n &optional (s 0)) (if (= n 0) s (opt-acc (- n 1) (+ s n))))
(defun opt-acc-fc (n &optional (s 0))
  (if (= n 0) s (funcall #'opt-acc-fc (- n 1) (+ s n))))
(defun opt-st (n &optional (a :dflt a-p))
  (cond ((= n 0) (list a a-p)) ((= n 1) (opt-st 0)) (t (opt-st (- n 1) n))))
(defun opt-st-up (n &optional (a :dflt a-p))
  (if (= n 0) (list a a-p) (if (= n 3) (opt-st-up (- n 1) :x) (opt-st-up (- n 1)))))
;; Native callers: a site per count, into each callee.
(defun opt-call-o () (list (opt-o 1) (opt-o 1 2) (opt-o 1 2 3)))
(defun opt-call-6 ()
  (list (opt-6 1 2 3) (opt-6 1 2 3 4) (opt-6 1 2 3 4 5) (opt-6 1 2 3 4 5 6)))
(defun opt-sum (a &optional (b 1)) (+ a b))
(defun opt-sum-loop (n)
  ;; Two-argument + only: a three-argument one calls the builtin, which misses.
  (let ((s 0)) (dotimes (i n s) (setq s (+ s (+ (opt-sum i) (opt-sum i 2)))))))
(defun opt-too-many () (opt-o 1 2 3 4))
(defun opt-too-few () (opt-o))
(defun opt-mk (n) (lambda (&optional (x n) (y (list x))) (list x y)))
(defun opt-fc (f) (list (funcall f) (funcall f 1) (funcall f 1 2)))
(defun opt-native-p (f) (and (amilisp::%jit-dump-bytes f) t))
(defun opt-misses-for (n)
  (let ((m0 (jds-stat :misses)))
    (opt-sum-loop n)
    (- (jds-stat :misses) m0)))

(check "jit-opt-native" '(t t t t t t t t t t t t t t t t t t)
  (mapcar #'opt-native-p
          (list #'opt-o #'opt-heap #'opt-special #'opt-err #'opt-nil
                #'opt-sees-sp #'opt-key #'opt-6 #'opt-7 #'opt-acc #'opt-acc-fc
                #'opt-st #'opt-st-up #'opt-call-o #'opt-call-6 #'opt-sum
                #'opt-sum-loop (opt-mk 0))))
;; The prologue's copy and NIL loops, decoded by %JIT-DISASSEMBLE.
#+m68k
(check "jit-opt-prologue-disassembles" '(t t t t)
  (let ((d (with-output-to-string (*standard-output*)
             (amilisp::%jit-disassemble #'opt-o))))
    (mapcar (lambda (s) (not (null (search s d))))
            '("adda.l d0,a0" "move.l -(a0),(a1)+" "clr.l (a1)+" "dbf d0,"))))
(check "jit-opt-defaults" '((1 10 (1 10) nil) (1 2 (1 2) nil) (1 2 3 t))
  (list (opt-o 1) (opt-o 1 2) (opt-o 1 2 3)))
(check "jit-opt-default-allocates" '((("k") ("k")) (:a ("k")) (:a :b))
  (list (opt-heap) (opt-heap :a) (opt-heap :a :b)))
(check "jit-opt-default-reads-special" '(:global :bound 3)
  (list (opt-special) (let ((*opt-s* :bound)) (opt-special)) (opt-special 3)))
(check "jit-opt-default-signals" '(5 "default 1")
  (list (opt-err 5) (handler-case (opt-err) (error (e) (princ-to-string e)))))
(check "jit-opt-missing-is-nil" '((nil nil) (1 nil) (1 2))
  (list (opt-nil) (opt-nil 1) (opt-nil 1 2)))
(check "jit-opt-default-sees-supplied-p" '((1 :missing) (5 :given) (5 6))
  (list (opt-sees-sp) (opt-sees-sp 5) (opt-sees-sp 5 6)))
(check "jit-opt-then-key" '((1 nil 7 nil) (1 2 7 nil) (1 2 9 t) (1 2 9 t))
  (list (opt-key 1) (opt-key 1 2) (opt-key 1 2 :k 9) (opt-key 1 2 :k 9 :k 10)))
(check "jit-opt-then-key-unknown" :caught
  (handler-case (progn (opt-key 1 2 :q 3) :no-error) (error () :caught)))
(check "jit-opt-six-positional"
  '((1 2 3 :d :e :f) (1 2 3 4 :e :f) (1 2 3 4 5 :f) (1 2 3 4 5 6))
  (opt-call-6))
(check "jit-opt-seven-positional" '((1 2 3 4 :e :f :g) (1 2 3 4 5 6 7))
  (list (opt-7 1 2 3 4) (opt-7 1 2 3 4 5 6 7)))
;; 20000 rounds: a native frame per round would exhaust the C stack.
(check "jit-opt-self-tail-call" '(200010000 200010000)
  (list (opt-acc 20000) (opt-acc-fc 20000)))
(check "jit-opt-self-tail-call-supplied-p"
  '((:dflt nil) (:dflt nil) (5 t) (:dflt nil))
  (list (opt-st 3) (opt-st 2) (opt-st 0 5) (opt-st-up 5)))
;; Twice: the first round fills the sites, the second hits them.
(check "jit-opt-direct-calls"
  '((1 10 (1 10) nil) (1 2 (1 2) nil) (1 2 3 t))
  (progn (opt-call-o) (opt-call-o)))
(check "jit-opt-direct-calls-value" 1002000 (opt-sum-loop 1000))
;; Before the m68k walker took &optional, the fill rule refused these
;; callees (:refused-abi) and every call missed.  Best of three, as
;; jit-site-hits-cost-no-misses: a collection empties every site.
(check "jit-opt-direct-calls-hit" 0
  (progn
    (opt-misses-for 10)
    (let ((best nil))
      (dotimes (k 3 best)
        (let ((d (- (opt-misses-for 1000) (opt-misses-for 10))))
          (when (or (null best) (< (abs d) (abs best))) (setq best d)))))))
(check "jit-opt-too-many" :caught
  (handler-case (progn (opt-too-many) :no-error) (program-error () :caught)))
(check "jit-opt-too-few" :caught
  (handler-case (progn (opt-too-few) :no-error) (program-error () :caught)))
(check "jit-opt-closures"
  '(((1 (1)) (1 (1)) (1 2)) ((("k") (("k"))) (1 (1)) (1 2)))
  (list (opt-fc (opt-mk 1)) (opt-fc (opt-mk (opt-k)))))
(check "jit-opt-threads" '(t t)
  (let ((ths (loop for i below 2
                   collect (let ((i i))
                             (mp:make-thread
                              (lambda ()
                                (loop repeat 200
                                      always (and (equal (opt-o i) (list i 10 (list i 10) nil))
                                                  (= (opt-acc 50 i) (+ i 1275))))))))))
    (mapcar #'mp:join-thread ths)))

;; --- &rest (the m68k walker: cl_jit_runtime_rest_prologue, or the keyword
;; prologue together with &key; AArch64: cl_jit_vmstack_ll_prologue).  The
;; entry passes the count as for &optional; the prologue copies the
;; positional arguments, NILs the other slots and conses the arguments past
;; the positional ones into a fresh list in the slot after them (CLHS
;; 3.4.1.3) -- which may collect, so the checks below churn the heap and
;; compact inside the callee.  Any count above the positional ones reaches
;; native code (OP_CALL's count is a byte).
(defun rest-all (&rest r) r)
(defun rest-ab (a &optional (b 2 bp) &rest r) (list a b bp r))
;; A default after &optional sees the earlier ones; &rest sees neither.
(defun rest-argc (a &optional (b (list a)) &rest r) (list a b r))
(defun rest-mut (&rest r) (setf (car r) :x) r)
(defun rest-heap (&rest r) (list (opt-k) r))
(defun rest-compact (a &rest r)
  (ext:gc-compact)
  (list a r (opt-k)))
(defun rest-key (a &rest r &key (k 1 kp) &allow-other-keys) (list a r k kp))
(defun rest-key-strict (&rest r &key k) (list r k))
(defun rest-key-compact (&rest r &key (k (progn (ext:gc-compact) :dflt)))
  (list r k))
;; Six positional parameters plus &rest.
(defun rest-6 (a b c d e &optional (f :f) &rest r) (list a b c d e f r))
;; Self tail calls up to the positional count stay a loop: the copy NILs
;; the rest slot, the empty list such a call conses.  20000 rounds would
;; exhaust the C stack with a native frame per round.
(defun rest-acc (n &optional (s 0) &rest more)
  (if (= n 0) (list s more) (rest-acc (- n 1) (+ s n))))
;; A self call with more arguments than that is a real call.
(defun rest-cnt (n &rest r) (if (= n 0) (length r) (rest-cnt (- n 1) 1 2 3)))
(defun rest-sum (a &rest r) (if r (+ a (car r)) a))
(defun rest-sum-loop (n)
  (let ((s 0)) (dotimes (i n s) (setq s (+ s (+ (rest-sum i) (rest-sum i 2)))))))
(defun rest-misses-for (n)
  (let ((m0 (jds-stat :misses)))
    (rest-sum-loop n)
    (- (jds-stat :misses) m0)))
;; Native callers: counts below, at and above six.
(defun rest-call-all ()
  (list (rest-all) (rest-all 1) (rest-all 1 2 3 4 5 6)
        (rest-all 1 2 3 4 5 6 7 8 9 10 11 12)))
(defun rest-call-many ()
  (rest-all 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19
            20 21 22 23 24 25 26 27 28 29 30 31 32 33 34 35 36 37 38 39))
(defun rest-call-fresh (i) (rest-compact (list i) (list (+ i 1)) (list (+ i 2))))
(defun rest-too-few () (rest-ab))
(defun rest-mk (n) (lambda (&rest r) (cons n r)))
(defun rest-fc (f) (list (funcall f) (funcall f 1) (funcall f 1 2 3 4 5 6 7 8)))

(check "jit-rest-native" '(t t t t t t t t t t t t t t t t t t t)
  (mapcar #'opt-native-p
          (list #'rest-all #'rest-ab #'rest-argc #'rest-mut #'rest-heap
                #'rest-compact #'rest-key #'rest-key-strict #'rest-key-compact
                #'rest-6 #'rest-acc #'rest-cnt #'rest-sum #'rest-sum-loop
                #'rest-call-all #'rest-call-many #'rest-call-fresh
                #'rest-fc (rest-mk 0))))
(check "jit-rest-list" '(nil (1) (1 2 3))
  (list (rest-all) (rest-all 1) (rest-all 1 2 3)))
(check "jit-rest-after-optional"
  '((1 2 nil nil) (1 5 t nil) (1 5 t (6)) (1 5 t (6 7 8)))
  (list (rest-ab 1) (rest-ab 1 5) (rest-ab 1 5 6) (rest-ab 1 5 6 7 8)))
(check "jit-rest-optional-default" '((1 (1) nil) (1 2 nil) (1 2 (3 4)))
  (list (rest-argc 1) (rest-argc 1 2) (rest-argc 1 2 3 4)))
;; A fresh list each call (the caller's arguments are not shared).
(check "jit-rest-fresh-list" '((:x 2) (:x 2))
  (list (rest-mut 1 2) (rest-mut 1 2)))
(check "jit-rest-body-allocates" '((("k") nil) (("k") (1 ("k"))))
  (list (rest-heap) (rest-heap 1 (opt-k))))
;; Compaction inside the callee: the rest list, the positional argument and
;; the heap objects in both survive and still read right.
(check "jit-rest-across-compaction"
  '(((7) ((8) (9)) ("k")) ((7) ((8) (9)) ("k")))
  (list (rest-call-fresh 7) (rest-call-fresh 7)))
(check "jit-rest-across-churn" t
  (loop for i below 300
        always (equal (rest-call-fresh i)
                      (list (list i) (list (list (+ i 1)) (list (+ i 2))) (list "k")))))
(check "jit-rest-and-key"
  '((1 nil 1 nil) (1 (:k 2) 2 t) (1 (:z 3 :k 2) 2 t) (1 (:k 2 :k 3) 2 t))
  (list (rest-key 1) (rest-key 1 :k 2) (rest-key 1 :z 3 :k 2)
        (rest-key 1 :k 2 :k 3)))
(check "jit-rest-and-key-errors" '(((:k 1) 1) :unknown :odd)
  (list (rest-key-strict :k 1)
        (handler-case (progn (rest-key-strict :q 1) :no-error)
          (error () :unknown))
        (handler-case (progn (rest-key-strict :k) :no-error)
          (error () :odd))))
;; The keyword prologue re-derives the bytecode after consing the list;
;; the default compacts on top.
(check "jit-rest-and-key-compaction" '((nil :dflt) ((:k 5) 5))
  (list (rest-key-compact) (rest-key-compact :k 5)))
(check "jit-rest-six-positional"
  '((1 2 3 4 5 :f nil) (1 2 3 4 5 6 nil) (1 2 3 4 5 6 (7 8)))
  (list (rest-6 1 2 3 4 5) (rest-6 1 2 3 4 5 6) (rest-6 1 2 3 4 5 6 7 8)))
;; (rest-acc 0 6 :x) keeps its extra; the rounds after an extra drop it.
(check "jit-rest-self-tail-call" '((200010000 nil) (6 (:x)) (6 nil) (6 nil))
  (list (rest-acc 20000) (rest-acc 0 6 :x) (rest-acc 3 0 :x) (rest-acc 3)))
(check "jit-rest-self-call-with-extras" '(0 3 3)
  (list (rest-cnt 0) (rest-cnt 1) (rest-cnt 10)))
;; Twice: the first round fills the sites, the second hits them.
(check "jit-rest-direct-calls"
  '(nil (1) (1 2 3 4 5 6) (1 2 3 4 5 6 7 8 9 10 11 12))
  (progn (rest-call-all) (rest-call-all)))
(check "jit-rest-forty-arguments" '(40 0 39)
  (let ((r (rest-call-many))) (list (length r) (first r) (car (last r)))))
(check "jit-rest-direct-calls-value" 1001000 (rest-sum-loop 1000))
;; The fill rule used to refuse &rest callees, so every call missed.  Best
;; of three, as jit-opt-direct-calls-hit.  REST-SUM conses its list, and
;; under AMILISP_GC_STRESS every allocation collects, which empties every
;; site: there every call misses by design, so the check stands down.
(check "jit-rest-direct-calls-hit" 0
  (if (ext:getenv "AMILISP_GC_STRESS")
      0
      (progn
        (rest-misses-for 10)
        (let ((best nil))
          (dotimes (k 3 best)
            (let ((d (- (rest-misses-for 1000) (rest-misses-for 10))))
              (when (or (null best) (< (abs d) (abs best))) (setq best d))))))))
(check "jit-rest-too-few" :caught
  (handler-case (progn (rest-too-few) :no-error) (program-error () :caught)))
(check "jit-rest-closures"
  '(((1) (1 1) (1 1 2 3 4 5 6 7 8)) ((("k")) (("k") 1) (("k") 1 2 3 4 5 6 7 8)))
  (list (rest-fc (rest-mk 1)) (rest-fc (rest-mk (opt-k)))))
(check "jit-rest-apply" '(100 300)
  (list (length (apply #'rest-all (make-list 100 :initial-element 1)))
        (length (apply #'rest-all (make-list 300 :initial-element 1)))))
(check "jit-rest-threads" '(t t)
  (let ((ths (loop for i below 2
                   collect (let ((i i))
                             (mp:make-thread
                              (lambda ()
                                (loop repeat 200
                                      always (and (equal (rest-ab i 2 3 4) (list i 2 t (list 3 4)))
                                                  (equal (rest-call-fresh i)
                                                         (list (list i)
                                                               (list (list (+ i 1)) (list (+ i 2)))
                                                               (list "k")))))))))))
    (mapcar #'mp:join-thread ths)))

;; --- More than six positional parameters.  The m68k walker used to decline
;; a function without &key whose required plus optional parameters exceeded
;; six, cl_jit_invoke refused to enter one, and a call site never filled for
;; more than six arguments.  The bound is OP_CALL's count byte now (255):
;; parameter i sits at 12+4*(n-1-i)(a6), a d16 for every such n, and a call
;; site passes counts above 127 with move.l instead of moveq.
(defmacro wide-defun (name n)
  (let ((ps (loop for i below n collect (intern (format nil "W~D" i)))))
    `(defun ,name ,ps (list ,(first ps) ,(second ps) ,(car (last ps)) (+ ,@ps)))))
(defmacro wide-call (name n) `(,name ,@(loop for i below n collect i)))
(defun wide-8 (a b c d e f g h) (list a b c d e f g h))
(defun wide-12 (a b c d e f g h i j k l) (list l k j i h g f e d c b a))
(defun wide-opt (a b c d e &optional (f :f) (g (list a e)) (h (list f g) hp))
  (list a b c d e f g h hp))
(wide-defun wide-255 255)
;; Arguments live across a compaction and a collection in the body.
(defun wide-compact (a b c d e f g h)
  (ext:gc-compact)
  (list a b c d e f g h (opt-k)))
;; Self tail calls stay loops: the required-only copy and the &optional one.
(defun wide-acc (n a b c d e f s)
  (if (= n 0) (list a b c d e f s) (wide-acc (- n 1) b c d e f a (+ s n))))
(defun wide-acc-opt (n a b c d e f &optional (s 0))
  (if (= n 0) (list a f s) (wide-acc-opt (- n 1) a b c d e f (+ s n))))
;; A self tail call with more than 127 arguments in an &optional function:
;; the copy stores the count with move.l, not a sign-extending moveq.
(defmacro wide-tco-defun (name n)
  (let ((ps (loop for i below n collect (intern (format nil "T~D" i)))))
    `(defun ,name (n ,@ps &optional (s 0))
       (if (= n 0)
           (list ,(first ps) ,(car (last ps)) s)
           (,name (- n 1) ,@ps (+ s n))))))
(wide-tco-defun wide-tco-opt 130)
(defun wide-tco-call (n)
  (wide-tco-opt n 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 32 33 34 35 36 37 38 39 40 41 42 43 44 45 46 47 48 49 50 51 52 53 54 55 56 57 58 59 60 61 62 63 64 65 66 67 68 69 70 71 72 73 74 75 76 77 78 79 80 81 82 83 84 85 86 87 88 89 90 91 92 93 94 95 96 97 98 99 100 101 102 103 104 105 106 107 108 109 110 111 112 113 114 115 116 117 118 119 120 121 122 123 124 125 126 127 128 129 130))
(check "jit-wide-self-tail-call-over-127" '(1 130 20100)
  (wide-tco-call 200))
;; Two-argument + only: a longer one calls the builtin, whose site misses.
(defun wide-sum (a b c d e f g) (+ a (+ b (+ c (+ d (+ e (+ f g)))))))
(defun wide-sum-loop (n)
  (let ((s 0)) (dotimes (i n s) (setq s (+ s (wide-sum i 1 1 1 1 1 1))))))
(defun wide-misses-for (n)
  (let ((m0 (jds-stat :misses)))
    (wide-sum-loop n)
    (- (jds-stat :misses) m0)))
;; Native callers: a site per count, and OP_CALL through a function value.
(defun wide-call-all ()
  (list (wide-8 1 2 3 4 5 6 7 8) (wide-12 1 2 3 4 5 6 7 8 9 10 11 12)
        (wide-opt 1 2 3 4 5) (wide-opt 1 2 3 4 5 6) (wide-opt 1 2 3 4 5 6 7)
        (wide-opt 1 2 3 4 5 6 7 8)))
(defun wide-call-255 () (wide-call wide-255 255))
(defun wide-fc (f) (funcall f 1 2 3 4 5 6 7 8))
(defun wide-mk (n) (lambda (a b c d e f g h) (list n a h (+ a b c d e f g h))))
(defun wide-too-many () (wide-8 1 2 3 4 5 6 7 8 9))
(defun wide-too-few () (wide-8 1 2 3 4 5 6 7))
(defun wide-opt-too-many () (wide-opt 1 2 3 4 5 6 7 8 9))

(check "jit-wide-native" '(t t t t t t t t t t t t t t t t t t t)
  (mapcar #'opt-native-p
          (list #'wide-8 #'wide-12 #'wide-opt #'wide-255 #'wide-compact
                #'wide-acc #'wide-acc-opt #'wide-sum #'wide-sum-loop
                #'wide-call-all #'wide-call-255 #'wide-fc (wide-mk 0)
                #'wide-too-many #'wide-too-few #'wide-opt-too-many
                #'opt-7 #'wide-misses-for #'wide-tco-opt)))
(check "jit-wide-order" '((1 2 3 4 5 6 7 8) (12 11 10 9 8 7 6 5 4 3 2 1))
  (list (wide-8 1 2 3 4 5 6 7 8) (wide-12 1 2 3 4 5 6 7 8 9 10 11 12)))
(check "jit-wide-optional"
  '((1 2 3 4 5 :f (1 5) (:f (1 5)) nil) (1 2 3 4 5 6 (1 5) (6 (1 5)) nil)
    (1 2 3 4 5 6 7 (6 7) nil) (1 2 3 4 5 6 7 8 t))
  (list (wide-opt 1 2 3 4 5) (wide-opt 1 2 3 4 5 6) (wide-opt 1 2 3 4 5 6 7)
        (wide-opt 1 2 3 4 5 6 7 8)))
;; Twice: the first round fills the sites, the second hits them.
(check "jit-wide-direct-calls"
  '((1 2 3 4 5 6 7 8) (12 11 10 9 8 7 6 5 4 3 2 1)
    (1 2 3 4 5 :f (1 5) (:f (1 5)) nil) (1 2 3 4 5 6 (1 5) (6 (1 5)) nil)
    (1 2 3 4 5 6 7 (6 7) nil) (1 2 3 4 5 6 7 8 t))
  (progn (wide-call-all) (wide-call-all)))
;; OP_CALL's limit, from a native caller (twice: fill, then hit) and APPLY.
(check "jit-wide-255" '((0 1 254 32385) (0 1 254 32385) (0 1 254 32385))
  (list (wide-call-255) (wide-call-255)
        (apply #'wide-255 (loop for i below 255 collect i))))
(check "jit-wide-across-compaction"
  '(((1) (2) (3) (4) (5) (6) (7) (8) ("k")) ((1) (2) (3) (4) (5) (6) (7) (8) ("k")))
  (loop repeat 2
        collect (wide-compact (list 1) (list 2) (list 3) (list 4)
                              (list 5) (list 6) (list 7) (list 8))))
;; 20000 rounds: a native frame per round would exhaust the C stack.  The
;; rotation checks that the copy reads every argument before it writes one.
(check "jit-wide-self-tail-call" '((:c :d :e :f :a :b 200010000) (1 6 200010000))
  (list (wide-acc 20000 :a :b :c :d :e :f 0)
        (wide-acc-opt 20000 1 2 3 4 5 6)))
(check "jit-wide-funcall-and-closures"
  '((1 2 3 4 5 6 7 8) (:n 1 8 36) (("k") 1 8 36))
  (list (wide-fc #'wide-8) (wide-fc (wide-mk :n)) (wide-fc (wide-mk (opt-k)))))
(check "jit-wide-arity-errors" '(:caught :caught :caught)
  (mapcar (lambda (f) (handler-case (progn (funcall f) :no-error)
                        (program-error () :caught)))
          (list #'wide-too-many #'wide-too-few #'wide-opt-too-many)))
(check "jit-wide-direct-calls-value" 505500 (wide-sum-loop 1000))
;; The fill rule refused more than six arguments, so every call missed.
;; Best of three, as jit-opt-direct-calls-hit.
(check "jit-wide-direct-calls-hit" 0
  (progn
    (wide-misses-for 10)
    (let ((best nil))
      (dotimes (k 3 best)
        (let ((d (- (wide-misses-for 1000) (wide-misses-for 10))))
          (when (or (null best) (< (abs d) (abs best))) (setq best d)))))))
(check "jit-wide-threads" '(t t)
  (let ((ths (loop for i below 2
                   collect (let ((i i))
                             (mp:make-thread
                              (lambda ()
                                (loop repeat 200
                                      always (and (equal (wide-8 i 2 3 4 5 6 7 8)
                                                         (list i 2 3 4 5 6 7 8))
                                                  (equal (wide-acc 50 i 2 3 4 5 6 0)
                                                         (list 3 4 5 6 i 2 1275))))))))))
    (mapcar #'mp:join-thread ths)))

;; --- Direct calls into &key callees.  The m68k walker entered an &key
;; function through a keyword ABI of its own (bc, nargs, args on the VM
;; stack), which a call site's hit path cannot pass, so the fill rule
;; refused such a callee (:refused-abi) and every call from native code
;; took the helper.  An &key function takes the positional ABI now, like
;; &optional and &rest: the count in D1, the arguments above A6 in
;; operand-stack order, and cl_jit_runtime_kw_prologue fills the frame and
;; matches the keywords from there.
(defun key-sum (a &key (b 1)) (+ a b))
(defun key-sum-loop (n)
  ;; Two-argument + only: a longer one calls the builtin, whose site misses.
  (let ((s 0)) (dotimes (i n s) (setq s (+ s (+ (key-sum i) (key-sum i :b 2)))))))
(defun key-misses-for (n)
  (let ((m0 (jds-stat :misses)))
    (key-sum-loop n)
    (- (jds-stat :misses) m0)))
(defun key-full (a &optional (b (list a) bp) &key (c (list b) cp) d)
  (list a b bp c cp d))
(defun key-allow (&key a &allow-other-keys) a)
(defun key-strict (&key a) a)
;; A body that conses, collects and compacts: the arguments and the key
;; values read right afterwards.
(defun key-compact (a &key (b (list a)) c)
  (ext:gc-compact)
  (list a b c (opt-k)))
;; &rest with &key, called with 129 arguments: past moveq's range, and the
;; prologue reads every one of them in operand-stack order.
(defun key-rest (a &rest r &key (k 0) &allow-other-keys) (list a k (length r)))
(defmacro key-rest-call-many (n)
  `(key-rest 0 :k 5 ,@(loop for i below (- n 3) collect (if (evenp i) :x i))))
(defun key-rest-many () (key-rest-call-many 129))
;; Not a self tail call (those stay off for &key): a real native call per
;; level, every one through a filled site.
(defun key-rec (n &key (acc 0))
  (if (= n 0) acc (+ 0 (key-rec (- n 1) :acc (+ acc n)))))
(defun key-mk (n) (lambda (&key (x n) (y (list x))) (list x y)))
(defun key-fc (f) (list (funcall f) (funcall f :x 1) (funcall f :y 2 :x 1)))
;; Native callers: a site per count and keyword spelling.
(defun key-call-full ()
  (list (key-full 1) (key-full 1 2) (key-full 1 2 :c 3) (key-full 1 2 :d 4 :c 3)
        (key-full 1 2 :c 3 :c 4)))
(defun key-call-allow ()
  (list (key-allow) (key-allow :a 1 :z 2) (key-strict :a 1 :z 2 :allow-other-keys t)))
(defun key-call-strict (k) (key-strict k 1))
(defun key-call-odd (k) (key-strict k))
(defun key-call-fresh (i) (key-compact (list i) :c (list (+ i 1))))

(check "jit-key-direct-native" '(t t t t t t t t t t t t t t t t t)
  (mapcar #'opt-native-p
          (list #'key-sum #'key-sum-loop #'key-misses-for #'key-full
                #'key-allow #'key-strict #'key-compact #'key-rest
                #'key-rest-many #'key-rec #'key-fc (key-mk 0)
                #'key-call-full #'key-call-allow #'key-call-strict
                #'key-call-odd #'key-call-fresh)))
;; Twice: the first round fills the sites, the second hits them.
(check "jit-key-direct-calls"
  '((1 (1) nil ((1)) nil nil) (1 2 t (2) nil nil) (1 2 t 3 t nil)
    (1 2 t 3 t 4) (1 2 t 3 t nil))
  (progn (key-call-full) (key-call-full)))
(check "jit-key-direct-allow-other-keys" '(nil 1 1)
  (progn (key-call-allow) (key-call-allow)))
(check "jit-key-direct-errors" '(1 :unknown :unknown :odd :odd)
  (list (key-call-strict :a)
        (handler-case (progn (key-call-strict :q) :no-error) (error () :unknown))
        (handler-case (progn (key-call-strict :q) :no-error) (error () :unknown))
        (handler-case (progn (key-call-odd :a) :no-error) (error () :odd))
        (handler-case (progn (key-call-odd :a) :no-error) (error () :odd))))
(check "jit-key-direct-value" 1002000 (key-sum-loop 1000))
;; Best of three, as jit-opt-direct-calls-hit.  KEY-SUM conses nothing, so
;; this holds under AMILISP_GC_STRESS too.
(check "jit-key-direct-calls-hit" 0
  (progn
    (key-misses-for 10)
    (let ((best nil))
      (dotimes (k 3 best)
        (let ((d (- (key-misses-for 1000) (key-misses-for 10))))
          (when (or (null best) (< (abs d) (abs best))) (setq best d)))))))
(check "jit-key-direct-across-compaction"
  '(((7) ((7)) (8) ("k")) ((7) ((7)) (8) ("k")))
  (list (key-call-fresh 7) (key-call-fresh 7)))
(check "jit-key-direct-129-arguments" '((0 5 128) (0 5 128))
  (list (key-rest-many) (key-rest-many)))
(check "jit-key-direct-recursion" '(20100 20100) (list (key-rec 200) (key-rec 200)))
(check "jit-key-direct-closures"
  '(((1 (1)) (1 (1)) (1 2)) ((("k") (("k"))) (1 (1)) (1 2)))
  (list (key-fc (key-mk 1)) (key-fc (key-mk (opt-k)))))
(check "jit-key-direct-threads" '(t t)
  (let ((ths (loop for i below 2
                   collect (let ((i i))
                             (mp:make-thread
                              (lambda ()
                                (loop repeat 200
                                      always (and (equal (key-full i 2 :d 4 :c 3)
                                                         (list i 2 t 3 t 4))
                                                  (= (key-sum-loop 10) 120)))))))))
    (mapcar #'mp:join-thread ths)))

;; The C stack a native call into a generic function costs.  Such a call
;; takes jit_dispatch's trampoline arm into cl_vm_apply; the arm used to copy
;; the arguments into a CL_Obj[256] of its own (1 KB per level).  Once &rest
;; functions turned native, ASDF's FIND-SYSTEM on the suite's 128K stack hit
;; the C-stack guard (run-tests.lisp's shim checks).  On a 68040 an 80000-byte
;; worker reached 23 levels of native -> GF -> native before, 38 after; the
;; interpreter's frames (AArch64) cost no C stack at all.
(defgeneric gfdepth-gf (n))
(defvar *gfdepth-max* 0)
(defun gfdepth-down (n)
  (setq *gfdepth-max* (max *gfdepth-max* n))
  (gfdepth-gf (+ n 1)))
(defmethod gfdepth-gf ((n integer)) (if (>= n 400) n (gfdepth-down n)))
(dotimes (i 20) (gfdepth-gf 399))
(check "jit-native-to-gf-c-stack-per-level" t
  (progn
    (setq *gfdepth-max* 0)
    (mp:join-thread
     (mp:make-thread
      (lambda () (handler-case (gfdepth-down 0) (error () nil)))
      :stack-size 80000 :vm-frames 4096 :vm-stack-size 16384))
    (or (>= *gfdepth-max* 30)
        (progn (format t ";; DIAG native->GF depth ~D on an 80000-byte stack~%"
                       *gfdepth-max*)
               nil))))

;; --- Loops without calls poll (the interpreter's OP_JMP safepoint).
;; The interpreter checks for a pending GC, an interrupt and Ctrl-C on every
;; backward jump; native code used to check only on its way into a call.
;; So a native loop that calls nothing ran to its end deaf: a peer's
;; stop-the-world collection waited for it, MP:INTERRUPT-THREAD was not
;; delivered, Ctrl-C did not break it.  JLP-SPIN is such a loop: an inline
;; fixnum add, an inline compare and a special-variable read per iteration.
;; It also stops by itself after N iterations (several seconds native), so
;; a regression costs time but cannot hang the suite.
(require "amiga/raw/exec")
(defvar *jlp-stop* nil)
(defvar *jlp-task* nil)
(defun jlp-spin (n)
  (let ((i 0))
    (loop until (or *jlp-stop* (>= i n)) do (setq i (+ i 1)))
    i))
(defconstant +jlp-n+ 5000000)
(jlp-spin 10)
(check "jit-loop-poll-spin-is-native" t
  (not (null (amilisp::%jit-dump-bytes #'jlp-spin))))
;; The poll at the loop header: subq.w #1,jit_loop_ctr(a3) ($536B d16),
;; bcc.w over the call ($6400 $0008), jsr abs.l ($4EB9).
#+m68k
(check "jit-loop-poll-emitted-at-loop-header" t
  (let ((b (amilisp::%jit-dump-bytes #'jlp-spin)))
    (let ((p (search '(#x53 #x6B) b)))
      (and p (equal (subseq b (+ p 4) (+ p 10)) '(#x64 #x00 #x00 #x08 #x4E #xB9))))))
;; A function without a loop gets no poll.
(defun jlp-straight (a b) (if (< a b) (+ a b) (- a b)))
(jlp-straight 1 2)
(check "jit-loop-poll-not-in-straight-code" '(t nil)
  (let ((b (amilisp::%jit-dump-bytes #'jlp-straight)))
    (list (not (null b)) (not (null (search '(#x53 #x6B) b))))))

;; A self tail call is a loop too: the native code branches back to its
;; entry, and polls on the way round.
(defun jlp-tail (i n)
  (if (or *jlp-stop* (>= i n)) i (jlp-tail (+ i 1) n)))
(jlp-tail 0 10)
;; These three are m68k-paced: a native host finishes +JLP-N+ iterations
;; long before the 0.2 s sleep is over (the AArch64 loop poll is
;; tests/test_jit_a64_walk.sh part 2's, a loop bounded by its stop flag).
#+m68k
(check "jit-loop-poll-self-tail-call-interrupt-delivered" '(:interrupted t)
  (progn
    (setq *jlp-stop* nil)
    (let ((th (mp:make-thread (lambda () (jlp-tail 0 +jlp-n+)) :name "jlp-tail")))
      (sleep 0.2)
      (mp:interrupt-thread th (lambda () (setq *jlp-stop* :interrupted)))
      (loop repeat 50 until *jlp-stop* do (sleep 0.1))
      (let ((r *jlp-stop*))
        (unless r (setq *jlp-stop* :forced))
        (list r (< (mp:join-thread th) +jlp-n+))))))

#+m68k
(check "jit-loop-poll-interrupt-delivered" '(:interrupted t)
  (progn
    (setq *jlp-stop* nil)
    (let ((th (mp:make-thread (lambda () (jlp-spin +jlp-n+)) :name "jlp-spin")))
      (sleep 0.2)
      (mp:interrupt-thread th (lambda () (setq *jlp-stop* :interrupted)))
      (loop repeat 50 until *jlp-stop* do (sleep 0.1))
      (let ((r *jlp-stop*))
        (unless r (setq *jlp-stop* :forced))
        (list r (< (mp:join-thread th) +jlp-n+))))))

;; A full GC requested from here has to stop the spinning thread at its
;; next loop iteration, not after its last one: the collection returns
;; while the spinner is still alive and short of N.
#+m68k
(check "jit-loop-poll-stw-gc-not-delayed" '(t t t)
  (progn
    (setq *jlp-stop* nil)
    (let ((th (mp:make-thread (lambda () (jlp-spin +jlp-n+)) :name "jlp-spin")))
      (sleep 0.3)
      (let* ((t0 (get-internal-real-time))
             (dummy (ext:gc))
             (dt (/ (- (get-internal-real-time) t0)
                    internal-time-units-per-second))
             (alive (mp:thread-alive-p th)))
        (declare (ignore dummy))
        (setq *jlp-stop* :done)
        (list (< dt 1) alive (< (mp:join-thread th) +jlp-n+))))))

;; Collections that now run INSIDE a native loop (at its poll) move the
;; objects the loop holds in its frame: a list walked over and over while a
;; peer compacts must still sum to the same value every pass.
(defun jlp-walk (l n)
  (let ((s 0))
    (dotimes (k n)
      (let ((p l))
        (loop while p do (setq s (+ s (car p)) p (cdr p)))))
    s))
(jlp-walk '(1 2) 1)
(stress-check "jit-loop-poll-compaction-inside-loop" '(t t)
  (let* ((l (loop for i from 1 to 50 collect i))
         (walker (mp:make-thread (lambda () (jlp-walk l 1500)) :name "jlp-walk"))
         (compactor (mp:make-thread
                     (lambda () (dotimes (i 15) (ext:gc-compact)) t)
                     :name "jlp-compactor")))
    (list (= (mp:join-thread walker) (* 1500 1275))
          (mp:join-thread compactor))))

;; Ctrl-C (SIGBREAKF_CTRL_C on the thread's own task) breaks the loop.  BREAK
;; goes through *DEBUGGER-HOOK*, which the spinner binds to leave the loop.
(check "jit-loop-poll-ctrl-c-breaks" '(:break t)
  (progn
    (setq *jlp-stop* nil *jlp-task* nil)
    (let ((th (mp:make-thread
               (lambda ()
                 (let ((*debugger-hook*
                         (lambda (c h)
                           (declare (ignore c h))
                           (setq *jlp-stop* :break)
                           (throw 'jlp-out 0))))
                   (catch 'jlp-out
                     (setq *jlp-task* (amiga.raw.exec:find-task nil))
                     (jlp-spin +jlp-n+))))
               :name "jlp-spin")))
      (loop repeat 50 until *jlp-task* do (sleep 0.1))
      (sleep 0.2)
      (when *jlp-task* (amiga.raw.exec:signal *jlp-task* #x1000))
      (loop repeat 50 until *jlp-stop* do (sleep 0.1))
      (let ((r *jlp-stop*))
        (unless r (setq *jlp-stop* :forced))
        (list r (< (mp:join-thread th) +jlp-n+))))))


; --- String-scan fast path (opcodes.h 0xC0-0xC4, specs/performance.md 4.4).
; Five opcodes, each with a walker template: AREF (helper call with the
; accessor kind), CHAREQ (inline character-tag test + CMP.L, helper for
; the type error), CMP_BR (inline fixnum / character compare branching
; straight to the target, helper slow path), PUSH_LOCAL / POP_LOCAL
; (helpers that take the slot's address in the LINK frame).  Every
; function below must compile natively — %JIT-DUMP-BYTES is non-NIL and
; the invoke counter moves — and agree with the bytecode semantics on
; the fast path, the slow path and the error path.  Declared speed 1
; here still runs the peephole, which is what fuses CMP_BR.
(defun jss-schar (s i) (schar s i))
(defun jss-char (s i) (char s i))
(defun jss-aref (v i) (aref v i))
(defun jss-svref (v i) (svref v i))
(defun jss-chareq (a b) (char= a b))
(defun jss-chareq-br (a b) (list (if (char= a b) :y :n) (unless (char= a b) :n)))
(defun jss-cmp (a b)
  (list (if (< a b) 1 0) (if (> a b) 1 0) (if (<= a b) 1 0) (if (>= a b) 1 0)
        (if (= a b) 1 0) (unless (< a b) 1) (unless (>= a b) 1) (if (not (= a b)) 1 0)))
(defun jss-push (x) (let ((s nil)) (push x s) (push 2 s) s))
(defun jss-pop (l) (list (pop l) (pop l) l))
(defun jss-scan (text)
  (let ((stack nil) (state 0) (i 0) (opens 0) (n (length text)))
    (loop while (< i n)
          do (let ((c (schar text i)))
               (case state
                 (0 (case c
                      (#\( (push i stack) (incf opens))
                      (#\) (pop stack))
                      (#\" (setq state 1))
                      (#\; (setq state 2))))
                 (1 (case c (#\\ (setq state 3)) (#\" (setq state 0))))
                 (2 (when (char= c #\Newline) (setq state 0)))
                 (t (setq state 1))))
             (incf i))
    (list opens state stack)))
(defun jss-native-p (f) (not (null (amilisp::%jit-dump-bytes f))))
(check "jss-all-native" t
  (every #'jss-native-p (list #'jss-schar #'jss-char #'jss-aref #'jss-svref
                              #'jss-chareq #'jss-chareq-br #'jss-cmp
                              #'jss-push #'jss-pop #'jss-scan)))
(check "jss-scan-counter-bump" t
  (let ((before (amilisp::%jit-invoke-count)))
    (jss-scan "()")
    (> (amilisp::%jit-invoke-count) before)))
(check "jss-schar" #\b (jss-schar "abc" 1))
(check "jss-char" #\c (jss-char "abc" 2))
(check "jss-char-fill-pointer" #\b
  (jss-char (make-array 4 :element-type 'character :fill-pointer 3 :initial-contents "abcd") 1))
(check "jss-aref-vector" 20 (jss-aref (vector 10 20 30) 1))
(check "jss-aref-string" #\a (jss-aref "abc" 0))
(check "jss-aref-bit" 1 (jss-aref #*0110 1))
(check "jss-svref" 30 (jss-svref (vector 10 20 30) 2))
(check "jss-schar-oob" :error (handler-case (jss-schar "abc" 3) (error () :error)))
(check "jss-schar-not-string" :error (handler-case (jss-schar 42 0) (error () :error)))
(check "jss-svref-not-simple" :error
  (handler-case (jss-svref (make-array 3 :fill-pointer 2) 0) (error () :error)))
(check "jss-svref-string" :error
  (handler-case (jss-svref "abc" 0) (error () :error)))
(check "jss-aref-list" :error (handler-case (jss-aref '(1 2) 0) (error () :error)))
(check "jss-chareq" '(t nil) (list (jss-chareq #\x #\x) (jss-chareq #\x #\y)))
(check "jss-chareq-type-error" :type-error
  (handler-case (jss-chareq #\x 1) (type-error () :type-error)))
(check "jss-chareq-br" '((:y nil) (:n :n)) (list (jss-chareq-br #\a #\a) (jss-chareq-br #\a #\b)))
(check "jss-chareq-br-type-error" :type-error
  (handler-case (jss-chareq-br 1 #\a) (type-error () :type-error)))
(check "jss-cmp-fixnum-lt" '(1 0 1 0 0 nil 1 1) (jss-cmp 1 2))
(check "jss-cmp-fixnum-gt" '(0 1 0 1 0 1 nil 1) (jss-cmp 2 1))
(check "jss-cmp-fixnum-eq" '(0 0 1 1 1 1 nil 0) (jss-cmp 3 3))
(check "jss-cmp-negative" '(1 0 1 0 0 nil 1 1) (jss-cmp -5 -1))
(check "jss-cmp-bignum" '(0 1 0 1 0 1 nil 1) (jss-cmp (expt 2 40) 1))
(check "jss-cmp-float" '(1 0 1 0 0 nil 1 1) (jss-cmp 1.5 2))
(check "jss-cmp-ratio" '(0 0 1 1 1 1 nil 0) (jss-cmp 1/2 0.5))
(check "jss-cmp-type-error" :type-error
  (handler-case (jss-cmp 'a 1) (type-error () :type-error)))
(check "jss-push" '(2 1) (jss-push 1))
(check "jss-pop" '(1 2 (3)) (jss-pop '(1 2 3)))
(check "jss-pop-nil" '(nil nil nil) (jss-pop nil))
(check "jss-pop-non-list" :type-error
  (handler-case (jss-pop 5) (type-error () :type-error)))
(check "jss-scan" '(4 0 (10 7 0)) (jss-scan "(a (b) (c (d"))
(check "jss-scan-strings-comments" '(1 2 (0))
  (jss-scan "(a \"x)\" ; )("))
; PUSH_LOCAL conses from native code with the list in a LINK-frame slot:
; a compaction in the middle must keep the partial list.
(defun jss-push-gc (n)
  (let ((s nil))
    (dotimes (i n)
      (push (make-string 8 :initial-element #\z) s)
      (when (= i 20) (ext:gc-compact)))
    (list (length s) (length (first s)))))
(check "jss-push-gc-native" t (jss-native-p #'jss-push-gc))
(check "jss-push-gc" '(50 8) (jss-push-gc 50))

; --- Hot compilation (the default since 0.12, jit.h) ---
; Outside eager mode a function is compiled when it turns hot: on its
; %JIT-HOT-THRESHOLD-th interpreted call, on its first call when it loops,
; and at definition when compiled under (optimize (speed 3)).  A function
; defined with the JIT off stays bytecode.  All of this is what lets a heap
; image, whose native code is dropped on restore, run native code again.
(amilisp::%jit-set-hot-threshold *jit-suite-hot-threshold*)
(defun hot-native-p (f) (and (amilisp::%jit-dump-bytes f) t))
;; The default is 8, unless AMILISP_JIT_HOT set another at boot (`make
;; test-jit-eager` runs everything with 0).  The checks below are written
;; against 8, so the section pins it.
(check "hot: default threshold"
  (if (ext:getenv "AMILISP_JIT_HOT") *jit-suite-hot-threshold* 8)
  (amilisp::%jit-hot-threshold))
(amilisp::%jit-set-hot-threshold 8)
; %JIT-SET-HOT-THRESHOLD documents its domain as (INTEGER 0 126) -- the
; count lives in 7 bits of bc->jit_hot (CL_BC_JIT_COUNT_MASK) and 127 is
; reserved as the CL_BC_JIT_SETTLED sentinel.  A negative value signals;
; a value past 126 used to be silently clamped by cl_jit_set_hot_threshold
; instead of also signaling, contradicting that documented type.
(check "hot: set-hot-threshold rejects a negative value" :type-error
  (handler-case (progn (amilisp::%jit-set-hot-threshold -1) :no-error)
    (type-error () :type-error)))
(check "hot: set-hot-threshold rejects past the 7-bit max" :type-error
  (handler-case (progn (amilisp::%jit-set-hot-threshold 127) :no-error)
    (type-error () :type-error)))
(check "hot: set-hot-threshold accepts the max" 126
  (let ((prev (amilisp::%jit-set-hot-threshold 126)))
    (prog1 (amilisp::%jit-hot-threshold)
      (amilisp::%jit-set-hot-threshold prev))))
(check "hot: threshold unchanged after a rejected out-of-range call" 8
  (amilisp::%jit-hot-threshold))
(defun hot-inc (x) (+ x 1))
(check "hot: not compiled at definition" nil (hot-native-p #'hot-inc))
(check "hot: interpreted below the threshold" nil
  (progn (dotimes (i 7) (hot-inc i)) (hot-native-p #'hot-inc)))
(check "hot: compiled on the threshold-th call" '(1 t t)
  (let ((before (amilisp::%jit-hot-compile-count)))
    (list (hot-inc 0) (hot-native-p #'hot-inc)
          (> (amilisp::%jit-hot-compile-count) before))))
(check "hot: native after it turned hot" '(42 t)
  (let ((before (amilisp::%jit-invoke-count)))
    (list (hot-inc 41) (> (amilisp::%jit-invoke-count) before))))
(defun hot-loop (n) (let ((s 0)) (dotimes (i n) (setq s (+ s i))) s))
(check "hot: a loop is not compiled at definition" nil (hot-native-p #'hot-loop))
(check "hot: a loop compiles on its first call" '(45 t)
  (list (hot-loop 10) (hot-native-p #'hot-loop)))
(defun hot-speed (x) (declare (optimize (speed 3))) (* x 2))
(check "hot: (speed 3) compiles at definition" t (hot-native-p #'hot-speed))
(amilisp::%jit-set-active nil)
(defun hot-cold (x) (+ x 3))
(amilisp::%jit-set-active t)
(check "hot: defined with the JIT off stays bytecode" '(23 nil)
  (let ((r 0)) (dotimes (i 21) (setq r (hot-cold i))) (list r (hot-native-p #'hot-cold))))
; A callee reached only from native code is counted by the native call path.
(defun hot-leaf (x) (* x 3))
(defun hot-driver (n) (let ((s 0)) (dotimes (i n) (setq s (+ s (hot-leaf i)))) s))
(check "hot: native callers count their interpreted callees" '(570 t t)
  (list (hot-driver 20) (hot-native-p #'hot-driver) (hot-native-p #'hot-leaf)))
; ... once per call: jit_dispatch hands the callee to the stub frame, whose
; OP_CALL counts it.  jit_dispatch used to count it as well, which made it
; hot after half the threshold.
(defun hot-leaf-once (x) (* x 5))
(defun hot-driver-native (n)
  (declare (optimize (speed 3)))
  (let ((s 0)) (dotimes (i n) (setq s (+ s (hot-leaf-once i)))) s))
(check "hot: a native caller's callee compiles on the threshold-th call" '(t nil t)
  (list (hot-native-p #'hot-driver-native)
        (progn (hot-driver-native (1- (amilisp::%jit-hot-threshold)))
               (hot-native-p #'hot-leaf-once))
        (progn (hot-driver-native 1) (hot-native-p #'hot-leaf-once))))
(check "hot: threshold 1 compiles on the first call" '(5 t)
  (let ((prev (amilisp::%jit-set-hot-threshold 1)))
    (defun hot-once (x) (+ x 4))
    (prog1 (list (hot-once 1) (hot-native-p #'hot-once))
      (amilisp::%jit-set-hot-threshold prev))))
; A TAIL call from interpreted code into native code runs native (call, then
; return from the caller's frame).  The native fast path used to skip tail
; calls, which kept a hot loop that a run-once entry function tail-calls
; interpreted forever.
(defun hot-tail-target (n) (let ((s 0)) (dotimes (i n) (setq s (+ s 1))) s))
(defun hot-tail-mv (x) (values x (+ x 1) (+ x 2)))
(amilisp::%jit-set-active nil)
(defun hot-tail-caller (n) (hot-tail-target n))          ; bytecode, tail call
(defun hot-tail-mv-caller (x) (hot-tail-mv x))
(amilisp::%jit-set-active t)
(check "hot: an interpreted tail call enters native code" '(t 100 t)
  (progn
    (hot-tail-target 1)                                  ; a loop: compiles now
    (let ((before (amilisp::%jit-invoke-count)))
      (list (hot-native-p #'hot-tail-target)
            (hot-tail-caller 100)
            (> (amilisp::%jit-invoke-count) before)))))
(check "hot: multiple values through a native tail call" '(t (5 6 7))
  (progn
    (dotimes (i 8) (hot-tail-mv i))
    (list (hot-native-p #'hot-tail-mv)
          (multiple-value-list (hot-tail-mv-caller 5)))))
; Self tail recursion that turns native part-way: the interpreted frame
; stays for one native call, the rest recurses in native code.
(defun hot-count-down (n) (if (= n 0) :done (hot-count-down (- n 1))))
(check "hot: deep self tail recursion that turns native" :done
  (hot-count-down 200000))

; JITEXPAND / %JIT-DISASSEMBLE show a fresh definition: they compile a
; function that has not turned hot yet -- but not one defined with the JIT
; off (HOT-COLD above).
(defun hot-shown (x) (- x 1))
(check "hot: %jit-disassemble compiles a fresh definition" '(nil t)
  (list (hot-native-p #'hot-shown)
        (progn (amilisp::%jit-disassemble #'hot-shown) (hot-native-p #'hot-shown))))
(check "hot: %jit-disassemble leaves a JIT-off definition alone" nil
  (progn (amilisp::%jit-disassemble #'hot-cold) (hot-native-p #'hot-cold)))

; Several threads calling one function while it turns hot.  A counting
; store that went stale could overwrite another thread's settle with a
; small count (cl_jit_note_call re-checks before it writes), and a
; declined function would then be tried again every 8 calls.  That race
; is too narrow to force from here, so this is a smoke test of concurrent
; settling.  hot-race-declined always declines: neither walker compiles
; DEFVAR's opcode.  %JIT-HOT-COMPILE-COUNT counts every function the hot path tries,
; so the target must be the only one counted in the window: the worker is
; (speed 3), settled at definition, and the thread's entry call into it
; (cl_vm_apply's stub OP_CALL, which counts like any interpreted call)
; therefore counts nothing -- a loop LAMBDA there would compile on its
; first call.  Two threads that reach the threshold together may both try
; (jit_m68k.c), hence at most 2.
(defun hot-race-declined (x a b c d e f) (defvar *hot-race-dv* nil) (or x a b c d e f))
(defun hot-race-worker ()
  (declare (optimize (speed 3)))
  (dotimes (i 400) (hot-race-declined i nil nil nil nil nil nil)))
(check "hot: concurrent calls settle a declined function" '(nil t)
  (let ((prev (amilisp::%jit-set-hot-threshold 8)))
    (unwind-protect
        (let ((before (amilisp::%jit-hot-compile-count))
              (workers nil))
          (dotimes (w 6)
            (push (mp:make-thread #'hot-race-worker :name "hot-race-worker")
                  workers))
          (mapc #'mp:join-thread workers)
          (list (hot-native-p #'hot-race-declined)
                (<= 1 (- (amilisp::%jit-hot-compile-count) before) 2)))
      (amilisp::%jit-set-hot-threshold prev))))

; Restore the suite-wide baseline established by run-tests.lisp's
; "declaim optimize" test — sections after this load expect speed 3.
(declaim (optimize (speed 3)))

(amilisp::%jit-set-hot-threshold *jit-suite-hot-threshold*)

; The last form: a runner can tell the whole file loaded (a reader error --
; a package missing on this platform, say -- ends a LOAD silently early).
(defparameter *test-jit-loaded-to-end* t)
