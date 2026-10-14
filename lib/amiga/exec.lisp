;;; amiga/exec.lisp — exec.library abstractions for AmiLisp
;;;
;;; Loaded via (require "amiga/exec").
;;; Memory introspection (AvailMem), chip-RAM helpers on top of the
;;; AMIGA package's ALLOC-CHIP/FREE-CHIP builtins, and the device-I/O
;;; calls every exec device module needs (OpenDevice, the IORequest and
;;; MsgPort allocators, SendIO/CheckIO/WaitIO/AbortIO) -- AMIGA.AUDIO
;;; and AMIGA.AHI build on those.

;; compile-time too: COMPILE-FILE (the host builds the lib/amiga FASLs) must see
;; these packages at read time, not only LOAD.
(eval-when (:compile-toplevel :load-toplevel :execute)
  (require "amiga/ffi"))

(defpackage "AMIGA.EXEC"
  (:use "CL" "FFI" "AMIGA.FFI")
  (:export
   ;; Library
   "*EXEC-BASE*"
   ;; Memory introspection
   "AVAIL-MEM"
   ;; MEMF_* requirement/option flags (exec/memory.h)
   "+MEMF-ANY+" "+MEMF-PUBLIC+" "+MEMF-CHIP+" "+MEMF-FAST+"
   "+MEMF-CLEAR+" "+MEMF-LARGEST+" "+MEMF-TOTAL+"
   ;; Chip RAM helpers
   "ALLOC-CHIP-BYTES"
   ;; Device I/O (exec/io.h)
   "CREATE-MSG-PORT" "DELETE-MSG-PORT"
   "CREATE-IO-REQUEST" "DELETE-IO-REQUEST"
   "OPEN-DEVICE" "CLOSE-DEVICE"
   "DO-IO" "SEND-IO" "CHECK-IO" "WAIT-IO" "ABORT-IO"
   "IO-REQUEST-DEVICE" "IO-REQUEST-ERROR"))

(in-package "AMIGA.EXEC")

;;; ================================================================
;;; Exec library base — ExecBase lives at absolute address 4, no
;;; OpenLibrary needed (exec is what OpenLibrary itself lives in).
;;; NIL on the host build, where this module is loaded for COMPILE-FILE
;;; (the host builds the lib/amiga FASLs): address 4 is not readable
;;; there -- same convention as the generated amiga/raw modules.
;;; ================================================================

(define-library-variable *exec-base*
  (when (member :amigaos *features*)
    (ffi:make-foreign-pointer (ffi:peek-u32 (ffi:make-foreign-pointer 4)))))

;;; ================================================================
;;; LVO offsets (from amiga/exec_lib.fd) -- each one is cross-checked
;;; against the generated AMIGA.RAW.EXEC table by
;;; tests/test_amiga_curated_vs_raw.lisp.
;;; ================================================================

(defconstant +lvo-avail-mem+         -216)  ; D1 requirements
(defconstant +lvo-open-device+       -444)  ; A0 name, D0 unit, A1 io, D1 flags
(defconstant +lvo-close-device+      -450)  ; A1 io
(defconstant +lvo-do-io+             -456)  ; A1 io
(defconstant +lvo-send-io+           -462)  ; A1 io
(defconstant +lvo-check-io+          -468)  ; A1 io
(defconstant +lvo-wait-io+           -474)  ; A1 io
(defconstant +lvo-abort-io+          -480)  ; A1 io
(defconstant +lvo-create-io-request+ -654)  ; A0 port, D0 size
(defconstant +lvo-delete-io-request+ -660)  ; A0 io
(defconstant +lvo-create-msg-port+   -666)
(defconstant +lvo-delete-msg-port+   -672)  ; A0 port

(defun %exec (lvo regs)
  "Call the exec.library function at LVO with the register list REGS."
  (amiga:call-library *exec-base* lvo regs))

;;; ================================================================
;;; MEMF_* flags (exec/memory.h) — AvailMem/AllocMem requirements.
;;; ================================================================

(defconstant +memf-any+     #x00000000)
(defconstant +memf-public+  #x00000001)
(defconstant +memf-chip+    #x00000002)
(defconstant +memf-fast+    #x00000004)
(defconstant +memf-clear+   #x00010000)
(defconstant +memf-largest+ #x00020000)
(defconstant +memf-total+   #x00080000)

;;; ================================================================
;;; AvailMem
;;; ================================================================

(defun avail-mem (&optional (requirements +memf-any+))
  "Free system memory in bytes -- exec.library AvailMem(REQUIREMENTS).
REQUIREMENTS is a MEMF_* mask: +MEMF-ANY+ (the default) counts all
free memory, +MEMF-CHIP+/+MEMF-FAST+ one pool, +MEMF-LARGEST+ ORed in
asks for the largest single free block instead of the sum, and
+MEMF-TOTAL+ for the pool's total size.  A snapshot, not a
reservation: other tasks allocate concurrently."
  (%exec +lvo-avail-mem+ (list :d1 requirements)))

;;; ================================================================
;;; Chip RAM helpers
;;; ================================================================

(defun alloc-chip-bytes (bytes)
  "Copy the (unsigned-byte 8) vector BYTES into a fresh chip-RAM
allocation -- the standard way to hand data to hardware that requires
chip memory (blitter masks, sprite images, audio samples).  Returns
the chip foreign pointer; the caller frees it with AMIGA:FREE-CHIP."
  (let ((chip (amiga:alloc-chip (length bytes))))
    (ffi:poke-bytes chip bytes)
    chip))

;;; ================================================================
;;; Device I/O (exec/io.h)
;;;
;;; The exec side of talking to a device: a reply port, an I/O request
;;; of the device's size, OpenDevice on it, then SendIO/CheckIO/WaitIO
;;; (or DoIO) per command.  Pointers in and out are foreign pointers;
;;; io_Error results are the signed byte exec leaves in D0.  See
;;; lib/amiga/audio.lisp (audio.device) and lib/amiga/ahi.lisp
;;; (ahi.device) for the pattern end to end.
;;; ================================================================

;;; struct IORequest field offsets (exec/io.i) -- pinned to the generated
;;; AMIGA.RAW.EXEC struct rows by tests/test_amiga_curated_vs_raw.lisp.
(defconstant +io-request-device-offset+ 20)   ; struct Device *, set by OpenDevice
(defconstant +io-request-error-offset+  31)   ; BYTE io_Error

(defun %byte-result (d0)
  "The BYTE an exec call left in D0 (OpenDevice/DoIO/WaitIO return
io_Error, a signed byte; the upper bits of D0 are not defined)."
  (let ((b (logand d0 #xFF)))
    (if (>= b #x80) (- b #x100) b)))

(defun create-msg-port ()
  "exec CreateMsgPort: a fresh message port, as a foreign pointer, or NIL."
  (let ((p (%exec +lvo-create-msg-port+ nil)))
    (if (zerop p) nil (ffi:make-foreign-pointer p))))

(defun delete-msg-port (port)
  "exec DeleteMsgPort.  Returns NIL."
  (%exec +lvo-delete-msg-port+ (list :a0 port))
  nil)

(defun create-io-request (port size)
  "exec CreateIORequest: a zeroed SIZE-byte I/O request replying to PORT,
as a foreign pointer (bounds-checked to SIZE), or NIL."
  (let ((p (%exec +lvo-create-io-request+ (list :a0 port :d0 size))))
    (if (zerop p) nil (ffi:make-foreign-pointer p size))))

(defun delete-io-request (io)
  "exec DeleteIORequest.  Returns NIL."
  (%exec +lvo-delete-io-request+ (list :a0 io))
  nil)

(defun open-device (name unit io &optional (flags 0))
  "exec OpenDevice: open the device NAME (a Lisp string, e.g.
\"audio.device\"), unit UNIT, on the request IO (from CREATE-IO-REQUEST,
its device-specific fields filled in as the device asks).  Returns the
io_Error code: 0 on success, else the device's (or exec's IOERR_*)
error code.  FLAGS are device-specific."
  (let ((cname (ffi:foreign-string name)))
    (unwind-protect
         (%byte-result (%exec +lvo-open-device+
                              (list :a0 cname :d0 unit :a1 io :d1 flags)))
      (ffi:free-foreign cname))))

(defun close-device (io)
  "exec CloseDevice on the request IO OpenDevice succeeded with.  Returns NIL."
  (%exec +lvo-close-device+ (list :a1 io))
  nil)

(defun do-io (io)
  "exec DoIO: perform the request IO synchronously.  Returns io_Error
\(0 = success).  Blocks until the device is done -- see SEND-IO for the
non-blocking road."
  (%byte-result (%exec +lvo-do-io+ (list :a1 io))))

(defun send-io (io)
  "exec SendIO: start the request IO asynchronously.  Poll it with
CHECK-IO, take it back with WAIT-IO.  Returns NIL."
  (%exec +lvo-send-io+ (list :a1 io))
  nil)

(defun check-io (io)
  "exec CheckIO: true when the request IO has completed (or was never
sent), NIL while the device still holds it."
  (not (zerop (%exec +lvo-check-io+ (list :a1 io)))))

(defun wait-io (io)
  "exec WaitIO: wait for the request IO to complete and take it off its
reply port.  Returns io_Error.  Blocks until then -- issue it on a
request CHECK-IO already reports complete (or one just aborted) to
keep the wait instant."
  (%byte-result (%exec +lvo-wait-io+ (list :a1 io))))

(defun abort-io (io)
  "exec AbortIO: ask the device to abort the request IO.  The request
still has to be reclaimed with WAIT-IO.  Returns NIL."
  (%exec +lvo-abort-io+ (list :a1 io))
  nil)

(defun io-request-device (io)
  "The device base OpenDevice stored in the request IO (io_Device), as a
foreign pointer -- for devices whose functions are called library-style
through it (ahi.device)."
  (ffi:make-foreign-pointer (ffi:peek-u32 io +io-request-device-offset+)))

(defun io-request-error (io)
  "The io_Error byte of the request IO (0 = success)."
  (ffi:peek-i8 io +io-request-error-offset+))

;;; ================================================================
;;; Provide module
;;; ================================================================

(provide "amiga/exec")
