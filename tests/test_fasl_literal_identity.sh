#!/bin/sh
# A literal that occurs twice in one top-level form is still one object after
# COMPILE-FILE + LOAD (CLHS 3.2.4.4: the loader may coalesce similar literals,
# it may not split one that was EQ in the source).
#
# The FASL writer kept identity for conses, symbols, structures, closures and
# locks only, so '(#1="s" #1#) came back as two strings; strings, vectors,
# multi-dimensional arrays, bit/byte vectors and pathnames are in the
# shared-object set since FASL v36.  The reader side has to register a vector
# before it reads the elements: an element with its own OBJ_DEF cleared the
# pending id, and a self-referential vector needs the shell anyway.
#
# Found on the way: the source reader patched a circular #n# back-reference
# through conses only, so #1=#(a #1#) kept the label's placeholder cons as
# its element (CLHS 2.4.8.15/16).
#
# Both legs run: an explicit COMPILE-FILE + LOAD, and LOAD's implicit cache
# (second LOAD of the source comes from the cached FASL).
#
# Run: sh tests/test_fasl_literal_identity.sh [path-to-amilisp]

AMILISP="${1:-build/host/amilisp}"
case "$AMILISP" in
    /*|[A-Za-z]:/*) ;;
    *) AMILISP="$(cd "$(dirname "$AMILISP")" && pwd)/$(basename "$AMILISP")" ;;
esac

passed=0
failed=0
TMP="${TMPDIR:-/tmp}/clamiga-litident-$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

check() {
    desc="$1"; cond="$2"
    if [ "$cond" = "yes" ]; then
        echo "  ok  $desc"; passed=$((passed + 1))
    else
        echo "  FAIL  $desc"; failed=$((failed + 1))
        [ -n "$out" ] && echo "$out" | tail -14 | sed 's/^/      | /'
    fi
}
has()  { echo "$out" | grep -q -- "$1" && echo yes || echo no; }

cat > "$TMP/lit.lisp" <<'EOF'
(defvar *li-s*    '(#1="s" #1# "s"))
(defvar *li-v*    '(#1=#(1 "x" (a)) #1#))
(defvar *li-circ* '#1=#(a #1# "t"))
(defvar *li-bv*   '(#1=#*1011 #1#))
(defvar *li-p*    '(#1=#P"foo/bar.lisp" #1#))
(defvar *li-a*    '(#1=#2A((1 "q") (#2="z" #2#)) #1#))
(defvar *li-nest* '(#1=#(#2="in" #2# #3=(k)) #1# #2# #3#))
(defvar *li-deep* '#1=#(x (y #(z #1#))))
EOF
cat > "$TMP/report.lisp" <<'EOF'
(defun li-report (tag)
  (format t "~A S ~S~%" tag (list (eq (first *li-s*) (second *li-s*)) (third *li-s*)))
  (format t "~A V ~S~%" tag (list (eq (first *li-v*) (second *li-v*)) (first *li-v*)))
  (format t "~A CIRC ~S~%" tag (list (eq *li-circ* (aref *li-circ* 1))
                                     (aref *li-circ* 0) (aref *li-circ* 2)))
  (format t "~A BV ~S~%" tag (list (eq (first *li-bv*) (second *li-bv*)) (first *li-bv*)))
  (format t "~A P ~S~%" tag (list (eq (first *li-p*) (second *li-p*))
                                  (pathname-name (first *li-p*))))
  (format t "~A A ~S~%" tag (let ((a (first *li-a*)))
                              (list (eq a (second *li-a*))
                                    (eq (aref a 1 0) (aref a 1 1))
                                    (aref a 0 1) (aref a 1 0))))
  (format t "~A NEST ~S~%" tag (let ((v (first *li-nest*)))
                                 (list (eq v (second *li-nest*))
                                       (eq (aref v 0) (aref v 1))
                                       (eq (aref v 0) (third *li-nest*))
                                       (eq (aref v 2) (fourth *li-nest*)))))
  (format t "~A DEEP ~S~%" tag (eq *li-deep*
                                   (aref (second (aref *li-deep* 1)) 1))))
EOF
cat > "$TMP/run.lisp" <<'EOF'
(load "report.lisp")
(load "lit.lisp")                       ; from source, writes the cache
(li-report "SRC")
(dolist (s '(*li-s* *li-v* *li-circ* *li-bv* *li-p* *li-a* *li-nest* *li-deep*))
  (makunbound s))
(load "lit.lisp")                       ; from the cached FASL
(li-report "CACHE")
(dolist (s '(*li-s* *li-v* *li-circ* *li-bv* *li-p* *li-a* *li-nest* *li-deep*))
  (makunbound s))
(compile-file "lit.lisp" :output-file "lit.fasl")
(load "lit.fasl")
(li-report "FASL")
(ext:gc)
(li-report "GC")
EOF
out=$(cd "$TMP" && AMILISP_FASL_CACHE_DIR="$TMP/cache" \
      "$AMILISP" --no-userinit --non-interactive --load run.lisp </dev/null 2>&1)

for leg in SRC CACHE FASL GC; do
    check "${leg}_shared_string_is_one_object"       "$(has "^$leg S (T \"s\")")"
    check "${leg}_shared_vector_is_one_object"       "$(has "^$leg V (T #(1 \"x\" (A)))")"
    check "${leg}_vector_can_contain_itself"         "$(has "^$leg CIRC (T A \"t\")")"
    check "${leg}_shared_bit_vector_is_one_object"   "$(has "^$leg BV (T #\\*1011)")"
    check "${leg}_shared_pathname_is_one_object"     "$(has "^$leg P (T \"bar\")")"
    check "${leg}_shared_array_and_its_elements"     "$(has "^$leg A (T T \"q\" \"z\")")"
    check "${leg}_labels_inside_a_labelled_vector"   "$(has "^$leg NEST (T T T T)")"
    check "${leg}_back_reference_below_list_and_vector" "$(has "^$leg DEEP T")"
done
check "the_cache_leg_really_came_from_a_fasl" \
      "$([ -n "$(find "$TMP/cache" -name '*.fasl' 2>/dev/null | head -1)" ] && echo yes || echo no)"

echo ""
echo "test_fasl_literal_identity: $passed passed, $failed failed"
[ "$failed" -eq 0 ]
