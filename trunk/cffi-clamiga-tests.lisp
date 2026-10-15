;; cffi-clamiga-tests.lisp — fiveam test suite for the AmiLisp CFFI backend.
;;
;; Loaded by trunk/load-and-test-cffi.lisp AFTER CFFI and FIVEAM are loaded
;; (so the package references below read cleanly).  Exercises the public CFFI
;; API: pointers, typed memory access, foreign strings, defcfun/foreign-funcall,
;; callbacks, and foreign structs.  Foreign-call/callback tests are host-only
;; (POSIX dlopen + libffi) and #-amigaos-guarded.

(defpackage :cffi-clamiga-test
  (:use :cl :cffi :fiveam)
  (:export #:run-cffi-clamiga-tests))

(in-package :cffi-clamiga-test)

(def-suite cffi-clamiga :description "AmiLisp CFFI backend tests")
(in-suite cffi-clamiga)

;;;; Pointers

(test pointers
  (is (pointerp (null-pointer)))
  (is (null-pointer-p (null-pointer)))
  (is (not (null-pointer-p (make-pointer 16))))
  (is (= 4096 (pointer-address (make-pointer 4096))))
  (is (= 4196 (pointer-address (inc-pointer (make-pointer 4096) 100))))
  (is (pointer-eq (make-pointer 42) (make-pointer 42)))
  (is (not (pointer-eq (make-pointer 42) (make-pointer 43)))))

;;;; Foreign type sizes (LP64 host)

(test type-sizes
  (is (= 1 (foreign-type-size :char)))
  (is (= 2 (foreign-type-size :short)))
  (is (= 4 (foreign-type-size :int)))
  (is (= 8 (foreign-type-size :long)))
  (is (= 8 (foreign-type-size :long-long)))
  (is (= 4 (foreign-type-size :float)))
  (is (= 8 (foreign-type-size :double)))
  (is (= 8 (foreign-type-size :pointer))))

;;;; Typed memory access

(test mem-ref-integers
  (with-foreign-object (p :int64)
    (setf (mem-ref p :char) -5)            (is (= -5 (mem-ref p :char)))
    (setf (mem-ref p :unsigned-char) 200)  (is (= 200 (mem-ref p :unsigned-char)))
    (setf (mem-ref p :short) -1000)        (is (= -1000 (mem-ref p :short)))
    (setf (mem-ref p :unsigned-short) 60000) (is (= 60000 (mem-ref p :unsigned-short)))
    (setf (mem-ref p :int) -123456)        (is (= -123456 (mem-ref p :int)))
    (setf (mem-ref p :unsigned-int) 4000000000) (is (= 4000000000 (mem-ref p :unsigned-int)))
    (setf (mem-ref p :long) -5000000000)   (is (= -5000000000 (mem-ref p :long)))
    (setf (mem-ref p :unsigned-long) 4294967300) (is (= 4294967300 (mem-ref p :unsigned-long)))))

(test mem-ref-floats
  (with-foreign-object (p :double)
    (setf (mem-ref p :float) 2.5) (is (= 2.5 (mem-ref p :float)))
    (setf (mem-ref p :double) 6.25d0) (is (= 6.25d0 (mem-ref p :double)))))

(test mem-ref-pointer
  (with-foreign-object (pp :pointer)
    (setf (mem-ref pp :pointer) (make-pointer 8192))
    (is (= 8192 (pointer-address (mem-ref pp :pointer))))))

(test mem-aref-array
  (with-foreign-object (arr :int 5)
    (loop for v in '(10 20 30 40 50) for i from 0 do (setf (mem-aref arr :int i) v))
    (is (equal '(10 20 30 40 50)
               (loop for i below 5 collect (mem-aref arr :int i))))))

;;;; Foreign strings

(test foreign-strings
  (with-foreign-string (s "hello world")
    (is (string= "hello world" (foreign-string-to-lisp s))))
  (is (string= "round-trip"
               (with-foreign-string (s "round-trip") (foreign-string-to-lisp s)))))

;;;; defcfun / foreign-funcall / callbacks / structs — host-only (dlopen)

#-amigaos
(progn
  (defcfun "strlen" :unsigned-long (s :string))
  (defcfun ("abs" c-abs) :int (n :int))
  (defcfun "labs" :long (n :long))
  (defcfun "pow" :double (b :double) (e :double))
  (defcfun "sqrt" :double (x :double))
  (defcfun "memset" :pointer (p :pointer) (c :int) (n :unsigned-long))
  (defcfun "strcmp" :int (a :string) (b :string))
  (defcfun "toupper" :int (c :int))

  (test defcfun-calls
    (is (= 6 (strlen "abcdef")))
    (is (= 123 (c-abs -123)))
    (is (= 5000000000 (labs -5000000000)))
    (is (= 65536d0 (pow 2d0 16d0)))
    (is (= 9d0 (sqrt 81d0)))
    (is (zerop (strcmp "abc" "abc")))
    (is (plusp (strcmp "b" "a")))
    (is (= 65 (toupper 97))))

  (test foreign-funcall-direct
    (is (= 7 (foreign-funcall "abs" :int -7 :int)))
    (is (= 1024d0 (foreign-funcall "pow" :double 2d0 :double 10d0 :double))))

  (test memset-pointer-arg
    (with-foreign-object (buf :char 8)
      (memset buf 65 8)
      (is (= 65 (mem-ref buf :unsigned-char)))
      (is (= 65 (mem-ref buf :unsigned-char 7)))))

  (defcallback int-compare :int ((a :pointer) (b :pointer))
    (- (mem-ref a :int) (mem-ref b :int)))

  (defcfun "qsort" :void
    (base :pointer) (n :unsigned-long) (sz :unsigned-long) (cmp :pointer))

  (test callbacks-qsort
    (with-foreign-object (arr :int 6)
      (loop for v in '(9 2 7 1 5 3) for i from 0 do (setf (mem-aref arr :int i) v))
      (qsort arr 6 (foreign-type-size :int) (callback int-compare))
      (is (equal '(1 2 3 5 7 9)
                 (loop for i below 6 collect (mem-aref arr :int i))))))

  ;; Callback redefinition reuses the same C closure / foreign pointer.
  (test callback-pointer-stable
    (let ((p1 (callback int-compare)))
      (defcallback int-compare :int ((a :pointer) (b :pointer))
        (- (mem-ref b :int) (mem-ref a :int)))   ; reversed
      (is (pointer-eq p1 (callback int-compare))))
    ;; restore ascending order for any later runs
    (defcallback int-compare :int ((a :pointer) (b :pointer))
      (- (mem-ref a :int) (mem-ref b :int)))))

;;;; Foreign structs

(defcstruct point (x :int) (y :int) (label :pointer))

(test foreign-struct
  (with-foreign-object (pt '(:struct point))
    (setf (foreign-slot-value pt '(:struct point) 'x) 11)
    (setf (foreign-slot-value pt '(:struct point) 'y) -22)
    (is (= 11 (foreign-slot-value pt '(:struct point) 'x)))
    (is (= -22 (foreign-slot-value pt '(:struct point) 'y)))
    (is (>= (foreign-type-size '(:struct point)) 16))))

(defun run-cffi-clamiga-tests ()
  "Run the suite and return T if every test passed."
  (run! 'cffi-clamiga))
