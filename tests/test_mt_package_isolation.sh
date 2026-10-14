#!/bin/sh
# Per-thread *PACKAGE*: a bind on one thread must never redirect another
# thread's reader/INTERN.  Pre-fix, cl_current_package was one shared C
# global re-synced from the calling thread on every *PACKAGE* bind/unbind:
# under Sly, the slynk control thread binds *PACKAGE* to SLYNK-IO-PACKAGE
# around every wire message while a worker COMPILE-FILEs, so the worker's
# reader interned random tokens into SLYNK-IO-PACKAGE — one source name
# became two symbols, defmacro bodies compiled parameter references as
# global lookups, and the poisoned cached FASL failed forever after with
# "Unbound variable: FORMAT-CONTROL" (cl-ppcre errors.fasl, 2026-08-11).
# Run: sh tests/test_mt_package_isolation.sh build/host/amilisp

CLAMIGA="${1:-build/host/amilisp}"
# Absolute path — case 2 runs with cwd inside the scratch dir.
CLAMIGA=$(cd "$(dirname "$CLAMIGA")" && pwd)/$(basename "$CLAMIGA")
passed=0
failed=0
total=0

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# ---- Case 1: deterministic bind-and-hold ------------------------------
# A worker binds *PACKAGE* to a bare package and HOLDS the binding while
# the main thread reads a fresh symbol.  Pre-fix this failed 100%: the
# worker's bind clobbered the shared global, so the main thread's reader
# interned into PKG-POISON-PROBE.
cat > "$WORK/mt-pkg-hold.lisp" <<'LISPEOF'
(defpackage :pkg-poison-probe (:use))
(defvar *bound* nil)
(defvar *release* nil)
(mp:make-thread
 (lambda ()
   (let ((*package* (find-package :pkg-poison-probe)))
     (setf *bound* t)
     ;; Hold the binding until released (bounded so a bug can't hang CI).
     (loop for i from 0 below 3000 until *release* do (sleep 0.01))))
 :name "binder")
(loop for i from 0 below 3000 until *bound* do (sleep 0.01))
(let ((sym (read-from-string "pkg-poison-fresh-sym")))
  (format t "MT-PKG-HOME:~a~%" (package-name (symbol-package sym))))
(format t "MT-PKG-LEAK:~a~%"
        (if (find-symbol "PKG-POISON-FRESH-SYM" :pkg-poison-probe) 1 0))
(setf *release* t)
(quit)
LISPEOF

out=$("$CLAMIGA" --no-userinit --load "$WORK/mt-pkg-hold.lisp" </dev/null 2>&1)

total=$((total + 1))
case "$out" in
  *"MT-PKG-HOME:COMMON-LISP-USER"*)
    echo "  ok  reader_ignores_peer_thread_package_bind"
    passed=$((passed + 1)) ;;
  *MT-PKG-HOME:*)
    echo "  FAIL  reader_ignores_peer_thread_package_bind (interned via peer's binding)"
    echo "$out" | grep "MT-PKG-HOME"
    failed=$((failed + 1)) ;;
  *)
    echo "  FAIL  reader_ignores_peer_thread_package_bind (run died)"
    echo "$out" | tail -5
    failed=$((failed + 1)) ;;
esac

total=$((total + 1))
case "$out" in
  *"MT-PKG-LEAK:0"*)
    echo "  ok  no_symbol_leaked_into_peer_package"
    passed=$((passed + 1)) ;;
  *)
    echo "  FAIL  no_symbol_leaked_into_peer_package"
    echo "$out" | grep "MT-PKG-LEAK" || echo "$out" | tail -5
    failed=$((failed + 1)) ;;
esac

# ---- Case 2: COMPILE-FILE under binder churn --------------------------
# The original field failure: a worker rapidly binds/unbinds *PACKAGE*
# (slynk autodoc traffic) while the main thread COMPILE-FILEs a file of
# defmacros.  Any symbol appearing in the bare package — or a macro
# expander that lost its lambda-list binding — is the poisoning.
cat > "$WORK/mt-pkg-compile.lisp" <<'LISPEOF'
(defpackage :fake-sio (:use))
(defpackage :myppcre (:use :cl))
(defvar *stop* nil)

(with-open-file (s "gen.lisp" :direction :output :if-exists :supersede)
  (format s "(in-package :myppcre)~%")
  (dotimes (i 40)
    (format s "(defmacro sig~a (format-control &rest format-arguments) ~
                 `(list :fc ,format-control ,@format-arguments))~%" i)))

(mp:make-thread
 (lambda ()
   (loop until *stop*
         do (let ((*package* (find-package :fake-sio)))
              (when (null *package*) (print :never)))))
 :name "binder")

(let ((leaked 0))
  (dotimes (iter 5)
    (compile-file "gen.lisp" :output-file "gen.fasl")
    (do-symbols (sym (find-package :fake-sio))
      (declare (ignorable sym))
      (incf leaked)))
  (setf *stop* t)
  (format t "MT-PKG-COMPILE-LEAKED:~a~%" leaked))
(load "gen.fasl")
(format t "MT-PKG-EXPAND:~a~%"
        (handler-case
            (if (equal (eval '(myppcre::sig0 "x" 1)) '(:fc "x" 1)) "OK" "WRONG")
          (error (e) (declare (ignorable e)) "ERROR")))
(quit)
LISPEOF

out=$(cd "$WORK" && "$CLAMIGA" --no-userinit --load "$WORK/mt-pkg-compile.lisp" </dev/null 2>&1)

total=$((total + 1))
case "$out" in
  *"MT-PKG-COMPILE-LEAKED:0"*)
    echo "  ok  compile_file_reader_unpoisoned_by_binder_thread"
    passed=$((passed + 1)) ;;
  *MT-PKG-COMPILE-LEAKED:*)
    echo "  FAIL  compile_file_reader_unpoisoned_by_binder_thread (symbols interned into FAKE-SIO)"
    echo "$out" | grep "MT-PKG-COMPILE-LEAKED"
    failed=$((failed + 1)) ;;
  *)
    echo "  FAIL  compile_file_reader_unpoisoned_by_binder_thread (run died)"
    echo "$out" | tail -5
    failed=$((failed + 1)) ;;
esac

total=$((total + 1))
case "$out" in
  *"MT-PKG-EXPAND:OK"*)
    echo "  ok  fasl_macro_expander_keeps_lambda_list_binding"
    passed=$((passed + 1)) ;;
  *)
    echo "  FAIL  fasl_macro_expander_keeps_lambda_list_binding"
    echo "$out" | grep "MT-PKG-EXPAND" || echo "$out" | tail -5
    failed=$((failed + 1)) ;;
esac

echo ""
echo "$passed passed, $failed failed, $total total"
[ "$failed" -eq 0 ]
