; Heap-image restore leg: runs in a `amilisp --image amilisp-test.img`
; session (see tests/amiga/image-save.lisp for the state this checks).
; Prints IMAGE-VERIFY-PASSED only if every check holds — the marker
; Makefile.cross's verify-amiga requires.

(setq *img-verify-fails* 0)
(defmacro img-check (name expected actual)
  `(handler-case
       (let ((e ,expected) (a ,actual))
         (if (equal e a)
             (format t "PASS: image ~a~%" ,name)
             (progn (setq *img-verify-fails* (+ *img-verify-fails* 1))
                    (format t "FAIL: image ~a - expected ~s got ~s~%"
                            ,name e a))))
     (error (c)
       (setq *img-verify-fails* (+ *img-verify-fails* 1))
       (format t "FAIL: image ~a - signaled: ~a~%" ,name c))))

(img-check "restored-p" t ext:*image-restored-p*)
(img-check "gc block-start index over the adopted payload" 0
           (progn (ext:gc) (ext:%gc-audit-hdr-index)))
(img-check "function" 144 (img-fib 12))
; Native code does not survive an image; a restored function is compiled
; again once it turns hot (IMG-FIB 12 is 465 calls).  Before 0.12 nothing in
; an image was ever compiled again.
(img-check "restored function compiled once hot" t
           (if (amilisp::%jit-active-p)
               (and (amilisp::%jit-dump-bytes #'img-fib) t)
               t))
(img-check "macro" 42 (img-twice 21))
(img-check "clos dispatch" "Rex speaks" (img-speak *img-pet*))
(img-check "new instance" "Fido speaks"
           (img-speak (make-instance 'img-animal :name "Fido")))
(img-check "struct" 3 (img-point-x *img-pt*))
(img-check "eq hashtable identity" 'img-hit (gethash *img-key* *img-ht*))
(img-check "closure over cell" 42 (funcall *img-close* 11))
(img-check "readtable macro" 4242 (read-from-string "!"))
(img-check "string-output-stream buffer" "pre-save;post"
           (progn (write-string "post" *img-sos*)
                  (get-output-stream-string *img-sos*)))
(img-check "restored lock" t
           (progn (mp:acquire-lock *img-lock*)
                  (mp:release-lock *img-lock*)
                  t))
(img-check "restored condvar usable" t
           (progn (mp:condition-notify *img-cv*) t))
(img-check "dead thread not alive" nil (mp:thread-alive-p *img-dead-thr*))
(img-check "dead thread join signals" t
           (handler-case (progn (mp:join-thread *img-dead-thr*) nil)
             (error () t)))
; :SHAKE-BINDINGS (spec Phase 3) — the image was saved with its binding
; table shed.  Names referenced before the save are ordinary symbols; the
; rest do not exist, and the reader says why instead of implying a typo.
(img-check "shed table keeps its marker" t
           (getf (amilisp::%binding-table-info "IMG-BT") :shed))
(img-check "shed table reports no entries" 0
           (getf (amilisp::%binding-table-info "IMG-BT") :entries))
(img-check "touched constant survives the shed" 11 (symbol-value *img-bt-kept*))
(img-check "touched name keeps its identity" t
           (eq *img-bt-kept* (find-symbol "+KEPT+" "IMG-BT")))
(img-check "touched libcall stub survives" -30
           (getf (ffi::%ffi-stub-info (symbol-function (find-symbol "IMG-CALL" "IMG-BT"))) :lvo))
(img-check "untouched name is gone" '(nil nil)
           (multiple-value-list (find-symbol "+GONE+" "IMG-BT")))
(img-check "untouched field accessor is gone" '(nil nil)
           (multiple-value-list (find-symbol "IMG-NODE-X" "IMG-BT")))
(img-check "reader explains a shed miss" t
           (handler-case (progn (read-from-string "img-bt:+gone+") nil)
             (error (c) (and (search "shed by SAVE-IMAGE" (format nil "~a" c)) t))))
(img-check "eager flip on a shed package is a no-op" t
           (progn (amilisp::%binding-table-materialize-all "IMG-BT")
                  (getf (amilisp::%binding-table-info "IMG-BT") :shed)))

; Process-derived state is re-derived on restore: this leg runs from
; AmiLisp:tests (call-on-ustartup), the save leg ran from AmiLisp:.
(img-check "dpd is the restoring cwd, not the saver's" t
           (let ((dpd (namestring *default-pathname-defaults*)))
             (and (string/= dpd *img-saved-dpd*)
                  (not (null (search "tests" dpd))))))
(img-check "random-state reseeded" t
           (/= (random 1000000) *img-rnd-next*))

; Library bases are re-derived before ~/.amilisprc runs
; (AMIGA.FFI:DEFINE-LIBRARY-VARIABLE); every library call path works from
; the restored process, and a base that is still a zeroed foreign pointer
; signals instead of jumping to -LVO.
(defun img-avail-fresh () (amiga.raw.exec:avail-mem 0))
(defun img-null-p (thunk)
  (let ((amiga.raw.exec:*exec-base* (ffi:make-foreign-pointer 0))
        (amiga.exec:*exec-base* (ffi:make-foreign-pointer 0)))
    (handler-case (progn (funcall thunk) :called)
      (error (c)
        (if (search "NULL pointer" (format nil "~a" c)) :null (format nil "~a" c))))))
(img-check "raw library base re-derived (ExecBase)" t
           (= (ffi:foreign-pointer-address amiga.raw.exec:*exec-base*)
              (ffi:peek-u32 (ffi:make-foreign-pointer 4))))
(img-check "curated library base re-derived (ExecBase)" t
           (= (ffi:foreign-pointer-address amiga.exec:*exec-base*)
              (ffi:peek-u32 (ffi:make-foreign-pointer 4))))
(img-check "library version re-derived from the new base" t
           (and (plusp amiga.raw.exec:*exec-version*)
                (eql amiga.raw.exec:*exec-version*
                     (amiga.ffi:library-version amiga.raw.exec:*exec-base*))))
(img-check "stub call from image code" t (plusp (img-avail-raw)))
(img-check "stub call compiled after the restore" t (plusp (img-avail-fresh)))
(img-check "call-library after the restore" t (plusp (amiga.exec:avail-mem)))
(img-check "NULL base signals: direct call" :null (img-null-p #'img-avail-fresh))
(img-check "NULL base signals: stub through funcall" :null
           (img-null-p (lambda () (funcall #'amiga.raw.exec:avail-mem 0))))
(img-check "NULL base signals: call-library" :null
           (img-null-p #'amiga.exec:avail-mem))
(img-check "NULL base signals: call-library-fast" :null
           (img-null-p (lambda ()
                         (amiga:call-library-fast amiga.exec:*exec-base* -216 0))))

; The restored heap must survive GC + compaction (offsets, JIT relink,
; blob-attached bytecode all get exercised by the collector).
(img-check "gc after restore" 55
           (progn (dotimes (i 20000) (cons i i))
                  (ext:gc)
                  (img-fib 10)))

(if (= *img-verify-fails* 0)
    (format t "~%IMAGE-VERIFY-PASSED~%")
    (format t "~%IMAGE-VERIFY-FAILED: ~a check(s)~%" *img-verify-fails*))
