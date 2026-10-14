;;; dev-tcp.lisp -- the development port over TCP (every platform)
;;;
;;; Loaded via (require "dev-tcp").
;;;
;;;   (ext.dev.tcp:start :port 4005 :token "...")   ; => a server
;;;
;;; The TCP twin of lib/amiga/arexx.lisp: a listener that hands each command
;;; string to EXT.DEV:HANDLE-COMMAND and ships the reply back -- what the
;;; host Clamacs talks to (specs/clamacs-host.md in the clamacs submodule,
;;; "The wire"), and, with :HOST, what a Mac Clamacs driving an Amiga amilisp
;;; over the LAN talks to.  Every decision about what a command MEANS stays
;;; in lib/dev-commands.lisp; this file is a transport.
;;;
;;; The protocol, both directions.  <n> counts CHARACTERS and the bytes on
;;; the wire are UTF-8 (the socket streams encode every character above 127
;;; and decode on the way in):
;;;
;;;   request:  "<n>\n" then n characters: the command line  (VERB argument)
;;;   reply:    "<rc> <n>\n" then n characters: the text
;;;
;;; No quoting, no escaping, newlines inside the text are the text's; RC is
;;; the ARexx severity ladder of EXT.DEV.  A closed connection is the port
;;; being gone.
;;;
;;; Who may connect (the three rules, shared with the editor's own port):
;;;
;;;   - BIND: 127.0.0.1.  The only way to another address is the :HOST
;;;     argument, spelled out by whoever starts the port -- one dotted-quad
;;;     address of this machine, never a wildcard.  The token authenticates,
;;;     it does not encrypt: a non-loopback bind is for a trusted network.
;;;   - TOKEN: the first request on every connection must be `AUTH <token>'.
;;;     Anything else -- a wrong token, another verb first, no frame within
;;;     *AUTH-SECONDS*, a length above +AUTH-LIMIT+ -- is answered
;;;     `20 <n>\nauthentication required' (nothing of it is run, echoed or
;;;     logged) and the connection is closed.  AUTH is not a verb of EXT.DEV:
;;;     the listener owns it, so no command can be reached around it.  The
;;;     token is compared in constant time.
;;;   - WHERE IT COMES FROM: 128 bits from the OS entropy source
;;;     (/dev/urandom), hex, per START -- printed once to this amilisp's
;;;     standard output, since whoever started it has to hand it to the
;;;     editor -- or the :TOKEN argument for a amilisp where there is no such
;;;     source (an Amiga, Windows: the user picks it) and for one started by
;;;     an editor that drew the token itself and put it in the child's
;;;     environment.  START refuses to listen without one or the other.
;;;
;;; After AUTH a command longer than *FRAME-LIMIT* is answered `20 <n>\nERROR:
;;; the command is too long', its body is read and dropped, and the
;;; connection stays: leaving the body on the wire would have it read as the
;;; next header.
;;;
;;; A connection costs a thread with a stack of its own, so a port open to
;;; the LAN takes at most *MAX-CONNECTIONS* at once and at most
;;; *MAX-UNAUTHENTICATED* of those still waiting to AUTH; one beyond either
;;; is closed unanswered, and silent connections cannot use up an Amiga's
;;; memory before *AUTH-SECONDS* runs out.
;;;
;;; The REPL's way back (lib/dev-repl.lisp): the editor attaches with
;;; `REPL-ATTACH tcp:HOST:PORT/TOKEN', naming ITS port and ITS token -- handed
;;; over on the already authenticated connection, so it is on no command line
;;; and in no file this amilisp reads -- and EXT.DEV:*REPL-SEND* routes a port
;;; name of that form to a connection SEND opens to the editor (AUTH first),
;;; kept open across the REPL thread's OUTPUT / READLINE / RESULT / DEBUGGER
;;; traffic.  Any other port name goes on to the sender this file found
;;; installed (AMIGA.AREXX's, when it loaded first; it passes `tcp:' names
;;; back the same way, so the load order does not matter).
;;;
;;; Concurrency: each connection is served on a thread of its own, so a
;;; second editor (or the REPL leg) is answered while a LOAD runs on the
;;; first; the commands share EXT.DEV's session state (the command package,
;;; LASTRESULT) as the ARexx port's do.
;;;
;;; See tests/test_dev_tcp.sh (host) and tests/amiga/dev-tcp-tests.lisp (the
;;; same server over loopback in the emulator).

(eval-when (:compile-toplevel :load-toplevel :execute)
  (require "dev-commands"))

(defpackage "EXT.DEV.TCP"
  (:use "CL")
  (:export
   ;; The server
   "START" "STOP" "RUNNING-P" "PORT" "HOST" "TOKEN" "WAIT" "*SERVER*"
   "*DEFAULT-PORT*" "*AUTH-SECONDS*" "*FRAME-LIMIT*" "+AUTH-LIMIT+"
   "*MAX-CONNECTIONS*" "*MAX-UNAUTHENTICATED*"
   ;; The token
   "RANDOM-HEX" "*ENTROPY-SOURCE*" "TOKEN-EQUAL-P"
   ;; The client side: an authenticated connection, one request on it, and
   ;; the frames themselves (an editor's port speaks the same protocol)
   "CONNECT" "REQUEST" "SEND" "DISCONNECT"
   "WRITE-REQUEST" "READ-REPLY" "WRITE-FRAME" "READ-FRAME" "WRITE-WIRE-TEXT"
   "PORT-NAME" "PARSE-PORT-NAME"))

(in-package "EXT.DEV.TCP")

(defvar *default-port* 4005
  "The port START listens on when none is given: what an editor's
`Connect' tries first.")

(defvar *auth-seconds* 5
  "How long a fresh connection has to send its AUTH.")

(defconstant +auth-limit+ 1024
  "The longest first frame: an AUTH needs 37 characters.")

(defvar *frame-limit* (* 16 1024 1024)
  "The longest command after AUTH.")

(defvar *max-connections* 8
  "The most connections a server serves at once; a connection above it is
closed unanswered.")

(defvar *max-unauthenticated* 3
  "The most of those that may still be waiting for their AUTH: fewer than
*MAX-CONNECTIONS*, so that a handful of silent connections cannot fill the
port.")

(defvar *idle-seconds* 1
  "The read timeout between frames: how soon a connection thread notices
a stop.")

(defvar *body-seconds* 30
  "How long a frame's body may take once its header is in.")

(defvar *thread-stack-size* (* 256 1024)
  "C stack of a connection thread: commands compile code, see
AMIGA.AREXX:START.")

(defvar *thread-vm-frames* 512)

;;; ----------------------------------------------------------------
;;; The token
;;; ----------------------------------------------------------------

(defvar *entropy-source* "/dev/urandom"
  "Where a token's bits come from: the OS's, never Lisp's RANDOM.  A
platform without it (AmigaOS, Windows) passes :TOKEN to START.")

(defun random-hex (&optional (bytes 16))
  "BYTES random bytes from the OS entropy source as lower-case hex, or NIL
when there is no such source."
  (handler-case
      (with-open-file (in *entropy-source* :element-type '(unsigned-byte 8))
        (let ((out (make-string (* 2 bytes)))
              (digits "0123456789abcdef"))
          (dotimes (i bytes out)
            (let ((b (read-byte in)))
              (setf (char out (* 2 i)) (char digits (ash b -4))
                    (char out (1+ (* 2 i))) (char digits (logand b 15)))))))
    (error () nil)))

(defun token-equal-p (a b)
  "Whether the two strings are the same, in time that depends on their
length and not on where they first differ."
  (let ((diff (logxor (length a) (length b)))
        (n (min (length a) (length b))))
    (declare (fixnum diff n))
    (dotimes (i n)
      (setq diff (logior diff (logxor (char-code (char a i)) (char-code (char b i))))))
    (zerop diff)))

;;; ----------------------------------------------------------------
;;; Frames
;;; ----------------------------------------------------------------

(defun read-frame-length (stream limit &optional (progress (cons 0 0)))
  "The `<n>\\n' header: N, or :CLOSED at end of file, :BAD for anything
that is not a number followed by a newline, :TOO-LONG above LIMIT with N as
the second value (read no further than the newline in any case: the body,
if the caller cares, is still on the wire).  PROGRESS is the (N . DIGITS)
read so far: a read that times out in the middle of a header leaves what it
consumed there, and calling again with the same cell carries on from it."
  (loop
    (let ((c (read-char stream nil nil)))
      (cond ((null c) (return :closed))
            ((char= c #\Newline)
             (let ((n (car progress)))
               (cond ((zerop (cdr progress)) (return :bad))
                     ((> n limit) (return (values :too-long n)))
                     (t (return n)))))
            ((and (char<= #\0 c #\9) (< (cdr progress) 10))
             (setf (car progress) (+ (* (car progress) 10) (- (char-code c) (char-code #\0))))
             (incf (cdr progress)))
            (t (return :bad))))))

(defun read-frame-body (stream n)
  "N characters: the text, or :CLOSED when fewer came."
  (let* ((text (make-string n))
         (got (read-sequence text stream)))
    (if (< got n) :closed text)))

(defun read-frame (stream limit)
  "A frame: its text, or one of READ-FRAME-LENGTH's keywords."
  (let ((n (read-frame-length stream limit)))
    (if (integerp n)
        (read-frame-body stream n)
        n)))

(defun ascii-text-p (text)
  (dotimes (i (length text) t)
    (when (>= (char-code (char text i)) 128)
      (return nil))))

(defun write-wire-text (stream text)
  "TEXT to STREAM as UTF-8, whatever kind of string it is.  WRITE-STRING of
an 8-bit string puts each character out as ONE byte, WRITE-CHAR (and a wide
string) encodes it, and the reader decodes: so a character above 127 goes
out one character at a time, and the frame's <n> stays a count of
characters."
  (if (ascii-text-p text)
      (write-string text stream)
      (dotimes (i (length text))
        (write-char (char text i) stream))))

(defun write-frame (stream rc text)
  "A reply: `<rc> <n>' and the text."
  (format stream "~D ~D~%" rc (length text))
  (write-wire-text stream text)
  (finish-output stream))

(defun write-request (stream line)
  "A request: `<n>' and the command line."
  (format stream "~D~%" (length line))
  (write-wire-text stream line)
  (finish-output stream))

(defun read-reply (stream)
  "A reply frame: (values RC TEXT), or NIL when the connection is gone or
the header is not a reply's."
  (let ((rc 0) (n 0) (rc-digits 0) (n-digits 0) (in-rc t))
    (loop
      (let ((c (read-char stream nil nil)))
        (cond ((null c) (return-from read-reply nil))
              ((char= c #\Newline) (return))
              ((char= c #\Space)
               (if in-rc (setq in-rc nil) (return-from read-reply nil)))
              ((char<= #\0 c #\9)
               (if in-rc
                   (progn (setq rc (+ (* rc 10) (digit-char-p c))) (incf rc-digits))
                   (progn (setq n (+ (* n 10) (digit-char-p c))) (incf n-digits))))
              (t (return-from read-reply nil)))))
    (when (and (plusp rc-digits) (plusp n-digits))
      (let* ((text (make-string n))
             (got (read-sequence text stream)))
        (and (= got n) (values rc text))))))

;;; ----------------------------------------------------------------
;;; The server
;;; ----------------------------------------------------------------

(defstruct (server (:constructor %make-server ()))
  listener
  (number 0)                ; the port bound
  host                      ; the address bound
  token
  (drawn nil)               ; the token was drawn here, not given
  (quit nil)
  thread                    ; the listener's
  (lock (mp:make-lock "dev-tcp"))
  (connections '())         ; the open connections' streams
  (workers '())             ; their threads
  (pending 0)               ; connections still waiting for their AUTH
  (served 0)                ; frames answered after AUTH, for the tests
  ;; The limits, copied from the specials when the port starts: a
  ;; dynamic binding is the starting thread's alone.
  (auth-seconds *auth-seconds*)
  (frame-limit *frame-limit*)
  (max-connections *max-connections*)
  (max-pending *max-unauthenticated*)
  (stack-size *thread-stack-size*)
  (vm-frames *thread-vm-frames*))

(defvar *server* nil
  "The server START made last, or NIL: what STOP, WAIT, PORT and TOKEN
mean without an argument.")

(defvar *servers* '()
  "Every running server, for the exit hook.")

(defun running-p (&optional (server *server*))
  (and server
       (server-thread server)
       (not (server-quit server))
       (mp:thread-alive-p (server-thread server))
       t))

(defun port (&optional (server *server*))
  "The port SERVER listens on, or NIL."
  (and server (server-number server)))

(defun host (&optional (server *server*))
  "The address SERVER is bound to, or NIL."
  (and server (server-host server)))

(defun token (&optional (server *server*))
  "SERVER's token, or NIL."
  (and server (server-token server)))

(defun refuse (stream)
  "The one answer an unauthenticated connection gets, then the end of it."
  (ignore-errors (write-frame stream ext.dev:+rc-fatal+ "authentication required"))
  (ignore-errors (close stream)))

(defun auth-frame-p (server frame)
  "Whether FRAME is `AUTH <token>' with this server's token."
  (and (stringp frame)
       (>= (length frame) 5)
       (string-equal frame "AUTH " :end1 5)
       (token-equal-p (string-trim '(#\Space #\Tab #\Return #\Newline) (subseq frame 5))
                      (server-token server))))

(defun skip-frame-body (server stream n)
  "Read and drop N characters, a chunk at a time under the long timeout:
true when they were all there, NIL when the client went or stalled or the
server is stopping."
  (let ((chunk (make-string 4096)))
    (setf (ext:socket-stream-timeout stream :input) *body-seconds*)
    (unwind-protect
         (handler-case
             (loop
               (when (or (<= n 0) (server-quit server))
                 (return (<= n 0)))
               (let ((got (read-sequence chunk stream :end (min n (length chunk)))))
                 (when (zerop got)
                   (return nil))
                 (decf n got)))
           (error () nil))
      (setf (ext:socket-stream-timeout stream :input) *idle-seconds*))))

(defun wait-for-frame (server stream limit)
  "Wait for the next frame's header with the socket's short read timeout,
going round on each timeout while the server is up, then read the body
under the long one: the frame, or a keyword of READ-FRAME-LENGTH.  A frame
above LIMIT has its body dropped first, so that what follows it on the
wire is the next header and not the tail of this one; :CLOSED when it
cannot be."
  (let ((progress (cons 0 0)))          ; the header's digits so far, across timeouts
    (loop
      (when (server-quit server)
        (return :closed))
      (multiple-value-bind (n length)
          (handler-case (read-frame-length stream limit progress)
            (ext:socket-timeout () :again)
            (error () :closed))
        (cond ((eq n :again))
              ((eq n :too-long)
               (return (if (skip-frame-body server stream length) :too-long :closed)))
              ((not (integerp n)) (return n))
              (t
               (setf (ext:socket-stream-timeout stream :input) *body-seconds*)
               (let ((body (handler-case (read-frame-body stream n)
                             (error () :closed))))
                 (setf (ext:socket-stream-timeout stream :input) *idle-seconds*)
                 (return body))))))))

(defun serve-frame (frame)
  "One command: (values RC TEXT).  The reply is made from an UNWIND-PROTECT
cleanup, as the ARexx port does it, so a command that blows up past
HANDLE-COMMAND's own guard still answers rather than leaving the editor
waiting."
  (let ((rc ext.dev:+rc-fatal+)
        (text "ERROR: the command was aborted before it produced a reply"))
    (unwind-protect
         (multiple-value-setq (rc text) (ext.dev:handle-command frame))
      (return-from serve-frame (values rc text)))))

(defvar *connection* nil
  "The stream of the connection this thread serves, while it does: STOP
run from a connection (`EVAL (ext.dev.tcp:stop)') leaves that one open for
its reply, and the loop closes it on its way out.")

(defun connection-loop (server stream)
  "One connection: the AUTH gate, then frames until the client goes."
  (let ((*connection* stream))
    (connection-loop-1 server stream)))

(defun connection-loop-1 (server stream)
  (let ((pending t))                    ; counted in SERVER-PENDING until its AUTH
    (unwind-protect
         (progn
           (setf (ext:socket-stream-timeout stream :input) (server-auth-seconds server))
           (let ((first (handler-case (read-frame stream +auth-limit+)
                          (error () :closed))))
             (cond ((not (auth-frame-p server first))
                    (refuse stream)
                    (return-from connection-loop-1 nil))
                   (t (mp:with-lock-held ((server-lock server))
                        (decf (server-pending server)))
                      (setq pending nil)
                      (write-frame stream ext.dev:+rc-ok+ "OK"))))
           ;; Authenticated.  A short timeout, so the loop notices a stop.
           (setf (ext:socket-stream-timeout stream :input) *idle-seconds*)
           (loop
             (let ((frame (wait-for-frame server stream (server-frame-limit server))))
               (cond ((stringp frame)
                      (multiple-value-bind (rc text) (serve-frame frame)
                        (mp:with-lock-held ((server-lock server))
                          (incf (server-served server)))
                        (handler-case (write-frame stream (or rc ext.dev:+rc-fatal+) (or text ""))
                          (error () (return)))))
                     ((eq frame :too-long)
                      (handler-case (write-frame stream ext.dev:+rc-fatal+ "ERROR: the command is too long")
                        (error () (return))))
                     (t (return))))))
      (ignore-errors (close stream))
      (mp:with-lock-held ((server-lock server))
        (when pending
          (decf (server-pending server)))
        (setf (server-connections server) (remove stream (server-connections server)))))))

(defun admit-connection (server stream)
  "Give STREAM a thread of its own unless the server already has its
*MAX-CONNECTIONS* or its *MAX-UNAUTHENTICATED*: true when it did.  The
threads that are done are dropped from the worker list on the way."
  (mp:with-lock-held ((server-lock server))
    (when (and (< (length (server-connections server)) (server-max-connections server))
               (< (server-pending server) (server-max-pending server)))
      (let ((thread (mp:make-thread (lambda () (connection-loop server stream))
                                    :name "dev-tcp-connection"
                                    :stack-size (server-stack-size server)
                                    :vm-frames (server-vm-frames server))))
        (incf (server-pending server))
        (push stream (server-connections server))
        (setf (server-workers server)
              (cons thread (remove-if-not #'mp:thread-alive-p (server-workers server))))
        t))))

(defun listener-loop (server)
  "Accept until told to stop; a connection gets a thread of its own, or is
closed unanswered when the server is full."
  (loop
    (let ((stream (handler-case (ext:socket-accept (server-listener server))
                    (error () nil))))
      (when (server-quit server)
        (when stream (ignore-errors (close stream)))
        (return))
      (cond ((null stream) (sleep 0.05))
            ((not (admit-connection server stream))
             (ignore-errors (close stream)))))))

(defun wildcard-host-p (host)
  "Whether HOST would bind every address of this machine: empty, `*', or an
address of nothing but zeros however it is spelled -- `0.0.0.0', `::', and
`00.0.0.0' too, for the platform layer's parser takes leading zeros and
reads that as INADDR_ANY."
  (or (zerop (length host))
      (string= host "*")
      (every (lambda (c) (find c "0.:")) host)))

(defun start (&key (port *default-port*)
                   (host "127.0.0.1")
                   (token nil token-given)
                   (stack-size *thread-stack-size*)
                   (vm-frames *thread-vm-frames*))
  "Listen on HOST (127.0.0.1 unless another dotted-quad address of this
machine is spelled out; never a wildcard) port PORT (0: one the OS picks)
and serve EXT.DEV's commands to every connection that authenticates with
TOKEN.  Without :TOKEN the token is drawn from the OS entropy source and
printed once; with neither, START refuses to listen.  Returns the server;
STOP takes it down, and the process exit does too.

STACK-SIZE and VM-FRAMES size the connection threads: commands compile
code (see the Amiga Stack Requirements notes in CLAUDE.md)."
  (unless (and (integerp port) (<= 0 port 65535))
    (error "EXT.DEV.TCP: the port must be 0-65535, not ~S" port))
  (unless (stringp host)
    (error "EXT.DEV.TCP: :host must be a dotted-quad address string, not ~S" host))
  (when (wildcard-host-p host)
    (error "EXT.DEV.TCP: :host ~S is a wildcard; the port binds one address of this machine (127.0.0.1 without :host)"
           host))
  (when token-given
    (unless (and (stringp token) (plusp (length token)))
      (error "EXT.DEV.TCP: :token must be a non-empty string")))
  (let ((server (%make-server))
        (drawn (not token-given)))
    (when drawn
      (setq token (random-hex))
      (unless token
        (error "EXT.DEV.TCP: no entropy source for the token (~A): pass :token to START -- the port does not listen without one"
               *entropy-source*)))
    (setf (server-token server) token
          (server-drawn server) drawn
          (server-host server) host
          (server-stack-size server) stack-size
          (server-vm-frames server) vm-frames
          (server-listener server)
          (handler-case (ext:socket-listen port host)
            (error (e)
              (error "EXT.DEV.TCP: cannot listen on ~A:~D: ~A" host port e)))
          (server-number server) (ext:socket-local-port (server-listener server)))
    ;; Take the port down when the process exits, whatever the reason: a
    ;; listener thread that outlives the VM is a crash on the next
    ;; connection (see AMIGA.AREXX:START for what it costs on an Amiga).
    ;; The registration is idempotent.
    (ext:add-exit-hook 'stop-all)
    (setf (server-thread server)
          (mp:make-thread (lambda () (listener-loop server))
                          :name "dev-tcp-port"
                          :stack-size (* 64 1024)))
    (push server *servers*)
    (setf *server* server)
    (when drawn
      (format t "~&; EXT.DEV.TCP: listening on ~A:~D, token ~A~%"
              host (server-number server) token)
      (finish-output))
    server))

(defun wait-for-threads (threads seconds)
  "Wait up to SECONDS for THREADS to finish -- never for the calling one,
which may be a connection thread running STOP."
  (let ((others (remove (mp:current-thread) threads))
        (deadline (+ (get-internal-real-time) (* seconds internal-time-units-per-second))))
    (loop while (and (some #'mp:thread-alive-p others)
                     (< (get-internal-real-time) deadline))
          do (sleep 0.05))))

(defun stop (&optional (server *server*))
  "Stop accepting, end every connection, close the connections to editors.
Safe from a connection thread (`EVAL (ext.dev.tcp:stop)' over the port
itself is how an editor ends a amilisp it started): that thread finishes
its reply after the rest is down.  True when a server was stopped."
  (when (and server (not (server-quit server)))
    (setf (server-quit server) t)
    ;; A connection of our own ends the accept the listener thread is in.
    (ignore-errors (close (ext:open-tcp-stream (if (string= (server-host server) "0.0.0.0")
                                                   "127.0.0.1"
                                                   (server-host server))
                                               (server-number server) 1)))
    (when (server-thread server)
      (wait-for-threads (list (server-thread server)) 3))
    (ignore-errors (close (server-listener server)))
    ;; The workers see the flag within their read timeout.
    (wait-for-threads (server-workers server) 3)
    (dolist (stream (mp:with-lock-held ((server-lock server))
                      (prog1 (server-connections server)
                        (setf (server-connections server) '()))))
      ;; Not the connection this very STOP came in on: its reply is
      ;; still to be written, and its loop closes it.
      (unless (eq stream *connection*)
        (ignore-errors (close stream))))
    (setf *servers* (remove server *servers*))
    (when (eq *server* server)
      (setf *server* (first *servers*)))
    (disconnect)
    t))

(defun stop-all ()
  "The exit hook: every running server, and the connections to editors."
  (dolist (server *servers*)
    (ignore-errors (stop server)))
  (disconnect)
  t)

(defun wait (&optional (server *server*))
  "Block until SERVER is stopped: what a `--load' preamble whose only job
is to serve the port ends with, so the amilisp stays up until an editor
sends `EVAL (ext.dev.tcp:stop)' or the user ends it.  Returns once the
connection threads are done too."
  (when server
    (loop until (server-quit server)
          do (sleep 0.2))
    (wait-for-threads (server-workers server) 3))
  t)

;;; ----------------------------------------------------------------
;;; The client side
;;; ----------------------------------------------------------------

(defun port-name (host port token)
  "The name an editor's port goes by in REPL-ATTACH: `tcp:HOST:PORT/TOKEN'."
  (format nil "tcp:~A:~D/~A" host port token))

(defun parse-port-name (name)
  "(values HOST PORT TOKEN) of a `tcp:HOST:PORT/TOKEN' name, or NIL for
any other string."
  (and (stringp name)
       (> (length name) 4)
       (string-equal name "tcp:" :end1 4)
       (let* ((slash (position #\/ name :start 4))
              (colon (and slash (position #\: name :start 4 :end slash :from-end t))))
         (and colon (> colon 4) (< (1+ colon) slash) (< (1+ slash) (length name))
              (let ((port (parse-integer name :start (1+ colon) :end slash :junk-allowed t)))
                (and port (<= 1 port 65535)
                     (values (subseq name 4 colon) port (subseq name (1+ slash)))))))))

(defun tcp-name-p (name)
  (and (stringp name) (> (length name) 4) (string-equal name "tcp:" :end1 4)))

(defun public-name (name)
  "NAME without its token, for messages: a token belongs in no error
text and no log."
  (let ((slash (and (stringp name) (position #\/ name))))
    (if slash (subseq name 0 slash) name)))

(defun connect (host port token &key (seconds 5))
  "An authenticated connection to the port at HOST:PORT: the stream, or
NIL with the reason as the second value."
  (let ((stream (handler-case (ext:open-tcp-stream host port seconds)
                  (error (e) (return-from connect (values nil (princ-to-string e)))))))
    (handler-case
        (progn
          (write-request stream (concatenate 'string "AUTH " token))
          (multiple-value-bind (rc text) (read-reply stream)
            (cond ((and rc (= rc ext.dev:+rc-ok+)) stream)
                  (t (ignore-errors (close stream))
                     (values nil (or text "no answer to AUTH"))))))
      (error (e)
        (ignore-errors (close stream))
        (values nil (princ-to-string e))))))

(defun request (stream line)
  "LINE over STREAM: (values RC TEXT), or NIL when the connection is gone."
  (write-request stream line)
  (read-reply stream))

;;; The connections to editors the REPL thread sends through: one per port
;;; name, opened on the first send, kept until it fails, DISCONNECT or STOP.
(defstruct (client (:constructor %make-client (name stream)))
  name stream (lock (mp:make-lock "dev-tcp-client")))

(defvar *clients* '())
(defvar *clients-lock* (mp:make-lock "dev-tcp-clients"))

(defun client-for (name)
  "The open client for NAME, or a fresh one; signals when the editor's
port cannot be reached or refuses the token."
  (or (mp:with-lock-held (*clients-lock*)
        (find name *clients* :key #'client-name :test #'string=))
      (multiple-value-bind (host port token) (parse-port-name name)
        (unless host
          (error "EXT.DEV.TCP: ~A is not a port name of the form tcp:HOST:PORT/TOKEN"
                 (public-name name)))
        (multiple-value-bind (stream reason) (connect host port token)
          (unless stream
            (error "EXT.DEV.TCP: cannot reach the editor's port ~A: ~A" (public-name name) reason))
          (let ((client (%make-client name stream)))
            (mp:with-lock-held (*clients-lock*)
              (push client *clients*))
            client)))))

(defun drop-client (client)
  (mp:with-lock-held (*clients-lock*)
    (setf *clients* (remove client *clients*)))
  (ignore-errors (close (client-stream client))))

(defun send (name command)
  "COMMAND to the editor's port NAME (`tcp:HOST:PORT/TOKEN'): (values RC
TEXT).  An editor that is gone is an error, which is how the REPL thread
learns to stop."
  (let ((client (client-for name)))
    (mp:with-lock-held ((client-lock client))
      (multiple-value-bind (rc text)
          (handler-case (request (client-stream client) command)
            (error (e)
              (drop-client client)
              (error "EXT.DEV.TCP: the editor's port ~A is gone: ~A" (public-name name) e)))
        (unless rc
          (drop-client client)
          (error "EXT.DEV.TCP: the editor's port ~A is gone" (public-name name)))
        (values rc text)))))

(defun disconnect (&optional name)
  "Close the connection to the editor's port NAME, or to every editor."
  (dolist (client (mp:with-lock-held (*clients-lock*)
                    (if name
                        (remove name *clients* :key #'client-name :test-not #'string=)
                        (copy-list *clients*))))
    (drop-client client))
  t)

;;; The REPL's way back: a `tcp:' name is ours, anything else goes to the
;;; sender that was there (AMIGA.AREXX's, or none).  Wrapped once, however
;;; often this file is loaded.
(defvar *previous-repl-send* nil)
(defvar *installed* nil)

(unless *installed*
  (setf *previous-repl-send* ext.dev:*repl-send*
        *installed* t
        ext.dev:*repl-send*
        (lambda (port command)
          (cond ((tcp-name-p port) (send port command))
                (*previous-repl-send* (funcall *previous-repl-send* port command))
                (t (error "EXT.DEV.TCP: no transport reaches port ~A (a TCP port is named tcp:HOST:PORT/TOKEN)"
                          (public-name port)))))))

(provide "dev-tcp")
