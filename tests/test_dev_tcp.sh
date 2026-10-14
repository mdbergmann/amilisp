#!/bin/sh
# Executable specification for EXT.DEV.TCP -- the development port over TCP
# (lib/dev-tcp.lisp): the transport the host Clamacs talks to, and the one
# a Mac Clamacs drives an Amiga amilisp with over the LAN.
#
# The command layer behind it is tests/test_dev_commands.sh's business; what
# is checked here is the port: the frames, the AUTH gate and the three rules
# of "Who may connect" (clamacs/specs/clamacs-host.md) -- loopback unless
# :host names one address, a token always, drawn from the OS or given --
# and the REPL's way back over a `tcp:HOST:PORT/TOKEN' name.
#
# Also run by the gc-stress suite (`make test-gc-stress', AMILISP_GC_STRESS=1):
# there only the compact "allocating paths" section runs, since the others
# lean on wall-clock timeouts that a compaction on every allocation stretches.
#
# Run: sh tests/test_dev_tcp.sh build/host/amilisp

AMILISP="${1:-build/host/amilisp}"
passed=0
failed=0
total=0
TMPD=$(mktemp -d "${TMPDIR:-/tmp}/test_dev_tcp_XXXXXX") || exit 1
trap 'rm -rf "$TMPD"' EXIT INT TERM

check() {
    desc="$1"; pattern="$2"; out="$3"
    total=$((total + 1))
    if echo "$out" | grep -q "$pattern"; then
        echo "  ok  $desc"
        passed=$((passed + 1))
    else
        echo "  FAIL  $desc (missing: $pattern)"
        echo "    output: $(echo "$out" | head -12)"
        failed=$((failed + 1))
    fi
}

check_not() {
    desc="$1"; pattern="$2"; out="$3"
    total=$((total + 1))
    if echo "$out" | grep -q "$pattern"; then
        echo "  FAIL  $desc (unexpected: $pattern)"
        echo "    output: $(echo "$out" | head -12)"
        failed=$((failed + 1))
    else
        echo "  ok  $desc"
        passed=$((passed + 1))
    fi
}

finish() {
    echo ""
    echo "test_dev_tcp: $passed passed, $failed failed, $total total"
    [ "$failed" -eq 0 ] && exit 0
    exit 1
}

# A Lisp script in a fresh image (a file, not a heredoc inside $( ): the
# shell re-parses quotes there and an apostrophe breaks it).
run_script() {
    TEST_TMPD="$TMPD" TEST_LAN="$LAN_ADDR" "$AMILISP" --no-userinit --batch < "$1" 2>&1 | grep -v '^; Loading'
}

# The machine's non-loopback address, if it has one: the bind rule's leg.
LAN_ADDR=$(ifconfig 2>/dev/null | awk '/inet / && $2 != "127.0.0.1" {print $2; exit}')
[ -n "$LAN_ADDR" ] || LAN_ADDR=$(ip -4 addr 2>/dev/null | awk '/inet / && $2 !~ /^127/ {sub("/.*","",$2); print $2; exit}')

# --- fixture -----------------------------------------------------------------

cat > "$TMPD/two-errors.lisp" <<'EOF'
(defun tcp-ok-1 () 1)
(error "first bad form")
(defun tcp-ok-2 () 2)
(tcp-undefined-function)
EOF

# ===========================================================================
# The allocating paths, small: frame building, a server, a client, the REPL's
# way back over a tcp: name, and the two builtins the port stands on
# (EXT:SOCKET-LISTEN with an address string, EXT:EXECUTABLE-PATH).  Under
# AMILISP_GC_STRESS=1 every allocation compacts, so a CL_Obj a builtin holds
# across one, or a stale offset in the threads' hand-offs, shows up here.
# ===========================================================================

cat > "$TMPD/stress.lisp" <<'EOF'
(require "dev-tcp")
(defun show (label &rest values) (format t "~&<<~a~{ ~a~}>>~%" label values))
(defun reply (stream line)
  (multiple-value-bind (rc text) (ext.dev.tcp:request stream line)
    (if rc (format nil "rc=~d ~a" rc (substitute #\~ #\Newline text)) "GONE")))
(defvar *sent* '())
(defvar *sent-lock* (mp:make-lock))
(defun record (verb arg)
  (mp:with-lock-held (*sent-lock*) (push (concatenate 'string verb " " arg) *sent*))
  (values 0 ""))
(ext.dev:define-raw-command "OUTPUT" (arg) (record "OUTPUT" arg))
(ext.dev:define-raw-command "RESULT" (arg) (record "RESULT" arg))

;; The builtins: a named bind serves a connection; the binary's own path
;; names a file.
(let* ((l (ext:socket-listen 0 "127.0.0.1"))
       (c (ext:open-tcp-stream "127.0.0.1" (ext:socket-local-port l) 5))
       (s (ext:socket-accept l)))
  (write-char #\Q c) (force-output c)
  (show "LISTEN" (char-code (read-char s)))
  (close c) (close s) (close l))
(show "EXECUTABLE" (let ((p (ext:executable-path))) (and (stringp p) (probe-file p) t)))

;; A server and a client: frames both ways, a character above 127, an error
;; reply, a refused token.
(defvar *s* (ext.dev.tcp:start :port 0 :token "stress-token"))
(defvar *c* (ext.dev.tcp:connect "127.0.0.1" (ext.dev.tcp:port *s*) "stress-token"))
(show "CONNECT" (and *c* t))
(show "PING" (reply *c* "PING"))
(show "EVAL" (and (search "42" (reply *c* "EVAL (values (make-string 300 :initial-element (code-char 120)) 42)")) t))
(show "UTF8" (reply *c* (concatenate 'string "EVAL (length \"" (string (code-char 233)) "x\")")))
(show "UNKNOWN" (reply *c* "FLURB"))
(show "REFUSED" (multiple-value-list (ext.dev.tcp:connect "127.0.0.1" (ext.dev.tcp:port *s*) "wrong")))

;; The REPL's way back: a second server plays the editor.
(defvar *editor* (ext.dev.tcp:start :port 0 :token "editor-token"))
(defvar *name* (ext.dev.tcp:port-name "127.0.0.1" (ext.dev.tcp:port *editor*) "editor-token"))
(show "ATTACH" (reply *c* (concatenate 'string "REPL-ATTACH " *name*)))
(reply *c* "REPL-EVAL (+ 2 3)")
(loop repeat 3000
      until (mp:with-lock-held (*sent-lock*) (find-if (lambda (s) (search "RESULT" s)) *sent*))
      do (sleep 0.02))
(show "REPL" (mp:with-lock-held (*sent-lock*)
               (mapcar (lambda (s) (substitute #\~ #\Newline s)) (reverse *sent*))))
(show "SEND" (multiple-value-list (ext.dev.tcp:send *name* "PING")))
(reply *c* "REPL-DETACH")
(show "STOP" (ext.dev.tcp:stop *s*) (ext.dev.tcp:stop *editor*)
      (ext.dev.tcp:running-p *s*) (length ext.dev.tcp::*clients*))
EOF
stress_out=$(run_script "$TMPD/stress.lisp")

check "SOCKET-LISTEN on an address string serves a connection" '<<LISTEN 81>>' "$stress_out"
check "EXECUTABLE-PATH names a file that exists"   '<<EXECUTABLE T>>' "$stress_out"
check "a right token connects"                     '<<CONNECT T>>' "$stress_out"
check "PING"                                       '<<PING rc=0 PONG>>' "$stress_out"
check "EVAL answers a long value"                  '<<EVAL T>>' "$stress_out"
check "a character above 127 is one in <n>"        '<<UTF8 rc=0 2~' "$stress_out"
check "an unknown verb is rc 20"                   '<<UNKNOWN rc=20 ERROR: unknown command: FLURB' "$stress_out"
check "a wrong token is refused"                   '<<REFUSED (NIL authentication required)>>' "$stress_out"
check "REPL-ATTACH takes a tcp: name"              '<<ATTACH rc=0 CL-USER>>' "$stress_out"
check "the REPL's RESULT reaches the editor"       '<<REPL (RESULT 0 CL-USER~5)>>' "$stress_out"
check "SEND asks the editor's port directly"       '<<SEND (0 PONG)>>' "$stress_out"
check "both servers stop and the client connections are dropped" '<<STOP T T NIL 0>>' "$stress_out"
check_not "no corruption or error escaped the stress script" 'corrupted\|type 0\|BADMARK\|badmark\|SIGSEGV\|Unbound\|^ERROR:\|^Backtrace:' "$stress_out"

if [ "${AMILISP_GC_STRESS:-0}" = "1" ]; then
    echo "  skip  the timing-based sections (AMILISP_GC_STRESS=1: the allocating paths only)"
    finish
fi

# ===========================================================================
# The port: frames, the AUTH gate, the refusals, the limits, a stop over
# the port itself.  One image: the server's state is the point.
# ===========================================================================

cat > "$TMPD/port.lisp" <<'EOF'
(require "dev-tcp")
(defvar *probe* nil)
(defun show (label &rest values) (format t "~&<<~a~{ ~a~}>>~%" label values))
(defun reply (stream line)
  (multiple-value-bind (rc text) (ext.dev.tcp:request stream line)
    (if rc (format nil "rc=~d ~a" rc (substitute #\~ #\Newline text)) "GONE")))
(defun raw (port) (ext:open-tcp-stream "127.0.0.1" port 5))
(defun closed-after-p (stream)
  (setf (ext:socket-stream-timeout stream :input) 5)
  (handler-case (null (read-char stream nil nil)) (error () nil)))
(defun read-reply-list (stream)
  (multiple-value-bind (rc text) (ext.dev.tcp:read-reply stream) (list rc text)))

;; READ-REPLY: a header that is not a reply's is NIL whatever precedes the
;; offence (the digits already read must not start a body), so is end of
;; file inside the header, and so is a body that comes up short.
(flet ((rr (text) (multiple-value-list (ext.dev.tcp:read-reply (make-string-input-stream text)))))
  (show "READ-REPLY" (rr (format nil "0 2~%ab")) (rr (format nil "0 2x~%abcdef"))
        (rr (format nil "0 5 junk~%abcde")) (rr "0 0") (rr (format nil "0 2~%a"))))

;; A given token, an ephemeral port.
(defvar *s* (ext.dev.tcp:start :port 0 :token "test-token"))
(show "START" (integerp (ext.dev.tcp:port)) (> (ext.dev.tcp:port) 0)
      (ext.dev.tcp:running-p) (ext.dev.tcp:host) (ext.dev.tcp:token)
      (and (member 'ext.dev.tcp::stop-all ext:*exit-hooks*) t))
(defvar *p* (ext.dev.tcp:port))

;; The right token is served.
(defvar *c* (ext.dev.tcp:connect "127.0.0.1" *p* "test-token"))
(show "CONNECT" (and *c* t))
(show "PING" (reply *c* "PING"))
(show "EVAL" (reply *c* "EVAL (+ 1 2)"))
(show "FORM" (reply *c* "(list 1 2 3)"))
(show "LOAD" (reply *c* (concatenate 'string "LOAD " (ext:getenv "TEST_TMPD") "/two-errors.lisp")))
(show "LASTRESULT" (reply *c* "LASTRESULT"))
(show "UNKNOWN" (reply *c* "FLURB"))
(show "EMPTY" (reply *c* ""))
;; A second connection at the same time.
(defvar *c2* (ext.dev.tcp:connect "127.0.0.1" *p* "test-token"))
(show "SECOND" (reply *c2* "PING") (reply *c* "PING"))
(close *c2*)
;; <n> counts characters, the bytes are UTF-8: e-acute is one character in
;; the header and two bytes on the wire, in both directions.
(let ((e-acute (string (code-char 233))))
  (ext.dev.tcp:write-request *c* (concatenate 'string "EVAL (length \"" e-acute "x\")"))
  (show "UTF8-IN" (read-reply-list *c*))
  (ext.dev.tcp:write-request *c* (concatenate 'string "EVAL (values \"" e-acute "\")"))
  (multiple-value-bind (rc text) (ext.dev.tcp:read-reply *c*)
    (show "UTF8-OUT" rc (and (search e-acute text) t) (position #\Newline text))))
(show "SERVED" (ext.dev.tcp::server-served *s*))

;; Refused: a connection that skips AUTH.  Its EVAL has a visible side
;; effect, which must not happen.
(let ((r (raw *p*)))
  (ext.dev.tcp:write-request r "EVAL (setq cl-user::*probe* :ran)")
  (show "SKIP-AUTH" (read-reply-list r) (closed-after-p r))
  (close r))
(sleep 0.2)
(show "SKIP-AUTH-PROBE" *probe*)
;; Refused: wrong tokens, the verb alone, an empty frame.
(dolist (bad (list "AUTH wrong-token" "AUTH test-token0" "AUTH est-token" "AUTH " "AUTH" ""))
  (let ((r (raw *p*)))
    (ext.dev.tcp:write-request r bad)
    (show "WRONG" (read-reply-list r) (closed-after-p r))
    (close r)))
;; The verb is case-insensitive and the token may carry a newline.
(let ((r (raw *p*)))
  (ext.dev.tcp:write-request r (format nil "auth test-token~%"))
  (show "AUTH-LOWER" (read-reply-list r) (reply r "PING"))
  (close r))
;; Refused: a first frame above the AUTH limit (on the header alone), and
;; one that is not a frame.
(let ((r (raw *p*)))
  (format r "~D~%" 2000) (finish-output r)
  (show "OVERSIZED" (read-reply-list r) (closed-after-p r))
  (close r))
(let ((r (raw *p*)))
  (write-string (format nil "EVAL (setq cl-user::*probe* :ran)~%") r) (finish-output r)
  (show "NOT-A-FRAME" (read-reply-list r) (closed-after-p r))
  (close r))
(sleep 0.2)
(show "REFUSED-PROBE" *probe* (ext.dev.tcp::server-served *s*))
;; The port is still up for a right token afterwards.
(show "STILL-UP" (reply *c* "PING"))
(close *c*)

;; A header whose digits arrive further apart than the idle timeout is still
;; one header: the timeout is how a connection thread notices a stop, not a
;; deadline on the client, and what was read before it must not be lost.
(let ((c (ext.dev.tcp:connect "127.0.0.1" *p* "test-token")))
  (write-string "1" c) (finish-output c)
  (sleep (+ ext.dev.tcp::*idle-seconds* 1.5))
  (write-string (format nil "0~%EVAL (+ 1)") c) (finish-output c)
  (multiple-value-bind (rc text) (ext.dev.tcp:read-reply c)
    (show "SPLIT-HEADER" rc (and text (plusp (length text)) (char= (char text 0) #\1))
          (reply c "PING")))
  (close c))

;; A silent connection is refused after the AUTH timeout, on its own.
(let* ((ext.dev.tcp:*auth-seconds* 1)
       (quick (ext.dev.tcp:start :port 0 :token "quick"))
       (r (raw (ext.dev.tcp:port quick)))
       (start (get-internal-real-time)))
  (show "SILENT" (read-reply-list r)
        (>= (- (get-internal-real-time) start) (* 0.9 internal-time-units-per-second))
        (closed-after-p r))
  (close r)
  (ext.dev.tcp:stop quick))

;; The caps: a connection beyond *MAX-UNAUTHENTICATED* still waiting for its
;; AUTH, or beyond *MAX-CONNECTIONS* in all, is closed unanswered, and the
;; slots come back as connections end.
(let* ((ext.dev.tcp:*max-connections* 3)
       (ext.dev.tcp:*max-unauthenticated* 2)
       (capped (ext.dev.tcp:start :port 0 :token "capped"))
       (p (ext.dev.tcp:port capped))
       (s1 (raw p))
       (s2 (raw p))
       (s3 (raw p)))
  (show "PENDING-CAP" (closed-after-p s3)
        (null (ext.dev.tcp:connect "127.0.0.1" p "capped"))
        (ext.dev.tcp::server-pending capped))
  (close s1) (close s2) (close s3)
  (let ((n 0))
    (loop while (and (ext.dev.tcp::server-connections capped) (< n 100)) do (sleep 0.05) (incf n)))
  (let ((c1 (ext.dev.tcp:connect "127.0.0.1" p "capped"))
        (c2 (ext.dev.tcp:connect "127.0.0.1" p "capped"))
        (c3 (ext.dev.tcp:connect "127.0.0.1" p "capped")))
    (show "CONNECTION-CAP" (and c1 c2 c3 t)
          (null (ext.dev.tcp:connect "127.0.0.1" p "capped"))
          (ext.dev.tcp::server-pending capped))
    (dolist (c (list c1 c2 c3))
      (when c (close c))))
  (ext.dev.tcp:stop capped))

;; After AUTH a too-long command is an error reply, its body dropped, and
;; the connection stays; a body that never comes ends it.
(let* ((ext.dev.tcp:*frame-limit* 64)
       (small (ext.dev.tcp:start :port 0 :token "small"))
       (c (ext.dev.tcp:connect "127.0.0.1" (ext.dev.tcp:port small) "small")))
  (ext.dev.tcp:write-request c (format nil "5~%hello~A" (make-string 100 :initial-element #\x)))
  (show "TOO-LONG" (read-reply-list c) (reply c "PING"))
  (ext.dev.tcp:write-request c (make-string 10000 :initial-element #\y))
  (show "TOO-LONG-CHUNKS" (read-reply-list c) (reply c "PING") (ext.dev.tcp::server-served small))
  (close c)
  (let ((c2 (ext.dev.tcp:connect "127.0.0.1" (ext.dev.tcp:port small) "small")))
    (format c2 "~D~%abc" 100) (finish-output c2) (close c2))
  (let ((n 0))
    (loop while (and (ext.dev.tcp::server-connections small) (< n 100)) do (sleep 0.05) (incf n))
    (show "SHORT-BODY" (null (ext.dev.tcp::server-connections small))))
  (ext.dev.tcp:stop small))

;; STOP: the listener refuses, the open connections are closed, *SERVER* is
;; back to the one still running.
(let ((c (ext.dev.tcp:connect "127.0.0.1" *p* "test-token")))
  (show "STOP" (ext.dev.tcp:stop *s*) (ext.dev.tcp:running-p *s*) ext.dev.tcp:*server*
        (closed-after-p c)
        (handler-case (progn (close (ext:open-tcp-stream "127.0.0.1" *p* 1)) :accepted)
          (error () :refused)))
  (close c))
(show "STOP-TWICE" (ext.dev.tcp:stop *s*))

;; EVAL (ext.dev.tcp:stop) over the port itself: the reply still arrives,
;; and WAIT (what a --load preamble ends with) returns.
(let* ((s (ext.dev.tcp:start :port 0 :token "self-stop"))
       (c (ext.dev.tcp:connect "127.0.0.1" (ext.dev.tcp:port s) "self-stop"))
       (waiter (mp:make-thread (lambda () (ext.dev.tcp:wait s) :waited))))
  (show "SELF-STOP" (reply c "EVAL (ext.dev.tcp:stop)"))
  (show "WAIT" (mp:join-thread waiter) (ext.dev.tcp:running-p s) (closed-after-p c))
  (close c))
EOF
port_out=$(run_script "$TMPD/port.lisp")

check "START answers a port, running, on 127.0.0.1 with the token, and registers the exit hook" \
      '<<START T T T 127.0.0.1 test-token T>>' "$port_out"
check "a right token is served"                    '<<CONNECT T>>' "$port_out"
check "PING"                                       '<<PING rc=0 PONG>>' "$port_out"
check "EVAL answers the value"                     '<<EVAL rc=0 3~' "$port_out"
check "a bare form is evaluated"                   '<<FORM rc=0 (1 2 3)~' "$port_out"
check "LOAD answers rc 10 and every diagnostic"    '<<LOAD rc=10 .*two-errors.lisp:2: ERROR: first bad form~.*two-errors.lisp:4: ERROR: Undefined function: TCP-UNDEFINED-FUNCTION~2 error(s), 0 warning(s)' "$port_out"
check "LASTRESULT replays it"                      '<<LASTRESULT rc=0 .*2 error(s), 0 warning(s)' "$port_out"
check "an unknown verb is rc 20"                   '<<UNKNOWN rc=20 ERROR: unknown command: FLURB' "$port_out"
check "an empty command is fine"                   '<<EMPTY rc=0 >>' "$port_out"
check "two connections are served at once"         '<<SECOND rc=0 PONG rc=0 PONG>>' "$port_out"
check "<n> counts characters: e-acute plus x is 2" '<<UTF8-IN (0 2' "$port_out"
check "a character above 127 comes back whole"     '<<UTF8-OUT 0 T ' "$port_out"
check "the frames were counted"                    '<<SERVED 1[0-9]>>' "$port_out"
check "a connection that skips AUTH is refused and closed" '<<SKIP-AUTH (20 authentication required) T>>' "$port_out"
check "and its command did not run"                '<<SKIP-AUTH-PROBE NIL>>' "$port_out"
n_wrong=$(echo "$port_out" | grep -c '<<WRONG (20 authentication required) T>>')
check "every wrong token is refused and closed (6)" '^6$' "$n_wrong"
check "AUTH is case-insensitive and tolerates a newline" '<<AUTH-LOWER (0 OK) rc=0 PONG>>' "$port_out"
check "an oversized first frame is refused on its header" '<<OVERSIZED (20 authentication required) T>>' "$port_out"
check "a first line that is no frame is refused"   '<<NOT-A-FRAME (20 authentication required) T>>' "$port_out"
check "nothing of a refused connection ran or was counted" '<<REFUSED-PROBE NIL 1[0-9]>>' "$port_out"
check "the port is still up afterwards"            '<<STILL-UP rc=0 PONG>>' "$port_out"
check "READ-REPLY: a header that is no reply's, or a short frame, is NIL" '<<READ-REPLY (0 ab) (NIL) (NIL) (NIL) (NIL)>>' "$port_out"
check "a header split by more than the idle timeout is still one header" '<<SPLIT-HEADER 0 T rc=0 PONG>>' "$port_out"
check "a silent connection is refused after the AUTH timeout" '<<SILENT (20 authentication required) T T>>' "$port_out"
check "beyond the unauthenticated cap a connection is closed unanswered" '<<PENDING-CAP T T 2>>' "$port_out"
check "beyond the connection cap too, once the slots are back, and none stays counted" '<<CONNECTION-CAP T T 0>>' "$port_out"
check "after AUTH a too-long command is an error, the connection stays" '<<TOO-LONG (20 ERROR: the command is too long) rc=0 PONG>>' "$port_out"
check "a body of many chunks is dropped whole"     '<<TOO-LONG-CHUNKS (20 ERROR: the command is too long) rc=0 PONG 2>>' "$port_out"
check "a body that never comes ends the connection" '<<SHORT-BODY T>>' "$port_out"
check "STOP closes the connections and the listener" '<<STOP T NIL NIL T REFUSED>>' "$port_out"
check "STOP twice is a no-op"                      '<<STOP-TWICE NIL>>' "$port_out"
check "EVAL (ext.dev.tcp:stop) over the port still answers" '<<SELF-STOP rc=0 T' "$port_out"
check "and WAIT returns with the server down and the connection closed" '<<WAIT WAITED NIL T>>' "$port_out"
# A LOAD reply carries the file's own log, backtraces included, on its
# marker line; an error of the script's own starts a line.
check_not "no error escaped the port script"       '^ERROR:\|^Backtrace:' "$port_out"

# ===========================================================================
# Who may connect: the bind, the token's source, the wildcards.
# ===========================================================================

cat > "$TMPD/rules.lisp" <<'EOF'
(require "dev-tcp")
(defun show (label &rest values) (format t "~&<<~a~{ ~a~}>>~%" label values))
(defun refused (form-thunk)
  (handler-case (progn (funcall form-thunk) :listened)
    (error (e) (princ-to-string e))))
;; A wildcard is refused, whatever the spelling; so is a bad type.
(show "WILDCARD" (refused (lambda () (ext.dev.tcp:start :port 0 :host "0.0.0.0" :token "t"))))
(show "WILDCARD6" (refused (lambda () (ext.dev.tcp:start :port 0 :host "::" :token "t"))))
(show "WILDCARD*" (refused (lambda () (ext.dev.tcp:start :port 0 :host "*" :token "t"))))
(show "EMPTY-HOST" (refused (lambda () (ext.dev.tcp:start :port 0 :host "" :token "t"))))
;; The platform layer's parser takes leading zeros, so an all-zero address
;; is INADDR_ANY in any spelling.
(dolist (zeros '("00.0.0.0" "0.00.0.0" "0.0.0.000" "000.000.000.000" "0:0:0:0:0:0:0:0"))
  (show "WILDCARD-ZEROS" zeros (refused (lambda () (ext.dev.tcp:start :port 0 :host zeros :token "t")))))
(show "BAD-HOST" (refused (lambda () (ext.dev.tcp:start :port 0 :host :any :token "t"))))
(show "NO-SERVER" ext.dev.tcp:*server*)
;; :host spelled out as loopback works; an address no interface has does not.
(let ((s (ext.dev.tcp:start :port 0 :host "127.0.0.1" :token "t")))
  (show "NAMED-LOOPBACK" (ext.dev.tcp:host s)
        (let ((c (ext.dev.tcp:connect "127.0.0.1" (ext.dev.tcp:port s) "t")))
          (prog1 (and c t) (when c (close c)))))
  (ext.dev.tcp:stop s))
(show "NO-SUCH-ADDRESS" (refused (lambda () (ext.dev.tcp:start :port 0 :host "203.0.113.1" :token "t"))))
;; A bad token argument.
(show "EMPTY-TOKEN" (refused (lambda () (ext.dev.tcp:start :port 0 :token ""))))
(show "BAD-PORT" (refused (lambda () (ext.dev.tcp:start :port 70000 :token "t"))))
;; No :token and no entropy source: the port does not listen.
(let ((ext.dev.tcp:*entropy-source* "/no/such/device"))
  (show "NO-ENTROPY" (refused (lambda () (ext.dev.tcp:start :port 0))) ext.dev.tcp:*server*))
;; A drawn token: 32 hex characters, printed once, and it works.  A platform
;; with no OS entropy source (Windows) has nothing to draw from: the leg
;; skips itself there, as the LAN leg does without a second address.
(if (null (ext.dev.tcp:random-hex))
    (show "DRAWN" :skipped)
    (let ((s (ext.dev.tcp:start :port 0)))
      (let ((tok (ext.dev.tcp:token s)))
        (show "DRAWN" (length tok) (every (lambda (c) (find c "0123456789abcdef")) tok)
              (let ((c (ext.dev.tcp:connect "127.0.0.1" (ext.dev.tcp:port s) tok)))
                (prog1 (and c t) (when c (close c)))))
        (show "DRAWN-TOKEN" tok))
      (ext.dev.tcp:stop s)))
;; The default bind is loopback: a connection to the machine's own
;; non-loopback address is refused (the script skips this leg without one).
(let ((s (ext.dev.tcp:start :port 0 :token "t"))
      (lan (or (ext:getenv "TEST_LAN") "")))
  (show "LAN" (if (string= lan "")
                  :skipped
                  (handler-case (progn (close (ext:open-tcp-stream lan (ext.dev.tcp:port s) 2)) :accepted)
                    (error () :refused))))
  (ext.dev.tcp:stop s))
EOF
rules_out=$(run_script "$TMPD/rules.lisp")

check ":host 0.0.0.0 is refused as a wildcard"    '<<WILDCARD .*wildcard' "$rules_out"
check ":host :: is refused as a wildcard"         '<<WILDCARD6 .*wildcard' "$rules_out"
check ":host * is refused as a wildcard"          '<<WILDCARD\* .*wildcard' "$rules_out"
check "an empty :host is refused"                 '<<EMPTY-HOST .*wildcard' "$rules_out"
n_zeros=$(echo "$rules_out" | grep -c '^<<WILDCARD-ZEROS [0-9.:]* .*wildcard')
check "an all-zero address is refused however its zeros are spelled (5)" '^5$' "$n_zeros"
check "a non-string :host is refused"             '<<BAD-HOST .*dotted-quad' "$rules_out"
check "no server was left behind by a refusal"    '<<NO-SERVER NIL>>' "$rules_out"
check ":host 127.0.0.1 spelled out listens there" '<<NAMED-LOOPBACK 127.0.0.1 T>>' "$rules_out"
check "an address no interface has is refused, named" '<<NO-SUCH-ADDRESS .*cannot listen on 203.0.113.1' "$rules_out"
check "an empty :token is refused"                '<<EMPTY-TOKEN .*non-empty' "$rules_out"
check "a port out of range is refused"            '<<BAD-PORT .*0-65535' "$rules_out"
check "no :token and no entropy source: no port"  '<<NO-ENTROPY .*no entropy source.*pass :token.*NIL>>' "$rules_out"
n_printed=$(echo "$rules_out" | grep -c '^; EXT.DEV.TCP: listening on')
if echo "$rules_out" | grep -q '^<<DRAWN SKIPPED>>'; then
    # No OS entropy source (native Windows): nothing can be drawn, and
    # START says so instead of listening -- covered by NO-ENTROPY above.
    echo "  note  no OS entropy source on this platform: the drawn-token legs are skipped"
    check "and a given token is never printed"    '^0$' "$n_printed"
else
    check "a drawn token is 32 hex characters and works" '<<DRAWN 32 T T>>' "$rules_out"
    drawn=$(echo "$rules_out" | sed -n 's/^<<DRAWN-TOKEN \(.*\)>>$/\1/p')
    check "the drawn token was printed once to standard output" "^; EXT.DEV.TCP: listening on 127.0.0.1:[0-9]*, token $drawn\$" "$rules_out"
    check "and a given token is never printed"    '^1$' "$n_printed"
fi
if [ -n "$LAN_ADDR" ]; then
    check "the default bind refuses the machine's non-loopback address ($LAN_ADDR)" '<<LAN REFUSED>>' "$rules_out"
else
    echo "  note  no non-loopback address on this machine: the LAN leg is skipped"
fi

# ===========================================================================
# The REPL's way back: a second server in the same image plays the editor
# (its OUTPUT / READLINE / RESULT / DEBUGGER are raw verbs that record what
# arrived), and the REPL attaches with a `tcp:HOST:PORT/TOKEN' name.
# ===========================================================================

cat > "$TMPD/repl.lisp" <<'EOF'
(require "dev-tcp")
(defun show (label &rest values) (format t "~&<<~a~{ ~a~}>>~%" label values))
(defun reply (stream line)
  (multiple-value-bind (rc text) (ext.dev.tcp:request stream line)
    (if rc (format nil "rc=~d ~a" rc (substitute #\~ #\Newline text)) "GONE")))
(defvar *sent* '())
(defvar *sent-lock* (mp:make-lock))
(defun record (verb arg)
  (mp:with-lock-held (*sent-lock*) (push (concatenate 'string verb " " arg) *sent*))
  (when (string= verb "READLINE")
    ;; The editor answers a READLINE with a command of its own, on the
    ;; wire to amilisp, while the REPL thread waits for this reply.
    (ext.dev:handle-command "REPL-INPUT typed line"))
  (values 0 ""))
(ext.dev:define-raw-command "OUTPUT" (arg) (record "OUTPUT" arg))
(ext.dev:define-raw-command "READLINE" (arg) (record "READLINE" arg))
(ext.dev:define-raw-command "RESULT" (arg) (record "RESULT" arg))
(ext.dev:define-raw-command "DEBUGGER" (arg) (record "DEBUGGER" arg))
(defun prefix-p (prefix s) (and (>= (length s) (length prefix)) (string= prefix s :end2 (length prefix))))
(defun wait-for (prefix)
  (loop repeat 500
        do (when (mp:with-lock-held (*sent-lock*) (find-if (lambda (s) (prefix-p prefix s)) *sent*))
             (return t))
           (sleep 0.02)))
(defun sent ()
  (mp:with-lock-held (*sent-lock*)
    (prog1 (mapcar (lambda (s) (substitute #\~ #\Newline s)) (reverse *sent*))
      (setf *sent* '()))))
(defun thread-alive ()
  (and ext.dev::*repl-thread* (mp:thread-alive-p ext.dev::*repl-thread*) t))

;; A: the amilisp; B: the editor.
(defvar *a* (ext.dev.tcp:start :port 0 :token "a-token"))
(defvar *b* (ext.dev.tcp:start :port 0 :token "b-token"))
(defvar *editor-name* (ext.dev.tcp:port-name "127.0.0.1" (ext.dev.tcp:port *b*) "b-token"))
(show "NAME" (multiple-value-list (ext.dev.tcp:parse-port-name *editor-name*)))
(show "NAME-BAD" (ext.dev.tcp:parse-port-name "tcp:127.0.0.1/tok") (ext.dev.tcp:parse-port-name "CLAMACS")
      (ext.dev.tcp:parse-port-name "tcp:127.0.0.1:99999/tok") (ext.dev.tcp:parse-port-name "tcp:h:1/"))
(defvar *c* (ext.dev.tcp:connect "127.0.0.1" (ext.dev.tcp:port *a*) "a-token"))
(show "ATTACH" (reply *c* (concatenate 'string "REPL-ATTACH " *editor-name* " DEBUG")))
(show "EVAL" (reply *c* "REPL-EVAL (progn (princ \"hello\") (terpri) (princ \"there\") (+ 2 3))"))
(wait-for "RESULT")
(show "SENT" (sent))
;; The connection to the editor is kept: one client, still open.
(show "CLIENT" (length ext.dev.tcp::*clients*) (ext.dev.tcp::server-connections *b*) (length (ext.dev.tcp::server-connections *b*)))
(reply *c* "REPL-EVAL (read-line)")
(wait-for "RESULT")
(show "READLINE" (sent))
(reply *c* "REPL-EVAL (error \"boom\")")
(wait-for "DEBUGGER 1")
(show "DEBUGGER" (first (sent)))
(show "ABORT" (reply *c* "ABORT"))
(wait-for "RESULT")
(sent)
;; The editor goes away: B stops; the next send fails and the REPL stops.
(ext.dev.tcp:stop *b*)
(sleep 0.3)
(show "EVAL-GONE" (reply *c* "REPL-EVAL (+ 1 1)"))
(loop repeat 250 while (thread-alive) do (sleep 0.02))
(show "THREAD-AFTER-GONE" (thread-alive) (length ext.dev.tcp::*clients*))
;; A wrong token in the attach name: the first send is refused, the REPL stops.
(defvar *b2* (ext.dev.tcp:start :port 0 :token "b2-token"))
(show "ATTACH-WRONG" (reply *c* (concatenate 'string "REPL-ATTACH " (ext.dev.tcp:port-name "127.0.0.1" (ext.dev.tcp:port *b2*) "wrong"))))
(reply *c* "REPL-EVAL (+ 1 1)")
(loop repeat 250 while (thread-alive) do (sleep 0.02))
(show "THREAD-AFTER-WRONG" (thread-alive) (sent) (ext.dev.tcp::server-served *b2*))
;; A name of neither kind, with no other sender installed.
(show "ATTACH-NONE" (reply *c* "REPL-ATTACH CLAMACS"))
(reply *c* "REPL-EVAL (+ 1 1)")
(loop repeat 250 while (thread-alive) do (sleep 0.02))
(show "THREAD-AFTER-NONE" (thread-alive))
;; And the right token again: it all works, and a later A stop closes the
;; connection to the editor.
(show "ATTACH-AGAIN" (reply *c* (concatenate 'string "REPL-ATTACH " (ext.dev.tcp:port-name "127.0.0.1" (ext.dev.tcp:port *b2*) "b2-token"))))
(reply *c* "REPL-EVAL (* 6 7)")
(wait-for "RESULT")
(show "AGAIN" (sent) (length ext.dev.tcp::*clients*))
(reply *c* "REPL-DETACH")
(close *c*)
(ext.dev.tcp:stop *a*)
(show "END" (length ext.dev.tcp::*clients*) (thread-alive))
(ext.dev.tcp:stop *b2*)
EOF
repl_out=$(run_script "$TMPD/repl.lisp")

check "a port name parses into host, port and token" '<<NAME (127.0.0.1 [0-9]* b-token)>>' "$repl_out"
check "and the malformed ones do not"            '<<NAME-BAD NIL NIL NIL NIL>>' "$repl_out"
check "REPL-ATTACH takes a tcp: name"            '<<ATTACH rc=0 CL-USER>>' "$repl_out"
check "REPL-EVAL answers at once"                '<<EVAL rc=0 >>' "$repl_out"
check "OUTPUT reaches the editor's port line by line, the last flushed before RESULT" '<<SENT (OUTPUT hello~ OUTPUT there RESULT 0 CL-USER~5)>>' "$repl_out"
check "the connection to the editor is one, and kept" '<<CLIENT 1 (#<.*) 1>>' "$repl_out"
check "READLINE is answered through REPL-INPUT"  '<<READLINE (READLINE  RESULT 0 CL-USER~"typed line"~NIL)>>' "$repl_out"
check "DEBUGGER announces a level"               '<<DEBUGGER DEBUGGER 1 CL-USER~SIMPLE-ERROR: boom' "$repl_out"
check "ABORT from the port"                      '<<ABORT rc=0' "$repl_out"
check "an editor that went stops the REPL"       '<<EVAL-GONE rc=0 >>' "$repl_out"
check "and its connection is dropped"            '<<THREAD-AFTER-GONE NIL 0>>' "$repl_out"
check "a wrong token in the attach name is accepted by REPL-ATTACH" '<<ATTACH-WRONG rc=0 CL-USER>>' "$repl_out"
check "but the first send is refused: the REPL stops and nothing was served" '<<THREAD-AFTER-WRONG NIL NIL 0>>' "$repl_out"
check "a name of no transport stops the REPL too" '<<THREAD-AFTER-NONE NIL>>' "$repl_out"
check "the right token works again"              '<<AGAIN (RESULT 0 CL-USER~42) 1>>' "$repl_out"
check "STOP closes the connection to the editor" '<<END 0 NIL>>' "$repl_out"
check_not "no token appears in an error message" 'b2-token\|wrong' "$(echo "$repl_out" | grep -i 'EXT.DEV.TCP:')"

finish
