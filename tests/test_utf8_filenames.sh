#!/bin/sh
# Regression: a file whose name is not representable in the platform's legacy
# 8-bit code page must still be created, listed by DIRECTORY, and reopened.
#
# On Windows this exercises the wide Win32 entry points in platform_win32.c.
# The ANSI ones (FindFirstFileA & co.) substitute '?' for every character the
# process code page cannot spell, so DIRECTORY used to hand back "???.lisp" —
# a name that matches no file and opens nothing, with no error anywhere.  The
# CRT calls (fopen, stat) are fine because main() puts the C locale in UTF-8
# mode, which is why creating the file always worked and only listing it did
# not: the failure was invisible until you looked.
#
# The driver is a UTF-8 source FILE, not an --eval argument, because the
# reader decodes UTF-8 from a stream and not from a C string.
#
# Run: sh tests/test_utf8_filenames.sh [path-to-amilisp]

AMILISP="${1:-build/host/amilisp}"
TMP="${TMPDIR:-/tmp}/clamiga_utf8names_$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/driver.lisp" <<'LISP'
(let* ((dir (or (ext:getenv "AMILISP_UTF8_TEST_DIR") "."))
       (path (concatenate 'string dir "/" "日本語ファイル.txt")))
  (with-open-file (s path :direction :output :if-exists :supersede)
    (write-string "content" s))
  (let* ((listed (directory (concatenate 'string dir "/*.txt")))
         (found  (first listed)))
    (format t "COUNT:~a~%" (length listed))
    (format t "REOPENABLE:~a~%" (if (and found (probe-file found)) "YES" "NO"))
    (format t "NAME-HAS-QUESTION-MARKS:~a~%"
            (if (and found (search "?" (namestring found))) "YES" "NO"))))
LISP

AMILISP_UTF8_TEST_DIR="$TMP" "$AMILISP" --no-userinit --non-interactive \
    --load "$TMP/driver.lisp" </dev/null > "$TMP/out" 2>&1
out=$(tr -d '\r' < "$TMP/out")

passed=0
failed=0
check() {
    desc="$1"; pattern="$2"
    if echo "$out" | grep -q "$pattern"; then
        echo "  ok  $desc"; passed=$((passed + 1))
    else
        echo "  FAIL  $desc (expected /$pattern/)"
        echo "$out" | sed 's/^/      /' | head -5
        failed=$((failed + 1))
    fi
}

check "directory_lists_the_non_ascii_file" "^COUNT:1$"
check "listed_name_reopens"                "^REOPENABLE:YES$"
check "listed_name_is_not_mangled"         "^NAME-HAS-QUESTION-MARKS:NO$"

echo ""
echo "test_utf8_filenames: $passed passed, $failed failed"
[ "$failed" -eq 0 ]
