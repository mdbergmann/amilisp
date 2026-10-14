;; Load and exercise closer-mop via the AmiLisp library fork.
;; Works on both host and Amiga.

(require "asdf")
(require "clos")

;; Make the quicklisp-managed dependencies resolvable through ASDF.
;; All quicklisp handling lives in this helper; this script only uses
;; asdf:load-system. closer-mop itself is the AmiLisp library fork,
;; cloned into quicklisp's local-projects tree and picked up via its
;; local-projects searcher (see README's Quicklisp section).
(load "trunk/load-libs-ql.lisp")

(format t "~%--- Loading closer-mop (AmiLisp fork) ---~%")
(asdf:load-system :closer-mop)

(in-package :cl-user)

(format t "~%--- Smoke-testing closer-mop bindings ---~%")

(defun report (label value)
  (format t "  ~A => ~S~%" label value))

(defclass cm-foo ()
  ((x :initarg :x :accessor cm-foo-x)
   (y :initarg :y)))

(defgeneric cm-probe (obj))
(defmethod cm-probe ((obj cm-foo)) (slot-value obj 'x))

(let ((c (find-class 'cm-foo))
      (gf (fdefinition 'cm-probe))
      (sd (car (c2mop:class-direct-slots (find-class 'cm-foo))))
      (esd (car (c2mop:class-slots (find-class 'cm-foo))))
      (inst (make-instance 'cm-foo :x 1 :y 2)))
  (report "classp(c)" (c2mop:classp c))
  (report "class-name" (class-name c))
  (report "direct-slot typep" (typep sd 'c2mop:direct-slot-definition))
  (report "effective-slot typep" (typep esd 'c2mop:effective-slot-definition))
  (report "slot-definition-name" (c2mop:slot-definition-name sd))
  (report "slot-definition-readers" (c2mop:slot-definition-readers sd))
  (report "slot-definition-location" (c2mop:slot-definition-location esd))
  (report "class-precedence-list length"
          (length (c2mop:class-precedence-list c)))
  (report "class-finalized-p" (c2mop:class-finalized-p c))
  (report "ensure-finalized" (eq (c2mop:ensure-finalized c) c))
  (report "subclassp standard-object"
          (c2mop:subclassp c (find-class 'standard-object)))
  (report "generic-function-name" (c2mop:generic-function-name gf))
  (report "generic-function-lambda-list" (c2mop:generic-function-lambda-list gf))
  (report "generic-function-methods length"
          (length (c2mop:generic-function-methods gf)))
  (report "compute-applicable-methods-using-classes validp"
          (multiple-value-bind (ms validp)
              (c2mop:compute-applicable-methods-using-classes
               gf (list (find-class 'cm-foo)))
            (declare (ignore ms))
            validp))
  (report "method-specializers"
          (c2mop:method-specializers
           (car (c2mop:generic-function-methods gf))))
  (report "eql-specializer intern identity"
          (eq (c2mop:intern-eql-specializer 7)
              (c2mop:intern-eql-specializer 7)))
  (report "eql-specializer-object"
          (c2mop:eql-specializer-object (c2mop:intern-eql-specializer 'sym)))
  (report "slot-value via SVUC" (slot-value inst 'x))
  (format t "~%--- closer-mop fork OK ---~%"))
