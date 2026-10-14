;;; verify-boot-image.lisp — prove a deployed amilisp.img is picked up and
;;; usable.  Run it from a directory WITHOUT a amilisp.img (the release
;;; root), so discovery has to take the beside-the-binary leg a deployment
;;; relies on, and without --image:
;;;
;;;   amilisp --no-userinit --non-interactive --load scripts/verify-boot-image.lisp
;;;
;;; Prints BOOT-IMAGE-VERIFIED and exits 0 on success; otherwise one
;;; BOOT-IMAGE-FAILED line per failed check and exit status 1.  A session
;;; that quietly fell back to the FASL boot is a failure here, not a pass.
;;;
;;; Counterpart of scripts/save-boot-image.lisp.  Driven by
;;; verify/realamiga/make-image.sh (FS-UAE), Makefile.mos `image` and the
;;; release script's smoke test; tests/test_boot_image_scripts.sh covers
;;; both scripts on the host.

(defvar *boot-image-failures* 0)

(defmacro boot-image-check (what form)
  `(let ((result (handler-case ,form
                   (error (e)
                     (format t "BOOT-IMAGE-FAILED: ~a signalled: ~a~%" ,what e)
                     :signalled))))
     (cond ((eq result :signalled) (incf *boot-image-failures*))
           ((not result)
            (incf *boot-image-failures*)
            (format t "BOOT-IMAGE-FAILED: ~a~%" ,what)))))

(format t "IMAGE-RESTORED-P ~a~%" ext:*image-restored-p*)
(format t "IMAGE-VERSION ~a~%" (lisp-implementation-version))

(boot-image-check "no image was restored - the session booted from the FASLs instead"
                  ext:*image-restored-p*)

;; boot.lisp content, out of the restored heap
(boot-image-check "LOOP" (= 55 (loop for i from 1 to 10 sum i)))
(boot-image-check "FORMAT directives"
                  (string= "1,2,3" (format nil "~{~a~^,~}" '(1 2 3))))
(boot-image-check "condition system"
                  (eq :caught (handler-case (error "boot image probe")
                                (simple-error () :caught))))

;; clos.lisp content: define new things against the restored metaobjects
(defstruct boot-image-pt x y)
(defclass boot-image-probe () ((x :initarg :x :accessor boot-image-probe-x)))
(defgeneric boot-image-describe (p))
(defmethod boot-image-describe ((p boot-image-probe))
  (format nil "probe ~a" (boot-image-probe-x p)))
(defmethod boot-image-describe ((p integer))
  (format nil "int ~a" p))

(boot-image-check "DEFSTRUCT accessors"
                  (= 7 (boot-image-pt-y (make-boot-image-pt :x 3 :y 7))))
(boot-image-check "CLOS dispatch"
                  (and (string= "probe 42"
                                (boot-image-describe
                                 (make-instance 'boot-image-probe :x 42)))
                       (string= "int 5" (boot-image-describe 5))))
(boot-image-check "CLOS slot writer"
                  (let ((p (make-instance 'boot-image-probe :x 1)))
                    (setf (boot-image-probe-x p) 2)
                    (= 2 (boot-image-probe-x p))))

;; The restored process must still find lib/ (REQUIRE searches the same
;; executable-relative places the boot does).
(boot-image-check "REQUIRE finds lib/ from the restored session"
                  (progn (require "gray-streams")
                         (not (null (find-package "GRAY")))))

(finish-output)
(cond ((zerop *boot-image-failures*)
       (format t "BOOT-IMAGE-VERIFIED~%")
       (finish-output))
      (t
       (format t "BOOT-IMAGE-FAILED: ~a check(s) failed~%" *boot-image-failures*)
       (finish-output)
       (quit 1)))
