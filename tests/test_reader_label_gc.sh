#!/bin/sh
# A garbage collection while a #n= labelled object is being read.
#
# #n= roots a placeholder cons for the label while it reads the object, so a
# #n# inside can refer back to it.  The placeholder's marker car used to be
# the reader's private #+/#- SKIP sentinel (0x06): "not a fixnum, not a
# character" is all gc_mark_obj filters, so a collection in that window took
# it for a heap offset and marked arena offset 6 as an object — SIGSEGV in a
# release build.  The marker is CL_UNBOUND now, which the collector knows.
# #. forces the collection inside the window; under real heap pressure any
# large labelled literal could hit it.
#
# Stress twin: the "read-label" case of tests/test_gc_stress_regression.sh.
#
# Run: sh tests/test_reader_label_gc.sh [path-to-amilisp]

CLAMIGA="${1:-build/host/amilisp}"
case "$CLAMIGA" in
    /*|[A-Za-z]:/*) ;;
    *) CLAMIGA="$(cd "$(dirname "$CLAMIGA")" && pwd)/$(basename "$CLAMIGA")" ;;
esac

passed=0
failed=0
TMP="${TMPDIR:-/tmp}/clamiga-rdlabel-$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

check() {
    desc="$1"; cond="$2"
    if [ "$cond" = "yes" ]; then
        echo "  ok  $desc"; passed=$((passed + 1))
    else
        echo "  FAIL  $desc"; failed=$((failed + 1))
        [ -n "$out" ] && echo "$out" | tail -12 | sed 's/^/      | /'
    fi
}
has()  { echo "$out" | grep -q -- "$1" && echo yes || echo no; }

cat > "$TMP/lit.lisp" <<'EOF'
(defvar *rl-file* '(#1=(f1 #.(progn (ext:gc) 'f2) f3) #1# #2=(h #.(progn (ext:gc) 'i) . #2#)))
EOF
cat > "$TMP/run.lisp" <<'EOF'
(defun rl-shape (l)
  (list (first l) (eq (first l) (second l))
        (let ((c (third l))) (list (first c) (second c) (eq (cddr c) c)))))
(format t "STRING ~S~%"
        (rl-shape (read-from-string
                   "(#1=(a #.(progn (ext:gc) 'b) c) #1# #2=(x #.(progn (ext:gc) 'y) . #2#))")))
(format t "NESTED ~S~%"
        (let ((l (read-from-string "(#1=(p #2=(q #.(progn (ext:gc) 'r) #1#)) #2#)")))
          (list (eq (second (first l)) (second l))
                (eq (third (second l)) (first l))
                (second (second l)))))
(format t "VECTOR ~S~%"
        (let ((v (read-from-string "#(#1=(v1 #.(progn (ext:gc) 'v2)) #1#)")))
          (list (aref v 0) (eq (aref v 0) (aref v 1)))))
(load "lit.lisp")
(format t "LOAD ~S~%" (rl-shape *rl-file*))
(format t "UNBOUND-MARKER-STAYS-PRIVATE ~S~%"
        (equal (read-from-string "(#1=(nil) #1# (nil))") '((nil) (nil) (nil))))
(ext:gc)
(format t "ALIVE ~S~%" (length (make-list 1000)))
EOF
out=$(cd "$TMP" && "$CLAMIGA" --no-userinit --non-interactive --no-fasl-cache \
      --load run.lisp </dev/null 2>&1)
check "gc_inside_shared_and_circular_labels" "$(has '^STRING ((A B C) T (X Y T))')"
check "gc_inside_nested_labels"              "$(has '^NESTED (T T R)')"
check "gc_inside_a_label_in_a_vector"        "$(has '^VECTOR ((V1 V2) T)')"
check "gc_inside_labels_of_a_loaded_file"    "$(has '^LOAD ((F1 F2 F3) T (H I T))')"
check "plain_nil_conses_are_not_placeholders" "$(has '^UNBOUND-MARKER-STAYS-PRIVATE T')"
check "heap_sound_afterwards"                "$(has '^ALIVE 1000')"

echo ""
echo "test_reader_label_gc: $passed passed, $failed failed"
[ "$failed" -eq 0 ]
