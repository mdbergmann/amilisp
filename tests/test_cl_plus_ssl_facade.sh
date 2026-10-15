#!/bin/sh
# The cl+ssl facade (lib/shims/cl+ssl/) must offer the API surface its
# consumers READ and call.  drakma and hunchentoot only need the stream
# constructors and contexts; websocket-driver's client additionally calls
# CL+SSL:ENSURE-INITIALIZED and (SETF CL+SSL:SSL-CHECK-VERIFY-P) before
# every wss:// connect.  A symbol the facade does not export is a READER
# error there, so the client's whole file failed to compile and the
# umbrella system (and CLOG on top of it) could not load (2026-09-06).
#
# No network: this checks that the exact forms the client contains read,
# that the entry points answer, and that the deprecated verify switch has
# the original's three-state semantics feeding MAKE-SSL-CLIENT-STREAM's
# :VERIFY default (never set -> the *...-VERIFY-DEFAULT* variable, set ->
# :OPTIONAL, cleared -> the variable again).
# Run: sh tests/test_cl_plus_ssl_facade.sh [path-to-amilisp]

AMILISP="${1:-build/host/amilisp}"
case "$AMILISP" in
    /*) ABS_AMILISP="$AMILISP" ;;
    *)  ABS_AMILISP="$(pwd)/$AMILISP" ;;
esac

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

passed=0
failed=0
total=0

check_contains() {
    desc="$1"
    needle="$2"
    haystack="$3"
    total=$((total + 1))
    case "$haystack" in
        *"$needle"*)
            echo "  ok  $desc"
            passed=$((passed + 1)) ;;
        *)
            echo "  FAIL  $desc"
            echo "    expected to contain: $needle"
            echo "    got: $(echo "$haystack" | tail -8)"
            failed=$((failed + 1)) ;;
    esac
}

# Separate --eval forms: asdf: symbols are only readable once the REQUIRE
# has been read AND evaluated, and cl+ssl: symbols once the system is loaded.
EVAL_REQUIRE='(require "asdf")'
EVAL_LOAD='(asdf:load-system "cl+ssl")'
EVAL_CHECKS='(progn
  ;; Reader leg: the forms websocket-driver/src/ws/client.lisp contains.
  ;; The symbols must be EXPORTED, or READ itself signals.
  (dolist (src (list "(cl+ssl:ensure-initialized)"
                     "(setf (cl+ssl:ssl-check-verify-p) t)"
                     "(cl+ssl:make-context :verify-mode cl+ssl:+ssl-verify-peer+ :verify-location :default)"
                     "(cl+ssl:make-context :verify-mode cl+ssl:+ssl-verify-none+)"
                     "(cl+ssl:with-global-context (ctx :auto-free-p t) (cl+ssl:make-ssl-client-stream s :hostname h :verify :optional))"))
    (format t "READS ~a~%"
            (handler-case (progn (read-from-string src) "ok")
              (error (e) (format nil "ERROR ~a" e)))))
  ;; Entry points.
  (format t "ENSURE-INITIALIZED ~a~%" (cl+ssl:ensure-initialized))
  (format t "ENSURE-INITIALIZED-KEYS ~a~%"
          (cl+ssl:ensure-initialized :method nil :rand-seed nil))
  ;; Three-state verify switch and the :VERIFY default it drives.
  (format t "VERIFY-UNSET ~a ~a~%"
          (cl+ssl:ssl-check-verify-p) (cl+ssl::%client-verify-default))
  (setf (cl+ssl:ssl-check-verify-p) t)
  (format t "VERIFY-SET ~a ~a~%"
          (cl+ssl:ssl-check-verify-p) (cl+ssl::%client-verify-default))
  (setf (cl+ssl:ssl-check-verify-p) nil)
  (format t "VERIFY-CLEARED ~a ~a~%"
          (cl+ssl:ssl-check-verify-p) (cl+ssl::%client-verify-default))
  (setf (cl+ssl:ssl-check-verify-p) :yes)
  (format t "VERIFY-GENERALIZED ~a~%" (cl+ssl:ssl-check-verify-p)))'

# Isolated FASL cache, as in test_shim_registry.sh.
CACHE="$WORKDIR/faslcache"

result=$(AMILISP_FASL_CACHE_DIR="$CACHE" "$ABS_AMILISP" --no-userinit \
    --non-interactive --heap 48M \
    --eval "$EVAL_REQUIRE" --eval "$EVAL_LOAD" --eval "$EVAL_CHECKS" \
    </dev/null 2>&1)

reads_ok=$(printf '%s\n' "$result" | grep -c '^READS ok$')
total=$((total + 1))
if [ "$reads_ok" -eq 5 ]; then
    echo "  ok  client_forms_read"
    passed=$((passed + 1))
else
    echo "  FAIL  client_forms_read (expected 5 forms to read, got $reads_ok)"
    echo "$result" | grep '^READS' | head -8
    failed=$((failed + 1))
fi
check_contains "ensure_initialized_returns_t"      "ENSURE-INITIALIZED T"      "$result"
check_contains "ensure_initialized_accepts_keys"   "ENSURE-INITIALIZED-KEYS T" "$result"
check_contains "verify_switch_unset_uses_default"  "VERIFY-UNSET NIL REQUIRED" "$result"
check_contains "verify_switch_set_means_optional"  "VERIFY-SET T OPTIONAL"     "$result"
check_contains "verify_switch_cleared_uses_default" "VERIFY-CLEARED NIL REQUIRED" "$result"
check_contains "verify_switch_generalized_boolean" "VERIFY-GENERALIZED T"      "$result"

# --- Summary ---

echo ""
echo "$passed passed, $failed failed, $total total"
if [ "$failed" -gt 0 ]; then
    echo "FAIL"
    exit 1
else
    echo "PASS"
    exit 0
fi
